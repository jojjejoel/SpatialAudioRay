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

void FUpdater::UpdateDualModeAudio(USpatialAudioComponent& Component, const float DeltaTime, const USpatialAudioSettings& Settings,
                                   const float CurvedOcclusion) {
	const float PrevSrcCrossfade = Component.AudioDiag.SourceCrossfade;
	const float PrevCurvedOcc = Component.AudioDiag.CurvedOcclusion;
	const float PrevVrtGain = Component.AudioDiag.VirtualGain;

	// Source volume is no longer crossfaded externally — the Source's own MetaSound graph
	// shapes volume/filtering continuously from CurvedOcclusion (including whatever floor it
	// should hold at full occlusion), driven purely by OcclusionParamName.
	// The virtual gate opens fully on total LoS loss, and (with VirtualCrossfadeStartOcclusion
	// below 1) partially through the pre-sweep band so the diffracted sound bleeds in before
	// full occlusion. Keyed to raw CurrentOcclusion — the same smoothed value the Source's
	// muffling follows — so both sides of the crossfade move together. The gate is slewed
	// rather than snapped so VirtualGain ramps instead of jumping in a single frame.
	const float RawRamp = Math::ComputeVirtualCrossfadeRamp(
		Component.CurrentOcclusion, Settings.VirtualCrossfadeStartOcclusion);
	Component.SmoothedCrossfadeRamp = Settings.VirtualCrossfadeSmoothingTime > 0.f
		? FMath::FInterpTo(Component.SmoothedCrossfadeRamp, RawRamp,
		                   DeltaTime, 1.f / Settings.VirtualCrossfadeSmoothingTime)
		: RawRamp;
	// The hard term keys off a completed blank ring cycle rather than bHasDirectLoS, which
	// drops on a single blocked sample while moving — one flickery frame must not pump the
	// gate. Regain stays instant (any clear sample resets the streak), and the LoS-break sweep
	// still fires from bHasDirectLoS, so the virtual position is seeded before the gate opens.
	const int32 RotationSteps = FMath::Clamp(Settings.OffsetRingRotationSteps, 1, 8);
	const bool bGateHasLoS = Component.NoLoSSampleStreak < RotationSteps;
	const bool bRampEnabled = Settings.VirtualCrossfadeStartOcclusion < 1.f;
	const float CrossfadeTarget = Math::ComputeVirtualCrossfadeTarget(
		bGateHasLoS, bRampEnabled && Component.VelocityScaling.IsStationary(),
		Component.SmoothedCrossfadeRamp);
	Component.CurrentVirtualCrossfade = Math::ComputeVirtualCrossfadeSlew(
		Component.CurrentVirtualCrossfade, CrossfadeTarget,
		Settings.VirtualCrossfadeFadeInTime, Settings.VirtualCrossfadeFadeOutTime, DeltaTime);
	const float VirtualCrossfade = Component.CurrentVirtualCrossfade;
	Component.AudioDiag.CurvedOcclusion = CurvedOcclusion;
	Component.AudioDiag.SourceCrossfade = 1.f;

	for (int32 i = Component.CachedAudioComponentSources.Num() - 1; i >= 0; --i) {
		if (UAudioComponent* Ac = Component.CachedAudioComponentSources[i].Get()) {
			Ac->SetFloatParameter(Settings.OcclusionParamName, CurvedOcclusion);
			Ac->SetVolumeMultiplier(Component.bDebugSilenceSource ? 0.f : 1.f);
		}
		else {
			// Finished bus one-shots auto-destroy; drop their stale entries here.
			Component.CachedAudioComponentSources.RemoveAt(i);
		}
	}

	AActor* OwnerActor = Component.GetOwner();
	if (!OwnerActor) {
		return;
	}
	const FVector ActorPos = OwnerActor->GetActorLocation();

	const float FadeStep = Settings.VirtualVoiceHandoffFadeTime > 0.f
		                       ? DeltaTime / Settings.VirtualVoiceHandoffFadeTime
		                       : 1.f;
	const float ParamBlendSpeed = Settings.PathAttenuationBlendTime > 0.f
		                              ? 1.f / Settings.PathAttenuationBlendTime
		                              : 1000.f;
	const float MoveSpeed = Settings.AudioSourceMoveTime > 0.f ? 1.f / Settings.AudioSourceMoveTime : 1000.f;

	float TotalVirtualGain = 0.f;
	float PrimaryGain = -1.f;
	float PrimaryPathBend = 0.f;
	FVector PrimaryOffset = FVector::ZeroVector;

	for (int32 SlotIdx = 0; SlotIdx < Component.VirtualSlots.Num(); ++SlotIdx) {
		FVirtualSlot& Slot = Component.VirtualSlots[SlotIdx];
		UAudioComponent* VC = Component.VirtualSlotComponents.IsValidIndex(SlotIdx)
			                      ? Component.VirtualSlotComponents[SlotIdx].Get()
			                      : nullptr;
		if (!VC || Slot.State == FVirtualSlot::EState::Idle) {
			continue;
		}

		if (Slot.State == FVirtualSlot::EState::FadingOut) {
			Slot.FadeAlpha -= FadeStep;
			if (Slot.FadeAlpha <= 0.f) {
				Slot = FVirtualSlot{};
				VC->SetFloatParameter(FName("VirtualGain"), 0.f);
				continue;
			}
			// Frozen params, live crossfade gate: a fading-out slot must still gate off
			// instantly when direct LoS is regained.
			const float Gain = Slot.FrozenGainScale * Slot.FadeAlpha * VirtualCrossfade;
			VC->SetFloatParameter(FName("VirtualGain"), Component.bDebugSilenceVirtual ? 0.f : Gain);
			TotalVirtualGain += Gain;
			continue;
		}

		if (Slot.State == FVirtualSlot::EState::FadingIn) {
			Slot.FadeAlpha = FMath::Min(Slot.FadeAlpha + FadeStep, 1.f);
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
			// The voice position IS the pulled-back cluster centroid (VirtualSourcePullbackDistance,
			// applied per edge in the clustering inputs) — no source→edge lerp: a fractional blend
			// scaled with source distance and cut straight through the geometry the path bends around.
			const FVector TargetOffset = Voice.SmoothedPosition - ActorPos;
			if (!Slot.bOffsetInit) {
				// A fresh slot snaps straight to its position — the fade-in envelope is the
				// transition; gliding there from the actor would sweep audibly through space.
				Slot.WorldOffset = TargetOffset;
				Slot.bOffsetInit = true;
			}
			else {
				Slot.WorldOffset = FMath::VInterpTo(Slot.WorldOffset, TargetOffset, DeltaTime, MoveSpeed);
			}
			VC->SetWorldLocation(ActorPos + Slot.WorldOffset);
		}

		Voice.CurrentWeightShare = FMath::FInterpTo(Voice.CurrentWeightShare, Voice.TargetWeightShare,
		                                            DeltaTime, ParamBlendSpeed);
		Voice.CurrentPathAttenuation = FMath::FInterpTo(Voice.CurrentPathAttenuation,
		                                                Voice.TargetPathAttenuation,
		                                                DeltaTime, ParamBlendSpeed);

		// No source- or listener-distance attenuation curve here — each slot's own
		// SoundAttenuation asset handles listener-proximity loudness natively via the engine,
		// since the component is physically positioned at its voice's location above.
		const FVector VirtualPos = ActorPos + Slot.WorldOffset;
		const float Leg1_Geom = FVector::Dist(ActorPos, VirtualPos);

		// Crossfade passed as 1: the gate multiplies in below, so FrozenGainScale stays
		// gate-free and keeps gating correctly after the voice releases the slot.
		const Math::FVirtualAudioParams VAP = Math::ComputeVirtualAudioParams(
			1.f, Voice.CurrentPathAttenuation, Leg1_Geom, Voice.PathDist, Component.MaxRayDistance, Settings);

		Slot.FrozenGainScale = VAP.VirtualGain * Voice.CurrentWeightShare;

		const float Gain = Slot.FrozenGainScale * Slot.FadeAlpha * VirtualCrossfade;
		VC->SetFloatParameter(FName("VirtualGain"), Component.bDebugSilenceVirtual ? 0.f : Gain);
		VC->SetFloatParameter(FName("VirtualPathBend"), VAP.VirtualPathBend);

		TotalVirtualGain += Gain;
		if (Gain > PrimaryGain) {
			PrimaryGain = Gain;
			PrimaryPathBend = VAP.VirtualPathBend;
			PrimaryOffset = Slot.WorldOffset;
		}
	}

	// HUD/Blueprint mirror of the loudest slot; diagnostics track the summed virtual level so
	// the spike detector still sees handoff crossfades as one continuous gain.
	Component.CurrentAudioComponentOffset = PrimaryGain >= 0.f ? PrimaryOffset : FVector::ZeroVector;

	{
		const float SrcDelta = 1.f - PrevSrcCrossfade;
		const float VrtDelta = TotalVirtualGain - PrevVrtGain;
		const float OccDelta = CurvedOcclusion - PrevCurvedOcc;

		Component.AudioDiag.DeltaSrcVol = SrcDelta;
		Component.AudioDiag.DeltaOcc = OccDelta;
		Component.AudioDiag.DeltaVrtGain = VrtDelta;

		Component.AudioDiag.SpikeTimer = FMath::Max(0.f, Component.AudioDiag.SpikeTimer - DeltaTime);
		const bool bGainSpike = FMath::Max3(FMath::Abs(SrcDelta), FMath::Abs(VrtDelta), FMath::Abs(OccDelta)) > 0.02f;
		if (bGainSpike) {
			Component.AudioDiag.SpikeTimer = 3.f;
			Component.AudioDiag.SpikeSrcDelta = SrcDelta;
			Component.AudioDiag.SpikeVrtGainDelta = VrtDelta;
			Component.AudioDiag.SpikeOccDelta = OccDelta;
		}
	}

	Component.AudioDiag.VirtualGain = TotalVirtualGain;
	Component.AudioDiag.VirtualPathBend = PrimaryPathBend;
}
