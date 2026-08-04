#include "Audio/AsyncCastManager.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"


bool FAsyncCastManager::TryDiscardStaleSweep(USpatialAudioComponent& Component, UWorld* World,
                                             const USpatialAudioSettings& Settings) {
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
	return true;
}

void FAsyncCastManager::AccumulateRefineProbes(USpatialAudioComponent& Component, const UWorld* World,
                                               const USpatialAudioSettings& Settings) {
	FVector WeightedPosSum = Component.Finalize.WeightedPosSum;
	float TotalWeight = Component.Finalize.TotalWeight;
	float WeightedDistSum = Component.Finalize.WeightedDistSum;

	for (const FFinalizeRefineProbe& Probe : Component.Finalize.RefineProbes) {
		const FVector EdgePoint = Probe.LoSOrigin;

		if (Component.bDrawDebugRays && Component.bShowEdgePoints && World) {
			DrawDebugSphere(World, EdgePoint, 14.f, 8, FColor(80, 255, 120), false, Settings.DebugLineDuration,
			                SDPG_Foreground, 2.f);
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

	Component.Finalize.WeightedPosSum = WeightedPosSum;
	Component.Finalize.TotalWeight = TotalWeight;
	Component.Finalize.WeightedDistSum = WeightedDistSum;
}

float FAsyncCastManager::RankScore(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                   const float PathDist, const FVector& Point) {
	const float MaxRay = FMath::Max(Component.MaxRayDistance, 1.f);
	return (1.f / (1.f + Settings.CandidateDistanceFalloff * PathDist / MaxRay))
		/ (1.f + Settings.ListenerDistanceFalloff
			* FVector::Dist(Component.AsyncListenerPos, Point) / MaxRay);
}

bool FAsyncCastManager::OutranksIncumbent(const USpatialAudioComponent& Component,
                                          const USpatialAudioSettings& Settings,
                                          const FStoredLoSPath& Found, const FCachedEdgePoint& Incumbent) {
	if (Incumbent.bRelayed) {
		return true;
	}
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
	if (Edge.bRelayed || Found.PathDist < Edge.EffectivePathDist() * 0.99f) {
		WriteEntry(Component, Edge, Found);
		return;
	}
	Edge.CapturedSourcePos = Component.AsyncSourcePos;
	Edge.CapturedListenerPos = Component.AsyncListenerPos;
}

bool FAsyncCastManager::IsWorseIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                         const FCachedEdgePoint& Candidate, const FCachedEdgePoint& Worst) {
	if (Candidate.bRelayed != Worst.bRelayed) {
		return Candidate.bRelayed;
	}
	return Candidate.LoSBounces > Worst.LoSBounces ||
	(Candidate.LoSBounces == Worst.LoSBounces
		&& RankScore(Component, Settings, Candidate.EffectivePathDist(), Candidate.EffectivePoint())
		< RankScore(Component, Settings, Worst.EffectivePathDist(), Worst.EffectivePoint()));
}

int32 FAsyncCastManager::FindWorstIncumbent(const USpatialAudioComponent& Component,
                                            const USpatialAudioSettings& Settings,
                                            const TArray<bool>& bMatchedThisCycle) {
	int32 WorstIdx = INDEX_NONE;
	for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
		if (bMatchedThisCycle[i]) {
			continue;
		}
		if (WorstIdx == INDEX_NONE
			|| IsWorseIncumbent(Component, Settings, Component.CachedEdgePoints[i],
			                    Component.CachedEdgePoints[WorstIdx])) {
			WorstIdx = i;
		}
	}
	return WorstIdx;
}

bool FAsyncCastManager::TryDisplaceWorstIncumbent(USpatialAudioComponent& Component,
                                                  const USpatialAudioSettings& Settings,
                                                  const FStoredLoSPath& Found, TArray<bool>& bMatchedThisCycle) {
	const int32 WorstIdx = FindWorstIncumbent(Component, Settings, bMatchedThisCycle);
	if (WorstIdx == INDEX_NONE || !
		OutranksIncumbent(Component, Settings, Found, Component.CachedEdgePoints[WorstIdx])) {
		return false;
	}
	WriteEntry(Component, Component.CachedEdgePoints[WorstIdx], Found);
	Component.CachedEdgePoints[WorstIdx].bNewSinceFillArm = true;
	bMatchedThisCycle[WorstIdx] = true;
	return true;
}

