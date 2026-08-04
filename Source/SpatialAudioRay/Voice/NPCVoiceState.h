#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceTypes.h"

struct FNPCVoiceBargeInAvailability {
	bool bTransition = false;
	bool bLostSight = false;
	bool bSightRegained = false;

	bool Has(ENPCVoiceBargeInReason Reason) const {
		switch (Reason) {
			case ENPCVoiceBargeInReason::EffortDrift: return bTransition;
			case ENPCVoiceBargeInReason::SightLost: return bLostSight;
			case ENPCVoiceBargeInReason::SightGained: return bSightRegained;
			default: return false;
		}
	}
};

struct FNPCVoiceAcousticState {
	float Occlusion = 0.f;
	float DirectDistanceCm = 0.f;
	float EffectiveDistanceCm = 0.f;
	bool bSightReactionPending = false;

	float DetourRatio() const {
		return EffectiveDistanceCm / FMath::Max(DirectDistanceCm, 1.f);
	}
};

struct FNPCVoiceSightState {
	bool bHidden = false;
	bool bInitialized = false;
	float LastChangeTime = -1e9f;
	bool bReactionDelivered = false;
};

struct FNPCVoiceEffortHysteresis {
	ENPCVoiceEffort Committed = ENPCVoiceEffort::Conversational;
	ENPCVoiceEffort Candidate = ENPCVoiceEffort::Conversational;
	float CandidateSince = 0.f;
	bool bInitialized = false;
};

struct FNPCVoicePlaybackState {
	bool bPlaying = false;
	float EndTime = 0.f;
	float NextLineTime = 0.f;
	FName ActiveLineId;
	FString ActiveText;
	FName LastLineId;
	ENPCVoiceEffort ActiveEffort = ENPCVoiceEffort::Conversational;
	bool bActiveIsBargeIn = false;
};

struct FNPCVoiceTransitionState {
	bool bPending = false;
	int32 PendingLine = INDEX_NONE;
	float PlayTime = 0.f;
	float LastTime = -1e9f;
};
