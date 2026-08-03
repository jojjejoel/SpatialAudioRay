#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceTypes.h"

// The voice scheduler's runtime state. Plain C++ with no reflection, held here rather than as
// loose component members so the pure decisions in NPCVoiceLogic.h can take them as parameters,
// which is what makes them testable without a component, world, or audio device.

/** Resolved once at load, per reason rather than one flag: the triggers are ranked and the first
 *  that fires wins, so a bank with Transition but no SightRegained lines would let a sight-gained
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
 *  legitimate for choosing WHAT to say, never for the virtual path's own gain. */
struct FNPCVoiceAcousticState {
	float Occlusion = 0.f;
	float DirectDistanceCm = 0.f;
	/** How far the sound actually travels: the straight line while clear, the diffraction
	 *  route while occluded. Drives the effort bucket. */
	float EffectiveDistanceCm = 0.f;
	/** Recent enough to be worth remarking on AND not already remarked on. Which of LostSight or
	 *  SightRegained it opens follows from the half the listener is in now, so one flag serves both. */
	bool bSightReactionPending = false;

	/** How much further the sound travels than the straight line. 1 = no detour. This is the
	 *  measurement no non-diffraction audio system can make, and the one BehindWall keys on. */
	float DetourRatio() const {
		return EffectiveDistanceCm / FMath::Max(DirectDistanceCm, 1.f);
	}
};

/** Edge detector for the listener crossing between visible and hidden. The voice layer's
 *  SOLE sight signal: both the content ladder's reaction window and the sight barge-in
 *  triggers read it, so the two cannot disagree about whether a break happened. */
struct FNPCVoiceSightState {
	bool bHidden = false;
	/** False until the first sample, which seeds bHidden without reporting a change: the starting
	 *  side is not something the NPC just watched happen. */
	bool bInitialized = false;
	/** Far in the past, so an unchanged scene offers no reaction content. */
	float LastChangeTime = -1e9f;
	/** Cleared by the next crossing. Without it the window is purely temporal, so the line after a
	 *  reaction re-announces the same event, and since a bucket change usually schedules that line,
	 *  it re-announces at a different effort, which is the tell. */
	bool bReactionDelivered = false;
};

/** Dwell-time hysteresis for the effort bucket: a mapped bucket must persist before it
 *  commits, so a player walking a band edge can't flip-flop the NPC's delivery. */
struct FNPCVoiceBucketHysteresis {
	/** What the next line plays at. */
	ENPCVoiceEffort Committed = ENPCVoiceEffort::Conversational;
	/** Effort the distance currently maps to, waiting out the dwell time. */
	ENPCVoiceEffort Candidate = ENPCVoiceEffort::Conversational;
	float CandidateSince = 0.f;
	/** False until the first sample, which commits instantly: starting from a default bucket would
	 *  mis-deliver the opening line. */
	bool bInitialized = false;
};

/** The line currently being spoken, plus the silence before the next one. */
struct FNPCVoicePlaybackState {
	bool bPlaying = false;
	/** When the current line finishes (row Duration + LineEndPadding). */
	float EndTime = 0.f;
	/** When the next line may start. */
	float NextLineTime = 0.f;
	FName ActiveLineId;
	FString ActiveText;
	/** Blocks an immediate repeat of the same line. */
	FName LastLineId;
	/** Effort the playing line was rendered at. The barge-in trigger compares the committed
	 *  bucket against this, not against the previous frame's bucket, so drift accumulated over
	 *  a long line still trips it. */
	ENPCVoiceEffort ActiveBucket = ENPCVoiceEffort::Conversational;
	/** True while the playing line is itself a barge-in. Barge-ins are never interrupted, and
	 *  the line after one follows quickly instead of waiting out the normal interval. */
	bool bActiveIsBargeIn = false;
};

/** A barge-in cut waiting out its declick fade, plus the rate limit on further cuts. */
struct FNPCVoiceTransitionState {
	bool bPending = false;
	int32 PendingLine = INDEX_NONE;
	/** When the pending line starts, after the interrupted line's fade has finished. */
	float PlayTime = 0.f;
	/** Stamped when a barge-in triggers. Starts far in the past so the first one is free. */
	float LastTime = -1e9f;
};
