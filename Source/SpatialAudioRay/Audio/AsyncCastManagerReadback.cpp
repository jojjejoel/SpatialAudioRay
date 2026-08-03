#include "Audio/AsyncCastManager.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"


// A sweep spans several frames against frozen positions, so the listener may have regained sight
// mid-flight. Publishing then registers edges that clear a frame later, which pumps the virtual
// voice audibly. Pre-sweep results are kept: warming the cache during partial LoS is the point.
bool FAsyncCastManager::TryDiscardStaleSweep(USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings) {
	if (Component.Finalize.bDirectLoSFound) {
		return false;
	}

	AActor* Owner = Component.GetOwner();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Owner || !Pawn) {
		return false;
	}

	const float SourceR = Component.AttenuationInnerRadius * Settings.SourceLoSSampleRadiusScale;
	const float OffsetFraction = FUpdater::SyncOffsetLoSFraction(
		Component, World, Owner->GetActorLocation(), Pawn->GetActorLocation(),
		Settings.DirectLoSSampleRadius,
		SourceR, SourceR,
		UE_HALF_PI / Component.ResolveRingRotationSteps());
	if (1.f - OffsetFraction >= Settings.PreSweepOcclusionThreshold) {
		return false;
	}

	Component.LastOffsetLoSFraction = OffsetFraction;
	Component.NoLoSSampleStreak = 0;
	Component.bHasDirectLoS = true;
	Component.TraceDiag.LastSweepDuration =
		Component.GetWorld()->GetTimeSeconds() - Component.TraceDiag.SweepStartTime;
	Component.Finalize.bPending = false;
	Component.Finalize.RefineProbes.Reset();
	Component.StoredLoSPaths.Reset();
	Component.StaggeredCycleIndex =
		(Component.StaggeredCycleIndex + 1) % FMath::Max(1, Settings.FullSweepCycleCount);
	return true;
}

void FAsyncCastManager::AccumulateRefineProbesIntoCycle(USpatialAudioComponent& Component, const UWorld* World, const USpatialAudioSettings& Settings) {
	FVector WeightedPosSum = Component.Finalize.WeightedPosSum;
	float TotalWeight = Component.Finalize.TotalWeight;
	float WeightedDistSum = Component.Finalize.WeightedDistSum;

	for (const FFinalizeRefineProbe& Probe : Component.Finalize.RefineProbes) {
		const FVector EdgePoint = Probe.LoSOrigin;

		if (Component.bDrawDebugRays && Component.bShowEdgePoints && World) {
			DrawDebugSphere(World, EdgePoint, 14.f, 8, FColor(80, 255, 120), false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
		}

		const float GeomDist = FVector::Dist(Component.AsyncSourcePos, EdgePoint);
		const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff * GeomDist
			/ FMath::Max(Component.MaxRayDistance, 1.f));
		const float Weight = DistW * Probe.BounceWeightFactor;
		WeightedPosSum += EdgePoint * Weight;
		WeightedDistSum += Probe.BasePathDist * Weight;
		TotalWeight += Weight;

		FStoredLoSPath StoredPath;
		StoredPath.LoSOrigin = EdgePoint;
		StoredPath.LoSBounces = Probe.LoSBounces;
		StoredPath.LoSCumulativeDistance = GeomDist;
		StoredPath.PathDist = Probe.BasePathDist;
		StoredPath.ShortestPath = Probe.ShortestPath;
		StoredPath.ShortestPathSegmentVerified = Probe.ShortestPathSegmentVerified;
		Component.StoredLoSPaths.Add(MoveTemp(StoredPath));
	}

	Component.TraceDiag.LastSweepDuration = Component.GetWorld()->GetTimeSeconds() - Component.TraceDiag.SweepStartTime;
	Component.Finalize.bPending = false;
	Component.Finalize.RefineProbes.Reset();

	Component.CycleAccum.RaysReached += Component.Finalize.RaysReached;
	Component.CycleAccum.MinLoSDist = FMath::Min(Component.CycleAccum.MinLoSDist, Component.Finalize.MinLoSDist);
	Component.CycleAccum.WeightedPos += WeightedPosSum;
	Component.CycleAccum.TotalWeight += TotalWeight;
	Component.CycleAccum.WeightedDist += WeightedDistSum;
	Component.CycleAccum.bDirectLoSFound |= Component.Finalize.bDirectLoSFound;
}

