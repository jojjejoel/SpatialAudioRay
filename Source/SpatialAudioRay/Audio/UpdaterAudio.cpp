#include "Audio/Updater.h"
#include "Audio/Math.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/SpatialAudioSettings.h"

#include "Components/AudioComponent.h"
#include "Sound/SoundAttenuation.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

void FUpdater::UpdateAudioParameters(USpatialAudioComponent& Component, const float DeltaTime,
                                     const USpatialAudioSettings& Settings) {
	UpdateDualModeAudio(Component, DeltaTime, Settings);
}

float FUpdater::UpdateVirtualCrossfadeGate(USpatialAudioComponent& Component, const float DeltaTime,
                                           const USpatialAudioSettings& Settings) {
	const float RawRamp = Math::ComputeVirtualCrossfadeRamp(
		Component.CurrentOcclusion, Settings.VirtualCrossfadeStartOcclusion);
	Component.SmoothedCrossfadeRamp = Settings.VirtualCrossfadeSmoothingTime > 0.f
		                                  ? FMath::FInterpTo(Component.SmoothedCrossfadeRamp, RawRamp,
		                                                     DeltaTime, 1.f / Settings.VirtualCrossfadeSmoothingTime)
		                                  : RawRamp;
	const int32 RotationSteps = Component.ResolveRingRotationSteps();
	const bool bGateHasLoS = Component.NoLoSSampleStreak < RotationSteps;
	const bool bRampEnabled = Settings.VirtualCrossfadeStartOcclusion < 1.f;
	const float CrossfadeTarget = Math::ComputeVirtualCrossfadeTarget(
		bGateHasLoS, bRampEnabled && Component.VelocityScaling.IsStationary(),
		Component.SmoothedCrossfadeRamp);
	Component.CurrentVirtualCrossfade = Math::ComputeVirtualCrossfadeSlew(
		Component.CurrentVirtualCrossfade, CrossfadeTarget, Settings.VirtualCrossfadeFadeTime, DeltaTime);
	return Component.CurrentVirtualCrossfade;
}

void FUpdater::ApplySourceOcclusionParams(USpatialAudioComponent& Component,
                                          const USpatialAudioSettings& Settings) {
	for (int32 i = Component.CachedAudioComponentSources.Num() - 1; i >= 0; --i) {
		if (UAudioComponent* Ac = Component.CachedAudioComponentSources[i].Get()) {
			Ac->SetFloatParameter(Settings.OcclusionParamName, Component.CurrentOcclusion);
			Ac->SetVolumeMultiplier(Component.bDebugSilenceSource ? 0.f : 1.f);
		}
		else {
			Component.CachedAudioComponentSources.RemoveAt(i);
		}
	}
}

FUpdater::FVoiceBlendRates FUpdater::ComputeVoiceBlendRates(const USpatialAudioSettings& Settings,
                                                            const float DeltaTime) {
	FVoiceBlendRates Rates;
	Rates.FadeStep = Settings.VirtualVoiceHandoffFadeTime > 0.f
		                 ? DeltaTime / Settings.VirtualVoiceHandoffFadeTime
		                 : 1.f;
	Rates.ParamBlendSpeed = Settings.PathAttenuationBlendTime > 0.f
		                        ? 1.f / Settings.PathAttenuationBlendTime
		                        : 1000.f;
	return Rates;
}

void FUpdater::TickFadingOutSlot(const USpatialAudioComponent& Component, FVirtualSlot& Slot, UAudioComponent* VC,
                                 const float FadeStep, const float VirtualCrossfade,
                                 FVirtualVoiceUpdateResult& OutResult) {
	Slot.FadeAlpha -= FadeStep;
	if (Slot.FadeAlpha <= 0.f) {
		Slot = FVirtualSlot{};
		VC->SetFloatParameter(FName("VirtualGain"), 0.f);
		return;
	}

	const float Gain = Slot.FrozenGainScale * Slot.FadeAlpha * VirtualCrossfade;
	VC->SetFloatParameter(FName("VirtualGain"), Component.bDebugSilenceVirtual ? 0.f : Gain);
	OutResult.TotalVirtualGain += Gain;
}

void FUpdater::MoveSlotToVoice(FVirtualSlot& Slot, UAudioComponent* VC, const FVirtualVoice& Voice,
                               const FVector& ActorPos) {
	Slot.WorldOffset = Voice.TargetPosition - ActorPos;
	Slot.bOffsetInit = true;
	VC->SetWorldLocation(ActorPos + Slot.WorldOffset);
}

