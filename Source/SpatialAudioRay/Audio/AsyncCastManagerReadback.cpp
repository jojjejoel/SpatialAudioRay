#include "Audio/AsyncCastManager.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/Math.h"
#include "Audio/Updater.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"


void FAsyncCastManager::ReadbackFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();

	// The sweep was submitted against stale positions: the listener may have regained LoS while
	// the multi-frame cast was in flight. Re-run the LoS sample synchronously at current
	// positions and discard the whole sweep when occlusion has fallen back below the pre-sweep
	// threshold — publishing then would register edges that get cleared a frame later, audibly
	// pumping the virtual voice. With PreSweepOcclusionThreshold at 1 this reduces to the
	// original rule (discard on ANY clear sample); below 1, results in the pre-warm band are
	// deliberately kept — warming the cache during partial LoS is the point of pre-sweeps.
	if (!Component.Finalize.bDirectLoSFound) {
		AActor* Owner = Component.GetOwner();
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Owner && Pawn) {
			const float SourceR = Settings.bEnableOffsetLoSChecks
				                      ? Component.AttenuationInnerRadius * Settings.SourceLoSSampleRadiusScale
				                      : 0.f;
			const float OffsetFraction = FUpdater::SyncOffsetLoSFraction(
				Component, World, Owner->GetActorLocation(), Pawn->GetActorLocation(),
				Settings.bEnableOffsetLoSChecks ? Settings.DirectLoSSampleRadius : 0.f,
				SourceR, SourceR,
				UE_HALF_PI / FMath::Clamp(Settings.OffsetRingRotationSteps, 1, 8));
			if (1.f - OffsetFraction < Settings.PreSweepOcclusionThreshold) {
				Component.LastOffsetLoSFraction = OffsetFraction;
				Component.NoLoSSampleStreak = 0;
				Component.bHasDirectLoS = true;
				Component.TraceDiag.LastSweepDuration =
					Component.GetWorld()->GetTimeSeconds() - Component.TraceDiag.SweepStartTime;
				Component.TraceDiag.LastFinalizeRetries = Component.TraceDiag.FinalizeRetries;
				Component.TraceDiag.FinalizeRetries = 0;
				Component.Finalize.bPending = false;
				Component.Finalize.RefineProbes.Reset();
				Component.StoredLoSPaths.Reset();
				Component.StaggeredCycleIndex =
					(Component.StaggeredCycleIndex + 1) % FMath::Max(1, Settings.FullSweepCycleCount);
				return;
			}
		}
	}

	int32 RaysReached = Component.Finalize.RaysReached;
	int32 TotalLoSBounces = Component.Finalize.TotalLoSBounces;
	float MinLoSDist = Component.Finalize.MinLoSDist;
	FVector WeightedPosSum = Component.Finalize.WeightedPosSum;
	float TotalWeight = Component.Finalize.TotalWeight;
	float WeightedDistSum = Component.Finalize.WeightedDistSum;
	bool bDirectLoSFound = Component.Finalize.bDirectLoSFound;

	for (const FFinalizeRefineProbe& RP : Component.Finalize.RefineProbes) {
		const FVector TrueEdge = RP.LoSOrigin;
		const float PathDistToEdge = RP.BasePathDist;

		if (Component.bDrawDebugRays && Component.bShowEdgePoints && World) {
			DrawDebugSphere(World, TrueEdge, 14.f, 8, FColor(80, 255, 120), false, Component.DebugLineDuration, 0, 2.f);
		}

		const float GeomDist = FVector::Dist(Component.AsyncSourcePos, TrueEdge);
		const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff * GeomDist
			/ FMath::Max(Component.MaxRayDistance, 1.f));
		const float Weight = DistW * RP.BounceWeightFactor;
		WeightedPosSum += TrueEdge * Weight;
		WeightedDistSum += PathDistToEdge * Weight;
		TotalWeight += Weight;

		FStoredLoSPath StoredPath;
		StoredPath.LoSOrigin = TrueEdge;
		StoredPath.LoSBounces = RP.LoSBounces;
		StoredPath.LoSCumulativeDistance = GeomDist;
		StoredPath.PathDist = PathDistToEdge;
		StoredPath.ShortestPath = RP.ShortestPath;
		StoredPath.ShortestPathVerifiedFrom = RP.ShortestPathVerifiedFrom;
		Component.StoredLoSPaths.Add(MoveTemp(StoredPath));
	}

	Component.TraceDiag.LastSweepDuration = Component.GetWorld()->GetTimeSeconds() - Component.TraceDiag.SweepStartTime;
	Component.TraceDiag.LastFinalizeRetries = Component.TraceDiag.FinalizeRetries;
	Component.TraceDiag.FinalizeRetries = 0;
	Component.Finalize.bPending = false;
	Component.Finalize.RefineProbes.Reset();

	Component.CycleAccum.RaysReached += RaysReached;
	Component.CycleAccum.LoSBounces += TotalLoSBounces;
	Component.CycleAccum.MinLoSDist = FMath::Min(Component.CycleAccum.MinLoSDist, MinLoSDist);
	Component.CycleAccum.WeightedPos += WeightedPosSum;
	Component.CycleAccum.TotalWeight += TotalWeight;
	Component.CycleAccum.WeightedDist += WeightedDistSum;
	Component.CycleAccum.bDirectLoSFound = Component.CycleAccum.bDirectLoSFound || bDirectLoSFound;

	Component.SuccessfulEdgeDirHints = BuildEdgeDirHints(Component.StoredLoSPaths, Component.AsyncSourcePos);

	if (Settings.bCacheEdgePoints && Component.StoredLoSPaths.Num() > 0) {
		const float MergeRadiusSq = FMath::Square(Settings.CachedEdgeMergeRadius);
		TArray<bool> bCycleMatched;
		bCycleMatched.Init(false, Component.CachedEdgePoints.Num());

		// Replacement rank mirrors the cluster priority: source-path falloff × listener-proximity
		// falloff (listener term inert while ListenerDistanceFalloff is 0). Bounce count stays
		// the primary key; the 1% hysteresis on the score prevents churn between near-equal
		// entries. An entry is only displaced by a candidate that ranks better — never just
		// because a recast from the new positions happened not to re-find it.
		const float MaxRay = FMath::Max(Component.MaxRayDistance, 1.f);
		auto RankScore = [&](const float PathDist, const FVector& Pos) -> float
		{
			return (1.f / (1.f + Settings.CandidateDistanceFalloff * PathDist / MaxRay))
				/ (1.f + Settings.ListenerDistanceFalloff
					* FVector::Dist(Component.AsyncListenerPos, Pos) / MaxRay);
		};
		auto IsBetter = [&](const FStoredLoSPath& SP, const FCachedEdgePoint& EP) -> bool
		{
			return SP.LoSBounces < EP.LoSBounces ||
				(SP.LoSBounces == EP.LoSBounces
					&& RankScore(SP.PathDist, SP.LoSOrigin)
						> RankScore(EP.EffectivePathDist(), EP.EffectivePoint()) * 1.01f);
		};
		auto WriteEntry = [&](FCachedEdgePoint& EP, const FStoredLoSPath& SP)
		{
			EP.EdgePoint = SP.LoSOrigin;
			EP.GeomDist = SP.LoSCumulativeDistance;
			EP.PathDist = SP.PathDist;
			EP.ShortestPath = SP.ShortestPath;
			EP.ShortestPathVerifiedFrom = SP.ShortestPathVerifiedFrom;
			EP.LoSBounces = SP.LoSBounces;
			EP.CapturedSourcePos = Component.AsyncSourcePos;
			EP.CapturedListenerPos = Component.AsyncListenerPos;
			EP.bPhase0Pending = false;
			EP.bEvicting = false;
			EP.bSourceSideEviction = false;
			EP.EvictionAlpha = 1.f;
			EP.PathCheckFailStreak = 0;
			// A sweep re-confirming the edge means a fresh listener-visible path exists — any
			// relay detour is obsolete.
			EP.ClearRelay();
			EP.LastLoSListenerPos = Component.AsyncListenerPos;
			EP.bHasLastLoSListenerPos = true;
		};

		bool bWorstReplacedThisCycle = false;

		for (const FStoredLoSPath& SP : Component.StoredLoSPaths) {
			int32 BestIdx = -1;
			float BestDist = MergeRadiusSq;
			for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
				const float DSq = FVector::DistSquared(Component.CachedEdgePoints[i].EdgePoint, SP.LoSOrigin);
				if (DSq < BestDist) {
					BestDist = DSq;
					BestIdx = i;
				}
			}

			if (BestIdx >= 0) {
				FCachedEdgePoint& EP = Component.CachedEdgePoints[BestIdx];
				if (IsBetter(SP, EP)) {
					WriteEntry(EP, SP);
				}
				else {
					EP.CapturedSourcePos = Component.AsyncSourcePos;
					EP.CapturedListenerPos = Component.AsyncListenerPos;
				}
				bCycleMatched[BestIdx] = true;
			}
			else {
				// No admission clustering here: the cache deliberately holds points from ALL
				// openings — per-frame voice clustering (Math::ClusterEdgePoints) is what groups
				// them into audible emitters and drops insignificant groups.
				if (Component.CachedEdgePoints.Num() < Settings.CachedEdgeMaxCount) {
					FCachedEdgePoint NewEP;
					WriteEntry(NewEP, SP);
					NewEP.bNewSinceFillArm = true;
					Component.CachedEdgePoints.Add(MoveTemp(NewEP));
					bCycleMatched.Add(true);
				}
				else if (!bWorstReplacedThisCycle) {
					int32 WorstIdx = -1;
					for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
						if (bCycleMatched[i]) {
							continue;
						}
						const FCachedEdgePoint& EP = Component.CachedEdgePoints[i];
						if (WorstIdx < 0) {
							WorstIdx = i;
							continue;
						}
						const FCachedEdgePoint& Worst = Component.CachedEdgePoints[WorstIdx];
						if (EP.LoSBounces > Worst.LoSBounces ||
							(EP.LoSBounces == Worst.LoSBounces
								&& RankScore(EP.EffectivePathDist(), EP.EffectivePoint())
									< RankScore(Worst.EffectivePathDist(), Worst.EffectivePoint()))) {
							WorstIdx = i;
						}
					}
					if (WorstIdx >= 0 && IsBetter(SP, Component.CachedEdgePoints[WorstIdx])) {
						WriteEntry(Component.CachedEdgePoints[WorstIdx], SP);
						// Displacement lands at a different location — a discovery, unlike the
						// merge-matched re-confirmation above, which keeps the entry's flag as-is.
						Component.CachedEdgePoints[WorstIdx].bNewSinceFillArm = true;
						bCycleMatched[WorstIdx] = true;
						bWorstReplacedThisCycle = true;
					}
				}
			}
		}
		Component.StoredLoSPaths.Reset();
	}

	const int32 CycleCount = FMath::Max(1, Settings.FullSweepCycleCount);
	Component.StaggeredCycleIndex = (Component.StaggeredCycleIndex + 1) % CycleCount;
	Component.CycleAccum.Index = 0;

	// Only arm idle mode when the sub-cycle that just finished wrapped the sequence — after
	// movement breaks idle, all remaining cycles must complete at normal pace first, otherwise
	// the rest of the sphere coverage would crawl at StationaryIdleMultiplier speed.
	const bool bSweepSequenceComplete = Component.StaggeredCycleIndex == 0;

	// Spend a unit of the post-movement cache-fill budget only when the completed sweep still
	// left the new-edge target short — a sweep that filled it is free, so the remaining budget
	// stays available if the new edges are evicted moments later.
	if (bSweepSequenceComplete && Component.SweepScheduling.CacheFillSweepsRemaining > 0
		&& Component.CountCacheFillEdges() < Settings.MovementCacheFillRequiredEdges) {
		--Component.SweepScheduling.CacheFillSweepsRemaining;
	}
	if (bSweepSequenceComplete && Settings.bCacheEdgePoints && !Settings.IsRateThrottlingDisabled()
		&& Component.VelocityScaling.SweepMultiplier > 0.95f && Component.VelocityScaling.EdgeMultiplier > 0.95f) {
		Component.SweepScheduling.bStationaryIdleMode = true;
		Component.SweepScheduling.StationaryIdleSourcePos = Component.AsyncSourcePos;
		Component.SweepScheduling.StationaryIdleListenerPos = Component.AsyncListenerPos;
	}

	RaysReached = Component.CycleAccum.RaysReached;
	TotalLoSBounces = Component.CycleAccum.LoSBounces;
	MinLoSDist = Component.CycleAccum.MinLoSDist;
	WeightedPosSum = Component.CycleAccum.WeightedPos;
	TotalWeight = Component.CycleAccum.TotalWeight;
	WeightedDistSum = Component.CycleAccum.WeightedDist;
	bDirectLoSFound = Component.CycleAccum.bDirectLoSFound;

	const int32 NumRays = Component.AsyncTotalRays;

	{
		FRayAccumulatorInput AccumIn;
		AccumIn.RaysReached = RaysReached;
		AccumIn.MinLoSDist = MinLoSDist;
		AccumIn.WeightedPos = WeightedPosSum;
		AccumIn.TotalWeight = TotalWeight;
		AccumIn.DirectDist = FVector::Dist(Component.AsyncSourcePos, Component.AsyncListenerPos);
		AccumIn.MaxRayDistance = Component.MaxRayDistance;
		AccumIn.bDirectLoSFound = bDirectLoSFound;
		const FRayAccumulatorOutput AccumOut = ComputeAudioFromRayAccumulator(AccumIn, Settings);

		Component.AudioDiag.FullExcessRatio = AccumIn.DirectDist > 0.f
			? FMath::Max(0.f, AccumOut.MinLoSDist / AccumIn.DirectDist - 1.f) : 0.f;
		// TargetOcclusion is deliberately not written here: occlusion is owned by the per-frame
		// offset-LoS sampler (TickDirectLoSSampling). The sweep's path-ratio occlusion is lower
		// than the fraction-derived value whenever a good diffraction path exists, so writing it
		// per completed cycle kept knocking the target off 100% while fully occluded.
		Component.TargetPathAttenuation = AccumOut.PathAttenuation;

		if (AccumOut.bHasVirtualSource) {
			const float AvgSourceToEdgeDist = WeightedDistSum / TotalWeight;
			Component.CurrentSourceToVirtualDistance = FMath::FInterpTo(
				Component.CurrentSourceToVirtualDistance, AvgSourceToEdgeDist,
				Component.GetWorld()->GetDeltaSeconds(), 10.0f);
			Component.TargetVirtualSourceLocation = AccumOut.VirtualSourcePos;
			Component.LastKnownEdgePoint = AccumOut.VirtualSourcePos;
			Component.bHasKnownEdge = true;
		}
	}

	if (TotalWeight > 0.f && Component.bDrawDebugRays) {
		TArray<FVector>& Path = Component.LoSDiffractionPaths.AddDefaulted_GetRef();
		Path.Add(Component.AsyncSourcePos);
		Path.Add(Component.TargetVirtualSourceLocation);
		Path.Add(Component.AsyncListenerPos);
	}

	Component.LastRaysReached = RaysReached;
	Component.LastAvgLoSBounces = RaysReached > 0
		                               ? static_cast<float>(TotalLoSBounces) / RaysReached
		                               : 0.f;

	// Pre-warm casts must not stomp bHasDirectLoS: partial LoS legitimately persists and the
	// per-frame sampler owns the flag (bDirectLoSFound is masked false for pre-sweeps anyway).
	if (!Component.bPreSweepCast) {
		Component.bHasDirectLoS = bDirectLoSFound;
	}
	if (bDirectLoSFound) {
		Component.TargetOcclusion = 0.f;
		Component.CachedEdgePoints.Empty();
		Component.CachedEdgeDirs.Empty();
	}
}

