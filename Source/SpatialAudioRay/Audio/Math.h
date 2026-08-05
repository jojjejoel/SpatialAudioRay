#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioSettings.h"
#include "SpatialAudioTypes.h"

namespace Math {
	inline FVector ReflectDirection(const FVector& Dir, const FVector& Normal) {
		return Dir - 2.f * FVector::DotProduct(Dir, Normal) * Normal;
	}

	inline FVector ComputeBouncedDirection(const FVector& InDir, const FVector& SurfaceNormal,
	                                       bool bApplyBias, const FVector& HitLocation,
	                                       const FVector& ListenerPos, float SurfaceRoughness,
	                                       float ListenerBias) {
		const FVector Reflected = ReflectDirection(InDir, SurfaceNormal);
		FVector RandH = FMath::VRand();
		if (FVector::DotProduct(RandH, SurfaceNormal) < 0.f) {
			RandH = -RandH;
		}
		FVector Result = FMath::Lerp(Reflected, RandH, SurfaceRoughness).GetSafeNormal();

		if (bApplyBias) {
			const FVector HitToLis = ListenerPos - HitLocation;
			const float HitLisDist = HitToLis.Size();
			if (HitLisDist > 0.f) {
				const FVector HitToListenerDir = HitToLis / HitLisDist;
				for (int32 Attempt = 0; Attempt < 20; ++Attempt) {
					FVector RandH2 = FMath::VRand();
					if (FVector::DotProduct(RandH2, SurfaceNormal) < 0.f) {
						RandH2 = -RandH2;
					}
					const FVector Cand = FMath::Lerp(Reflected, RandH2, SurfaceRoughness).GetSafeNormal();
					if (FMath::FRand() < (1.f - FMath::Abs(FVector::DotProduct(Cand, HitToListenerDir)))) {
						Result = Cand;
						break;
					}
				}
			}
		}

		if (ListenerBias > 0.f) {
			const FVector ToListener = (ListenerPos - HitLocation).GetSafeNormal();
			Result = FMath::Lerp(Result, ToListener, ListenerBias).GetSafeNormal();
			if (FVector::DotProduct(Result, SurfaceNormal) < 0.f) {
				Result -= 2.f * FVector::DotProduct(Result, SurfaceNormal) * SurfaceNormal;
				Result.Normalize();
			}
		}

		return Result;
	}

	inline float ComputeConeCosine(float Distance, float Radius) {
		return Distance / FMath::Sqrt(Distance * Distance + Radius * Radius);
	}

	inline bool IsWithinPathBudget(float CumulativeDistance, const FVector& Point,
	                               const FVector& ListenerPos, float Budget) {
		return CumulativeDistance + FVector::Dist(Point, ListenerPos) <= Budget;
	}

	/** Ceiling on samples taken along one crawl or one flight segment. Crawl submits three traces
	 *  per step up front, so this bounds the largest single-frame burst in the sweep. */
	constexpr int32 MaxDiffractionSamples = 16;

	inline int32 ComputeDiffractionStepCount(float Distance, float StepSize) {
		return FMath::Clamp(FMath::FloorToInt(Distance / FMath::Max(StepSize, 1.f)),
		                    1, MaxDiffractionSamples);
	}

	inline float ComputeNextSegmentLength(float MaxRayDistance, float RemainingBudget,
	                                      float MaxStraightFlightDistance) {
		float Length = FMath::Min(MaxRayDistance, RemainingBudget);
		if (MaxStraightFlightDistance > 0.f) {
			Length = FMath::Min(Length, MaxStraightFlightDistance);
		}
		return Length;
	}

	inline TArray<FVector> GenerateFibonacciDirections(int32 NumRays, const FVector& PoleDir = FVector::UpVector) {
		TArray<FVector> Directions;
		if (NumRays <= 0) {
			return Directions;
		}
		Directions.Reserve(NumRays);

		const float GoldenAngle = PI * (3.f - FMath::Sqrt(5.f));
		const FQuat PoleRotation = FQuat::FindBetweenNormals(FVector::UpVector, PoleDir.GetSafeNormal());

		for (int32 i = 0; i < NumRays; ++i) {
			const float Y = 1.f - (i / FMath::Max(1.f, static_cast<float>(NumRays - 1))) * 2.f;
			const float Radius = FMath::Sqrt(FMath::Max(0.f, 1.f - Y * Y));
			const float Theta = GoldenAngle * i;
			const FVector LocalDir(FMath::Cos(Theta) * Radius, FMath::Sin(Theta) * Radius, Y);
			Directions.Emplace(PoleRotation.RotateVector(LocalDir));
		}

		return Directions;
	}

