#include "Audio/EdgeCache.h"

#include "Audio/SpatialAudioComponent.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"

namespace {
	// Distinct name from the equivalent in AsyncCastManagerSubmit.cpp. Two file-local functions
	// with the same name collide in a unity build.
	bool IsEdgeTraceClear(const FTraceDatum& Datum) {
		return Datum.OutHits.IsEmpty() || !Datum.OutHits[0].bBlockingHit;
	}
}

// The per-edge validation intervals are periods per entry, not per cache. Dividing by the entry
// count holds an edge's check rate steady whether the cache carries one or six, and turns what
// used to be a burst of N submissions on one tick into one submission per slice.
float FEdgeCache::PerEdgeInterval(const USpatialAudioComponent& Component, const float Interval) {
	return Interval / FMath::Max(1, Component.CachedEdgePoints.Num());
}

int32 FEdgeCache::SelectRoundRobinEdge(const TArray<FCachedEdgePoint>& Points, int32& Cursor,
                                       const TFunctionRef<bool(const FCachedEdgePoint&)>& ShouldSkip) {
	const int32 Num = Points.Num();
	for (int32 Step = 0; Step < Num; ++Step) {
		const int32 Candidate = (Cursor + Step) % Num;
		if (!ShouldSkip(Points[Candidate])) {
			Cursor = (Candidate + 1) % Num;
			return Candidate;
		}
	}
	return INDEX_NONE;
}

void FEdgeCache::SubmitPolylineRecheckTraces(USpatialAudioComponent& Component, UWorld* World,
                                             const TArray<FVector>& Path, float EndInset) {
	Component.PathRecheck.Handles.Reset();
	Component.PathRecheck.SegStarts.Reset();
	Component.PathRecheck.SegEnds.Reset();
	for (int32 SegIdx = 0; SegIdx + 1 < Path.Num(); ++SegIdx) {
		FVector SegStart = Path[SegIdx];
		FVector SegEnd = Path[SegIdx + 1];
		const FVector Along = SegEnd - SegStart;
		const float SegLen = Along.Size();
		if (SegLen <= EndInset * 2.f + 1.f) {
			continue;
		}
		const FVector SegDir = Along / SegLen;
		SegStart += SegDir * EndInset;
		SegEnd -= SegDir * EndInset;
		Component.PathRecheck.Handles.Add(Component.SubmitAsyncTrace(World, SegStart, SegEnd));
		Component.PathRecheck.Handles.Add(Component.SubmitAsyncTrace(World, SegEnd, SegStart));
		Component.PathRecheck.SegStarts.Add(SegStart);
		Component.PathRecheck.SegEnds.Add(SegEnd);
	}
}

TArray<float> FEdgeCache::ComputeProgressiveMoveThresholds(const USpatialAudioComponent& Component,
                                                            const FVector& SourcePos, float MoveThreshold) {
	TArray<float> EffectiveThresholds;
	EffectiveThresholds.Init(MoveThreshold, Component.CachedEdgePoints.Num());
	if (MoveThreshold <= 0.f) {
		return EffectiveThresholds;
	}

	struct FSourceDrift { int32 EdgeIdx; float Drift; };
	TArray<FSourceDrift> Ranked;
	for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
		const FCachedEdgePoint& Edge = Component.CachedEdgePoints[i];
		if (!Edge.bEvicting) {
			// FVector::Dist returns double, which a braced initialiser will not narrow to float.
			const float Drift = FVector::Dist(SourcePos, Edge.CapturedSourcePos);
			Ranked.Add({i, Drift});
		}
	}
	Ranked.Sort([](const FSourceDrift& A, const FSourceDrift& B) { return A.Drift > B.Drift; });

	// Stalest edge gets the smallest threshold so one movement cannot expire every edge at once.
	const int32 NumRanked = Ranked.Num();
	for (int32 Rank = 0; Rank < NumRanked; ++Rank) {
		EffectiveThresholds[Ranked[Rank].EdgeIdx] = MoveThreshold * (Rank + 1.f) / NumRanked;
	}
	return EffectiveThresholds;
}

bool FEdgeCache::TickEvictionFade(FCachedEdgePoint& Edge, float DeltaTime, float EvictFadeTime) {
	if (!Edge.bEvicting) {
		return false;
	}
	if (EvictFadeTime <= 0.f) {
		return true;
	}
	Edge.EvictionAlpha -= DeltaTime / EvictFadeTime;
	return Edge.EvictionAlpha <= 0.f;
}