FAsyncCastManager::FRayAccumulatorOutput FAsyncCastManager::ComputeAudioFromRayAccumulator(
	const FRayAccumulatorInput& In,
	const USpatialAudioSettings& Settings) {
	FRayAccumulatorOutput Out;

	const float AvgLoSDist = In.RaysReached > 0 ? In.MinLoSDist : In.MaxRayDistance;
	Out.MinLoSDist = AvgLoSDist;

	Out.OcclusionValue = (In.bDirectLoSFound)
		                     ? Math::ComputeOcclusionFromPathRatio(AvgLoSDist, In.DirectDist, Settings)
		                     : 1.f;

	Out.PathAttenuation = Math::ComputePathAttenuation(AvgLoSDist, In.MaxRayDistance, Settings);

	if (!In.bDirectLoSFound && In.TotalWeight > 0.f) {
		Out.VirtualSourcePos = In.WeightedPos / In.TotalWeight;
		Out.bHasVirtualSource = true;
	}

	return Out;
}

FAsyncCastManager::FCachedPointAccum FAsyncCastManager::AccumulateCachedPoints(
	const TArray<FCachedEdgePoint>& Points,
	const FVector& ListenerPos,
	const USpatialAudioSettings& Settings) {
	FCachedPointAccum Out;
	for (const FCachedEdgePoint& EP : Points) {
		++Out.RaysReached;
		Out.MinLoSDist = FMath::Min(Out.MinLoSDist, EP.EffectivePathDist());
		const float DistW = EP.EvictionAlpha / (1.f + Settings.CandidateDistanceFalloff
			* EP.GeomDist / FMath::Max(Settings.MaxRayDistance, 1.f));
		Out.WeightedPos += EP.EffectivePoint() * DistW;
		Out.WeightedDist += EP.EffectivePathDist() * DistW;
		Out.TotalWeight += DistW;
	}
	return Out;
}


