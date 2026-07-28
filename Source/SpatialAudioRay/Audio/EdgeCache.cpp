// Implementation for FSpatialEdgeCache — forwards to USpatialAudioComponent methods.
#include "Audio/EdgeCache.h"

#include "Audio/SpatialAudioComponent.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"

int32 FEdgeCache::SelectRoundRobinEdge(const TArray<FCachedEdgePoint>& Points, int32& Cursor,
                                       TFunctionRef<bool(const FCachedEdgePoint&)> ShouldSkip) {
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
                                             const TArray<FVector>& Path, float Pull) {
	Component.PathRecheck.Handles.Reset();
	Component.PathRecheck.SegStarts.Reset();
	Component.PathRecheck.SegEnds.Reset();
	for (int32 s = 0; s + 1 < Path.Num(); ++s) {
		FVector A = Path[s];
		FVector B = Path[s + 1];
		const FVector AB = B - A;
		const float Len = AB.Size();
		if (Len <= Pull * 2.f + 1.f) {
			continue;
		}
		const FVector Dir = AB / Len;
		A += Dir * Pull;
		B -= Dir * Pull;
		// All segments submitted up front (no early-out on the first blocked one, unlike the
		// old sync walk) — same batch-then-evaluate shape as the async crawl probes.
		Component.PathRecheck.Handles.Add(Component.SubmitAsyncTrace(World, A, B));
		Component.PathRecheck.Handles.Add(Component.SubmitAsyncTrace(World, B, A));
		Component.PathRecheck.SegStarts.Add(A);
		Component.PathRecheck.SegEnds.Add(B);
	}
}

TArray<float> FEdgeCache::ComputeProgressiveMoveThresholds(const USpatialAudioComponent& Component,
                                                            const FVector& SrcPos, float MoveThresh) {
	TArray<float> EffectiveThresholds;
	EffectiveThresholds.SetNum(Component.CachedEdgePoints.Num());
	for (float& V : EffectiveThresholds) { V = MoveThresh; }
	if (MoveThresh <= 0.f) {
		return EffectiveThresholds;
	}

	struct FRankEntry { int32 Idx; float Delta; };
	TArray<FRankEntry> Sortable;
	for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
		const FCachedEdgePoint& EP = Component.CachedEdgePoints[i];
		if (!EP.bEvicting) {
			const float SrcDelta = FVector::Dist(SrcPos, EP.CapturedSourcePos);
			Sortable.Add({i, SrcDelta});
		}
	}
	Sortable.Sort([](const FRankEntry& A, const FRankEntry& B) { return A.Delta > B.Delta; });
	const int32 N = Sortable.Num();
	for (int32 k = 0; k < N; ++k) {
		EffectiveThresholds[Sortable[k].Idx] = MoveThresh * (k + 1.f) / N;
	}
	return EffectiveThresholds;
}

bool FEdgeCache::TickEvictionFade(FCachedEdgePoint& EP, float DeltaTime, float EvictFadeTime) {
	if (!EP.bEvicting) {
		return false;
	}
	if (EvictFadeTime <= 0.f || (EP.EvictionAlpha -= DeltaTime / EvictFadeTime) <= 0.f) {
		return true;
	}
	return false;
}

void FEdgeCache::TickPhase0Readback(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                    const FVector& SrcPos, const FVector& LisPos,
                                    const USpatialAudioSettings& Settings) {
	if (!EP.bPhase0Pending) {
		return;
	}

	FTraceDatum D;
	if (!World->QueryTraceData(EP.AsyncPhase0Handle, D)) {
		return;
	}

	EP.bPhase0Pending = false;

	bool bBlocked = !D.OutHits.IsEmpty() && D.OutHits[0].bBlockingHit;
	// Try shrinking the edge back to its own previous anchor before falling back to the
	// listener-side workarounds below: if that inner point already sees the listener directly,
	// it's a strictly better outcome than a fan or a relay — the diffraction it stood in for
	// isn't needed anymore, so nothing downstream has to keep working around the blocked point.
	if (bBlocked && !EP.bRelayed
		&& TryPromoteToInnerAnchor(Component, EP, World, LisPos, /*bAllowSubSegmentRefine=*/false)) {
		bBlocked = false;
	}

	if (bBlocked) {
		if (Settings.bEnableOffsetLoSChecks && Settings.DirectLoSSampleRadius > 0.f) {
			SubmitPhase0OffsetFan(Component, EP, World, LisPos, Settings.DirectLoSSampleRadius);
		}
		else if (!TryRelayRescue(Component, EP, World, LisPos)) {
			StartEviction(Component, EP, SrcPos);
		}
	}
	else {
		if (!EP.bRelayed) {
			EP.LastLoSListenerPos = LisPos;
			EP.bHasLastLoSListenerPos = true;
		}
		if (EP.bEvicting && !EP.bSourceSideEviction) {
			EP.bEvicting = false;
			EP.EvictionAlpha = 1.f;
			EP.CapturedSourcePos = SrcPos;
			EP.CapturedListenerPos = LisPos;
		}
	}
}