void FEdgeCache::TickPhase0Readback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                    const FVector& SourcePos, const FVector& ListenerPos,
                                    const USpatialAudioSettings& Settings) {
	if (!Edge.bPhase0Pending) {
		return;
	}

	FTraceDatum Phase0Data;
	if (!World->QueryTraceData(Edge.AsyncPhase0Handle, Phase0Data)) {
		return;
	}

	Edge.bPhase0Pending = false;

	bool bBlocked = !IsEdgeTraceClear(Phase0Data);
	if (bBlocked && !Edge.bRelayed
		&& TryPromoteToInnerAnchor(Component, Edge, World, ListenerPos, /*bAllowSubSegmentRefine=*/false)) {
		bBlocked = false;
	}

	if (bBlocked) {
		if (Settings.DirectLoSSampleRadius > 0.f) {
			SubmitPhase0OffsetFan(Component, Edge, World, ListenerPos, Settings.DirectLoSSampleRadius);
		}
		else {
			RescueOrEvict(Component, Edge, World, SourcePos, ListenerPos);
		}
		return;
	}

	if (!Edge.bRelayed) {
		Edge.LastLoSListenerPos = ListenerPos;
		Edge.bHasLastLoSListenerPos = true;
	}
	RestoreFromListenerSideEviction(Edge, SourcePos, ListenerPos);
}

void FEdgeCache::DrawProbeResult(const USpatialAudioComponent& Component, const UWorld* World,
                                 const FVector& Point, const bool bClear) {
	if (!Component.bDrawDebugRays || !Component.bShowShortestPaths) {
		return;
	}
	// Outlives the regular debug lines so probe patterns stay readable between check intervals.
	DrawDebugSphere(World, Point, 7.f, 6, bClear ? FColor::Green : FColor::Red,
	                false, Component.GetSettings().DebugLineDuration * 4.f, SDPG_Foreground, 1.5f);
}

bool FEdgeCache::ProbeListenerLoSPoint(const USpatialAudioComponent& Component, const UWorld* World,
                                       const FVector& ListenerPos, const FVector& Point) {
	FHitResult Hit;
	const bool bClear = !Component.TraceLine(World, Hit, ListenerPos, Point)
		&& !Component.TraceLine(World, Hit, Point, ListenerPos);
	DrawProbeResult(Component, World, Point, bClear);
	return bClear;
}

// Only ever returns a point that traced clear, so a non-monotone segment can cost an improvement
// but never move the edge into geometry.
int32 FEdgeCache::ResolveStepsForMergeRadius(const USpatialAudioComponent& Component, const float Span) {
	// Converging to the merge radius keeps two relays bisecting toward one corner close enough for
	// MergeCoincidentEdges to collapse them. A fixed count resolves a long leg too coarsely.
	const float Tolerance = FMath::Max(Component.GetSettings().CachedEdgeMergeRadius * 0.5f, 1.f);
	return FMath::Clamp(FMath::CeilToInt(FMath::Log2(FMath::Max(Span / Tolerance, 1.f))), 5, 10);
}

FVector FEdgeCache::BisectListenerLoS(const USpatialAudioComponent& Component, const UWorld* World, const FVector& ListenerPos,
                                      const FVector& BlockedEnd, const FVector& ClearEnd, bool& bOutFoundClear,
                                      const int32 ExplicitSteps) {
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::Bisect);
	FVector BlockedPoint = BlockedEnd;
	FVector ClearPoint = ClearEnd;
	bOutFoundClear = false;

	const int32 Steps = ExplicitSteps > 0
		                        ? ExplicitSteps
		                        : ResolveStepsForMergeRadius(Component, FVector::Dist(BlockedPoint, ClearPoint));

	for (int32 Step = 0; Step < Steps; ++Step) {
		const FVector Mid = (BlockedPoint + ClearPoint) * 0.5f;
		if (ProbeListenerLoSPoint(Component, World, ListenerPos, Mid)) {
			ClearPoint = Mid;
			bOutFoundClear = true;
		}
		else {
			BlockedPoint = Mid;
		}
	}
	return ClearPoint;
}

bool FEdgeCache::TryPromoteToInnerAnchor(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
                                         const FVector& ListenerPos, const bool bAllowSubSegmentRefine) {
	if (Edge.ShortestPath.Num() < 2) {
		return false;
	}
	const FVector InnerAnchor = Edge.ShortestPath[Edge.ShortestPath.Num() - 2];
	if (TryJumpToPreviousVertex(Component, Edge, World, ListenerPos, InnerAnchor)) {
		return true;
	}
	if (!bAllowSubSegmentRefine) {
		return false;
	}
	return TryRefineAlongFinalSegment(Component, Edge, World, ListenerPos, InnerAnchor);
}