	inline float ComputeRayDirectionWeight(const FVector& Dir, const FVector& S2LDir,
	                                       float DirectLoSFraction, float DirectLoSSampleRadius,
	                                       float Distance) {
		const float Dot = FVector::DotProduct(Dir, S2LDir);
		const float LateralWeight = 1.f - FMath::Abs(Dot);

		float ForwardPenalty;
		if (DirectLoSSampleRadius > 0.f && Distance > 0.f) {
			const float ConeCos = ComputeConeCosine(Distance, DirectLoSSampleRadius);
			const float OneMinusCone = 1.f - ConeCos;
			const float NormForward = OneMinusCone > KINDA_SMALL_NUMBER
				                          ? FMath::Clamp((Dot - ConeCos) / OneMinusCone, 0.f, 1.f)
				                          : (Dot >= 1.f ? 1.f : 0.f);
			ForwardPenalty = NormForward * (1.f - DirectLoSFraction);
		}
		else {
			ForwardPenalty = FMath::Max(0.f, Dot) * (1.f - DirectLoSFraction);
		}

		return FMath::Max(0.f, LateralWeight - ForwardPenalty);
	}

	constexpr float MinFalloffScale = 0.05f;

	inline float ComputeFalloffScaleForOuterRadius(float TargetOuterCm, float InnerRadius,
	                                               float BaseFalloffDistance) {
		if (TargetOuterCm <= 0.f || BaseFalloffDistance <= 0.f) {
			return 1.f;
		}
		return FMath::Clamp((TargetOuterCm - InnerRadius) / BaseFalloffDistance, MinFalloffScale, 1.f);
	}

	inline float ComputeEffectiveAcousticDistance(float DirectDist, float PathDist, float Occlusion,
	                                              float OcclusionFloor = 0.f) {
		const float Floor = FMath::Clamp(OcclusionFloor, 0.f, 0.99f);
		const float Alpha = FMath::Clamp((Occlusion - Floor) / (1.f - Floor), 0.f, 1.f);
		return FMath::Lerp(DirectDist, FMath::Max(PathDist, DirectDist), Alpha);
	}

	constexpr int32 PointAwaitingCluster = -2;