// Forward+reverse listener LoS probe at P. Draws itself under the shortest-path view (key 0):
// green = this point on the path sees the listener, red = blocked. Longer duration than
// regular debug lines so probe patterns stay visible between the slow check intervals.
// Shared by inner-anchor promotion and the relay→edge conversion.
bool FEdgeCache::ProbeListenerLoSPoint(USpatialAudioComponent& Component, UWorld* World,
                                       const FVector& LisPos, const FVector& P) {
	FHitResult Hit;
	const bool bClear = !Component.TraceLine(World, Hit, LisPos, P)
		&& !Component.TraceLine(World, Hit, P, LisPos);
	if (Component.bDrawDebugRays && Component.bShowShortestPaths) {
		DrawDebugSphere(World, P, 7.f, 6, bClear ? FColor::Green : FColor::Red,
		                false, Component.GetSettings().DebugLineDuration * 4.f, SDPG_Foreground, 1.5f);
	}
	return bClear;
}

// 5-step bisection along the straight segment between a listener-blocked end and a
// listener-clear end, bracketing the LoS transition — physically the diffraction corner on
// that segment. Returns the innermost point that itself traced clear (bOutFoundClear true),
// or ClearEnd unchanged when no midpoint cleared (non-monotone shadowing); it never returns
// an untraced-blocked point, so a broken monotonicity assumption can only miss an
// improvement.
FVector FEdgeCache::BisectListenerLoS(USpatialAudioComponent& Component, UWorld* World, const FVector& LisPos,
                                      const FVector& BlockedEnd, const FVector& ClearEnd, bool& bOutFoundClear,
                                      const int32 ExplicitSteps) {
	FVector Lo = BlockedEnd;
	FVector Hi = ClearEnd;
	bOutFoundClear = false;

	// Derived step count (ExplicitSteps 0) follows the bracket rather than a constant: N halvings
	// only localise the corner to span/2^N, so a fixed count makes accuracy scale with how long
	// the segment happens to be. That matters most for relay→edge conversion, whose bracket is
	// the whole edge→relay leg — at a fixed 5 steps a 20m leg resolved the corner to only ~60cm,
	// so two relays converging on the SAME corner from different legs each landed somewhere in
	// their own ~60cm window, well outside CachedEdgeMergeRadius, and MergeCoincidentEdges could
	// never collapse them. Converging to the merge radius instead makes "same corner" mean the
	// same thing to both: resolving finer than the distance at which two points ARE one corner is
	// wasted traces, resolving coarser guarantees siblings stay split. Floor of 5 keeps every
	// caller at least as precise as the original fixed count; ceiling of 10 bounds the cost on a
	// very long leg (still ~20cm on a 200m bracket, inside a default merge radius).
	int32 Steps = ExplicitSteps;
	if (Steps <= 0) {
		const float Tolerance = FMath::Max(Component.GetSettings().CachedEdgeMergeRadius * 0.5f, 1.f);
		const float Span = FVector::Dist(Lo, Hi);
		Steps = FMath::Clamp(
			FMath::CeilToInt(FMath::Log2(FMath::Max(Span / Tolerance, 1.f))), 5, 10);
	}

	for (int32 Step = 0; Step < Steps; ++Step) {
		const FVector Mid = (Lo + Hi) * 0.5f;
		if (ProbeListenerLoSPoint(Component, World, LisPos, Mid)) {
			Hi = Mid;
			bOutFoundClear = true;
		}
		else {
			Lo = Mid;
		}
	}
	return Hi;
}