void FAsyncCastManager::UpdateMissDirState(
	const FSpatialRayState& Ray,
	const FVector& SourcePos,
	const FVector& ListenerPos,
	const TArray<FVector>& CachedEdgeDirs,
	TArray<FCachedMissDir>& InOutMissDirs,
	bool& bGeometryChangeDetected,
	const USpatialAudioSettings& Settings) {
	if (Ray.bWasMissDir && Ray.bLoSFound) {
		bGeometryChangeDetected = true;
		if (Settings.CachedMissExclusionAngleDeg > 0.f) {
			const float MissMinDot = FMath::Cos(
				FMath::DegreesToRadians(Settings.CachedMissExclusionAngleDeg));
			for (int32 k = InOutMissDirs.Num() - 1; k >= 0; --k) {
				if (FVector::DotProduct(Ray.Dir, InOutMissDirs[k].Dir) >= MissMinDot) {
					InOutMissDirs.RemoveAt(k);
					break;
				}
			}
		}
	}

	if (!Ray.bLoSFound && !Ray.bWasMissDir
		&& Settings.bCacheEdgePoints
		&& Settings.CachedMissExclusionAngleDeg > 0.f
		&& !Settings.IsDirectionSkippingDisabled()
		&& InOutMissDirs.Num() < Settings.CachedMissDirMaxCount) {
		const float EdgeMinDot = Settings.CachedEdgeExclusionAngleDeg > 0.f
			                         ? FMath::Cos(FMath::DegreesToRadians(Settings.CachedEdgeExclusionAngleDeg))
			                         : 2.f;
		bool bEdgeCovered = false;
		for (const FVector& EDir : CachedEdgeDirs) {
			if (FVector::DotProduct(Ray.Dir, EDir) >= EdgeMinDot) {
				bEdgeCovered = true;
				break;
			}
		}
		if (!bEdgeCovered) {
			const float MissMinDot = FMath::Cos(
				FMath::DegreesToRadians(Settings.CachedMissExclusionAngleDeg));
			bool bDuplicate = false;
			for (const FCachedMissDir& MD : InOutMissDirs) {
				if (FVector::DotProduct(Ray.Dir, MD.Dir) >= MissMinDot) {
					bDuplicate = true;
					break;
				}
			}
			if (!bDuplicate) {
				FCachedMissDir NewMD;
				NewMD.Dir = Ray.Dir;
				NewMD.CapturedSourcePos = SourcePos;
				NewMD.CapturedListenerPos = ListenerPos;
				InOutMissDirs.Add(MoveTemp(NewMD));
			}
		}
	}
}