bool FEdgeCache::TryJumpToPreviousVertex(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                         const UWorld* World, const FVector& ListenerPos,
                                         const FVector& InnerAnchor) {
	if (!ProbeListenerLoSPoint(Component, World, ListenerPos, InnerAnchor)) {
		return false;
	}
	Edge.PathDist = FMath::Max(0.f, Edge.PathDist - FVector::Dist(InnerAnchor, Edge.EdgePoint));
	Edge.EdgePoint = InnerAnchor;
	Edge.ShortestPath.Pop();
	if (!Edge.ShortestPathSegmentVerified.IsEmpty()) {
		Edge.ShortestPathSegmentVerified.Pop();
	}
	Edge.GeomDist = FVector::Dist(Edge.CapturedSourcePos, Edge.EdgePoint);
	return true;
}

bool FEdgeCache::TryRefineAlongFinalSegment(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
                                            const FVector& ListenerPos, const FVector& InnerAnchor) {
	// Unverified hops hug geometry, so a point partway along one can sit inside a wall.
	const int32 SegIdx = Edge.ShortestPath.Num() - 2;
	if (!Edge.ShortestPathSegmentVerified.IsValidIndex(SegIdx) || !Edge.ShortestPathSegmentVerified[SegIdx]) {
		return false;
	}

	// Below this the refined point is within trace noise of the current edge and would just jitter.
	const float MinMove = FMath::Max(Component.GetSettings().RaySurfaceBias * 4.f, 4.f);
	if (FVector::Dist(InnerAnchor, Edge.EdgePoint) <= MinMove * 2.f) {
		return false;
	}

	bool bFoundClear = false;
	const FVector RefinedPoint = BisectListenerLoS(Component, World, ListenerPos, InnerAnchor, Edge.EdgePoint,
	                                               bFoundClear,
	                                               Component.GetSettings().ShortestPathPromotionBisectSteps);
	if (!bFoundClear || FVector::Dist(RefinedPoint, Edge.EdgePoint) < MinMove) {
		return false;
	}

	// On the same segment, so the last polyline vertex moves inward with it rather than popping.
	Edge.PathDist = FMath::Max(0.f, Edge.PathDist - FVector::Dist(RefinedPoint, Edge.EdgePoint));
	Edge.EdgePoint = RefinedPoint;
	Edge.ShortestPath.Last() = RefinedPoint;
	Edge.GeomDist = FVector::Dist(Edge.CapturedSourcePos, Edge.EdgePoint);
	return true;
}

// Candidates must go through ResolveOffsetPoint: a point embedded in a wall traces outward with a
// silent false-clear.
void FEdgeCache::SubmitPhase0OffsetFan(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                       const FVector& ListenerPos, float OffsetRadius) {
	if (Edge.bPhase0OffsetPending) {
		return;
	}

	const FVector Target = Edge.EffectivePoint();
	const FVector ToListenerDir = (ListenerPos - Target).GetSafeNormal();
	FVector RightDir = FVector::CrossProduct(ToListenerDir, FVector::UpVector).GetSafeNormal();
	if (RightDir.IsNearlyZero()) {
		RightDir = FVector::CrossProduct(ToListenerDir, FVector::RightVector).GetSafeNormal();
	}

	const FVector Candidates[4] = {
		ListenerPos + FVector::UpVector * OffsetRadius,
		ListenerPos - FVector::UpVector * OffsetRadius,
		ListenerPos + RightDir * OffsetRadius,
		ListenerPos - RightDir * OffsetRadius,
	};
	for (int32 i = 0; i < 4; ++i) {
		Edge.Phase0OffsetPts[i] = FUpdater::ResolveOffsetPoint(Component, World, ListenerPos, Candidates[i]);
		Edge.Phase0OffsetHandles[i] = Component.SubmitAsyncTrace(World, Edge.Phase0OffsetPts[i], Target);
	}
	Edge.bPhase0OffsetPending = true;
}

void FEdgeCache::TickPhase0OffsetReadback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                          const FVector& SourcePos, const FVector& ListenerPos) {
	if (!Edge.bPhase0OffsetPending) {
		return;
	}

	bool bFanClear[4] = {};
	if (!ReadOffsetFanTraces(Edge, World, bFanClear)) {
		return;
	}

	DrawOffsetFan(Component, Edge, World, bFanClear);

	int32 ClearIdx = INDEX_NONE;
	for (int32 i = 0; i < 4; ++i) {
		if (bFanClear[i]) {
			ClearIdx = i;
			break;
		}
	}

	if (ClearIdx == INDEX_NONE) {
		RescueOrEvict(Component, Edge, World, SourcePos, ListenerPos);
		return;
	}

	if (!Edge.bRelayed) {
		// Not ListenerPos: the centre was blocked, so a relay anchored there fails the same corner.
		Edge.LastLoSListenerPos = Edge.Phase0OffsetPts[ClearIdx];
		Edge.bHasLastLoSListenerPos = true;
	}
	RestoreFromListenerSideEviction(Edge, SourcePos, ListenerPos);
}