// Shrinks the cached edge back along its own polyline toward the source. Two stages:
// (1) the previous polyline vertex — if it already has direct, unobstructed listener LoS, the
// edge jumps the whole segment there: no diffraction is needed for what's now a direct final
// leg. PathDist is corrected by the trimmed segment's straight-line length (exact when the
// segment was verified, a close estimate otherwise).
// (2) when that vertex is blocked and bAllowSubSegmentRefine is set, a short binary search
// along the final segment brackets the LoS transition point — physically the actual geometric
// corner. Discovery quantizes edge points to CrawlStepSize/DiffractionEdgeSampleStep, so this
// converges the emitter onto the real corner and lets it slide continuously along the wall as
// the listener moves past, instead of staying parked until the whole next vertex clears.
// Refinement only runs on a VERIFIED final segment (unverified hops hug geometry — a point
// partway along one can sit inside a wall) and assumes LoS along the segment is roughly
// monotone (closer to the corner = more visible), which bisection needs to bracket; every
// accepted point has itself traced clear forward+reverse, so a broken assumption can only
// miss an improvement, never move the edge to a blocked point. The rescue call site
// (TickPhase0Readback) passes false: it re-fires every interval while the edge is blocked,
// and the bisection traces would recur in pinhole states — the slow opportunistic round-robin
// (TickInnerAnchorPromotion) is the intended home for refinement.
bool FEdgeCache::TryPromoteToInnerAnchor(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                         const FVector& LisPos, const bool bAllowSubSegmentRefine) {
	if (EP.ShortestPath.Num() < 2) {
		return false;
	}
	const FVector Inner = EP.ShortestPath[EP.ShortestPath.Num() - 2];
	if (ProbeListenerLoSPoint(Component, World, LisPos, Inner)) {
		EP.PathDist = FMath::Max(0.f, EP.PathDist - FVector::Dist(Inner, EP.EdgePoint));
		EP.EdgePoint = Inner;
		EP.ShortestPath.Pop();
		if (!EP.ShortestPathSegmentVerified.IsEmpty()) {
			EP.ShortestPathSegmentVerified.Pop();
		}
		EP.GeomDist = FVector::Dist(EP.CapturedSourcePos, EP.EdgePoint);
		return true;
	}

	if (!bAllowSubSegmentRefine) {
		return false;
	}
	const int32 SegIdx = EP.ShortestPath.Num() - 2;
	if (!EP.ShortestPathSegmentVerified.IsValidIndex(SegIdx) || !EP.ShortestPathSegmentVerified[SegIdx]) {
		return false;
	}
	// Minimum accepted movement: below this the refined point is within trace-noise range of
	// the current edge and re-running every interval would just jitter it.
	const float MinMove = FMath::Max(Component.GetSettings().RaySurfaceBias * 4.f, 4.f);
	if (FVector::Dist(Inner, EP.EdgePoint) <= MinMove * 2.f) {
		return false;
	}

	bool bFoundClear = false;
	const FVector Hi = BisectListenerLoS(Component, World, LisPos, Inner, EP.EdgePoint, bFoundClear,
	                                     Component.GetSettings().ShortestPathPromotionBisectSteps);
	if (!bFoundClear || FVector::Dist(Hi, EP.EdgePoint) < MinMove) {
		return false;
	}

	// Hi lies on the verified straight segment, so the trimmed length is exact. The edge stays
	// on the same segment — the polyline's last vertex moves inward with it, no pop.
	EP.PathDist = FMath::Max(0.f, EP.PathDist - FVector::Dist(Hi, EP.EdgePoint));
	EP.EdgePoint = Hi;
	EP.ShortestPath.Last() = Hi;
	EP.GeomDist = FVector::Dist(EP.CapturedSourcePos, EP.EdgePoint);
	return true;
}

// Fired only when the center listener→edge trace came back blocked, so the extra traces cost
// nothing while the edge is comfortably visible. Candidates must go through ResolveOffsetPoint:
// an offset point embedded in a wall would trace outward with a silent false-clear.
void FEdgeCache::SubmitPhase0OffsetFan(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                       const FVector& LisPos, float OffsetR) {
	if (EP.bPhase0OffsetPending) {
		return;
	}

	const FVector Target = EP.EffectivePoint();
	const FVector ToListenerDir = (LisPos - Target).GetSafeNormal();
	FVector RightDir = FVector::CrossProduct(ToListenerDir, FVector::UpVector).GetSafeNormal();
	if (RightDir.IsNearlyZero()) {
		RightDir = FVector::CrossProduct(ToListenerDir, FVector::RightVector).GetSafeNormal();
	}

	const FVector Candidates[4] = {
		LisPos + FVector::UpVector * OffsetR,
		LisPos - FVector::UpVector * OffsetR,
		LisPos + RightDir * OffsetR,
		LisPos - RightDir * OffsetR,
	};
	for (int32 i = 0; i < 4; ++i) {
		EP.Phase0OffsetPts[i] = FUpdater::ResolveOffsetPoint(Component, World, LisPos, Candidates[i]);
		EP.Phase0OffsetHandles[i] = Component.SubmitAsyncTrace(World, EP.Phase0OffsetPts[i], Target);
	}
	EP.bPhase0OffsetPending = true;
}

