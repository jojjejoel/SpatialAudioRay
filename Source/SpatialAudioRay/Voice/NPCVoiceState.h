#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceTypes.h"

// The voice scheduler's runtime state, held as structs rather than loose component members so the
// decisions in NPCVoiceLogic.h can take them as parameters and stay testable without a component,
// world, or audio device.

/** Resolved once at load. Per reason rather than one flag because the triggers are ranked and the
 *  first that fires wins: a bank with Transition but no SightRegained lines would let a sight-gained
 *  tick claim the barge-in, find nothing, and abort, swallowing the effort drift that fired on the
 *  same tick and did have content. */
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

/** Sampled once per tick and fed to content selection. Listener-relative throughout, which is
 *  legitimate for choosing WHAT to say and never for the virtual path's own gain. */
struct FNPCVoiceAcousticState {
	float Occlusion = 0.f;
	float DirectDistanceCm = 0.f;
	/** The straight line while clear, the diffraction route while occluded. */
	float EffectiveDistanceCm = 0.f;
	bool bSightReactionPending = false;

	float DetourRatio() const {
		return EffectiveDistanceCm / FMath::Max(DirectDistanceCm, 1.f);
	}
};

/** Edge detector for the listener crossing between visible and hidden, and the voice layer's sole
 *  sight signal, so the content ladder and the sight barge-ins cannot disagree about a break. */
struct FNPCVoiceSightState {
	bool bHidden = false;
	/** The first sample seeds bHidden without reporting a change: the starting side is not
	 *  something the NPC just watched happen. */
	bool bInitialized = false;
	float LastChangeTime = -1e9f;
	/** Without it the reaction window is purely temporal, so the line after a reaction re-announces
	 *  the same crossing, usually at a different effort because an effort change is what scheduled
	 *  it. Cleared by the next crossing. */
	bool bReactionDelivered = false;
};

/** Dwell-time hysteresis for the effort, so a player walking a band edge cannot flip-flop the
 *  NPC's delivery. */
struct FNPCVoiceEffortHysteresis {
	ENPCVoiceEffort Committed = ENPCVoiceEffort::Conversational;
	ENPCVoiceEffort Candidate = ENPCVoiceEffort::Conversational;
	float CandidateSince = 0.f;
	/** The first sample commits instantly: opening on a default effort would mis-deliver the
	 *  first line. */
	bool bInitialized = false;
};

/** The line currently being spoken, plus the silence before the next one. */
struct FNPCVoicePlaybackState {
	bool bPlaying = false;
	float EndTime = 0.f;
	float NextLineTime = 0.f;
	FName ActiveLineId;
	FString ActiveText;
	/** Blocks an immediate repeat of the same line. */
	FName LastLineId;
	/** Effort the playing line was rendered at. Barge-in compares the committed effort against
	 *  this rather than the previous frame's, so drift accumulated over a long line still trips. */
	ENPCVoiceEffort ActiveEffort = ENPCVoiceEffort::Conversational;
	/** Barge-ins are never themselves interrupted, and the line after one follows quickly. */
	bool bActiveIsBargeIn = false;
};

/** A barge-in cut waiting out its declick fade, plus the rate limit on further cuts. */
struct FNPCVoiceTransitionState {
	bool bPending = false;
	int32 PendingLine = INDEX_NONE;
	float PlayTime = 0.f;
	/** Far in the past, so the first barge-in of the session is free. */
	float LastTime = -1e9f;
};