bool FEdgeCache::ReadOffsetFanTraces(FCachedEdgePoint& Edge, UWorld* World, bool (&OutFanClear)[4]) {
	FTraceDatum Data[4];
	for (int32 i = 0; i < 4; ++i) {
		if (!World->QueryTraceData(Edge.Phase0OffsetHandles[i], Data[i])) {
			return false;
		}
	}
	Edge.bPhase0OffsetPending = false;
	for (int32 i = 0; i < 4; ++i) {
		OutFanClear[i] = IsEdgeTraceClear(Data[i]);
	}
	return true;
}

void FEdgeCache::DrawOffsetFan(const USpatialAudioComponent& Component, const FCachedEdgePoint& Edge, const UWorld* World,
                               const bool (&FanClear)[4]) {
	if (!Component.bDrawDebugRays || !Component.bShowOffsetLoSChecks) {
		return;
	}
	for (int32 i = 0; i < 4; ++i) {
		DrawDebugLine(World, Edge.Phase0OffsetPts[i], Edge.EffectivePoint(),
		              FanClear[i] ? FColor::Green : FColor::Red, false,
		              Component.GetSettings().DebugLineDuration, 0, 0.75f);
	}
}

void FEdgeCache::RestoreFromListenerSideEviction(FCachedEdgePoint& Edge, const FVector& SourcePos,
                                                 const FVector& ListenerPos) {
	if (!Edge.bEvicting || Edge.bSourceSideEviction) {
		return;
	}
	Edge.bEvicting = false;
	Edge.EvictionAlpha = 1.f;
	Edge.CapturedSourcePos = SourcePos;
	Edge.CapturedListenerPos = ListenerPos;
}

void FEdgeCache::RescueOrEvict(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                               const FVector& SourcePos, const FVector& ListenerPos) {
	if (!SubmitRelayRescueTraces(Component, Edge, World, ListenerPos)) {
		StartEviction(Component, Edge, SourcePos);
	}
}

// Deliberately ignores sibling edges. A "skip while any direct edge exists" gate made the outcome
// depend on processing order and killed most of a batch of simultaneous losses.
bool FEdgeCache::SubmitRelayRescueTraces(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                         UWorld* World, const FVector& ListenerPos) {
	if (Edge.bRescuePending) {
		return true;
	}
	if (Edge.bRelayed || !Edge.bHasLastLoSListenerPos) {
		return false;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::Relay);
	const FVector& Relay = Edge.LastLoSListenerPos;
	Edge.RescueHandles[0] = Component.SubmitAsyncTrace(World, Edge.EdgePoint, Relay);
	Edge.RescueHandles[1] = Component.SubmitAsyncTrace(World, Relay, Edge.EdgePoint);
	Edge.RescueHandles[2] = Component.SubmitAsyncTrace(World, Relay, ListenerPos);
	Edge.RescueHandles[3] = Component.SubmitAsyncTrace(World, ListenerPos, Relay);
	Edge.bRescuePending = true;
	return true;
}

void FEdgeCache::TickRelayRescueReadback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                         const FVector& SourcePos, const FVector& ListenerPos) {
	if (!Edge.bRescuePending) {
		return;
	}

	FTraceDatum Data[4];
	for (int32 i = 0; i < 4; ++i) {
		if (!World->QueryTraceData(Edge.RescueHandles[i], Data[i])) {
			return;
		}
	}
	Edge.bRescuePending = false;

	// The source moved while the traces flew. A relay only re-verifies the listener legs, so it
	// cannot speak for a stale source side.
	if (Edge.bEvicting && Edge.bSourceSideEviction) {
		return;
	}

	for (const FTraceDatum& Leg : Data) {
		if (!IsEdgeTraceClear(Leg)) {
			StartEviction(Component, Edge, SourcePos);
			return;
		}
	}

	Edge.bRelayed = true;
	Edge.RelayPoint = Edge.LastLoSListenerPos;
	Edge.RelayDist = FVector::Dist(Edge.EdgePoint, Edge.RelayPoint);
	Edge.bEvicting = false;
	Edge.EvictionAlpha = 1.f;
	// Reset the movement baseline, or the walk to the second corner evicts the fresh relay at once.
	Edge.CapturedListenerPos = ListenerPos;
}