void FEdgeCache::TickPhase0OffsetReadback(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                          const FVector& SrcPos, const FVector& LisPos) {
	if (!EP.bPhase0OffsetPending) {
		return;
	}

	FTraceDatum Data[4];
	for (int32 i = 0; i < 4; ++i) {
		if (!World->QueryTraceData(EP.Phase0OffsetHandles[i], Data[i])) {
			return;
		}
	}

	EP.bPhase0OffsetPending = false;

	bool bClearArr[4] = {};
	bool bAnyClear = false;
	for (int32 i = 0; i < 4; ++i) {
		bClearArr[i] = Data[i].OutHits.IsEmpty();
		bAnyClear |= bClearArr[i];
	}

	if (Component.bDrawDebugRays && Component.bShowOffsetLoSChecks) {
		for (int32 i = 0; i < 4; ++i) {
			DrawDebugLine(World, EP.Phase0OffsetPts[i], EP.EffectivePoint(),
			              bClearArr[i] ? FColor::Green : FColor::Red, false,
			              Component.GetSettings().DebugLineDuration, 0, 0.75f);
		}
	}

	if (bAnyClear) {
		if (!EP.bRelayed) {
			// Anchor on the fan point that actually saw the edge: the center was blocked, so
			// LisPos itself has no edge LoS — a rescue anchored there fails its edge→relay leg
			// against the same corner that blocked the center.
			for (int32 i = 0; i < 4; ++i) {
				if (bClearArr[i]) {
					EP.LastLoSListenerPos = EP.Phase0OffsetPts[i];
					EP.bHasLastLoSListenerPos = true;
					break;
				}
			}
		}
		if (EP.bEvicting && !EP.bSourceSideEviction) {
			EP.bEvicting = false;
			EP.EvictionAlpha = 1.f;
			EP.CapturedSourcePos = SrcPos;
			EP.CapturedListenerPos = LisPos;
		}
	}
	else if (!TryRelayRescue(Component, EP, World, LisPos)) {
		StartEviction(Component, EP, SrcPos);
	}
}

// Last resort before eviction: route the edge through the most recent listener position that
// could still see it. Both legs (edge→relay, relay→current listener) run sync with reverse
// hygiene — this fires once at the moment LoS is lost, not per frame. Single relay level:
// an already-relayed edge whose relay went dark just evicts. Deliberately independent of
// sibling edges' state: the old "skip while any direct edge exists" gate made rescue depend
// on processing order (with N edges going dark the same tick, the first N−1 processed saw
// still-direct siblings and evicted), and once one relay converted into a direct edge it
// permanently locked out every remaining rescue retry — 8 simultaneous losses produced 1
// converted edge and 7 deaths. Every totally-blocked edge whose legs verify now preserves
// itself; the conversion turns each into an honest corner entry and rank/clustering decide
// audibility.
bool FEdgeCache::TryRelayRescue(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                const FVector& LisPos) {
	if (EP.bRelayed || !EP.bHasLastLoSListenerPos) {
		return false;
	}
	const FVector& Relay = EP.LastLoSListenerPos;
	FHitResult Hit;
	const bool bLegAClear = !Component.TraceLine(World, Hit, EP.EdgePoint, Relay)
		&& !Component.TraceLine(World, Hit, Relay, EP.EdgePoint);
	if (!bLegAClear
		|| Component.TraceLine(World, Hit, Relay, LisPos)
		|| Component.TraceLine(World, Hit, LisPos, Relay)) {
		return false;
	}

	EP.bRelayed = true;
	EP.RelayPoint = Relay;
	EP.RelayDist = FVector::Dist(EP.EdgePoint, Relay);
	EP.bEvicting = false;
	EP.EvictionAlpha = 1.f;
	// A successful rescue is a revalidation: reset the movement baseline, otherwise the walk
	// to the second corner (large listener delta by definition) movement-evicts the freshly
	// relayed edge within moments.
	EP.CapturedListenerPos = LisPos;
	return true;
}

