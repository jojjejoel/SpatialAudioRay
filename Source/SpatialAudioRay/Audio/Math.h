#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioSettings.h"
#include "SpatialAudioTypes.h"

namespace Math {

	inline FVector ReflectDirection(const FVector& Dir, const FVector& Normal) {
		return Dir - 2.f * FVector::DotProduct(Dir, Normal) * Normal;
	}

	// Reflects InDir off SurfaceNormal, blends toward a random hemisphere sample by SurfaceRoughness
	// (0 = mirror, 1 = fully diffuse), and — when bApplyBias is set — rejection-samples up to 20
	// diffuse candidates toward whichever one leans most toward the listener (bias strength implicit
	// in the acceptance probability, not a direct lerp, so roughness keeps controlling the spread).
	// BounceListenerBias then pulls the result the rest of the way toward the listener directly,
	// re-projecting above the surface if that pull would send it through. Shared by the async
	// bounce/crawl-bounce-out paths and the sync ProcessRayHit perp-wall/fallback bounces — all
	// four must scatter identically.
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

	// Every LoS probe is gated on this same sum, and it only grows as a ray travels further:
	// moving a distance D can close the gap to the listener by at most D, so the total can never
	// dip back under the budget once it has passed it. That makes the bound a lossless prune —
	// a ray that fails here can never produce a passing probe again, so flying it on would burn
	// traces for no possible payoff.
	inline bool IsWithinPathBudget(float CumulativeDistance, const FVector& Point,
	                               const FVector& ListenerPos, float Budget) {
		return CumulativeDistance + FVector::Dist(Point, ListenerPos) <= Budget;
	}

	// Length of the next straight flight segment: the ray's own reach, capped by what is left of
	// the total path budget, then by MaxStraightFlightDistance when that turn-forcing cap is on
	// (0 = off). Callers with no budget to spend pass an unbounded RemainingBudget.
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

	/** Floor on the attenuation falloff scale — a zero-length falloff would cut straight from
	 *  full volume to silence at the inner radius. */
	constexpr float MinFalloffScale = 0.05f;

	// Scale to apply to the attenuation FalloffDistance so the audible edge lands at
	// TargetOuterCm. Two clamps, both load-bearing: inside InnerRadius the engine plays at full
	// volume, so that radius is a hard floor on reach (a target at or below it yields the
	// shortest legal falloff, not silence); and the scale never exceeds 1 because the ray/LoS
	// ranges are captured at base scale, so a sound must never be audible past where the ray
	// system searches for diffraction paths. TargetOuterCm <= 0 means "the asset's own range".
	inline float ComputeFalloffScaleForOuterRadius(float TargetOuterCm, float InnerRadius,
	                                               float BaseFalloffDistance) {
		if (TargetOuterCm <= 0.f || BaseFalloffDistance <= 0.f) {
			return 1.f;
		}
		return FMath::Clamp((TargetOuterCm - InnerRadius) / BaseFalloffDistance, MinFalloffScale, 1.f);
	}

	// How far the sound effectively travels to reach the listener: the straight line while
	// clear, the diffraction path while occluded, blended by the smoothed occlusion so
	// partial-LoS states interpolate instead of switching at a threshold. A selection/content
	// input (NPC vocal effort), never gain math. PathDist is floored at DirectDist — the two
	// legs are independently smoothed and can momentarily sum below the straight line, which
	// no physical path can be shorter than.
	// OcclusionFloor delays the detour: below it the result is the straight line exactly, above
	// it the diffraction route phases in across the remaining range, reaching PathDist at full
	// occlusion. Remapped rather than switched, so crossing the floor doesn't jump the result.
	//
	// The floor exists because edge caching starts finding routes well before the source is
	// actually hidden — the pre-sweep band deliberately pre-warms the cache while partial line
	// of sight remains. Blending those routes in from occlusion 0 charges a listener for a
	// detour the sound is not taking: most of what they hear is still arriving straight, and a
	// long way round to a corner they can see past inflates the distance for no audible reason.
	// A caller that consumes this to describe the listener's situation should pass the same
	// threshold it uses to call them hidden, so the two never disagree.
	inline float ComputeEffectiveAcousticDistance(float DirectDist, float PathDist, float Occlusion,
	                                              float OcclusionFloor = 0.f) {
		const float Floor = FMath::Clamp(OcclusionFloor, 0.f, 0.99f);
		const float Alpha = FMath::Clamp((Occlusion - Floor) / (1.f - Floor), 0.f, 1.f);
		return FMath::Lerp(DirectDist, FMath::Max(PathDist, DirectDist), Alpha);
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
