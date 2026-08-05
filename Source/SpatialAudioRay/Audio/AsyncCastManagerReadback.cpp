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

	const float SourceR = Component.AttenuationInnerRadius;
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
		const float Weight = DistW;
		WeightedPosSum += EdgePoint * Weight;
		WeightedDistSum += Probe.BasePathDist * Weight;
		TotalWeight += Weight;

		FStoredLoSPath StoredPath;
		StoredPath.LoSOrigin = EdgePoint;
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

float FAsyncCastManager::RankScore(const FCacheMergeContext& Ctx, const USpatialAudioSettings& Settings,
                                   const float PathDist, const FVector& Point) {
	const float MaxRay = FMath::Max(Ctx.MaxRayDistance, 1.f);
	return (1.f / (1.f + Settings.CandidateDistanceFalloff * PathDist / MaxRay))
		/ (1.f + Settings.ListenerDistanceFalloff
			* FVector::Dist(Ctx.ListenerPos, Point) / MaxRay);
}

bool FAsyncCastManager::OutranksIncumbent(const FCacheMergeContext& Ctx, const USpatialAudioSettings& Settings,
                                          const FStoredLoSPath& Found, const FCachedEdgePoint& Incumbent) {
	if (Incumbent.bRelayed) {
		return true;
	}
	return RankScore(Ctx, Settings, Found.PathDist, Found.LoSOrigin)
		> RankScore(Ctx, Settings, Incumbent.EffectivePathDist(), Incumbent.EffectivePoint()) * 1.01f;
}

void FAsyncCastManager::WriteEntry(const FCacheMergeContext& Ctx, FCachedEdgePoint& Edge,
                                   const FStoredLoSPath& Found) {
	Edge.EdgePoint = Found.LoSOrigin;
	Edge.GeomDist = Found.LoSCumulativeDistance;
	Edge.PathDist = Found.PathDist;
	Edge.ShortestPath = Found.ShortestPath;
	Edge.ShortestPathSegmentVerified = Found.ShortestPathSegmentVerified;
	Edge.CapturedSourcePos = Ctx.SourcePos;
	Edge.CapturedListenerPos = Ctx.ListenerPos;
	Edge.bPhase0Pending = false;
	Edge.bEvicting = false;
	Edge.ClearRelay();
	Edge.LastLoSListenerPos = Ctx.ListenerPos;
	Edge.bHasLastLoSListenerPos = true;
}

int32 FAsyncCastManager::FindMergeCandidate(const TArray<FCachedEdgePoint>& Edges, const FVector& Point,
                                            const float MergeRadiusSq) {
	int32 BestIdx = INDEX_NONE;
	float BestDistSq = MergeRadiusSq;
	for (int32 i = 0; i < Edges.Num(); ++i) {
		const float DistSq = FVector::DistSquared(Edges[i].EdgePoint, Point);
		if (DistSq < BestDistSq) {
			BestDistSq = DistSq;
			BestIdx = i;
		}
	}
	return BestIdx;
}

void FAsyncCastManager::MergeIntoSameCorner(const FCacheMergeContext& Ctx, FCachedEdgePoint& Edge,
                                            const FStoredLoSPath& Found) {
	if (Edge.bRelayed || Found.PathDist < Edge.EffectivePathDist() * 0.99f) {
		WriteEntry(Ctx, Edge, Found);
		return;
	}
	Edge.CapturedSourcePos = Ctx.SourcePos;
	Edge.CapturedListenerPos = Ctx.ListenerPos;
}

bool FAsyncCastManager::IsWorseIncumbent(const FCacheMergeContext& Ctx, const USpatialAudioSettings& Settings,
                                         const FCachedEdgePoint& Candidate, const FCachedEdgePoint& Worst) {
	if (Candidate.bRelayed != Worst.bRelayed) {
		return Candidate.bRelayed;
	}
	return RankScore(Ctx, Settings, Candidate.EffectivePathDist(), Candidate.EffectivePoint())
		< RankScore(Ctx, Settings, Worst.EffectivePathDist(), Worst.EffectivePoint());
}

