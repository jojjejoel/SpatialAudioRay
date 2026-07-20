#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioSettings.h"
#include "SpatialAudioTypes.h"

namespace Math {

	inline FVector ReflectDirection(const FVector& Dir, const FVector& Normal) {
		return Dir - 2.f * FVector::DotProduct(Dir, Normal) * Normal;
	}

	inline float ComputeConeCosine(float Distance, float Radius) {
		return Distance / FMath::Sqrt(Distance * Distance + Radius * Radius);
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

	inline float ComputeOcclusionFromPathRatio(float AvgPathDist, float DirectDist,
	                                           const USpatialAudioSettings& S) {
		const float PathExcessRatio = DirectDist > 0.f
			                              ? FMath::Max(0.f, AvgPathDist / DirectDist - 1.f)
			                              : 0.f;
		return FMath::Clamp(PathExcessRatio / FMath::Max(S.OcclusionExcessPathScale, 0.001f), 0.f, 1.f);
	}
	
	// Leg1Geom = straight-line source→virtual-position distance, same basis VirtualPathBend
	// already blends against (see ComputeVirtualAudioParams) — PathAttenuationGeomBlend applies
	// the same idea to gain. Blending the distance itself (rather than two separately-computed
	// attenuation values) keeps this a single pass through the existing formula below.
	inline float ComputePathAttenuation(float AvgPathDist, float Leg1Geom,
	                                    float MaxRayDistance, const USpatialAudioSettings& S) {
		const float BlendedDist = FMath::Lerp(AvgPathDist, Leg1Geom, S.PathAttenuationGeomBlend);
		return FMath::Clamp(BlendedDist / FMath::Max(MaxRayDistance, 1.f) * S.PathAttenuationStrength, 0.f, 1.f);
	}

	// Two weights per point. The source-side weight (eviction confidence + geometric falloff
	// from the source) drives the PathDist average and TotalWeight (→ per-voice gain share) and
	// must never depend on listener position. The rank weight additionally decays with
	// listener→edge distance and drives only centroids and which clusters win the voice slots —
	// listener proximity may steer WHERE emitters sit and WHICH edges are chosen, never how
	// loud/muffled a voice is (that's the engine attenuation's job on the moved emitter).
	// Grouping/merging stays keyed on the edge points themselves; EmitterPullback only moves the
	// OUTPUT centroid (per-edge EmitterPoint average) — pulled-back points converge toward the
	// source, and clustering on them would merge voices that sit at clearly distinct openings.
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

		// Centroids drift as points join, so greedy assignment alone can leave two clusters
		// closer than the radius. Merge those so near-co-located voices never coexist.
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
		// Gain is purely the crossfade gate/slew times how much the traveled diffraction path
		// attenuates the signal — no source- or listener-distance curve here. The Virtual cue's
		// own SoundAttenuation asset (evaluated natively by the engine against its actual world
		// position) is the sole thing controlling loudness based on proximity to the listener.
		Out.VirtualGain = VirtualCrossfade * (1.f - PathAttenuation);

		// How much longer the traveled (bent/crawled) path to the virtual position is than a
		// straight line to that same point, normalized so bend reaches 1 at
		// VirtualPathBendFullExcess — plus a traveled-distance term (air-absorption analog,
		// same Leg1 basis as PathAttenuation) so far edges sound duller even on straight
		// single-corner paths. Sent as a single combined parameter — the MetaSound graph
		// derives both HPF cutoff and reverb wetness from it internally, since both are
		// fundamentally the same "how muffled is this path" signal, just shaped differently.
		// Independent of listener position — moving the listener without changing the
		// virtual position leaves this untouched.
		const float Leg1ExcessRatio = Leg1Geom > 0.f
			? FMath::Max(0.f, Leg1Traveled / Leg1Geom - 1.f) : 0.f;
		const float FullExcess = FMath::Max(Settings.VirtualPathBendFullExcess, KINDA_SMALL_NUMBER);
		const float DistanceBend = Settings.VirtualPathBendDistanceStrength
			* Leg1Traveled / FMath::Max(MaxRayDistance, 1.f);
		Out.VirtualPathBend = FMath::Clamp(Leg1ExcessRatio / FullExcess + DistanceBend, 0.f, 1.f);

		return Out;
	}

	// The occlusion-keyed fade-in ramp: maps smoothed occlusion in [StartOcclusion, 1] onto a
	// [0, 1] gate level, so the diffracted sound can bleed in through the pinhole/pre-sweep
	// band while the source still has partial LoS and the voices are already positioned.
	// StartOcclusion >= 1 disables the ramp (always 0). Note the mapping amplifies any wobble
	// in the occlusion input by 1/(1-StartOcclusion) — the caller low-passes the result before
	// the gate target sees it.
	inline float ComputeVirtualCrossfadeRamp(float CurrentOcclusion, float StartOcclusion)
	{
		if (StartOcclusion >= 1.f) {
			return 0.f;
		}
		return FMath::Clamp((CurrentOcclusion - StartOcclusion) / (1.f - StartOcclusion), 0.f, 1.f);
	}

	// The crossfade gate's target level. LoS loss (as judged by the caller — a completed blank
	// ring cycle, not a single sample) forces a hard 1: occlusion smoothing lags the break, and
	// waiting for it would delay the virtual's entrance. bSuppressHardGate (stationary scene
	// with the ramp enabled) drops that term: a marginal pinhole can blank a full sampling
	// rotation for a few frames, and the hard term would pump the gate between the ramp level
	// and full on every episode. A genuine stationary loss still reaches full gate — smoothed
	// occlusion rises to 1 and the ramp follows, coherent with the source's muffling deepening
	// on the same curve. Movement is genuine change and keeps the hard term.
	inline float ComputeVirtualCrossfadeTarget(bool bHasLoS, bool bSuppressHardGate, float SmoothedRamp)
	{
		const float HardGate = (bHasLoS || bSuppressHardGate) ? 0.f : 1.f;
		return FMath::Max(HardGate, SmoothedRamp);
	}

	// Slews the virtual crossfade gate toward its target at a fixed rate rather than snapping
	// instantly, so VirtualGain ramps over FadeInTime/FadeOutTime seconds instead of jumping
	// in a single frame when the target moves.
	inline float ComputeVirtualCrossfadeSlew(
		float CurrentCrossfade, float TargetCrossfade, float FadeInTime, float FadeOutTime, float DeltaTime)
	{
		const bool bFadingIn = TargetCrossfade > CurrentCrossfade;
		const float FadeTime = bFadingIn ? FadeInTime : FadeOutTime;
		const float SlewRate = FadeTime > 0.f ? 1.f / FadeTime : 1000.f;
		return FMath::FInterpConstantTo(CurrentCrossfade, TargetCrossfade, DeltaTime, SlewRate);
	}
}
