#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NPCVoiceTypes.h"
#include "NPCVoiceSettings.generated.h"

/**
 * Shared, designer-tunable settings for UNPCVoiceComponent.
 * Create one asset in the Content Browser and assign it to every voice component;
 * if none is assigned, the class defaults (CDO) are used.
 */
UCLASS(BlueprintType)
class SPATIALAUDIORAY_API UNPCVoiceSettings : public UDataAsset {
	GENERATED_BODY()

public:

	// ── Effort Buckets ────────────────────────────────────────────────────────
	// Bands over the EFFECTIVE acoustic distance (USpatialAudioComponent::
	// GetEffectiveAcousticDistance — straight line while clear, diffraction path length
	// while occluded) select effort inversely: whisper when the listener is acoustically
	// close, shout when far. Occlusion never shifts the bucket directly — a bent path is
	// simply longer, so someone just around a small corner gets at most a small step up
	// while someone three rooms deep walks the bands toward Shout. Perceived loudness
	// contrast is deliberately left to timbre — the engine's attenuation owns loudness
	// (far shout and close whisper land at similar levels; the difference is the
	// performance).

	/** Effective acoustic distance (cm) at or below which the NPC whispers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float WhisperMaxDistance = 600.f;

	/** Effective acoustic distance (cm) at or below which the NPC talks conversationally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float ConversationalMaxDistance = 1500.f;

	/** Effective acoustic distance (cm) at or below which the NPC projects (raised).
	 *  Beyond it: shout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float RaisedMaxDistance = 3000.f;

	/** At or above this occlusion, line selection switches to Occluded-category content
	 *  ("I can hear you back there"). No longer shifts the effort bucket — path length
	 *  already encodes being hidden. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OcclusionShiftThreshold = 0.75f;

	/** Seconds a differing candidate bucket must persist before the committed bucket follows.
	 *  Keeps players dancing on a band edge from flip-flopping the NPC's delivery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float BucketDwellTime = 1.0f;

	// ── Effort Reach ──────────────────────────────────────────────────────────
	// Per-effort attenuation falloff scale, applied via SetAttenuationFalloffScale at each
	// line start: a whisper carries meters, a shout carries the map. The wavs stay at one
	// LUFS (effort = timbre); reach differences live entirely in the engine attenuation.
	// Author the actor's attenuation for SHOUT reach and scale the rest DOWN — the spatial
	// component's ray/LoS ranges are captured at base scale, so a scale above 1 would make
	// a line audible beyond where the ray system searches for diffraction paths.

	/** Falloff-distance scale while whispering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Reach",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float WhisperFalloffScale = 0.15f;

	/** Falloff-distance scale for conversational lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Reach",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float ConversationalFalloffScale = 0.4f;

	/** Falloff-distance scale for raised lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Reach",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float RaisedFalloffScale = 0.7f;

	/** Falloff-distance scale while shouting — the base the attenuation is authored for. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Reach",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float ShoutFalloffScale = 1.0f;

	float GetFalloffScale(ENPCVoiceEffort Effort) const {
		switch (Effort) {
			case ENPCVoiceEffort::Whisper: return WhisperFalloffScale;
			case ENPCVoiceEffort::Conversational: return ConversationalFalloffScale;
			case ENPCVoiceEffort::Raised: return RaisedFalloffScale;
			default: return ShoutFalloffScale;
		}
	}

	// ── Transitions (barge-in) ────────────────────────────────────────────────
	// A playing line is normally never modified mid-flight; the one exception is a dramatic
	// effort jump. When the committed bucket drifts far enough from the bucket the playing
	// line STARTED at, the line is cut (short declick fade) and a dedicated
	// Transition-category line fires in the jump's direction ("oh, you're right here" /
	// "hey, you're running off!"), followed quickly by a full line at the new effort.

	/** Minimum bucket-step jump (|committed − playing line's bucket|) that triggers a
	 *  barge-in. 1 = any band change interrupts (dwell time + cooldown keep it from
	 *  chattering); 2 = only whisper↔raised-scale jumps. 0 = barge-in disabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0", ClampMax = "3"))
	int32 TransitionBucketDelta = 1;

	/** Fade-out (seconds) applied to the interrupted line — a declick, not an audible fade. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float TransitionFadeOutTime = 0.06f;

	/** Minimum seconds between barge-ins, so a player yo-yoing across a band edge can't
	 *  turn the NPC into a stutterer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float TransitionCooldownSeconds = 8.f;

	/** Don't barge in when the playing line has less than this left — it ends on its own
	 *  before the interruption would read as intentional. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float TransitionMinRemainingTime = 0.75f;

	/** Silence between a finished transition line and the full line that follows. Replaces
	 *  the normal LineIntervalMin/Max wait — the transition announces a change, so the
	 *  follow-up at the new effort should come quickly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Transitions",
		meta = (ClampMin = "0.0"))
	float PostTransitionLineDelay = 0.6f;

	// ── Line Scheduling ───────────────────────────────────────────────────────

	/** Minimum silence (seconds) between the end of one line and the start of the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float LineIntervalMin = 4.f;

	/** Maximum silence (seconds) between lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float LineIntervalMax = 9.f;

	/** Seconds a cooldown group stays blocked after one of its lines plays. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float CooldownGroupSeconds = 12.f;

	/** Grace added to a row's Duration before the scheduler declares the line finished —
	 *  covers MetaSound generator tail so the next line can't clip the current one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Line Scheduling",
		meta = (ClampMin = "0.0"))
	float LineEndPadding = 0.2f;
};