	inline void ClusterEdgePoints(const TArray<FCachedEdgePoint>& Points, float ClusterRadius,
	                              float CandidateDistanceFalloff, const FVector& ListenerPos,
	                              float ListenerDistanceFalloff, float MaxRayDistance,
	                              int32 MaxClusters, TArray<FEdgeCluster>& OutClusters,
	                              TArray<int32>* OutPointToCluster = nullptr) {
		OutClusters.Reset();
		if (OutPointToCluster) {
			OutPointToCluster->Init(INDEX_NONE, Points.Num());
		}
		if (MaxClusters <= 0) {
			return;
		}
		const float GroupRadiusSq = FMath::Square(FMath::Max(ClusterRadius, 0.f));

		struct FAccum {
			FVector PosSum = FVector::ZeroVector;
			float PathSum = 0.f;
			float SrcWeight = 0.f;
			float RankWeight = 0.f;
			FVector Centroid = FVector::ZeroVector;
		};
		TArray<FAccum> Accums;

		for (int32 PointIdx = 0; PointIdx < Points.Num(); ++PointIdx) {
			const FCachedEdgePoint& Ep = Points[PointIdx];
			const FVector EpPos = Ep.EffectivePoint();
			const float SrcW = 1.f / (1.f + CandidateDistanceFalloff
				* Ep.GeomDist / FMath::Max(MaxRayDistance, 1.f));
			const float RankW = SrcW / (1.f + ListenerDistanceFalloff
				* FVector::Dist(ListenerPos, EpPos) / FMath::Max(MaxRayDistance, 1.f));
			if (RankW <= KINDA_SMALL_NUMBER) {
				continue;
			}
			if (OutPointToCluster) {
				(*OutPointToCluster)[PointIdx] = PointAwaitingCluster;
			}

			int32 Best = INDEX_NONE;
			float BestDistSq = GroupRadiusSq;
			for (int32 i = 0; i < Accums.Num(); ++i) {
				const float DistSq = FVector::DistSquared(Accums[i].Centroid, EpPos);
				if (DistSq <= BestDistSq) {
					BestDistSq = DistSq;
					Best = i;
				}
			}
			if (Best == INDEX_NONE) {
				Best = Accums.AddDefaulted();
			}
			FAccum& A = Accums[Best];
			A.PosSum += EpPos * RankW;
			A.PathSum += Ep.EffectivePathDist() * SrcW;
			A.SrcWeight += SrcW;
			A.RankWeight += RankW;
			A.Centroid = A.PosSum / A.RankWeight;
		}

		bool bMerged = true;
		while (bMerged) {
			bMerged = false;
			for (int32 i = 0; i < Accums.Num() && !bMerged; ++i) {
				for (int32 j = i + 1; j < Accums.Num() && !bMerged; ++j) {
					if (FVector::DistSquared(Accums[i].Centroid, Accums[j].Centroid) <= GroupRadiusSq) {
						Accums[i].PosSum += Accums[j].PosSum;
						Accums[i].PathSum += Accums[j].PathSum;
						Accums[i].SrcWeight += Accums[j].SrcWeight;
						Accums[i].RankWeight += Accums[j].RankWeight;
						Accums[i].Centroid = Accums[i].PosSum / Accums[i].RankWeight;
						Accums.RemoveAtSwap(j);
						bMerged = true;
					}
				}
			}
		}

		Accums.Sort([](const FAccum& A, const FAccum& B) { return A.RankWeight > B.RankWeight; });

		const int32 Count = FMath::Min(MaxClusters, Accums.Num());
		OutClusters.Reserve(Count);
		for (int32 i = 0; i < Count; ++i) {
			FEdgeCluster Cluster;
			Cluster.Centroid = Accums[i].Centroid;
			Cluster.PathDist = Accums[i].PathSum / Accums[i].SrcWeight;
			Cluster.TotalWeight = Accums[i].SrcWeight;
			OutClusters.Add(Cluster);
		}

		if (!OutPointToCluster) {
			return;
		}
		for (int32 PointIdx = 0; PointIdx < Points.Num(); ++PointIdx) {
			if ((*OutPointToCluster)[PointIdx] != PointAwaitingCluster) {
				continue;
			}
			const FVector EpPos = Points[PointIdx].EffectivePoint();
			int32 Best = INDEX_NONE;
			float BestDistSq = GroupRadiusSq;
			for (int32 i = 0; i < OutClusters.Num(); ++i) {
				const float DistSq = FVector::DistSquared(OutClusters[i].Centroid, EpPos);
				if (DistSq <= BestDistSq) {
					BestDistSq = DistSq;
					Best = i;
				}
			}
			(*OutPointToCluster)[PointIdx] = Best;
		}
	}

	/** Distance-scaled budget for a count: it follows priority down but never past the floor that
	 *  MaxDistanceEffortScale sets for a source at maximum range. */
	inline int32 ScaleCountByDistancePriority(int32 FullValue, float Priority, float MaxDistanceScale) {
		const int32 Floor = FMath::RoundToInt(FullValue * MaxDistanceScale);
		return FMath::Clamp(FMath::Max(FMath::RoundToInt(FullValue * Priority), Floor), 0, FullValue);
	}

	struct FSweepPacingState {
		float CurrentPriority = 1.f;
		float SweepMultiplier = 1.f;
		float EdgeMultiplier = 1.f;
		bool bStationary = false;
		bool bCacheFillPending = false;
		bool bStationaryIdleMode = false;
	};

