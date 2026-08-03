#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NPCVoiceTypes.h"
#include "NPCVoiceSettings.generated.h"

/**
 * Shared, designer-tunable settings for UNPCVoiceComponent. Create one asset in the Content
 * Browser and assign it to every voice component; if none is assigned, the class defaults are used.
 */
UCLASS(BlueprintType)
class SPATIALAUDIORAY_API UNPCVoiceSettings : public UDataAsset {
	GENERATED_BODY()

public:

	// ── Effort Bands ──────────────────────────────────────────────────────────
	// Bands over the EFFECTIVE acoustic distance, so a bent path is simply a longer one and
	// occlusion never shifts the effort directly. OcclusionShiftThreshold gates both halves of
	// that: it switches content to the occluded set, and holds effort on the straight line until
	// it is crossed, since routes are cached well before the source is actually hidden and
	// charging for the long way round makes the NPC strain at a listener it can plainly see.

	/** Effective acoustic distance (cm) at or below which the NPC whispers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Bands",
		meta = (ClampMin = "0.0"))
	float WhisperMaxDistance = 400.f;

	/** Effective acoustic distance (cm) at or below which the NPC talks conversationally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Bands",
		meta = (ClampMin = "0.0"))
	float ConversationalMaxDistance = 1100.f;

	/** Effective acoustic distance (cm) at or below which the NPC projects; beyond it, shout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Bands",
		meta = (ClampMin = "0.0"))
	float RaisedMaxDistance = 1800.f;

	/** Occlusion at or above which the listener counts as hidden. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Bands",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OcclusionShiftThreshold = 0.8f;

	/** Seconds a differing candidate effort must persist before the committed effort follows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Bands",
		meta = (ClampMin = "0.0"))
	float EffortDwellTime = 0.5f;

	// ── Effort Reach ──────────────────────────────────────────────────────────
	// Derived from the bands above rather than authored separately: an effort exists to be heard
	// across its own band. Headroom covers the lag between the two, since effort commits only after
	// EffortDwellTime and a line already playing finishes at the effort it started at. Applied per
	// line via SetAttenuationOuterRadius; the wavs stay at one LUFS.

	/** Reach = the effort's own band max × this, reach being where the sound falls to silence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Reach",
		meta = (ClampMin = "1.0"))
	float EffortReachHeadroom = 5.f;

	/** Shout returns 0 = the attenuation asset's own range, anchoring what the rest scale down from. */
	float GetEffortReachDistance(ENPCVoiceEffort Effort) const {
		switch (Effort) {
			case ENPCVoiceEffort::Whisper: return WhisperMaxDistance * EffortReachHeadroom;
			case ENPCVoiceEffort::Conversational: return ConversationalMaxDistance * EffortReachHeadroom;
			case ENPCVoiceEffort::Raised: return RaisedMaxDistance * EffortReachHeadroom;
			default: return 0.f;
		}
	}

	// ── Content Contexts ──────────────────────────────────────────────────────
	// Which acoustic situation the listener is in, so the bank can carry a line for it. Content
	// selection only; a context with no line falls back to the generic entry for its half.
	// PartialOcclusionThreshold sits far below OcclusionShiftThreshold on purpose: that one asks
	// whether the listener can see the NPC at all, this one whether the path is unobstructed, which
	// turns false as soon as any offset sample blocks.

	/** Occlusion at or above which a visible listener selects PartiallyOccluded over Clear. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PartialOcclusionThreshold = 0.3f;

	/** Hidden listeners within this straight-line distance (cm) may select BehindWall content. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "0.0"))
	float BehindWallMaxDirectDistance = 800.f;

	/** How many times longer than the straight line the route must be to count as all the way around. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "1.0"))
	float BehindWallMinDetourRatio = 2.0f;

	/** Hidden listeners whose route stays within this multiple of the straight line select AroundCorner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "1.0"))
	float AroundCornerMaxDetourRatio = 1.1f;

	/** Seconds after a sight change during which its content outranks the spatial contexts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "0.0"))
	float SightChangeReactionWindow = 6.f;

	// ── Effort Gain ───────────────────────────────────────────────────────────
	// How much louder the effort is AT THE SOURCE, orthogonal to reach: a shout is louder AND
	// carries further. Shout anchors at 0 and everything scales DOWN, because the sources share one
	// bus and boosting above 0 risks clipping it when several sum.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float WhisperGainDb = -24.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float ConversationalGainDb = -16.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float RaisedGainDb = -10.f;

	/** The anchor: leave at 0 and tune the others down against it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float ShoutGainDb = 0.f;

	float GetEffortGainDb(ENPCVoiceEffort Effort) const {
		switch (Effort) {
			case ENPCVoiceEffort::Whisper: return WhisperGainDb;
			case ENPCVoiceEffort::Conversational: return ConversationalGainDb;
			case ENPCVoiceEffort::Raised: return RaisedGainDb;
			default: return ShoutGainDb;
		}
	}

	// ── Transitions ───────────────────────────────────────────────────────────
	// A playing line is normally never modified mid-flight. The exceptions are sight breaking,
	// sight returning, and committed effort drifting far from what the line was rendered at.

	/** Minimum effort-step jump that triggers a barge-in. 0 disables the effort trigger only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0", ClampMax = "3"))
	int32 TransitionEffortDelta = 1;

	/** Fade-out (seconds) applied to the interrupted line: a declick, not an audible fade. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float TransitionFadeOutTime = 0.06f;

	/** Minimum seconds between barge-ins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float TransitionCooldownSeconds = 2.f;

	/** Don't barge in when the playing line has less than this left. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float TransitionMinRemainingTime = 0.75f;

	/** Silence between a finished transition line and the full line that follows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float PostTransitionLineDelay = 0.272f;

	// ── Line Scheduling ───────────────────────────────────────────────────────

	/** Minimum silence (seconds) between the end of one line and the start of the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float LineIntervalMin = 1.f;

	/** Maximum silence (seconds) between lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float LineIntervalMax = 1.f;

	/** Seconds a cooldown group stays blocked after one of its lines plays. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float CooldownGroupSeconds = 1.f;

	/** Grace added to a row's Duration before the scheduler declares the line finished. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float LineEndPadding = 0.2f;

	// ── Debug ─────────────────────────────────────────────────────────────────

	/** Size multiplier for the voice component's on-screen debug lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Debug",
		meta = (ClampMin = "0.5", ClampMax = "4.0"))
	float DebugTextScale = 2.f;
};