float FAsyncCastManager::RankScore(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                   const float PathDist, const FVector& Point) {
	const float MaxRay = FMath::Max(Component.MaxRayDistance, 1.f);
	return (1.f / (1.f + Settings.CandidateDistanceFalloff * PathDist / MaxRay))
		/ (1.f + Settings.ListenerDistanceFalloff
			* FVector::Dist(Component.AsyncListenerPos, Point) / MaxRay);
}

bool FAsyncCastManager::OutranksIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                          const FStoredLoSPath& Found, const FCachedEdgePoint& Incumbent) {
	// A relay is a stopgap through an old listener position, so its low bounce count must not
	// lock a full cache against every new find.
	if (Incumbent.bRelayed) {
		return true;
	}
	// Bounce count is the primary key because these two describe DIFFERENT corners, where it
	// still carries information about path quality. The 1% margin is anti-churn hysteresis.
	return Found.LoSBounces < Incumbent.LoSBounces ||
		(Found.LoSBounces == Incumbent.LoSBounces
			&& RankScore(Component, Settings, Found.PathDist, Found.LoSOrigin)
				> RankScore(Component, Settings, Incumbent.EffectivePathDist(), Incumbent.EffectivePoint()) * 1.01f);
}

void FAsyncCastManager::WriteEntry(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                   const FStoredLoSPath& Found) {
	Edge.EdgePoint = Found.LoSOrigin;
	Edge.GeomDist = Found.LoSCumulativeDistance;
	Edge.PathDist = Found.PathDist;
	Edge.ShortestPath = Found.ShortestPath;
	Edge.ShortestPathSegmentVerified = Found.ShortestPathSegmentVerified;
	Edge.LoSBounces = Found.LoSBounces;
	Edge.CapturedSourcePos = Component.AsyncSourcePos;
	Edge.CapturedListenerPos = Component.AsyncListenerPos;
	Edge.bPhase0Pending = false;
	Edge.bEvicting = false;
	Edge.bSourceSideEviction = false;
	Edge.EvictionAlpha = 1.f;
	// A sweep re-confirming the edge proves a fresh listener-visible path, so any relay is obsolete.
	Edge.ClearRelay();
	Edge.LastLoSListenerPos = Component.AsyncListenerPos;
	Edge.bHasLastLoSListenerPos = true;
}

int32 FAsyncCastManager::FindMergeCandidate(const USpatialAudioComponent& Component, const FVector& Point,
                                            const float MergeRadiusSq) {
	int32 BestIdx = INDEX_NONE;
	float BestDistSq = MergeRadiusSq;
	for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
		const float DistSq = FVector::DistSquared(Component.CachedEdgePoints[i].EdgePoint, Point);
		if (DistSq < BestDistSq) {
			BestDistSq = DistSq;
			BestIdx = i;
		}
	}
	return BestIdx;
}

void FAsyncCastManager::MergeIntoSameCorner(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
                                            const FStoredLoSPath& Found) {
	// One corner means one shared listener leg, so only travelled distance separates the routes.
	// Bounce count is deliberately not the primary key here, unlike OutranksIncumbent. The 1%
	// margin stops near-equal routes rewriting the entry, which drops an in-flight probe each time.
	if (Edge.bRelayed || Found.PathDist < Edge.EffectivePathDist() * 0.99f) {
		WriteEntry(Component, Edge, Found);
		return;
	}
	Edge.CapturedSourcePos = Component.AsyncSourcePos;
	Edge.CapturedListenerPos = Component.AsyncListenerPos;
}

bool FAsyncCastManager::IsWorseIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                         const FCachedEdgePoint& Candidate, const FCachedEdgePoint& Worst) {
	// Relays are categorically worst, so bounce and rank only separate entries within one class.
	if (Candidate.bRelayed != Worst.bRelayed) {
		return Candidate.bRelayed;
	}
	return Candidate.LoSBounces > Worst.LoSBounces ||
		(Candidate.LoSBounces == Worst.LoSBounces
			&& RankScore(Component, Settings, Candidate.EffectivePathDist(), Candidate.EffectivePoint())
				< RankScore(Component, Settings, Worst.EffectivePathDist(), Worst.EffectivePoint()));
}

