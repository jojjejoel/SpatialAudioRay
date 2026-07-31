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

void FUpdater::UpdateAudioParameters(USpatialAudioComponent& Component, const float DeltaTime, const USpatialAudioSettings& Settings) {
	const float CurvedOcclusion = FMath::Pow(Component.CurrentOcclusion, Settings.OcclusionCurveExponent);
	UpdateDualModeAudio(Component, DeltaTime, Settings, CurvedOcclusion);
}

float FUpdater::UpdateVirtualCrossfadeGate(USpatialAudioComponent& Component, const float DeltaTime, const USpatialAudioSettings& Settings) {
	// Source volume is not crossfaded here at all. Its MetaSound graph shapes volume and filtering
	// from CurvedOcclusion directly. Keying this gate to the same smoothed occlusion keeps both
	// sides of the crossfade moving together.
	const float RawRamp = Math::ComputeVirtualCrossfadeRamp(
		Component.CurrentOcclusion, Settings.VirtualCrossfadeStartOcclusion);
	Component.SmoothedCrossfadeRamp = Settings.VirtualCrossfadeSmoothingTime > 0.f
		? FMath::FInterpTo(Component.SmoothedCrossfadeRamp, RawRamp,
		                   DeltaTime, 1.f / Settings.VirtualCrossfadeSmoothingTime)
		: RawRamp;
	// Keyed to a completed blank ring cycle rather than bHasDirectLoS, which drops on a single
	// blocked sample while moving. One flickery frame must not pump the gate.
	const int32 RotationSteps = FMath::Clamp(Settings.OffsetRingRotationSteps, 1, 8);
	const bool bGateHasLoS = Component.NoLoSSampleStreak < RotationSteps;
	const bool bRampEnabled = Settings.VirtualCrossfadeStartOcclusion < 1.f;
	const float CrossfadeTarget = Math::ComputeVirtualCrossfadeTarget(
		bGateHasLoS, bRampEnabled && Component.VelocityScaling.IsStationary(),
		Component.SmoothedCrossfadeRamp);
	Component.CurrentVirtualCrossfade = Math::ComputeVirtualCrossfadeSlew(
		Component.CurrentVirtualCrossfade, CrossfadeTarget,
		Settings.VirtualCrossfadeFadeInTime, Settings.VirtualCrossfadeFadeOutTime, DeltaTime);
	return Component.CurrentVirtualCrossfade;
}