// While relayed, Phase 0 only watches the relay point, so two blind spots need a periodic
// re-check: direct listener→edge LoS returning (listener walked back around the corner —
// without un-relaying, the voice stays parked at the relay with its longer path), and the
// edge→relay leg going dark (it was verified only once, at rescue time — dynamic geometry
// closing in between would otherwise keep playing through). Both are pure validation with no
// same-frame dependency, so the four traces (forward + reverse per leg) are submitted async
// on the Phase 0 interval and read back the following tick(s) — the one-frame skew matches
// Phase 0's own accepted staleness.
void FEdgeCache::TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                      const FVector& SrcPos, const FVector& LisPos, bool bIntervalFired) {
	if (!EP.bRelayed) {
		return;
	}
	// No yield-to-direct-edge check here anymore: it predated the relay→edge conversion, when
	// a relay could persist parked at an old listener position while a real edge carried the
	// sound. Now every relay converts on its first maintenance readback, and the yield's only
	// remaining reachable effect was killing sibling relays mid-conversion — with N edges
	// relayed at once, the first conversion made HasOtherDirectEdge true and evicted the other
	// N−1 before their readbacks landed, instead of letting each convert to its own corner.

	if (EP.bRelayCheckPending) {
		FTraceDatum Data[4];
		for (int32 i = 0; i < 4; ++i) {
			if (!World->QueryTraceData(EP.RelayCheckHandles[i], Data[i])) {
				return;
			}
		}
		EP.bRelayCheckPending = false;

		auto IsClear = [](const FTraceDatum& Datum) {
			return Datum.OutHits.IsEmpty() || !Datum.OutHits[0].bBlockingHit;
		};
		// Direct listener↔edge LoS returned: snap back to the true edge and its shorter path.
		// LisPos is one frame newer than the position the traces verified — accepted skew.
		if (IsClear(Data[0]) && IsClear(Data[1])) {
			EP.ClearRelay();
			EP.LastLoSListenerPos = LisPos;
			EP.bHasLastLoSListenerPos = true;
			EP.CapturedListenerPos = LisPos;
			return;
		}
		// Drop the relay before evicting: Phase 0's clear-restore path un-evicts on listener LoS to
		// EffectivePoint(), and the listener typically still sees the relay point — keeping bRelayed
		// would resurrect a path that is broken upstream every interval. Un-relayed, the standard
		// machinery decides: fan finds the edge → lives directly; all blocked → rescue re-attempts
		// against the same anchor and fails on the same broken leg → fades out.
		if (!IsClear(Data[2]) || !IsClear(Data[3])) {
			EP.ClearRelay();
			StartEviction(Component, EP, SrcPos);
			return;
		}

		// Relay confirmed still valid — upgrade it to a real edge instead of staying a stopgap.
		// The LoS transition along the verified edge→relay leg is the actual second corner the
		// relay bends around: bisect for it and convert IN PLACE — append the corner to the
		// polyline (a sub-segment of the traced-clear leg, so verified), extend PathDist by the
		// exact straight piece, count the extra corner in LoSBounces so ranking stays honest.
		// The entry becomes a first-class cached edge: standard Phase 0/promotion maintenance
		// takes over, and it substitutes for rays / excludes its direction again — it is now
		// backed by verified geometry, unlike the relay it replaces. Fallback when no midpoint
		// traces clear (non-monotone shadowing): the relay point itself — same acoustics as the
		// relay, and the promotion refinement walks it back toward the true corner on later
		// intervals since the appended segment is exactly the bracket it bisects.
		if (EP.ShortestPath.Num() < 2) {
			// Defensive: entries without a stored polyline treat the straight source→edge flight
			// as their path (same reading as the recheck's fallback).
			EP.ShortestPath = {EP.CapturedSourcePos, EP.EdgePoint};
			EP.ShortestPathSegmentVerified = {true};
		}
		bool bFoundClear = false;
		const FVector Corner = BisectListenerLoS(Component, World, LisPos, EP.EdgePoint, EP.RelayPoint, bFoundClear);
		EP.PathDist += FVector::Dist(EP.EdgePoint, Corner);
		EP.EdgePoint = Corner;
		EP.ShortestPath.Add(Corner);
		EP.ShortestPathSegmentVerified.Add(true);
		EP.LoSBounces += 1;
		EP.GeomDist = FVector::Dist(EP.CapturedSourcePos, EP.EdgePoint);
		EP.CapturedListenerPos = LisPos;
		EP.bNewSinceFillArm = true;
		EP.LastLoSListenerPos = LisPos;
		EP.bHasLastLoSListenerPos = true;
		EP.ClearRelay();
		return;
	}

	if (!bIntervalFired) {
		return;
	}
	EP.RelayCheckHandles[0] = Component.SubmitAsyncTrace(World, LisPos, EP.EdgePoint);
	EP.RelayCheckHandles[1] = Component.SubmitAsyncTrace(World, EP.EdgePoint, LisPos);
	EP.RelayCheckHandles[2] = Component.SubmitAsyncTrace(World, EP.EdgePoint, EP.RelayPoint);
	EP.RelayCheckHandles[3] = Component.SubmitAsyncTrace(World, EP.RelayPoint, EP.EdgePoint);
	EP.bRelayCheckPending = true;
}

void FEdgeCache::StartEviction(USpatialAudioComponent& Component, FCachedEdgePoint& EP, const FVector& SrcPos,
                               const bool bSourceSide) {
	if (EP.bEvicting) {
		return;
	}
	EP.bEvicting = true;
	EP.bSourceSideEviction = bSourceSide;
	const FVector ToEdge = EP.EdgePoint - SrcPos;
	const float Len = ToEdge.Size();
	if (Len > 1.f) {
		Component.SuccessfulEdgeDirHints.Add(ToEdge / Len);
	}
	Component.SweepScheduling.bMovementRequested = true;
}