// While relayed, Phase 0 only watches the relay point, so these two legs need their own re-check.
void FEdgeCache::TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                      const FVector& SourcePos, const FVector& ListenerPos, bool bIntervalFired) {
	if (!Edge.bRelayed) {
		return;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::Relay);
	// No yield-to-direct-edge check here. Every relay converts on its first readback, so the yield
	// only ever killed siblings mid-conversion.

	if (!Edge.bRelayCheckPending) {
		if (bIntervalFired) {
			SubmitRelayCheckTraces(Component, Edge, World, ListenerPos);
		}
		return;
	}

	FTraceDatum Data[4];
	if (!ReadRelayCheckTraces(Edge, World, Data)) {
		return;
	}

	if (IsEdgeTraceClear(Data[0]) && IsEdgeTraceClear(Data[1])) {
		Edge.ClearRelay();
		Edge.LastLoSListenerPos = ListenerPos;
		Edge.bHasLastLoSListenerPos = true;
		Edge.CapturedListenerPos = ListenerPos;
		return;
	}

	// Relay dropped before evicting, or Phase 0's clear-restore would keep resurrecting a path
	// that is already broken upstream.
	if (!IsEdgeTraceClear(Data[2]) || !IsEdgeTraceClear(Data[3])) {
		Edge.ClearRelay();
		StartEviction(Component, Edge, SourcePos);
		return;
	}

	ConvertRelayToEdge(Component, Edge, World, ListenerPos);
}

bool FEdgeCache::ReadRelayCheckTraces(FCachedEdgePoint& Edge, UWorld* World, FTraceDatum (&OutData)[4]) {
	for (int32 i = 0; i < 4; ++i) {
		if (!World->QueryTraceData(Edge.RelayCheckHandles[i], OutData[i])) {
			return false;
		}
	}
	Edge.bRelayCheckPending = false;
	return true;
}

void FEdgeCache::SubmitRelayCheckTraces(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                        UWorld* World, const FVector& ListenerPos) {
	Edge.RelayCheckHandles[0] = Component.SubmitAsyncTrace(World, ListenerPos, Edge.EdgePoint);
	Edge.RelayCheckHandles[1] = Component.SubmitAsyncTrace(World, Edge.EdgePoint, ListenerPos);
	Edge.RelayCheckHandles[2] = Component.SubmitAsyncTrace(World, Edge.EdgePoint, Edge.RelayPoint);
	Edge.RelayCheckHandles[3] = Component.SubmitAsyncTrace(World, Edge.RelayPoint, Edge.EdgePoint);
	Edge.bRelayCheckPending = true;
}

// The LoS transition along the edge to relay leg is the second corner the relay bends around.
// Converting leaves the entry backed by verified geometry, so it counts as a real edge again.
void FEdgeCache::ConvertRelayToEdge(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
                                    const FVector& ListenerPos) {
	if (Edge.ShortestPath.Num() < 2) {
		// Same straight source to edge fallback the recheck uses.
		Edge.ShortestPath = {Edge.CapturedSourcePos, Edge.EdgePoint};
		Edge.ShortestPathSegmentVerified = {true};
	}

	bool bFoundClear = false;
	const FVector Corner = BisectListenerLoS(Component, World, ListenerPos, Edge.EdgePoint, Edge.RelayPoint,
	                                         bFoundClear);

	Edge.PathDist += FVector::Dist(Edge.EdgePoint, Corner);
	Edge.EdgePoint = Corner;
	Edge.ShortestPath.Add(Corner);
	Edge.ShortestPathSegmentVerified.Add(true);
	Edge.LoSBounces += 1;
	Edge.GeomDist = FVector::Dist(Edge.CapturedSourcePos, Edge.EdgePoint);
	Edge.CapturedListenerPos = ListenerPos;
	Edge.bNewSinceFillArm = true;
	Edge.LastLoSListenerPos = ListenerPos;
	Edge.bHasLastLoSListenerPos = true;
	Edge.ClearRelay();
}

void FEdgeCache::StartEviction(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const FVector& SourcePos,
                               const bool bSourceSide) {
	if (Edge.bEvicting) {
		return;
	}
	Edge.bEvicting = true;
	Edge.bSourceSideEviction = bSourceSide;
	Component.SweepScheduling.bMovementRequested = true;
}

