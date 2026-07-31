#pragma once

#include "CoreMinimal.h"
#include "Audio/AsyncCastManager.h"

class UAudioComponent;
class USpatialAudioComponent;
class USpatialAudioSettings;
class UWorld;
struct FEdgeCluster;
struct FVirtualVoice;
struct FVirtualSlot;

class FUpdater {
public:
	/** Runs every frame, including while a full cast is in flight and the update cast does not,
	 *  so occlusion keeps draining instead of stalling for the sweep duration. */
	static void TickDirectLoSSampling(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);
	static void PerformUpdateRayCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void UpdateAudioParameters(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);
	static void PerformLoSBreakSweep(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);

	// A blocked candidate is clamped to the hit location, nudged back toward the listener so the
	// next trace does not start inside geometry, rather than discarded.
	static FVector ResolveOffsetPoint(const USpatialAudioComponent& Component, const UWorld* World,
	                                  const FVector& ListenerPos, const FVector& CandidatePoint);

	/** Source-visibility fraction (0, 0.2 ... 1) over 5 samples: the listener centre plus a 4-point
	 *  ring of radius OffsetR, each paired with a point at lateral radius SourceRingR around the
	 *  source, lifted onto the SourceR sphere. Both ring radii <= 0 runs the centre trace only;
	 *  SourceR 0 makes it a point source. */
	static float SyncOffsetLoSFraction(USpatialAudioComponent& Component, UWorld* World,
	                                   const FVector& SourcePos, const FVector& ListenerPos,
	                                   float OffsetR, float SourceR, float SourceRingR,
	                                   float RingStepRad);

private:
	// Source and listener positions plus the world, resolved once per cast entry point.
	struct FCastContext {
		UWorld* World = nullptr;
		FVector SourcePos = FVector::ZeroVector;
		FVector ListenerPos = FVector::ZeroVector;
	};
	static bool TryResolveCastContext(const USpatialAudioComponent& Component, FCastContext& OutContext);
	static bool IsOutOfRange(const FCastContext& Context, float MaxRayDistance) {
		return FVector::DistSquared(Context.SourcePos, Context.ListenerPos) > FMath::Square(MaxRayDistance);
	}

	static void UpdateDualModeAudio(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings,
	                                float CurvedOcclusion);

	// ── UpdateDualModeAudio phases ────────────────────────────────────────────
	// Advances the smoothed crossfade ramp/gate and returns the current VirtualCrossfade value.
	static float UpdateVirtualCrossfadeGate(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);
	static void ApplySourceOcclusionParams(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings, float CurvedOcclusion);

	// PrimaryGain only selects which slot's bend the HUD shows; the audible gain is the sum.
	struct FVirtualVoiceUpdateResult {
		float TotalVirtualGain = 0.f;
		float PrimaryGain = -1.f;
		float PrimaryPathBend = 0.f;
	};
	struct FVoiceBlendRates {
		float FadeStep = 1.f;
		float ParamBlendSpeed = 1000.f;
		float MoveSpeed = 1000.f;
	};
	static FVoiceBlendRates ComputeVoiceBlendRates(const USpatialAudioSettings& Settings, float DeltaTime);
	static void TickFadingOutSlot(const USpatialAudioComponent& Component, FVirtualSlot& Slot, UAudioComponent* VC,
	                              float FadeStep, float VirtualCrossfade, FVirtualVoiceUpdateResult& OutResult);
	static void MoveSlotToVoice(FVirtualSlot& Slot, UAudioComponent* VC, const FVirtualVoice& Voice,
	                            const FVector& ActorPos, float DeltaTime, float MoveSpeed);
	static void ApplyVoiceAudioParams(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                  FVirtualSlot& Slot, FVirtualVoice& Voice, UAudioComponent* VC,
	                                  const FVector& ActorPos, float DeltaTime, float ParamBlendSpeed,
	                                  float VirtualCrossfade, FVirtualVoiceUpdateResult& OutResult);
	static FVirtualVoiceUpdateResult UpdateVirtualVoiceSlots(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                                         float DeltaTime, float VirtualCrossfade, const FVector& ActorPos);

	// ── TickDirectLoSSampling phases ─────────────────────────────────────────
	static void TrySampleOffsetLoS(USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings,
	                               float DeltaTime, const FVector& SourcePos, const FVector& ListenerPos,
	                               int32 RotationSteps);
	static void UpdateSmoothedOcclusionFromSamples(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                               float DeltaTime, int32 RotationSteps);