bool FEdgeCache::TickMovementThresholdEviction(USpatialAudioComponent& Component, FCachedEdgePoint& EP,
                                               const FVector& SrcPos, float EffectiveMoveThresh) {
	if (EP.bEvicting || EffectiveMoveThresh <= 0.f) {
		return false;
	}

	// Source movement only: it stales the captured source-side data (PathDist/ShortestPath)
	// and nothing else revalidates that side. Listener movement never evicts — listener
	// validity is Phase 0's job (LoS checks, offset fan, relay), and an edge is otherwise only
	// displaced when a sweep finds one that ranks better, never just because a recast from the
	// new positions happened not to re-find it.
	const float SrcDelta = FVector::Dist(SrcPos, EP.CapturedSourcePos);
	if (SrcDelta <= EffectiveMoveThresh) {
		return false;
	}

	EP.bEvicting = true;
	EP.bSourceSideEviction = true;
	EP.EvictionAlpha = 1.f;
	Component.SweepScheduling.bMovementRequested = true;
	return true;
}

void FEdgeCache::TickPhase0Submission(const USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                      const FVector& LisPos, bool bIntervalFired) {
	if (!EP.bPhase0Pending && !EP.bPhase0OffsetPending && bIntervalFired && !(EP.bEvicting && EP.bSourceSideEviction)) {
		EP.AsyncPhase0Handle = Component.SubmitAsyncTrace(World, LisPos, EP.EffectivePoint());
		EP.bPhase0Pending = true;
	}
}

void FEdgeCache::TickCachedEdgeEviction(USpatialAudioComponent& Component, const float DeltaTime, const USpatialAudioSettings& Settings) {
	if (!Settings.bCacheEdgePoints) {
		return;
	}

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

	const FVector SrcPos = OwnerActor->GetActorLocation();
	const FVector LisPos = Pawn->GetActorLocation();

	if (Component.bPhase0HandlesStale) {
		for (FCachedEdgePoint& EP : Component.CachedEdgePoints) {
			EP.bPhase0Pending = false;
			EP.bPhase0OffsetPending = false;
			EP.bRelayCheckPending = false;
		}
		Component.PathRecheck.bPending = false;
	}
	Component.bPhase0HandlesStale = false;

	const float EvictFadeTime = Settings.CachedEdgeEvictionFadeTime;
	const float MoveThresh = Settings.CachedEdgeUpdateMoveThreshold;

	Component.Phase0Timer += DeltaTime;
	const bool bIntervalFired = Component.Phase0Timer >= Settings.Phase0CheckInterval * Component.VelocityScaling.EdgeMultiplier;
	if (bIntervalFired) {
		Component.Phase0Timer = 0.f;
	}

	// Progressive effective threshold per point: non-evicting points are ranked by movement delta
	// (most stale first), so evictions spread across the movement range instead of all triggering
	// at once when the threshold is crossed.
	const TArray<float> EffectiveThresholds = ComputeProgressiveMoveThresholds(Component, SrcPos, MoveThresh);

	for (int32 i = Component.CachedEdgePoints.Num() - 1; i >= 0; --i) {
		FCachedEdgePoint& EP = Component.CachedEdgePoints[i];

		if (TickEvictionFade(EP, DeltaTime, EvictFadeTime)) {
			Component.CachedEdgePoints.RemoveAt(i);
			continue;
		}

		TickPhase0Readback(Component, EP, World, SrcPos, LisPos, Settings);
		TickPhase0OffsetReadback(Component, EP, World, SrcPos, LisPos);

		if (TickMovementThresholdEviction(Component, EP, SrcPos, EffectiveThresholds[i])) {
			continue;
		}

		TickRelayMaintenance(Component, EP, World, SrcPos, LisPos, bIntervalFired);
		TickPhase0Submission(Component, EP, World, LisPos, bIntervalFired);
	}

	TickShortestPathReadback(Component, World, SrcPos, Settings);
	TickShortestPathRecheck(Component, World, SrcPos, DeltaTime, Settings);
	TickInnerAnchorPromotion(Component, World, LisPos, DeltaTime, Settings);

	// Last: every EdgePoint move above (relay conversion, promotion, refinement) can land an
	// entry on top of another, and this is the only place that notices.
	MergeCoincidentEdges(Component, Settings);
}