void FAsyncCastManager::MergeStoredPathsIntoCache(USpatialAudioComponent& Component,
                                                  const USpatialAudioSettings& Settings) {
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

		if (Component.CachedEdgePoints.Num() < Settings.CachedEdgeMaxCount) {
			FCachedEdgePoint NewEdge;
			WriteEntry(Component, NewEdge, Found);
			NewEdge.bNewSinceFillArm = true;
			Component.CachedEdgePoints.Add(MoveTemp(NewEdge));
			bMatchedThisCycle.Add(true);
			continue;
		}

		if (!bWorstReplacedThisCycle) {
			bWorstReplacedThisCycle = TryDisplaceWorstIncumbent(Component, Settings, Found, bMatchedThisCycle);
		}
	}
	Component.StoredLoSPaths.Reset();
}

void FAsyncCastManager::AdvanceIdleState(USpatialAudioComponent& Component,
                                         const USpatialAudioSettings& Settings) {
	if (Component.SweepScheduling.CacheFillSweepsRemaining > 0
		&& Component.CountCacheFillEdges() < Settings.MovementCacheFillRequiredEdges) {
		--Component.SweepScheduling.CacheFillSweepsRemaining;
	}
	if (Component.VelocityScaling.IsStationary()) {
		Component.SweepScheduling.bStationaryIdleMode = true;
		Component.SweepScheduling.StationaryIdleSourcePos = Component.AsyncSourcePos;
		Component.SweepScheduling.StationaryIdleListenerPos = Component.AsyncListenerPos;
	}
}

void FAsyncCastManager::PublishSweepAudioTargets(USpatialAudioComponent& Component,
                                                 const USpatialAudioSettings& Settings) {
	FRayAccumulatorInput AccumIn;
	AccumIn.RaysReached = Component.Finalize.RaysReached;
	AccumIn.MinLoSDist = Component.Finalize.MinLoSDist;
	AccumIn.WeightedPos = Component.Finalize.WeightedPosSum;
	AccumIn.TotalWeight = Component.Finalize.TotalWeight;
	AccumIn.MaxRayDistance = Component.MaxRayDistance;
	AccumIn.bDirectLoSFound = Component.Finalize.bDirectLoSFound;
	const FRayAccumulatorOutput AccumOut = ComputeAudioFromRayAccumulator(AccumIn);

	const float Leg1Geom = AccumOut.bHasVirtualSource
		                       ? FVector::Dist(Component.AsyncSourcePos, AccumOut.VirtualSourcePos)
		                       : AccumOut.MinLoSDist;
	Component.TargetPathAttenuation = Component.ComputePathAttenuationCurved(
		AccumOut.MinLoSDist, Leg1Geom, Settings);

	if (!AccumOut.bHasVirtualSource) {
		return;
	}

	const float AvgSourceToEdgeDist = Component.Finalize.WeightedDistSum / Component.Finalize.TotalWeight;
	Component.CurrentSourceToVirtualDistance = FMath::FInterpTo(
		Component.CurrentSourceToVirtualDistance, AvgSourceToEdgeDist,
		Component.GetWorld()->GetDeltaSeconds(), 10.0f);
	Component.TargetVirtualSourceLocation = AccumOut.VirtualSourcePos;
}

void FAsyncCastManager::ReadbackFinalizeBatch(USpatialAudioComponent& Component,
                                              const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();

	if (TryDiscardStaleSweep(Component, World, Settings)) {
		return;
	}

	AccumulateRefineProbes(Component, World, Settings);

	if (Component.StoredLoSPaths.Num() > 0) {
		MergeStoredPathsIntoCache(Component, Settings);
	}

	AdvanceIdleState(Component, Settings);
	PublishSweepAudioTargets(Component, Settings);

	if (Component.Finalize.TotalWeight > 0.f && Component.bDrawDebugRays) {
		TArray<FVector>& Path = Component.LoSDiffractionPaths.AddDefaulted_GetRef();
		Path.Add(Component.AsyncSourcePos);
		Path.Add(Component.TargetVirtualSourceLocation);
		Path.Add(Component.AsyncListenerPos);
	}

	if (!Component.bPreSweepCast) {
		Component.bHasDirectLoS = Component.Finalize.bDirectLoSFound;
	}
	if (Component.Finalize.bDirectLoSFound) {
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
		const float DistW = Edge.EvictionAlpha / (1.f + Settings.CandidateDistanceFalloff
			* Edge.GeomDist / FMath::Max(MaxRayDistance, 1.f));
		Out.WeightedPos += Edge.EffectivePoint() * DistW;
		Out.WeightedDist += Edge.EffectivePathDist() * DistW;
		Out.TotalWeight += DistW;
	}
	return Out;
}