bool FEdgeCache::TickMovementThresholdEviction(USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                               const FVector& SourcePos, float EffectiveMoveThreshold) {
	if (Edge.bEvicting || EffectiveMoveThreshold <= 0.f) {
		return false;
	}

	// Source movement only. It stales the captured source-side data and nothing else revalidates
	// that side. Listener validity is Phase 0's job, so listener movement never evicts.
	const float SrcDelta = FVector::Dist(SourcePos, Edge.CapturedSourcePos);
	if (SrcDelta <= EffectiveMoveThreshold) {
		return false;
	}

	Edge.bEvicting = true;
	Edge.bSourceSideEviction = true;
	Edge.EvictionAlpha = 1.f;
	Component.SweepScheduling.bMovementRequested = true;
	return true;
}

void FEdgeCache::TickPhase0Submission(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                      const FVector& ListenerPos, bool bIntervalFired) {
	if (!Edge.bPhase0Pending && !Edge.bPhase0OffsetPending && !Edge.bRescuePending && bIntervalFired
		&& !(Edge.bEvicting && Edge.bSourceSideEviction)) {
		Edge.AsyncPhase0Handle = Component.SubmitAsyncTrace(World, ListenerPos, Edge.EffectivePoint());
		Edge.bPhase0Pending = true;
	}
}

void FEdgeCache::ClearStalePendingChecks(USpatialAudioComponent& Component) {
	if (Component.bPhase0HandlesStale) {
		for (FCachedEdgePoint& Edge : Component.CachedEdgePoints) {
			Edge.bPhase0Pending = false;
			Edge.bPhase0OffsetPending = false;
			Edge.bRelayCheckPending = false;
			Edge.bRescuePending = false;
		}
		Component.PathRecheck.bPending = false;
	}
	Component.bPhase0HandlesStale = false;
}

bool FEdgeCache::AdvancePhase0Timer(USpatialAudioComponent& Component, const float DeltaTime,
                                    const USpatialAudioSettings& Settings) {
	Component.Phase0Timer += DeltaTime;
	const float Slice = PerEdgeInterval(
		Component, Settings.Phase0CheckInterval * Component.VelocityScaling.EdgeMultiplier);
	if (Component.Phase0Timer < Slice) {
		return false;
	}
	Component.Phase0Timer = 0.f;
	return true;
}

bool FEdgeCache::TickSingleEdge(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                const FVector& SourcePos, const FVector& ListenerPos, const float DeltaTime,
                                const float EffectiveMoveThreshold, const bool bIntervalFired,
                                const USpatialAudioSettings& Settings) {
	if (TickEvictionFade(Edge, DeltaTime, Settings.CachedEdgeEvictionFadeTime)) {
		return true;
	}

	TickRelayRescueReadback(Component, Edge, World, SourcePos, ListenerPos);
	TickPhase0Readback(Component, Edge, World, SourcePos, ListenerPos, Settings);
	TickPhase0OffsetReadback(Component, Edge, World, SourcePos, ListenerPos);

	// Already fading out, so skip the re-checks below and save the traces.
	if (TickMovementThresholdEviction(Component, Edge, SourcePos, EffectiveMoveThreshold)) {
		return false;
	}

	TickRelayMaintenance(Component, Edge, World, SourcePos, ListenerPos, bIntervalFired);
	TickPhase0Submission(Component, Edge, World, ListenerPos, bIntervalFired);
	return false;
}

void FEdgeCache::TickCachedEdgeEviction(USpatialAudioComponent& Component, const float DeltaTime, const USpatialAudioSettings& Settings) {
	Component.CurrentTraceBucket = USpatialAudioComponent::ETraceBucket::Phase0;
	if (Component.bHasDirectLoS) {
		Component.bPhase0HandlesStale = true;
		return;
	}

	UWorld* World = Component.GetWorld();
	AActor* OwnerActor = Component.GetOwner();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!World || !OwnerActor || !Pawn) {
		return;
	}

	const FVector SourcePos = OwnerActor->GetActorLocation();
	const FVector ListenerPos = Pawn->GetActorLocation();

	ClearStalePendingChecks(Component);

	// One entry per slice rather than the whole cache at once. The timer is already divided by
	// the entry count, so each edge still comes round every Phase0CheckInterval.
	// Passing over source-side evictions matches TickPhase0Submission's own guard, so a slice is
	// never spent on an entry that would decline to submit.
	const int32 SubmitIdx = AdvancePhase0Timer(Component, DeltaTime, Settings)
		                        ? SelectRoundRobinEdge(Component.CachedEdgePoints, Component.Phase0Cursor,
		                                               [](const FCachedEdgePoint& Edge) {
			                                               return Edge.bEvicting && Edge.bSourceSideEviction;
		                                               })
		                        : INDEX_NONE;

	const TArray<float> EffectiveThresholds = ComputeProgressiveMoveThresholds(
		Component, SourcePos, Settings.CachedEdgeUpdateMoveThreshold);

	// Backwards so a removal cannot disturb the indices still to come, which also keeps SubmitIdx
	// pointing at the entry it was chosen for.
	for (int32 i = Component.CachedEdgePoints.Num() - 1; i >= 0; --i) {
		if (TickSingleEdge(Component, Component.CachedEdgePoints[i], World, SourcePos, ListenerPos,
		                   DeltaTime, EffectiveThresholds[i], i == SubmitIdx, Settings)) {
			Component.CachedEdgePoints.RemoveAt(i);
		}
	}

	TickShortestPathReadback(Component, World, SourcePos, Settings);
	TickShortestPathRecheck(Component, World, SourcePos, DeltaTime, Settings);
	TickInnerAnchorPromotion(Component, World, ListenerPos, DeltaTime, Settings);

	// Runs last because every EdgePoint move above can land an entry on top of another.
	MergeCoincidentEdges(Component, Settings);
}