int32 FAsyncCastManager::FindWorstIncumbent(const TArray<FCachedEdgePoint>& Edges, const FCacheMergeContext& Ctx,
                                            const USpatialAudioSettings& Settings,
                                            const TArray<bool>& bMatchedThisCycle) {
	int32 WorstIdx = INDEX_NONE;
	for (int32 i = 0; i < Edges.Num(); ++i) {
		if (bMatchedThisCycle[i]) {
			continue;
		}
		if (WorstIdx == INDEX_NONE || IsWorseIncumbent(Ctx, Settings, Edges[i], Edges[WorstIdx])) {
			WorstIdx = i;
		}
	}
	return WorstIdx;
}

bool FAsyncCastManager::TryDisplaceWorstIncumbent(TArray<FCachedEdgePoint>& Edges, const FCacheMergeContext& Ctx,
                                                  const USpatialAudioSettings& Settings,
                                                  const FStoredLoSPath& Found, TArray<bool>& bMatchedThisCycle) {
	const int32 WorstIdx = FindWorstIncumbent(Edges, Ctx, Settings, bMatchedThisCycle);
	if (WorstIdx == INDEX_NONE || !OutranksIncumbent(Ctx, Settings, Found, Edges[WorstIdx])) {
		return false;
	}
	WriteEntry(Ctx, Edges[WorstIdx], Found);
	Edges[WorstIdx].bNewSinceFillArm = true;
	bMatchedThisCycle[WorstIdx] = true;
	return true;
}

void FAsyncCastManager::MergeStoredPaths(TArray<FCachedEdgePoint>& Edges, const TArray<FStoredLoSPath>& Found,
                                         const FCacheMergeContext& Ctx, const USpatialAudioSettings& Settings) {
	const float MergeRadiusSq = FMath::Square(Settings.CachedEdgeMergeRadius);
	TArray<bool> bMatchedThisCycle;
	bMatchedThisCycle.Init(false, Edges.Num());

	bool bWorstReplacedThisCycle = false;

	for (const FStoredLoSPath& Path : Found) {
		const int32 MergeIdx = FindMergeCandidate(Edges, Path.LoSOrigin, MergeRadiusSq);
		if (MergeIdx != INDEX_NONE) {
			MergeIntoSameCorner(Ctx, Edges[MergeIdx], Path);
			bMatchedThisCycle[MergeIdx] = true;
			continue;
		}

		if (Edges.Num() < Ctx.MaxEdgeCount) {
			FCachedEdgePoint NewEdge;
			WriteEntry(Ctx, NewEdge, Path);
			NewEdge.bNewSinceFillArm = true;
			Edges.Add(MoveTemp(NewEdge));
			bMatchedThisCycle.Add(true);
			continue;
		}

		if (!bWorstReplacedThisCycle) {
			bWorstReplacedThisCycle = TryDisplaceWorstIncumbent(Edges, Ctx, Settings, Path, bMatchedThisCycle);
		}
	}
}

void FAsyncCastManager::MergeStoredPathsIntoCache(USpatialAudioComponent& Component,
                                                  const USpatialAudioSettings& Settings) {
	FCacheMergeContext Ctx;
	Ctx.SourcePos = Component.AsyncSourcePos;
	Ctx.ListenerPos = Component.AsyncListenerPos;
	Ctx.MaxRayDistance = Component.MaxRayDistance;
	Ctx.MaxEdgeCount = Component.GetEffectiveCachedEdgeMaxCount();

	MergeStoredPaths(Component.CachedEdgePoints, Component.StoredLoSPaths, Ctx, Settings);
	Component.StoredLoSPaths.Reset();
}

void FAsyncCastManager::AdvanceIdleState(USpatialAudioComponent& Component,
                                         const USpatialAudioSettings& Settings) {
	if (Component.IsCacheFillPending()) {
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

	Component.TargetPathAttenuation = Component.ComputePathAttenuationCurved(AccumOut.MinLoSDist, Settings);

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
		Out.MinLoSDist = FMath::Min(Out.MinLoSDist, Edge.OutputPathDist());
		const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff
			* Edge.GeomDist / FMath::Max(MaxRayDistance, 1.f));
		Out.WeightedPos += Edge.OutputPoint() * DistW;
		Out.WeightedDist += Edge.OutputPathDist() * DistW;
		Out.TotalWeight += DistW;
	}
	return Out;
}