	// ── PerformUpdateRayCast phases ──────────────────────────────────────────
	struct FEdgeWeightAccum {
		int32 RaysReached = 0;
		FVector WeightedPos = FVector::ZeroVector;
		float PosWeightTotal = 0.f;
		float SrcWeightTotal = 0.f;
		float WeightedDistSum = 0.f;
	};
	static FEdgeWeightAccum AccumulateCachedEdgeWeights(USpatialAudioComponent& Component, const UWorld* World,
	                                                    const USpatialAudioSettings& Settings, const FVector& ListenerPos);
	static void ClearCacheOnConfirmedDirectLoS(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void UpdateVirtualSourceTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
	                                      const FVector& SourcePos);
	static void UpdatePathAttenuationTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
	                                        const USpatialAudioSettings& Settings, const FVector& SourcePos,
	                                        bool bVirtualPathActive);

	/** Within-glide-range matches keep their slot and glide. Everything else fades out in place
	 *  while a replacement fades in on a fresh slot. */
	static void SyncVirtualVoicesToClusters(USpatialAudioComponent& Component,
	                                        const TArray<FEdgeCluster>& Clusters,
	                                        const USpatialAudioSettings& Settings);

	// ── SyncVirtualVoicesToClusters phases ───────────────────────────────────
	struct FDesired {
		FVector Position;
		float PathDist;
		float PathAttenuation;
		float WeightShare;
		int32 MatchedVoice = INDEX_NONE;
	};
	static TArray<FDesired> BuildDesiredVoices(const USpatialAudioComponent& Component, const TArray<FEdgeCluster>& Clusters,
	                                           const USpatialAudioSettings& Settings);
	static void MatchVoicesToDesired(const TArray<FVirtualVoice>& Voices, TArray<FDesired>& Desired,
	                                 const USpatialAudioSettings& Settings, TArray<bool>& OutVoiceClaimed);
	static void FadeOutUnmatchedVoices(USpatialAudioComponent& Component, TArray<FVirtualVoice>& Voices,
	                                   const TArray<bool>& VoiceClaimed);
	static void AssignDesiredToVoices(USpatialAudioComponent& Component, TArray<FVirtualVoice>& Voices,
	                                  TArray<FDesired>& Desired);

	// ── PerformLoSBreakSweep phases ──────────────────────────────────────────
	struct FLoSBreakRayResult {
		bool bFound = false;
		FVector Point = FVector::ZeroVector;
		float CumDist = 0.f;
	};
	// Per-sweep constants every ray of one LoS-break sweep shares.
	struct FSweepRayParams {
		FVector ToListenerDir = FVector::ForwardVector;
		float SteerDist = 0.f;
		float MaxPathDist = 0.f;
		int32 EffMaxBounces = 0;
		bool bBias = false;
	};
	static FAsyncCastManager::FRayAccumulatorInput AccumulateLoSBreakHits(
		const USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings,
		const FVector& SourcePos, const FVector& ListenerPos, const FSweepRayParams& RayParams);
	static FVector PickBiasedLaunchDirection(const USpatialAudioComponent& Component,
	                                         const USpatialAudioSettings& Settings,
	                                         const FVector& ToListenerDir, float SteerDist, bool bBias);
	static bool TryConfirmLoSAtPoint(const USpatialAudioComponent& Component, const UWorld* World, const FVector& Point,
	                                 const FVector& ListenerPos, float CumDist, float MaxPathDist,
	                                 FLoSBreakRayResult& OutResult);
	static bool TrySampleSegmentForLoS(const USpatialAudioComponent& Component, const UWorld* World,
	                                   const USpatialAudioSettings& Settings, const FVector& SegOrigin,
	                                   const FVector& SegDir, float SegLen, float SegStartCumDist,
	                                   const FVector& ListenerPos, float MaxPathDist,
	                                   FLoSBreakRayResult& OutResult);
	static FLoSBreakRayResult TraceSingleLoSBreakRay(const USpatialAudioComponent& Component, UWorld* World,
	                                                 const USpatialAudioSettings& Settings, const FVector& SourcePos,
	                                                 const FVector& ListenerPos, const FVector& ToListenerDir, bool bBias,
	                                                 int32 EffMaxBounces, float MaxPathDist, float SteerDist, int32 RayIndex);
};