// Cached entries drift onto each other as relay conversion and promotion move them, and the
// sweep's merge radius only gates incoming finds. Duplicates on one corner each cost a cache slot,
// a ray substitution, an exclusion direction, and a share of the voice mix.
// Keyed on EdgePoint rather than EffectivePoint(): relays rescued through the same fan point
// share a RelayPoint while their real corners differ.
void FEdgeCache::MergeCoincidentEdges(USpatialAudioComponent& Component,
                                      const USpatialAudioSettings& Settings) {
	const float MergeRadiusSq = FMath::Square(Settings.CachedEdgeMergeRadius);
	if (MergeRadiusSq <= 0.f) {
		return;
	}

	// Both loops run backward so a removal can only shift entries already visited.
	for (int32 i = Component.CachedEdgePoints.Num() - 1; i >= 0; --i) {
		if (!IsMergeCandidate(Component.CachedEdgePoints[i])) {
			continue;
		}
		for (int32 j = i - 1; j >= 0; --j) {
			FCachedEdgePoint& Later = Component.CachedEdgePoints[i];
			FCachedEdgePoint& Earlier = Component.CachedEdgePoints[j];
			if (!IsMergeCandidate(Earlier)
				|| FVector::DistSquared(Later.EdgePoint, Earlier.EdgePoint) >= MergeRadiusSq) {
				continue;
			}
			// Survives either way, or the cache-fill burst re-surveys a position it has covered.
			const bool bEitherIsNew = Later.bNewSinceFillArm || Earlier.bNewSinceFillArm;

			if (TravelledFurther(Earlier, Later)) {
				Later.bNewSinceFillArm = bEitherIsNew;
				Component.CachedEdgePoints.RemoveAt(j);
				// i slid down with the removal; keep scanning the remaining lower entries so a
				// three-way pile collapses in one pass rather than one merge per tick.
				--i;
				continue;
			}
			Earlier.bNewSinceFillArm = bEitherIsNew;
			Component.CachedEdgePoints.RemoveAt(i);
			break;
		}
	}
}

bool FEdgeCache::IsMergeCandidate(const FCachedEdgePoint& Edge) {
	return !Edge.bRelayed && !Edge.bEvicting;
}

bool FEdgeCache::TravelledFurther(const FCachedEdgePoint& Edge, const FCachedEdgePoint& Other) {
	return Edge.EffectivePathDist() > Other.EffectivePathDist();
}

// Nothing else watches the source side of a cached path. Unverified segments are checked too, so
// this also evicts on ordinary multi-corner paths rather than only on real geometry change.
void FEdgeCache::TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
                                         const FVector& SourcePos, const float DeltaTime,
                                         const USpatialAudioSettings& Settings) {
	if (Settings.ShortestPathRecheckInterval <= 0.f || Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::PathCheck);
	Component.ShortestPathCheckTimer += DeltaTime;
	if (Component.ShortestPathCheckTimer < PerEdgeInterval(Component, Settings.ShortestPathRecheckInterval)
		|| Component.PathRecheck.bPending) {
		return;
	}
	Component.ShortestPathCheckTimer = 0.f;

	const int32 Idx = SelectRoundRobinEdge(Component.CachedEdgePoints, Component.ShortestPathCheckCursor,
	                                       [](const FCachedEdgePoint& Edge) { return Edge.bEvicting; });
	if (Idx == INDEX_NONE) {
		return;
	}

	FCachedEdgePoint& Edge = Component.CachedEdgePoints[Idx];

	TArray<FVector> StraightFallback;
	const TArray<FVector>& Path = ResolveRecheckPath(Edge, StraightFallback);

	// A trace grazing its own anchor surface is corner clipping, not an obstruction.
	const float EndInset = FMath::Max(Settings.RaySurfaceBias, 1.f);
	SubmitPolylineRecheckTraces(Component, World, Path, EndInset);
	if (Component.PathRecheck.Handles.IsEmpty()) {
		return;
	}
	Component.PathRecheck.EdgePoint = Edge.EdgePoint;
	Component.PathRecheck.bPending = true;
}