// The sweep's merge radius only ever gated ADMISSION — an incoming find within
// CachedEdgeMergeRadius of an existing entry re-confirms it instead of adding a second one.
// Entries already in the cache were never compared against each other, and they do not hold
// still: relay→edge conversion bisects to a corner, and promotion/refinement walk the edge
// inward along its own path. The relay case is the one that piles up, because it is inherently
// simultaneous — several edges bending around the SAME physical corner lose LoS on the same
// tick, each converts along its own edge→relay leg, and those legs all terminate at that one
// corner. The duplicates then cost real things: cache slots, one SubstituteCount each against
// the sweep ray budget, one exclusion direction each (so sweeps under-search a region holding a
// single corner), and — since Math::ClusterEdgePoints sums member weights into Cluster
// .TotalWeight — a larger share of the voice mix than a genuinely distinct opening gets.
//
// Survivor = shortest EffectivePathDist(). At one point the listener leg is identical and the
// rank score's listener term cancels exactly, so the only thing left that distinguishes two
// entries is how far the sound travelled to arrive — the same shortest-path rule occlusion and
// GetEffectiveAcousticDistance already run on. Bounce count deliberately does NOT enter here
// (unlike the sweep's IsBetter, which compares entries at DIFFERENT positions): a 3-bounce 8m
// route to a corner beats a 1-bounce 40m route to the same corner.
//
// Relayed entries are excluded, and the test is on EdgePoint rather than EffectivePoint():
// relays rescued through the same clear fan point share a RelayPoint while their EdgePoints are
// genuinely distinct corners, so keying on the presented point would delete real edges — the
// same class of failure as the removed relay-yield rule, which killed sibling relays the moment
// the first one converted. A relay clears bRelayed on conversion, so it is picked up on the
// very next tick anyway. Evicting entries are skipped for the mirror reason: they are already
// leaving, and merging into one would hand the survivor a fade that is on its way out.
void FEdgeCache::MergeCoincidentEdges(USpatialAudioComponent& Component,
                                      const USpatialAudioSettings& Settings) {
	const float MergeRadiusSq = FMath::Square(Settings.CachedEdgeMergeRadius);
	if (MergeRadiusSq <= 0.f) {
		return;
	}
	auto IsMergeable = [](const FCachedEdgePoint& EP) {
		return !EP.bRelayed && !EP.bEvicting;
	};

	// Both loops run backward so a removal can only shift entries already visited.
	for (int32 i = Component.CachedEdgePoints.Num() - 1; i >= 0; --i) {
		if (!IsMergeable(Component.CachedEdgePoints[i])) {
			continue;
		}
		for (int32 j = i - 1; j >= 0; --j) {
			if (!IsMergeable(Component.CachedEdgePoints[j]) ||
				FVector::DistSquared(Component.CachedEdgePoints[i].EdgePoint,
				                     Component.CachedEdgePoints[j].EdgePoint) >= MergeRadiusSq) {
				continue;
			}
			// The discovery flag survives either way: if either entry found this corner since
			// the cache-fill burst armed, the corner was found, and dropping that would make the
			// burst re-survey a position it has already covered.
			const bool bNewEither = Component.CachedEdgePoints[i].bNewSinceFillArm ||
				Component.CachedEdgePoints[j].bNewSinceFillArm;

			if (Component.CachedEdgePoints[i].EffectivePathDist() <
				Component.CachedEdgePoints[j].EffectivePathDist()) {
				Component.CachedEdgePoints[i].bNewSinceFillArm = bNewEither;
				Component.CachedEdgePoints.RemoveAt(j);
				// i slid down with the removal; keep scanning the remaining lower entries so a
				// three-way pile collapses in one pass rather than one merge per tick.
				--i;
				continue;
			}
			Component.CachedEdgePoints[j].bNewSinceFillArm = bNewEither;
			Component.CachedEdgePoints.RemoveAt(i);
			break;
		}
	}
}

