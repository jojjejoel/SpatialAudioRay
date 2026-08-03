#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioSettings.h"
#include "SpatialAudioTypes.h"

namespace Math {

	inline FVector ReflectDirection(const FVector& Dir, const FVector& Normal) {
		return Dir - 2.f * FVector::DotProduct(Dir, Normal) * Normal;
	}

	// Reflects, then blends toward a random hemisphere sample by SurfaceRoughness. With bApplyBias,
	// rejection-samples up to 20 candidates toward the listener: the bias lives in the acceptance
	// probability rather than a lerp, so roughness keeps controlling spread. Shared by the async
	// bounce and crawl-bounce-out paths, which must scatter identically.
	inline FVector ComputeBouncedDirection(const FVector& InDir, const FVector& SurfaceNormal,
	                                       bool bApplyBias, const FVector& HitLocation,
	                                       const FVector& ListenerPos, float SurfaceRoughness,
	                                       float BounceListenerBias) {
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

		if (BounceListenerBias > 0.f) {
			const FVector ToListener = (ListenerPos - HitLocation).GetSafeNormal();
			Result = FMath::Lerp(Result, ToListener, BounceListenerBias).GetSafeNormal();
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

	// Every LoS probe is gated on this sum, and it only grows: moving distance D closes the gap by
	// at most D, so once past the budget it can never dip back under. A ray failing here can never
	// produce a passing probe, so flying it on is pure trace waste.
	inline bool IsWithinPathBudget(float CumulativeDistance, const FVector& Point,
	                               const FVector& ListenerPos, float Budget) {
		return CumulativeDistance + FVector::Dist(Point, ListenerPos) <= Budget;
	}

	// The ray's own reach, capped by remaining budget and then by MaxStraightFlightDistance (0 = off).
	// Callers with no budget to spend pass an unbounded RemainingBudget.
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

	/** A zero-length falloff would cut straight from full volume to silence at the inner radius. */
	constexpr float MinFalloffScale = 0.05f;

	// Two clamps, both load-bearing: InnerRadius is a hard floor on reach, since the engine plays at
	// full volume inside it; and the scale never exceeds 1 because the ray and LoS ranges are captured
	// at base scale, so a sound must never be audible past where the system searches for paths.
	inline float ComputeFalloffScaleForOuterRadius(float TargetOuterCm, float InnerRadius,
	                                               float BaseFalloffDistance) {
		if (TargetOuterCm <= 0.f || BaseFalloffDistance <= 0.f) {
			return 1.f;
		}
		return FMath::Clamp((TargetOuterCm - InnerRadius) / BaseFalloffDistance, MinFalloffScale, 1.f);
	}

	// The straight line while clear, the diffraction path while occluded, blended by smoothed
	// occlusion so partial LoS interpolates instead of switching. Selection input only, never gain
	// math. Floored at DirectDist: the two legs smooth independently and can momentarily sum below
	// the straight line. OcclusionFloor holds the result at the straight line until the listener is
	// genuinely hidden, since the pre-sweep band caches routes around corners they can still see past.
	inline float ComputeEffectiveAcousticDistance(float DirectDist, float PathDist, float Occlusion,
	                                              float OcclusionFloor = 0.f) {
		const float Floor = FMath::Clamp(OcclusionFloor, 0.f, 0.99f);
		const float Alpha = FMath::Clamp((Occlusion - Floor) / (1.f - Floor), 0.f, 1.f);
		return FMath::Lerp(DirectDist, FMath::Max(PathDist, DirectDist), Alpha);
	}

	// Two weights per point. The source-side weight drives the PathDist average and the per-voice
	// gain share, and must never depend on listener position. The rank weight adds listener-distance
	// decay and drives only centroids and which clusters win voice slots: proximity may steer WHERE
	// emitters sit, never how loud they are. Grouping stays keyed on the edge points themselves;
	// EmitterPullback moves only the output centroid, since pulled-back points converge toward the
	// source and would merge voices at clearly distinct openings.
	inline void ClusterEdgePoints(const TArray<FCachedEdgePoint>& Points, float ClusterRadius,
	                              float CandidateDistanceFalloff, const FVector& ListenerPos,
	                              float ListenerDistanceFalloff, float MaxRayDistance,
	                              float EmitterPullback,
	                              int32 MaxClusters, TArray<FEdgeCluster>& OutClusters) {
		OutClusters.Reset();
		if (MaxClusters <= 0 || ClusterRadius <= 0.f) {
			return;
		}

		struct FAccum {
			FVector PosSum = FVector::ZeroVector;
			FVector EmitterPosSum = FVector::ZeroVector;
			float PathSum = 0.f;
			float SrcWeight = 0.f;
			float RankWeight = 0.f;
			FVector Centroid = FVector::ZeroVector;
		};
		TArray<FAccum> Accums;

		for (const FCachedEdgePoint& Ep : Points) {
			const FVector EpPos = Ep.EffectivePoint();
			const float SrcW = Ep.EvictionAlpha / (1.f + CandidateDistanceFalloff
				* Ep.GeomDist / FMath::Max(MaxRayDistance, 1.f));
			const float RankW = SrcW / (1.f + ListenerDistanceFalloff
				* FVector::Dist(ListenerPos, EpPos) / FMath::Max(MaxRayDistance, 1.f));
			if (RankW <= KINDA_SMALL_NUMBER) {
				continue;
			}

			int32 Best = INDEX_NONE;
			float BestDistSq = FMath::Square(ClusterRadius);
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
			A.EmitterPosSum += Ep.EmitterPoint(EmitterPullback) * RankW;
			A.PathSum += Ep.EffectivePathDist() * SrcW;
			A.SrcWeight += SrcW;
			A.RankWeight += RankW;
			A.Centroid = A.PosSum / A.RankWeight;
		}

		// Centroids drift as points join, so greedy assignment alone can leave two within the radius.
		bool bMerged = true;
		while (bMerged) {
			bMerged = false;
			for (int32 i = 0; i < Accums.Num() && !bMerged; ++i) {
				for (int32 j = i + 1; j < Accums.Num() && !bMerged; ++j) {
					if (FVector::DistSquared(Accums[i].Centroid, Accums[j].Centroid)
						<= FMath::Square(ClusterRadius)) {
						Accums[i].PosSum += Accums[j].PosSum;
						Accums[i].EmitterPosSum += Accums[j].EmitterPosSum;
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
			Cluster.Centroid = Accums[i].EmitterPosSum / Accums[i].RankWeight;
			Cluster.PathDist = Accums[i].PathSum / Accums[i].SrcWeight;
			Cluster.TotalWeight = Accums[i].SrcWeight;
			OutClusters.Add(Cluster);
		}
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

	inline FVirtualAudioParams ComputeVirtualAudioParams(
		float VirtualCrossfade,
		float PathAttenuation,
		float Leg1Geom,
		float Leg1Traveled,
		float MaxRayDistance,
		const USpatialAudioSettings& Settings)
	{
		FVirtualAudioParams Out;
		// Purely the gate times how much the travelled path attenuates: no distance curve here. The
		// Virtual cue's own SoundAttenuation asset is the sole control of loudness by proximity.
		Out.VirtualGain = VirtualCrossfade * (1.f - PathAttenuation);

		// How much longer the travelled path is than a straight line to the same point, normalized so
		// bend reaches 1 at VirtualPathBendFullExcess, plus a travelled-distance term so far edges
		// sound duller even on straight single-corner paths. One combined parameter: the MetaSound
		// derives both HPF cutoff and reverb wetness from it. Listener-independent.
		const float Leg1ExcessRatio = Leg1Geom > 0.f
			? FMath::Max(0.f, Leg1Traveled / Leg1Geom - 1.f) : 0.f;
		const float FullExcess = FMath::Max(Settings.VirtualPathBendFullExcess, KINDA_SMALL_NUMBER);
		const float DistanceBend = Settings.VirtualPathBendDistanceStrength
			* Leg1Traveled / FMath::Max(MaxRayDistance, 1.f);
		Out.VirtualPathBend = FMath::Clamp(Leg1ExcessRatio / FullExcess + DistanceBend, 0.f, 1.f);

		return Out;
	}

	// Maps smoothed occlusion in [StartOcclusion, 1] onto a [0, 1] gate, so the diffracted sound
	// bleeds in through the pre-sweep band while the voices are already positioned. StartOcclusion
	// >= 1 disables it. Amplifies input wobble by 1/(1-Start), so the caller low-passes the result.
	inline float ComputeVirtualCrossfadeRamp(float CurrentOcclusion, float StartOcclusion)
	{
		if (StartOcclusion >= 1.f) {
			return 0.f;
		}
		return FMath::Clamp((CurrentOcclusion - StartOcclusion) / (1.f - StartOcclusion), 0.f, 1.f);
	}

	// LoS loss (a completed blank ring cycle, not a single sample) forces a hard 1, since occlusion
	// smoothing lags the break. bSuppressHardGate drops that term for a stationary scene with the
	// ramp on, where a marginal pinhole can blank a full rotation and pump the gate. A genuine
	// stationary loss still reaches full gate as smoothed occlusion rises and the ramp follows.
	inline float ComputeVirtualCrossfadeTarget(bool bHasLoS, bool bSuppressHardGate, float SmoothedRamp)
	{
		const float HardGate = (bHasLoS || bSuppressHardGate) ? 0.f : 1.f;
		return FMath::Max(HardGate, SmoothedRamp);
	}

	// Slews rather than snapping, so VirtualGain ramps over FadeInTime/FadeOutTime.
	inline float ComputeVirtualCrossfadeSlew(
		float CurrentCrossfade, float TargetCrossfade, float FadeInTime, float FadeOutTime, float DeltaTime)
	{
		const bool bFadingIn = TargetCrossfade > CurrentCrossfade;
		const float FadeTime = bFadingIn ? FadeInTime : FadeOutTime;
		const float SlewRate = FadeTime > 0.f ? 1.f / FadeTime : 1000.f;
		return FMath::FInterpConstantTo(CurrentCrossfade, TargetCrossfade, DeltaTime, SlewRate);
	}
}