void FUpdater::ApplySourceOcclusionParams(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings, const float CurvedOcclusion) {
	for (int32 i = Component.CachedAudioComponentSources.Num() - 1; i >= 0; --i) {
		if (UAudioComponent* Ac = Component.CachedAudioComponentSources[i].Get()) {
			Ac->SetFloatParameter(Settings.OcclusionParamName, CurvedOcclusion);
			Ac->SetVolumeMultiplier(Component.bDebugSilenceSource ? 0.f : 1.f);
		}
		else {
			// Finished bus one-shots auto-destroy, so drop their stale entries here.
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
	Rates.MoveSpeed = Settings.AudioSourceMoveTime > 0.f ? 1.f / Settings.AudioSourceMoveTime : 1000.f;
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

	// Frozen params but a live gate, so a fading slot still cuts off when direct LoS returns.
	const float Gain = Slot.FrozenGainScale * Slot.FadeAlpha * VirtualCrossfade;
	VC->SetFloatParameter(FName("VirtualGain"), Component.bDebugSilenceVirtual ? 0.f : Gain);
	OutResult.TotalVirtualGain += Gain;
}

void FUpdater::MoveSlotToVoice(FVirtualSlot& Slot, UAudioComponent* VC, const FVirtualVoice& Voice,
                               const FVector& ActorPos, const float DeltaTime, const float MoveSpeed) {
	const FVector TargetOffset = Voice.SmoothedPosition - ActorPos;
	if (!Slot.bOffsetInit) {
		// A fresh slot snaps. The fade-in envelope is the transition, and gliding in from the actor
		// would sweep audibly through space.
		Slot.WorldOffset = TargetOffset;
		Slot.bOffsetInit = true;
	}
	else {
		Slot.WorldOffset = FMath::VInterpTo(Slot.WorldOffset, TargetOffset, DeltaTime, MoveSpeed);
	}
	VC->SetWorldLocation(ActorPos + Slot.WorldOffset);
}

void FUpdater::ApplyVoiceAudioParams(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                     FVirtualSlot& Slot, FVirtualVoice& Voice, UAudioComponent* VC,
                                     const FVector& ActorPos, const float DeltaTime, const float ParamBlendSpeed,
                                     const float VirtualCrossfade, FVirtualVoiceUpdateResult& OutResult) {
	Voice.CurrentWeightShare = FMath::FInterpTo(Voice.CurrentWeightShare, Voice.TargetWeightShare,
	                                            DeltaTime, ParamBlendSpeed);
	Voice.CurrentPathAttenuation = FMath::FInterpTo(Voice.CurrentPathAttenuation,
	                                                Voice.TargetPathAttenuation,
	                                                DeltaTime, ParamBlendSpeed);

	// No listener-distance term here. The slot's own SoundAttenuation asset handles proximity
	// loudness natively, since the component sits at its voice's location.
	const float Leg1Geom = FVector::Dist(ActorPos, ActorPos + Slot.WorldOffset);

	// Crossfade passed as 1 so FrozenGainScale stays gate-free and keeps gating correctly after
	// the voice releases the slot. The gate multiplies in below.
	const Math::FVirtualAudioParams VAP = Math::ComputeVirtualAudioParams(
		1.f, Voice.CurrentPathAttenuation, Leg1Geom, Voice.PathDist, Component.MaxRayDistance, Settings);

	Slot.FrozenGainScale = VAP.VirtualGain * Voice.CurrentWeightShare;

	const float Gain = Slot.FrozenGainScale * Slot.FadeAlpha * VirtualCrossfade;
	VC->SetFloatParameter(FName("VirtualGain"), Component.bDebugSilenceVirtual ? 0.f : Gain);
	VC->SetFloatParameter(FName("VirtualPathBend"), VAP.VirtualPathBend);

	OutResult.TotalVirtualGain += Gain;
	if (Gain > OutResult.PrimaryGain) {
		OutResult.PrimaryGain = Gain;
		OutResult.PrimaryPathBend = VAP.VirtualPathBend;
		OutResult.PrimaryOffset = Slot.WorldOffset;
	}
}

FUpdater::FVirtualVoiceUpdateResult FUpdater::UpdateVirtualVoiceSlots(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                                                       const float DeltaTime, const float VirtualCrossfade,
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
		if (Settings.bDriveSourcePosition) {
			MoveSlotToVoice(Slot, VC, Voice, ActorPos, DeltaTime, Rates.MoveSpeed);
		}
		ApplyVoiceAudioParams(Component, Settings, Slot, Voice, VC, ActorPos, DeltaTime,
		                      Rates.ParamBlendSpeed, VirtualCrossfade, Result);
	}

	return Result;
}

void FUpdater::UpdateDualModeAudio(USpatialAudioComponent& Component, const float DeltaTime, const USpatialAudioSettings& Settings,
                                   const float CurvedOcclusion) {
	const float VirtualCrossfade = UpdateVirtualCrossfadeGate(Component, DeltaTime, Settings);
	Component.AudioDiag.CurvedOcclusion = CurvedOcclusion;
	Component.AudioDiag.SourceCrossfade = 1.f;

	ApplySourceOcclusionParams(Component, Settings, CurvedOcclusion);

	AActor* OwnerActor = Component.GetOwner();
	if (!OwnerActor) {
		return;
	}

	const FVirtualVoiceUpdateResult Result = UpdateVirtualVoiceSlots(
		Component, Settings, DeltaTime, VirtualCrossfade, OwnerActor->GetActorLocation());

	// The HUD mirrors the loudest slot, while the diagnostic gain is the summed level.
	Component.CurrentAudioComponentOffset = Result.PrimaryGain >= 0.f ? Result.PrimaryOffset : FVector::ZeroVector;

	Component.AudioDiag.VirtualGain = Result.TotalVirtualGain;
	Component.AudioDiag.VirtualPathBend = Result.PrimaryPathBend;
}