int32 FAsyncCastManager::FindWorstIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                            const TArray<bool>& bMatchedThisCycle) {
	int32 WorstIdx = INDEX_NONE;
	for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
		if (bMatchedThisCycle[i]) {
			continue;
		}
		if (WorstIdx == INDEX_NONE
			|| IsWorseIncumbent(Component, Settings, Component.CachedEdgePoints[i], Component.CachedEdgePoints[WorstIdx])) {
			WorstIdx = i;
		}
	}
	return WorstIdx;
}

bool FAsyncCastManager::TryDisplaceWorstIncumbent(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                                  const FStoredLoSPath& Found, TArray<bool>& bMatchedThisCycle) {
	const int32 WorstIdx = FindWorstIncumbent(Component, Settings, bMatchedThisCycle);
	if (WorstIdx == INDEX_NONE || !OutranksIncumbent(Component, Settings, Found, Component.CachedEdgePoints[WorstIdx])) {
		return false;
	}
	WriteEntry(Component, Component.CachedEdgePoints[WorstIdx], Found);
	// A displacement lands at a different corner, so it counts as a discovery; a merge does not.
	Component.CachedEdgePoints[WorstIdx].bNewSinceFillArm = true;
	bMatchedThisCycle[WorstIdx] = true;
	return true;
}

void FAsyncCastManager::MergeStoredPathsIntoCache(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	const float MergeRadiusSq = FMath::Square(Settings.CachedEdgeMergeRadius);
	TArray<bool> bMatchedThisCycle;
	bMatchedThisCycle.Init(false, Component.CachedEdgePoints.Num());

	bool bWorstReplacedThisCycle = false;

	for (const FStoredLoSPath& Found : Component.StoredLoSPaths) {
		const int32 MergeIdx = FindMergeCandidate(Component, Found.LoSOrigin, MergeRadiusSq);
		if (MergeIdx != INDEX_NONE) {
			MergeIntoSameCorner(Component, Component.CachedEdgePoints[MergeIdx], Found);
			bMatchedThisCycle[MergeIdx] = true;
			continue;
		}

		// The cache deliberately holds points from ALL openings. Voice clustering is what groups
		// them into emitters and drops the small groups.
		if (Component.CachedEdgePoints.Num() < Settings.CachedEdgeMaxCount) {
			FCachedEdgePoint NewEdge;
			WriteEntry(Component, NewEdge, Found);
			NewEdge.bNewSinceFillArm = true;
			Component.CachedEdgePoints.Add(MoveTemp(NewEdge));
			bMatchedThisCycle.Add(true);
			continue;
		}

		// Displaced only by a candidate that outranks it, never for not being re-found. One per cycle.
		if (!bWorstReplacedThisCycle) {
			bWorstReplacedThisCycle = TryDisplaceWorstIncumbent(Component, Settings, Found, bMatchedThisCycle);
		}
	}
	Component.StoredLoSPaths.Reset();
}

void FAsyncCastManager::AdvanceSweepCycleAndIdleState(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	const int32 CycleCount = FMath::Max(1, Settings.FullSweepCycleCount);
	Component.StaggeredCycleIndex = (Component.StaggeredCycleIndex + 1) % CycleCount;
	Component.CycleAccum.Index = 0;

	// Only arm idle when the finished sub-cycle wrapped the sequence, or the remaining sphere
	// coverage would crawl at StationaryIdleMultiplier speed after movement breaks idle.
	const bool bSweepSequenceComplete = Component.StaggeredCycleIndex == 0;

	// Spend cache-fill budget only when the sweep still left the target short. A sweep that
	// filled it is free, so the budget survives if those edges are evicted moments later.
	if (bSweepSequenceComplete && Component.SweepScheduling.CacheFillSweepsRemaining > 0
		&& Component.CountCacheFillEdges() < Settings.MovementCacheFillRequiredEdges) {
		--Component.SweepScheduling.CacheFillSweepsRemaining;
	}
	if (bSweepSequenceComplete && Component.VelocityScaling.IsStationary()) {
		Component.SweepScheduling.bStationaryIdleMode = true;
		Component.SweepScheduling.StationaryIdleSourcePos = Component.AsyncSourcePos;
		Component.SweepScheduling.StationaryIdleListenerPos = Component.AsyncListenerPos;
	}
}

