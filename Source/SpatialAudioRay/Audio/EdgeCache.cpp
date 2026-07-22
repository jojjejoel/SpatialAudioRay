// Implementation for FSpatialEdgeCache — forwards to USpatialAudioComponent methods.
#include "Audio/EdgeCache.h"

#include "Audio/SpatialAudioComponent.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"

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
	if (bBlocked && !EP.bRelayed && TryPromoteToInnerAnchor(Component, EP, World, LisPos)) {
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

// Shrinks the cached edge back to the previous point on its own polyline when that point
// already has direct, unobstructed listener LoS — the listener can see past the recorded edge
// to an interior anchor, so that anchor is the more accurate (and more robust) presentation
// point: no diffraction is needed for what's now a direct final leg. PathDist is corrected by
// removing the trimmed segment's straight-line length, which is exact when that segment was
// verified and a close estimate otherwise (traveled distance for an unverified hop is usually
// only slightly longer than the straight cut between its endpoints).
bool FEdgeCache::TryPromoteToInnerAnchor(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                         const FVector& LisPos) {
	if (EP.ShortestPath.Num() < 2) {
		return false;
	}
	const FVector Inner = EP.ShortestPath[EP.ShortestPath.Num() - 2];
	FHitResult Hit;
	if (Component.TraceLine(World, Hit, LisPos, Inner) || Component.TraceLine(World, Hit, Inner, LisPos)) {
		return false;
	}

	EP.PathDist = FMath::Max(0.f, EP.PathDist - FVector::Dist(Inner, EP.EdgePoint));
	EP.EdgePoint = Inner;
	EP.ShortestPath.Pop();
	if (!EP.ShortestPathSegmentVerified.IsEmpty()) {
		EP.ShortestPathSegmentVerified.Pop();
	}
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

// A "direct" edge is alive and not routed through a relay — its own Phase 0 keeps confirming
// real listener→edge LoS.
bool FEdgeCache::HasOtherDirectEdge(const USpatialAudioComponent& Component, const FCachedEdgePoint& Self) {
	for (const FCachedEdgePoint& EP : Component.CachedEdgePoints) {
		if (&EP != &Self && !EP.bEvicting && !EP.bRelayed) {
			return true;
		}
	}
	return false;
}

// Last resort before eviction: route the edge through the most recent listener position that
// could still see it. Both legs (edge→relay, relay→current listener) run sync with reverse
// hygiene — this fires once at the moment LoS is lost, not per frame. Single relay level:
// an already-relayed edge whose relay went dark just evicts. Skipped entirely while a direct
// edge exists — the relay only bridges an otherwise-empty presentation, and a voice parked at
// an old listener position sounds like it comes from the wrong side of the corner once a real
// edge is carrying the sound.
bool FEdgeCache::TryRelayRescue(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                const FVector& LisPos) {
	if (EP.bRelayed || !EP.bHasLastLoSListenerPos || HasOtherDirectEdge(Component, EP)) {
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
// sync re-check: direct listener→edge LoS returning (listener walked back around the corner —
// without un-relaying, the voice stays parked at the relay with its longer path), and the
// edge→relay leg going dark (it was verified only once, at rescue time — dynamic geometry
// closing in between would otherwise keep playing through). One round-trip each per Phase 0
// interval, only while relayed.
void FEdgeCache::TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
                                      const FVector& SrcPos, const FVector& LisPos, bool bIntervalFired) {
	if (!EP.bRelayed) {
		return;
	}
	// Yield to real edges, checked every frame rather than on the interval (it's a flag scan,
	// no traces): the moment a directly-visible edge exists, the relay's bridging job is over
	// (same wrong-side-of-corner rationale as the rescue gate). The relay must be dropped
	// before evicting — see the severed-leg case below for why keeping it would resurrect
	// the edge every interval.
	if (HasOtherDirectEdge(Component, EP)) {
		EP.ClearRelay();
		StartEviction(Component, EP, SrcPos);
		return;
	}
	if (!bIntervalFired) {
		return;
	}
	FHitResult Hit;
	if (!Component.TraceLine(World, Hit, LisPos, EP.EdgePoint)
		&& !Component.TraceLine(World, Hit, EP.EdgePoint, LisPos)) {
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
	if (Component.TraceLine(World, Hit, EP.EdgePoint, EP.RelayPoint)
		|| Component.TraceLine(World, Hit, EP.RelayPoint, EP.EdgePoint)) {
		EP.ClearRelay();
		StartEviction(Component, EP, SrcPos);
	}
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
		}
	}
	Component.bPhase0HandlesStale = false;

	const float EvictFadeTime = Settings.CachedEdgeEvictionFadeTime;
	const float MoveThresh = Settings.CachedEdgeUpdateMoveThreshold;

	Component.Phase0Timer += DeltaTime;
	const bool bIntervalFired = Component.Phase0Timer >= Settings.Phase0CheckInterval * Component.VelocityScaling.EdgeMultiplier;
	if (bIntervalFired) {
		Component.Phase0Timer = 0.f;
	}

	// Compute a progressive effective threshold for each point. Sort non-evicting points by
	// their individual movement delta (most stale first). The k-th point at rank k of N gets
	// threshold * (k+1)/N, so evictions spread across the movement range instead of all
	// triggering at once when the threshold is crossed.
	TArray<float> EffectiveThresholds;
	EffectiveThresholds.SetNum(Component.CachedEdgePoints.Num());
	for (float& V : EffectiveThresholds) { V = MoveThresh; }
	if (MoveThresh > 0.f) {
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
	}

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

	TickShortestPathRecheck(Component, World, SrcPos, DeltaTime, Settings);
	TickInnerAnchorPromotion(Component, World, LisPos, DeltaTime, Settings);
}

// Source-side counterpart of Phase 0: re-traces the stored string-pulled polyline PathDist was
// measured along, catching geometry that closed the source→edge path after discovery (a static
// source, a door closing — nothing else sees it: Phase 0 watches the listener leg, movement
// eviction watches source position, and rank hysteresis discards the worse-ranking re-finds a
// closed path produces). One edge per interval, sync — a handful of traces at most. Every segment
// is checked, including unverified ones (a raw crawl/bounce hop the string pull couldn't shortcut
// past) — a deliberate choice: those were already blocked at discovery, so this will also evict
// on ordinary multi-corner diffraction paths the moment they're rechecked, not just on genuine
// geometry change. Enabling ShortestPathRecheckInterval accepts that trade-off.
void FEdgeCache::TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
                                         const FVector& SrcPos, const float DeltaTime,
                                         const USpatialAudioSettings& Settings) {
	if (Settings.ShortestPathRecheckInterval <= 0.f || Component.CachedEdgePoints.IsEmpty()) {
		return;
	}
	Component.ShortestPathCheckTimer += DeltaTime;
	if (Component.ShortestPathCheckTimer < Settings.ShortestPathRecheckInterval) {
		return;
	}
	Component.ShortestPathCheckTimer = 0.f;

	const int32 Num = Component.CachedEdgePoints.Num();
	int32 Idx = INDEX_NONE;
	for (int32 Step = 0; Step < Num; ++Step) {
		const int32 Candidate = (Component.ShortestPathCheckCursor + Step) % Num;
		if (!Component.CachedEdgePoints[Candidate].bEvicting) {
			Idx = Candidate;
			break;
		}
	}
	if (Idx == INDEX_NONE) {
		return;
	}
	Component.ShortestPathCheckCursor = (Idx + 1) % Num;

	FCachedEdgePoint& EP = Component.CachedEdgePoints[Idx];

	// Entries without a stored polyline (e.g. seeded by the replay sweep, always 0-bounce, so
	// the straight source→edge segment was the actual flight) fall back to that segment.
	TArray<FVector> FallbackPath;
	const TArray<FVector>* Path = &EP.ShortestPath;
	if (EP.ShortestPath.Num() < 2) {
		FallbackPath = {EP.CapturedSourcePos, EP.EdgePoint};
		Path = &FallbackPath;
	}

	// Segment endpoints sit within ~RaySurfaceBias of geometry, so pull both ends in before
	// tracing — a trace grazing its own anchor surface is corner clipping, not an obstruction.
	// Every segment is traced, verified or not: an unverified segment failing again doesn't
	// distinguish "still the same corner" from "geometry changed," so this deliberately treats
	// them the same and evicts on either — a real trade-off, not an oversight (see the eviction
	// comment below).
	const float Pull = FMath::Max(Settings.RaySurfaceBias, 1.f);
	bool bBlocked = false;
	FVector BlockedA = FVector::ZeroVector;
	FVector BlockedB = FVector::ZeroVector;
	FHitResult Hit;
	for (int32 s = 0; s + 1 < Path->Num() && !bBlocked; ++s) {
		FVector A = (*Path)[s];
		FVector B = (*Path)[s + 1];
		const FVector AB = B - A;
		const float Len = AB.Size();
		if (Len <= Pull * 2.f + 1.f) {
			continue;
		}
		const FVector Dir = AB / Len;
		A += Dir * Pull;
		B -= Dir * Pull;
		bBlocked = Component.TraceLine(World, Hit, A, B) || Component.TraceLine(World, Hit, B, A);
		BlockedA = A;
		BlockedB = B;
	}

	if (bBlocked) {
		if (Component.bDrawDebugRays && Component.bShowShortestPaths) {
			DrawDebugLine(World, BlockedA, BlockedB, FColor::Red, false,
			              Settings.DebugLineDuration * 4.f, 0, 3.f);
		}
		// Source-side eviction: the listener leg is typically still clear here, and Phase 0's
		// clear-restore would resurrect the edge every interval, faster than the fade completes.
		StartEviction(Component, EP, SrcPos, /*bSourceSide=*/true);
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

	const int32 Num = Component.CachedEdgePoints.Num();
	int32 Idx = INDEX_NONE;
	for (int32 Step = 0; Step < Num; ++Step) {
		const int32 Candidate = (Component.ShortestPathPromotionCursor + Step) % Num;
		if (!Component.CachedEdgePoints[Candidate].bEvicting && !Component.CachedEdgePoints[Candidate].bRelayed) {
			Idx = Candidate;
			break;
		}
	}
	if (Idx == INDEX_NONE) {
		return;
	}
	Component.ShortestPathPromotionCursor = (Idx + 1) % Num;

	TryPromoteToInnerAnchor(Component, Component.CachedEdgePoints[Idx], World, LisPos);
}