void FUpdater::ApplyVoiceAudioParams(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                     FVirtualSlot& Slot, FVirtualVoice& Voice, UAudioComponent* VC,
                                     const float DeltaTime, const float ParamBlendSpeed,
                                     const float VirtualCrossfade, FVirtualVoiceUpdateResult& OutResult) {
	Voice.CurrentWeightShare = FMath::FInterpTo(Voice.CurrentWeightShare, Voice.TargetWeightShare,
	                                            DeltaTime, ParamBlendSpeed);
	Voice.CurrentPathAttenuation = FMath::FInterpTo(Voice.CurrentPathAttenuation,
	                                                Voice.TargetPathAttenuation,
	                                                DeltaTime, ParamBlendSpeed);

	const Math::FVirtualAudioParams VAP = Math::ComputeVirtualAudioParams(
		1.f, Voice.CurrentPathAttenuation, Voice.PathDist, Component.MaxRayDistance, Settings);

	Slot.FrozenGainScale = VAP.VirtualGain * Voice.CurrentWeightShare;
	Voice.CurrentPathBend = VAP.VirtualPathBend;

	const float Gain = Slot.FrozenGainScale * Slot.FadeAlpha * VirtualCrossfade;
	VC->SetFloatParameter(FName("VirtualGain"), Component.bDebugSilenceVirtual ? 0.f : Gain);
	VC->SetFloatParameter(FName("VirtualPathBend"), VAP.VirtualPathBend);

	OutResult.TotalVirtualGain += Gain;
}

FUpdater::FVirtualVoiceUpdateResult FUpdater::UpdateVirtualVoiceSlots(USpatialAudioComponent& Component,
                                                                      const USpatialAudioSettings& Settings,
                                                                      const float DeltaTime,
                                                                      const float VirtualCrossfade,
                                                                      const FVector& ActorPos) {
	FVirtualVoiceUpdateResult Result;
	const FVoiceBlendRates Rates = ComputeVoiceBlendRates(Settings, DeltaTime);

	for (int32 SlotIdx = 0; SlotIdx < Component.VirtualSlots.Num(); ++SlotIdx) {
		FVirtualSlot& Slot = Component.VirtualSlots[SlotIdx];
		UAudioComponent* VC = Component.VirtualSlotComponents.IsValidIndex(SlotIdx)
			                      ? Component.VirtualSlotComponents[SlotIdx].Get()
			                      : nullptr;
		if (!VC || Slot.State == FVirtualSlot::EState::Idle) {
			continue;
		}

		if (Slot.State == FVirtualSlot::EState::FadingOut) {
			TickFadingOutSlot(Component, Slot, VC, Rates.FadeStep, VirtualCrossfade, Result);
			continue;
		}

		if (Slot.State == FVirtualSlot::EState::FadingIn) {
			Slot.FadeAlpha = FMath::Min(Slot.FadeAlpha + Rates.FadeStep, 1.f);
			if (Slot.FadeAlpha >= 1.f) {
				Slot.State = FVirtualSlot::EState::Active;
			}
		}

		if (!Component.VirtualVoices.IsValidIndex(Slot.VoiceIndex)
			|| !Component.VirtualVoices[Slot.VoiceIndex].bActive) {
			Slot.State = FVirtualSlot::EState::FadingOut;
			continue;
		}

		FVirtualVoice& Voice = Component.VirtualVoices[Slot.VoiceIndex];
		MoveSlotToVoice(Slot, VC, Voice, ActorPos);
		ApplyVoiceAudioParams(Component, Settings, Slot, Voice, VC, DeltaTime,
		                      Rates.ParamBlendSpeed, VirtualCrossfade, Result);
	}

	return Result;
}

void FUpdater::UpdateDualModeAudio(USpatialAudioComponent& Component, const float DeltaTime,
                                   const USpatialAudioSettings& Settings) {
	const float VirtualCrossfade = UpdateVirtualCrossfadeGate(Component, DeltaTime, Settings);

	ApplySourceOcclusionParams(Component, Settings);

	AActor* OwnerActor = Component.GetOwner();
	if (!OwnerActor) {
		return;
	}

	const FVirtualVoiceUpdateResult Result = UpdateVirtualVoiceSlots(
		Component, Settings, DeltaTime, VirtualCrossfade, OwnerActor->GetActorLocation());

	Component.AudioDiag.VirtualGain = Result.TotalVirtualGain;
}