// Source-side counterpart of Phase 0: re-traces the stored string-pulled polyline PathDist was
// measured along, catching geometry that closed the source→edge path after discovery (a static
// source, a door closing — nothing else sees it: Phase 0 watches the listener leg, movement
// eviction watches source position, and rank hysteresis discards the worse-ranking re-finds a
// closed path produces). One edge per interval; pure validation with no same-frame dependency,
// so every segment's traces are submitted async up front here and evaluated by
// TickShortestPathReadback the following tick. Every segment is checked, including unverified
// ones (a raw crawl/bounce hop the string pull couldn't shortcut past) — a deliberate choice:
// those were already blocked at discovery, so this will also evict on ordinary multi-corner
// diffraction paths the moment they're rechecked, not just on genuine geometry change.
// Enabling ShortestPathRecheckInterval accepts that trade-off.
void FEdgeCache::TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
                                         const FVector& SrcPos, const float DeltaTime,
                                         const USpatialAudioSettings& Settings) {
	if (Settings.ShortestPathRecheckInterval <= 0.f || Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	Component.ShortestPathCheckTimer += DeltaTime;
	if (Component.ShortestPathCheckTimer < Settings.ShortestPathRecheckInterval
		|| Component.PathRecheck.bPending) {
		// A still-pending batch holds the timer past the interval; the readback consumes it and
		// the next tick submits — intervals are far longer than a frame, so no check is skipped.
		return;
	}
	Component.ShortestPathCheckTimer = 0.f;

	const int32 Idx = SelectRoundRobinEdge(Component.CachedEdgePoints, Component.ShortestPathCheckCursor,
	                                       [](const FCachedEdgePoint& EP) { return EP.bEvicting; });
	if (Idx == INDEX_NONE) {
		return;
	}

	FCachedEdgePoint& EP = Component.CachedEdgePoints[Idx];

	// Entries without a stored polyline fall back to the straight source→edge segment.
	TArray<FVector> FallbackPath;
	const TArray<FVector>* Path = &EP.ShortestPath;
	if (EP.ShortestPath.Num() < 2) {
		FallbackPath = {EP.CapturedSourcePos, EP.EdgePoint};
		Path = &FallbackPath;
	}

	// Segment endpoints sit within ~RaySurfaceBias of geometry, so pull both ends in before
	// tracing — a trace grazing its own anchor surface is corner clipping, not an obstruction.
	const float Pull = FMath::Max(Settings.RaySurfaceBias, 1.f);
	SubmitPolylineRecheckTraces(Component, World, *Path, Pull);
	if (Component.PathRecheck.Handles.IsEmpty()) {
		return;
	}
	Component.PathRecheck.EdgePoint = EP.EdgePoint;
	Component.PathRecheck.bPending = true;
}

void FEdgeCache::TickShortestPathReadback(USpatialAudioComponent& Component, UWorld* World,
                                          const FVector& SrcPos, const USpatialAudioSettings& Settings) {
	if (!Component.PathRecheck.bPending) {
		return;
	}
	// All-or-nothing, same shape as the async crawl batch: evaluate only once every segment's
	// traces are ready. All handles were submitted the same tick, so they complete together.
	TArray<FTraceDatum> Data;
	Data.SetNum(Component.PathRecheck.Handles.Num());
	for (int32 i = 0; i < Component.PathRecheck.Handles.Num(); ++i) {
		if (!World->QueryTraceData(Component.PathRecheck.Handles[i], Data[i])) {
			return;
		}
	}
	Component.PathRecheck.bPending = false;

	int32 BlockedSeg = INDEX_NONE;
	for (int32 i = 0; i < Data.Num(); ++i) {
		if (!Data[i].OutHits.IsEmpty() && Data[i].OutHits[0].bBlockingHit) {
			BlockedSeg = i / 2;
			break;
		}
	}
	if (BlockedSeg == INDEX_NONE) {
		return;
	}

	// Re-find the checked entry by exact edge position: WriteEntry and inner-anchor promotion
	// are the only EdgePoint writers, so a mismatch means the path this batch traced no longer
	// exists — drop the stale result rather than evict whatever sits at the index now.
	for (FCachedEdgePoint& EP : Component.CachedEdgePoints) {
		if (!EP.bEvicting && EP.EdgePoint == Component.PathRecheck.EdgePoint) {
			if (Component.bDrawDebugRays && Component.bShowShortestPaths) {
				DrawDebugLine(World, Component.PathRecheck.SegStarts[BlockedSeg],
				              Component.PathRecheck.SegEnds[BlockedSeg], FColor::Red, false,
				              Settings.DebugLineDuration * 4.f, 0, 3.f);
			}
			// Source-side eviction: the listener leg is typically still clear here, and Phase 0's
			// clear-restore would resurrect the edge every interval, faster than the fade completes.
			StartEviction(Component, EP, SrcPos, /*bSourceSide=*/true);
			break;
		}
	}
}

// Opportunistic counterpart to TryPromoteToInnerAnchor's use inside TickPhase0Readback: that one
// only fires as a rescue the moment the edge itself goes blocked, so an edge that's had clear
// listener LoS since discovery never migrates inward even once a shorter path becomes available
// (e.g. the listener walked well past the corner). This runs regardless of the edge's current
// LoS state, one edge per interval (round-robin, independent cursor from the recheck above), and
// tries only the single point immediately before it — one step per interval, so a long path
// walks itself back toward the source gradually rather than jumping there in one check.
void FEdgeCache::TickInnerAnchorPromotion(USpatialAudioComponent& Component, UWorld* World,
                                          const FVector& LisPos, const float DeltaTime,
                                          const USpatialAudioSettings& Settings) {
	if (Settings.ShortestPathPromotionInterval <= 0.f || Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	Component.ShortestPathPromotionTimer += DeltaTime;
	if (Component.ShortestPathPromotionTimer < Settings.ShortestPathPromotionInterval) {
		return;
	}
	Component.ShortestPathPromotionTimer = 0.f;

	const int32 Idx = SelectRoundRobinEdge(Component.CachedEdgePoints, Component.ShortestPathPromotionCursor,
	                                       [](const FCachedEdgePoint& EP) { return EP.bEvicting || EP.bRelayed; });
	if (Idx == INDEX_NONE) {
		return;
	}

	TryPromoteToInnerAnchor(Component, Component.CachedEdgePoints[Idx], World, LisPos,
	                        /*bAllowSubSegmentRefine=*/true);
}