void FAsyncCastManager::PublishSweepAudioTargets(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	FRayAccumulatorInput AccumIn;
	AccumIn.RaysReached = Component.CycleAccum.RaysReached;
	AccumIn.MinLoSDist = Component.CycleAccum.MinLoSDist;
	AccumIn.WeightedPos = Component.CycleAccum.WeightedPos;
	AccumIn.TotalWeight = Component.CycleAccum.TotalWeight;
	AccumIn.MaxRayDistance = Component.MaxRayDistance;
	AccumIn.bDirectLoSFound = Component.CycleAccum.bDirectLoSFound;
	const FRayAccumulatorOutput AccumOut = ComputeAudioFromRayAccumulator(AccumIn);

	// TargetOcclusion is owned by the per-frame sampler. A sweep-derived path-ratio value reads
	// lower whenever a good diffraction path exists, and kept knocking the target off 100%.
	const float Leg1Geom = AccumOut.bHasVirtualSource
		? FVector::Dist(Component.AsyncSourcePos, AccumOut.VirtualSourcePos)
		: AccumOut.MinLoSDist;
	Component.TargetPathAttenuation = Component.ComputePathAttenuationCurved(
		AccumOut.MinLoSDist, Leg1Geom, Settings);

	if (!AccumOut.bHasVirtualSource) {
		return;
	}

	const float AvgSourceToEdgeDist = Component.CycleAccum.WeightedDist / Component.CycleAccum.TotalWeight;
	Component.CurrentSourceToVirtualDistance = FMath::FInterpTo(
		Component.CurrentSourceToVirtualDistance, AvgSourceToEdgeDist,
		Component.GetWorld()->GetDeltaSeconds(), 10.0f);
	Component.TargetVirtualSourceLocation = AccumOut.VirtualSourcePos;
}

void FAsyncCastManager::ReadbackFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();

	if (TryDiscardStaleSweep(Component, World, Settings)) {
		return;
	}

	AccumulateRefineProbesIntoCycle(Component, World, Settings);

	if (Component.StoredLoSPaths.Num() > 0) {
		MergeStoredPathsIntoCache(Component, Settings);
	}

	AdvanceSweepCycleAndIdleState(Component, Settings);
	PublishSweepAudioTargets(Component, Settings);

	if (Component.CycleAccum.TotalWeight > 0.f && Component.bDrawDebugRays) {
		TArray<FVector>& Path = Component.LoSDiffractionPaths.AddDefaulted_GetRef();
		Path.Add(Component.AsyncSourcePos);
		Path.Add(Component.TargetVirtualSourceLocation);
		Path.Add(Component.AsyncListenerPos);
	}

	// Pre-warm casts must not stomp bHasDirectLoS: the per-frame sampler owns it.
	if (!Component.bPreSweepCast) {
		Component.bHasDirectLoS = Component.CycleAccum.bDirectLoSFound;
	}
	if (Component.CycleAccum.bDirectLoSFound) {
		Component.TargetOcclusion = 0.f;
		Component.CachedEdgePoints.Empty();
	}
}

FAsyncCastManager::FRayAccumulatorOutput FAsyncCastManager::ComputeAudioFromRayAccumulator(
	const FRayAccumulatorInput& In) {
	FRayAccumulatorOutput Out;

	Out.MinLoSDist = In.RaysReached > 0 ? In.MinLoSDist : In.MaxRayDistance;

	if (!In.bDirectLoSFound && In.TotalWeight > 0.f) {
		Out.VirtualSourcePos = In.WeightedPos / In.TotalWeight;
		Out.bHasVirtualSource = true;
	}

	return Out;
}

FAsyncCastManager::FCachedPointAccum FAsyncCastManager::AccumulateCachedPoints(
	const TArray<FCachedEdgePoint>& Points,
	const float MaxRayDistance,
	const USpatialAudioSettings& Settings) {
	FCachedPointAccum Out;
	for (const FCachedEdgePoint& Edge : Points) {
		++Out.RaysReached;
		Out.MinLoSDist = FMath::Min(Out.MinLoSDist, Edge.EffectivePathDist());
		// Same normaliser as AccumulateRefineProbesIntoCycle: cached points and fresh probes are
		// summed into one weighted centroid, so both must weigh distance on the same scale.
		const float DistW = Edge.EvictionAlpha / (1.f + Settings.CandidateDistanceFalloff
			* Edge.GeomDist / FMath::Max(MaxRayDistance, 1.f));
		Out.WeightedPos += Edge.EffectivePoint() * DistW;
		Out.WeightedDist += Edge.EffectivePathDist() * DistW;
		Out.TotalWeight += DistW;
	}
	return Out;
}
