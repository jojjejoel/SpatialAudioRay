#pragma once

#include "CoreMinimal.h"

class UAudioComponent;
class USpatialAudioComponent;
class USpatialAudioSettings;
class UWorld;
struct FEdgeCluster;
struct FVirtualVoice;
struct FVirtualSlot;

class FUpdater {
public:
	static void TickDirectLoSSampling(USpatialAudioComponent& Component, float DeltaTime,
	                                  const USpatialAudioSettings& Settings);
	static void PerformUpdateRayCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void UpdateAudioParameters(USpatialAudioComponent& Component, float DeltaTime,
	                                  const USpatialAudioSettings& Settings);

	static FVector ResolveOffsetPoint(const USpatialAudioComponent& Component, const UWorld* World,
	                                  const FVector& ListenerPos, const FVector& CandidatePoint);

	static float SyncOffsetLoSFraction(USpatialAudioComponent& Component, UWorld* World,
	                                   const FVector& SourcePos, const FVector& ListenerPos,
	                                   float OffsetR, float SourceR, float SourceRingR,
	                                   float RingStepRad);

private:
	struct FCastContext {
		UWorld* World = nullptr;
		FVector SourcePos = FVector::ZeroVector;
		FVector ListenerPos = FVector::ZeroVector;
	};

	static bool TryResolveCastContext(const USpatialAudioComponent& Component, FCastContext& OutContext);

	static bool IsOutOfRange(const FCastContext& Context, float MaxRayDistance) {
		return FVector::DistSquared(Context.SourcePos, Context.ListenerPos) > FMath::Square(MaxRayDistance);
	}

	static void UpdateDualModeAudio(USpatialAudioComponent& Component, float DeltaTime,
	                                const USpatialAudioSettings& Settings);

	static float UpdateVirtualCrossfadeGate(USpatialAudioComponent& Component, float DeltaTime,
	                                        const USpatialAudioSettings& Settings);
	static void ApplySourceOcclusionParams(USpatialAudioComponent& Component);

	struct FVirtualVoiceUpdateResult {
		float TotalVirtualGain = 0.f;
	};

	struct FVoiceBlendRates {
		float FadeStep = 1.f;
		float ParamBlendSpeed = 1000.f;
	};

	static FVoiceBlendRates ComputeVoiceBlendRates(const USpatialAudioSettings& Settings, float DeltaTime);
	static void TickFadingOutSlot(const USpatialAudioComponent& Component, FVirtualSlot& Slot, UAudioComponent* VC,
	                              float FadeStep, float VirtualCrossfade, FVirtualVoiceUpdateResult& OutResult);
	static void MoveSlotToVoice(FVirtualSlot& Slot, UAudioComponent* VC, const FVirtualVoice& Voice,
	                            const FVector& ActorPos);
	static void ApplyVoiceAudioParams(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                  FVirtualSlot& Slot, FVirtualVoice& Voice, UAudioComponent* VC,
	                                  float DeltaTime, float ParamBlendSpeed,
	                                  float VirtualCrossfade, FVirtualVoiceUpdateResult& OutResult);
	static FVirtualVoiceUpdateResult UpdateVirtualVoiceSlots(USpatialAudioComponent& Component,
	                                                         const USpatialAudioSettings& Settings,
	                                                         float DeltaTime, float VirtualCrossfade,
	                                                         const FVector& ActorPos);

	static void TrySampleOffsetLoS(USpatialAudioComponent& Component, UWorld* World,
	                               const USpatialAudioSettings& Settings,
	                               float DeltaTime, const FVector& SourcePos, const FVector& ListenerPos,
	                               int32 RotationSteps);
	static void UpdateOcclusionFromSamples(USpatialAudioComponent& Component, int32 RotationSteps);

	struct FEdgeWeightAccum {
		FVector WeightedPos = FVector::ZeroVector;
		float PosWeightTotal = 0.f;
		float SrcWeightTotal = 0.f;
		float WeightedDistSum = 0.f;
	};

	static FEdgeWeightAccum AccumulateCachedEdgeWeights(USpatialAudioComponent& Component,
	                                                    const USpatialAudioSettings& Settings,
	                                                    const FVector& ListenerPos);
	static void ClearCacheOnConfirmedDirectLoS(USpatialAudioComponent& Component,
	                                           const USpatialAudioSettings& Settings);
	static void UpdateVirtualSourceTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
	                                      const FVector& SourcePos);
	static void UpdatePathAttenuationTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
	                                        const USpatialAudioSettings& Settings, bool bVirtualPathActive);

	static void SyncVirtualVoicesToClusters(USpatialAudioComponent& Component,
	                                        const TArray<FEdgeCluster>& Clusters,
	                                        const USpatialAudioSettings& Settings);

	struct FDesired {
		FVector Position;
		float PathDist;
		float PathAttenuation;
		float WeightShare;
		int32 MatchedVoice = INDEX_NONE;
	};

	static TArray<FDesired> BuildDesiredVoices(const USpatialAudioComponent& Component,
	                                           const TArray<FEdgeCluster>& Clusters,
	                                           const USpatialAudioSettings& Settings);
	static void MatchVoicesToDesired(const TArray<FVirtualVoice>& Voices, TArray<FDesired>& Desired,
	                                 const USpatialAudioSettings& Settings, TArray<bool>& OutVoiceClaimed);
	static void FadeOutUnmatchedVoices(USpatialAudioComponent& Component, TArray<FVirtualVoice>& Voices,
	                                   const TArray<bool>& VoiceClaimed);
	static void AssignDesiredToVoices(USpatialAudioComponent& Component, TArray<FVirtualVoice>& Voices,
	                                  TArray<FDesired>& Desired);
};