	/** The three cases are exclusive rather than multiplied. A cache fill outstanding while stopped
	 *  wins first, so an unfilled burst cannot idle-crawl. Idle wins next and REPLACES velocity
	 *  scaling: they are opposite answers to the same question, and applying both lands back near
	 *  the base interval. */
	inline float ComputeSweepInterval(const FSweepPacingState& State, const USpatialAudioSettings& Settings) {
		const float EffortScale = FMath::Max(Settings.MaxDistanceEffortScale, KINDA_SMALL_NUMBER);
		const float BaseInterval = FMath::Lerp(
			Settings.FullSweepInterval / EffortScale, Settings.FullSweepInterval, State.CurrentPriority);

		if (State.bStationary && State.bCacheFillPending) {
			return BaseInterval * Settings.MinSweepIntervalScale;
		}
		if (State.bStationaryIdleMode) {
			return BaseInterval * Settings.StationaryIdleMultiplier;
		}
		return BaseInterval * FMath::Min(State.SweepMultiplier, State.EdgeMultiplier);
	}

	struct FSweepDispatchState {
		bool bAsyncCastActive = false;
		bool bFinalizePending = false;
		bool bInRange = false;
		bool bPreSweepBand = false;
		bool bConfirmedLoSLoss = false;
		bool bEarlySweepRequested = false;
		float TimeSinceFullCast = 0.f;
		float SweepInterval = 0.f;
	};

	/** The pre-sweep band is a second way in besides confirmed sight loss, and an early request is a
	 *  second way past the timer. Both were added later, and both are easy to break by tightening
	 *  the wrong clause. */
	inline bool ShouldDispatchSweep(const FSweepDispatchState& State) {
		if (State.bAsyncCastActive || State.bFinalizePending || !State.bInRange) {
			return false;
		}
		if (!State.bPreSweepBand && !State.bConfirmedLoSLoss) {
			return false;
		}
		return State.TimeSinceFullCast >= State.SweepInterval || State.bEarlySweepRequested;
	}

	inline bool HasAnyDirectLoS(const TArray<FSpatialRayState>& rays) {
		for (const FSpatialRayState& Ray : rays) {
			if (Ray.bLoSFound && Ray.LoSBounces == 0) {
				return true;
			}
		}
		return false;
	}

	struct FVirtualAudioParams {
		float VirtualGain;
		float VirtualPathBend;
	};

	inline float ComputeVirtualPathBend(float Leg1Traveled, float MaxRayDistance,
	                                    const USpatialAudioSettings& Settings) {
		const float DistanceBend = Settings.VirtualPathBendDistanceStrength
			* Leg1Traveled / FMath::Max(MaxRayDistance, 1.f);
		return FMath::Clamp(DistanceBend, 0.f, 1.f);
	}

	inline FVirtualAudioParams ComputeVirtualAudioParams(
		float VirtualCrossfade,
		float PathAttenuation,
		float Leg1Traveled,
		float MaxRayDistance,
		const USpatialAudioSettings& Settings) {
		FVirtualAudioParams Out;
		Out.VirtualGain = VirtualCrossfade * (1.f - PathAttenuation);
		Out.VirtualPathBend = ComputeVirtualPathBend(Leg1Traveled, MaxRayDistance, Settings);
		return Out;
	}

	inline float ComputeVirtualCrossfadeRamp(float CurrentOcclusion, float StartOcclusion) {
		if (StartOcclusion >= 1.f) {
			return 0.f;
		}
		return FMath::Clamp((CurrentOcclusion - StartOcclusion) / (1.f - StartOcclusion), 0.f, 1.f);
	}

	inline float ComputeVirtualCrossfadeTarget(bool bHasLoS, bool bSuppressHardGate, float SmoothedRamp) {
		const float HardGate = (bHasLoS || bSuppressHardGate) ? 0.f : 1.f;
		return FMath::Max(HardGate, SmoothedRamp);
	}

	inline float ComputeVirtualCrossfadeSlew(
		float CurrentCrossfade, float TargetCrossfade, float FadeTime, float DeltaTime) {
		const float SlewRate = FadeTime > 0.f ? 1.f / FadeTime : 1000.f;
		return FMath::FInterpConstantTo(CurrentCrossfade, TargetCrossfade, DeltaTime, SlewRate);
	}
}