const TArray<FVector>& FEdgeCache::ResolveRecheckPath(const FCachedEdgePoint& Edge,
                                                     TArray<FVector>& OutStraightFallback) {
	if (Edge.ShortestPath.Num() >= 2) {
		return Edge.ShortestPath;
	}
	OutStraightFallback = {Edge.CapturedSourcePos, Edge.EdgePoint};
	return OutStraightFallback;
}

int32 FEdgeCache::FindFirstBlockedSegment(const TArray<FTraceDatum>& Data) {
	for (int32 i = 0; i < Data.Num(); ++i) {
		if (!IsEdgeTraceClear(Data[i])) {
			return i / 2;
		}
	}
	return INDEX_NONE;
}

FCachedEdgePoint* FEdgeCache::FindEntryByExactEdgePoint(USpatialAudioComponent& Component,
                                                        const FVector& EdgePoint) {
	for (FCachedEdgePoint& Edge : Component.CachedEdgePoints) {
		if (!Edge.bEvicting && Edge.EdgePoint == EdgePoint) {
			return &Edge;
		}
	}
	return nullptr;
}

void FEdgeCache::TickShortestPathReadback(USpatialAudioComponent& Component, UWorld* World,
                                          const FVector& SourcePos, const USpatialAudioSettings& Settings) {
	if (!Component.PathRecheck.bPending) {
		return;
	}
	TArray<FTraceDatum> Data;
	Data.SetNum(Component.PathRecheck.Handles.Num());
	for (int32 i = 0; i < Component.PathRecheck.Handles.Num(); ++i) {
		if (!World->QueryTraceData(Component.PathRecheck.Handles[i], Data[i])) {
			return;
		}
	}
	Component.PathRecheck.bPending = false;

	const int32 BlockedSeg = FindFirstBlockedSegment(Data);
	if (BlockedSeg == INDEX_NONE) {
		return;
	}

	FCachedEdgePoint* Edge = FindEntryByExactEdgePoint(Component, Component.PathRecheck.EdgePoint);
	if (!Edge) {
		return;
	}

	if (Component.bDrawDebugRays && Component.bShowShortestPaths) {
		DrawDebugLine(World, Component.PathRecheck.SegStarts[BlockedSeg],
		              Component.PathRecheck.SegEnds[BlockedSeg], FColor::Red, false,
		              Settings.DebugLineDuration * 4.f, 0, 3.f);
	}
	// The listener leg is typically still clear here, and Phase 0's clear-restore would otherwise
	// resurrect the edge faster than the fade completes.
	StartEviction(Component, *Edge, SourcePos, /*bSourceSide=*/true);
}

// The promotion inside TickPhase0Readback only fires once an edge goes blocked, so an edge with
// clear LoS since discovery would otherwise never migrate inward when a shorter path opens up.
void FEdgeCache::TickInnerAnchorPromotion(USpatialAudioComponent& Component, const UWorld* World,
                                          const FVector& ListenerPos, const float DeltaTime,
                                          const USpatialAudioSettings& Settings) {
	if (Settings.ShortestPathPromotionInterval <= 0.f || Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::PathCheck);
	Component.ShortestPathPromotionTimer += DeltaTime;
	if (Component.ShortestPathPromotionTimer < PerEdgeInterval(Component, Settings.ShortestPathPromotionInterval)) {
		return;
	}
	Component.ShortestPathPromotionTimer = 0.f;

	const int32 Idx = SelectRoundRobinEdge(Component.CachedEdgePoints, Component.ShortestPathPromotionCursor,
	                                       [](const FCachedEdgePoint& Edge) { return Edge.bEvicting || Edge.bRelayed; });
	if (Idx == INDEX_NONE) {
		return;
	}

	TryPromoteToInnerAnchor(Component, Component.CachedEdgePoints[Idx], World, ListenerPos,
	                        /*bAllowSubSegmentRefine=*/true);
}
