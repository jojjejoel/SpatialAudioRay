#include "Audio/EdgeCache.h"

#include "Audio/Math.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"

float FEdgeCache::PerEdgeInterval(const USpatialAudioComponent& Component, const float Interval) {
	return Interval / FMath::Max(1, Component.CachedEdgePoints.Num());
}

float FEdgeCache::EdgeCheckSlice(const USpatialAudioComponent& Component,
                                 const USpatialAudioSettings& Settings) {
	return PerEdgeInterval(Component, Settings.CachedEdgeCheckInterval
	                       * Math::ShareOfBudgetStretch(Component.TraceBudgetStretch, Math::EdgeCheckBudgetShare));
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
                                             const TArray<FVector>& Path, const TArray<bool>& SegmentVerified,
                                             float EndInset) {
	Component.PathRecheck.Handles.Reset();
	Component.PathRecheck.SegStarts.Reset();
	Component.PathRecheck.SegEnds.Reset();
	for (int32 SegIdx = 0; SegIdx + 1 < Path.Num(); ++SegIdx) {
		if (!SegmentVerified.IsValidIndex(SegIdx) || !SegmentVerified[SegIdx]) {
			continue;
		}
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

void FEdgeCache::TickPhase0Readback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                    const FVector& ListenerPos, const USpatialAudioSettings& Settings) {
	if (!Edge.bPhase0Pending) {
		return;
	}

	FTraceDatum Phase0Data;
	if (!World->QueryTraceData(Edge.AsyncPhase0Handle, Phase0Data)) {
		return;
	}

	Edge.bPhase0Pending = false;

	bool bBlocked = !IsTraceClear(Phase0Data);
	if (bBlocked && !Edge.bRelayed
		&& TryPromoteToInnerAnchor(Component, Edge, World, ListenerPos, /*bAllowSubSegmentRefine=*/false)) {
		bBlocked = false;
	}

	if (bBlocked) {
		if (Settings.DirectLoSSampleRadius > 0.f) {
			SubmitPhase0OffsetFan(Component, Edge, World, ListenerPos, Settings.DirectLoSSampleRadius);
		}
		else {
			RescueOrEvict(Component, Edge, World, ListenerPos);
		}
		return;
	}

	if (!Edge.bRelayed) {
		Edge.LastLoSListenerPos = ListenerPos;
		Edge.bHasLastLoSListenerPos = true;
	}
}

void FEdgeCache::DrawProbeResult(const USpatialAudioComponent& Component, const UWorld* World,
                                 const FVector& Point, const bool bClear) {
	if (!Component.bDrawDebugRays || !Component.bShowShortestPaths) {
		return;
	}
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

int32 FEdgeCache::ResolveStepsForMergeRadius(const USpatialAudioComponent& Component, const float Span) {
	const float Tolerance = FMath::Max(Component.GetSettings().CachedEdgeMergeRadius * 0.5f, 1.f);
	return FMath::Clamp(FMath::CeilToInt(FMath::Log2(FMath::Max(Span / Tolerance, 1.f))), 5, 10);
}

FVector FEdgeCache::BisectListenerLoS(const USpatialAudioComponent& Component, const UWorld* World,
                                      const FVector& ListenerPos,
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

bool FEdgeCache::TryPromoteToInnerAnchor(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                         const UWorld* World,
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

bool FEdgeCache::TryRefineAlongFinalSegment(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                            const UWorld* World,
                                            const FVector& ListenerPos, const FVector& InnerAnchor) {
	const int32 SegIdx = Edge.ShortestPath.Num() - 2;
	if (!Edge.ShortestPathSegmentVerified.IsValidIndex(SegIdx) || !Edge.ShortestPathSegmentVerified[SegIdx]) {
		return false;
	}

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

	Edge.PathDist = FMath::Max(0.f, Edge.PathDist - FVector::Dist(RefinedPoint, Edge.EdgePoint));
	Edge.EdgePoint = RefinedPoint;
	Edge.ShortestPath.Last() = RefinedPoint;
	Edge.GeomDist = FVector::Dist(Edge.CapturedSourcePos, Edge.EdgePoint);
	return true;
}

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
                                          const FVector& ListenerPos) {
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
		RescueOrEvict(Component, Edge, World, ListenerPos);
		return;
	}

	if (!Edge.bRelayed) {
		Edge.LastLoSListenerPos = Edge.Phase0OffsetPts[ClearIdx];
		Edge.bHasLastLoSListenerPos = true;
	}
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
		OutFanClear[i] = IsTraceClear(Data[i]);
	}
	return true;
}

void FEdgeCache::DrawOffsetFan(const USpatialAudioComponent& Component, const FCachedEdgePoint& Edge,
                               const UWorld* World,
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

void FEdgeCache::RescueOrEvict(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                               const FVector& ListenerPos) {
	if (!SubmitRelayRescueTraces(Component, Edge, World, ListenerPos)) {
		StartEviction(Component, Edge);
	}
}

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
                                         const FVector& ListenerPos) {
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

	if (Edge.bEvicting) {
		return;
	}

	for (const FTraceDatum& Leg : Data) {
		if (!IsTraceClear(Leg)) {
			StartEviction(Component, Edge);
			return;
		}
	}

	Edge.bRelayed = true;
	Edge.RelayPoint = Edge.LastLoSListenerPos;
	Edge.RelayDist = FVector::Dist(Edge.EdgePoint, Edge.RelayPoint);
	Edge.CapturedListenerPos = ListenerPos;
}

void FEdgeCache::TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                      const FVector& ListenerPos, bool bIntervalFired) {
	if (!Edge.bRelayed) {
		return;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::Relay);

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

	if (IsTraceClear(Data[0]) && IsTraceClear(Data[1])) {
		Edge.ClearRelay();
		Edge.LastLoSListenerPos = ListenerPos;
		Edge.bHasLastLoSListenerPos = true;
		Edge.CapturedListenerPos = ListenerPos;
		return;
	}

	if (!IsTraceClear(Data[2]) || !IsTraceClear(Data[3])) {
		Edge.ClearRelay();
		StartEviction(Component, Edge);
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

void FEdgeCache::ConvertRelayToEdge(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                    const UWorld* World,
                                    const FVector& ListenerPos) {
	if (Edge.ShortestPath.Num() < 2) {
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
	Edge.GeomDist = FVector::Dist(Edge.CapturedSourcePos, Edge.EdgePoint);
	Edge.CapturedListenerPos = ListenerPos;
	Edge.bNewSinceFillArm = true;
	Edge.LastLoSListenerPos = ListenerPos;
	Edge.bHasLastLoSListenerPos = true;
	Edge.ClearRelay();
}

void FEdgeCache::StartEviction(USpatialAudioComponent& Component, FCachedEdgePoint& Edge) {
	Edge.bEvicting = true;
	Component.SweepScheduling.bEarlySweepRequested = true;
}

void FEdgeCache::TickPhase0Submission(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                      const FVector& ListenerPos, bool bIntervalFired) {
	if (!Edge.bPhase0Pending && !Edge.bPhase0OffsetPending && !Edge.bRescuePending && bIntervalFired
		&& !Edge.bEvicting) {
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
	const float Slice = EdgeCheckSlice(Component, Settings);
	if (Component.Phase0Timer < Slice) {
		return false;
	}
	Component.Phase0Timer = 0.f;
	return true;
}

bool FEdgeCache::TickSingleEdge(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
                                const FVector& ListenerPos, const bool bIntervalFired,
                                const USpatialAudioSettings& Settings) {
	if (Edge.bEvicting) {
		return true;
	}

	TickRelayRescueReadback(Component, Edge, World, ListenerPos);
	TickPhase0Readback(Component, Edge, World, ListenerPos, Settings);
	TickPhase0OffsetReadback(Component, Edge, World, ListenerPos);

	TickRelayMaintenance(Component, Edge, World, ListenerPos, bIntervalFired);
	TickPhase0Submission(Component, Edge, World, ListenerPos, bIntervalFired);
	return false;
}

void FEdgeCache::TickCachedEdgeEviction(USpatialAudioComponent& Component, const float DeltaTime,
                                        const USpatialAudioSettings& Settings) {
	Component.CurrentTraceBucket = USpatialAudioComponent::ETraceBucket::Phase0;
	/** Entries are kept, not cleared, so returning restores the diffraction rather than rebuilding it. */
	if (Component.bHasDirectLoS || !Component.bInAudibleRange) {
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

	const int32 SubmitIdx = AdvancePhase0Timer(Component, DeltaTime, Settings)
		                        ? SelectRoundRobinEdge(Component.CachedEdgePoints, Component.Phase0Cursor,
		                                               [](const FCachedEdgePoint& Edge) { return Edge.bEvicting; })
		                        : INDEX_NONE;

	for (int32 i = Component.CachedEdgePoints.Num() - 1; i >= 0; --i) {
		if (TickSingleEdge(Component, Component.CachedEdgePoints[i], World, ListenerPos,
		                   i == SubmitIdx, Settings)) {
			Component.CachedEdgePoints.RemoveAt(i);
		}
	}

	TickShortestPathReadback(Component, World, Settings);
	TickShortestPathRecheck(Component, World, SourcePos, DeltaTime, Settings);
	TickInnerAnchorPromotion(Component, World, ListenerPos, DeltaTime, Settings);

	MergeCoincidentEdges(Component, Settings);
	TrimToEffectiveCap(Component);
}

void FEdgeCache::MergeCoincidentEdges(USpatialAudioComponent& Component,
                                      const USpatialAudioSettings& Settings) {
	const float MergeRadiusSq = FMath::Square(Settings.CachedEdgeMergeRadius);
	if (MergeRadiusSq <= 0.f) {
		return;
	}

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
			const bool bEitherIsNew = Later.bNewSinceFillArm || Earlier.bNewSinceFillArm;

			if (TravelledFurther(Earlier, Later)) {
				Later.bNewSinceFillArm = bEitherIsNew;
				Component.CachedEdgePoints.RemoveAt(j);
				--i;
				continue;
			}
			Earlier.bNewSinceFillArm = bEitherIsNew;
			Component.CachedEdgePoints.RemoveAt(i);
			break;
		}
	}
}

/** Retreating shrinks the cap below a cache filled up close. Longest route goes first. */
void FEdgeCache::TrimToEffectiveCap(USpatialAudioComponent& Component) {
	const int32 Cap = Component.GetEffectiveCachedEdgeMaxCount();
	while (Component.CachedEdgePoints.Num() > Cap) {
		int32 WorstIdx = 0;
		for (int32 i = 1; i < Component.CachedEdgePoints.Num(); ++i) {
			if (TravelledFurther(Component.CachedEdgePoints[i], Component.CachedEdgePoints[WorstIdx])) {
				WorstIdx = i;
			}
		}
		Component.CachedEdgePoints.RemoveAt(WorstIdx);
	}
}

bool FEdgeCache::IsMergeCandidate(const FCachedEdgePoint& Edge) {
	return !Edge.bRelayed && !Edge.bEvicting;
}

bool FEdgeCache::TravelledFurther(const FCachedEdgePoint& Edge, const FCachedEdgePoint& Other) {
	return Edge.EffectivePathDist() > Other.EffectivePathDist();
}

void FEdgeCache::TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
                                         const FVector& SourcePos, const float DeltaTime,
                                         const USpatialAudioSettings& Settings) {
	if (Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::PathCheck);
	Component.ShortestPathCheckTimer += DeltaTime;
	if (Component.ShortestPathCheckTimer < EdgeCheckSlice(Component, Settings)
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

	TArray<FVector> Path = ResolveRecheckPath(Edge);

	TArray<bool> FallbackVerified;
	const bool bHasStoredPath = Edge.ShortestPath.Num() >= 2;
	if (!bHasStoredPath) {
		FallbackVerified = {true};
	}
	const TArray<bool>& SegmentVerified = bHasStoredPath
		                                      ? Edge.ShortestPathSegmentVerified
		                                      : FallbackVerified;

	const bool bSourceLegVerified = SegmentVerified.IsValidIndex(0) && SegmentVerified[0];
	if (bSourceLegVerified) {
		Path[0] = SourcePos;
	}
	else if (FVector::DistSquared(SourcePos, Path[0]) > 1.f) {
		StartEviction(Component, Edge);
		return;
	}

	const float EndInset = FMath::Max(Settings.RaySurfaceBias, 1.f);
	SubmitPolylineRecheckTraces(Component, World, Path, SegmentVerified, EndInset);
	if (Component.PathRecheck.Handles.IsEmpty()) {
		return;
	}
	Component.PathRecheck.EdgePoint = Edge.EdgePoint;
	Component.PathRecheck.bReanchored = bSourceLegVerified;
	Component.PathRecheck.ReanchorSource = SourcePos;
	Component.PathRecheck.bPending = true;
}

TArray<FVector> FEdgeCache::ResolveRecheckPath(const FCachedEdgePoint& Edge) {
	if (Edge.ShortestPath.Num() >= 2) {
		return Edge.ShortestPath;
	}
	return {Edge.CapturedSourcePos, Edge.EdgePoint};
}

int32 FEdgeCache::FindFirstBlockedSegment(const TArray<FTraceDatum>& Data) {
	for (int32 i = 0; i < Data.Num(); ++i) {
		if (!IsTraceClear(Data[i])) {
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

void FEdgeCache::ApplyRecheckReanchor(FCachedEdgePoint& Edge, const FVector& LiveSourcePos) {
	if (Edge.ShortestPath.Num() < 2) {
		Edge.CapturedSourcePos = LiveSourcePos;
		Edge.GeomDist = FVector::Dist(LiveSourcePos, Edge.EdgePoint);
		return;
	}

	const FVector& NextVertex = Edge.ShortestPath[1];
	const float OldLeg = FVector::Dist(Edge.ShortestPath[0], NextVertex);
	const float NewLeg = FVector::Dist(LiveSourcePos, NextVertex);

	Edge.PathDist = FMath::Max(0.f, Edge.PathDist - OldLeg + NewLeg);
	Edge.ShortestPath[0] = LiveSourcePos;
	Edge.CapturedSourcePos = LiveSourcePos;
	Edge.GeomDist = FVector::Dist(LiveSourcePos, Edge.EdgePoint);
}

void FEdgeCache::TickShortestPathReadback(USpatialAudioComponent& Component, UWorld* World,
                                          const USpatialAudioSettings& Settings) {
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

	FCachedEdgePoint* Edge = FindEntryByExactEdgePoint(Component, Component.PathRecheck.EdgePoint);
	if (!Edge) {
		return;
	}

	const int32 BlockedSeg = FindFirstBlockedSegment(Data);
	if (BlockedSeg == INDEX_NONE) {
		if (Component.PathRecheck.bReanchored) {
			ApplyRecheckReanchor(*Edge, Component.PathRecheck.ReanchorSource);
		}
		return;
	}

	if (Component.bDrawDebugRays && Component.bShowShortestPaths) {
		DrawDebugLine(World, Component.PathRecheck.SegStarts[BlockedSeg],
		              Component.PathRecheck.SegEnds[BlockedSeg], FColor::Red, false,
		              Settings.DebugLineDuration * 4.f, 0, 3.f);
	}
	StartEviction(Component, *Edge);
}

void FEdgeCache::TickInnerAnchorPromotion(USpatialAudioComponent& Component, const UWorld* World,
                                          const FVector& ListenerPos, const float DeltaTime,
                                          const USpatialAudioSettings& Settings) {
	if (Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	const USpatialAudioComponent::FTraceBucketScope Bucket(Component, USpatialAudioComponent::ETraceBucket::PathCheck);
	Component.ShortestPathPromotionTimer += DeltaTime;
	if (Component.ShortestPathPromotionTimer < EdgeCheckSlice(Component, Settings)) {
		return;
	}
	Component.ShortestPathPromotionTimer = 0.f;

	const int32 Idx = SelectRoundRobinEdge(Component.CachedEdgePoints, Component.ShortestPathPromotionCursor,
	                                       [](const FCachedEdgePoint& Edge)
	                                       {
		                                       return Edge.bEvicting || Edge.bRelayed;
	                                       });
	if (Idx == INDEX_NONE) {
		return;
	}

	TryPromoteToInnerAnchor(Component, Component.CachedEdgePoints[Idx], World, ListenerPos,
	                        /*bAllowSubSegmentRefine=*/true);
}
