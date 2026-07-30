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

	/** At or above this occlusion the listener counts as hidden, and line selection switches
	 *  from the visible content contexts to the occluded ones.
	 *
	 *  It never shifts the effort bucket directly — path length already encodes being hidden —
	 *  but it IS the point from which the diffraction route starts counting toward the
	 *  effective acoustic distance (passed to GetEffectiveAcousticDistance as its detour
	 *  floor). Below it the effort follows the straight line: cached routes exist well before
	 *  the source is hidden, and charging the NPC for a long way round while most of the sound
	 *  still arrives straight makes it strain at a listener it can plainly see. Tying both to
	 *  one threshold keeps effort and content from disagreeing about whether the detour is
	 *  real. Nothing jumps at the crossing — the route phases in across the range above it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OcclusionShiftThreshold = 0.75f;

	/** Seconds a differing candidate bucket must persist before the committed bucket follows.
	 *  Keeps players dancing on a band edge from flip-flopping the NPC's delivery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float BucketDwellTime = 1.0f;

	// ── Effort Reach ──────────────────────────────────────────────────────────
	// Audible reach is DERIVED from the bands above rather than authored separately: an effort
	// exists to be heard across its own band, so a whisper only has to carry as far as the
	// distance at which the NPC would switch to conversational anyway. Applied at each line
	// start via USpatialAudioComponent::SetAttenuationOuterRadius. The wavs stay at one LUFS
	// (effort = timbre); reach differences live entirely in the engine attenuation.

	/** Reach = the effort's own band max × this. Above 1 the effort stays audible past the
	 *  band edge, which the scheduler needs: the bucket only commits after BucketDwellTime,
	 *  and a line already playing finishes at its starting effort, so the listener is
	 *  regularly a little past the boundary while an older effort is still speaking — without
	 *  headroom that reads as the audio cutting out mid-word. Note reach is where the sound
	 *  reaches SILENCE, so it is already quiet at the band edge; raise this if an effort feels
	 *  too faint at the top of its own band. 1 = dies exactly at the boundary (tightest,
	 *  most dramatic: walking out of earshot silences the line until a barge-in reacts). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Reach",
		meta = (ClampMin = "1.0"))
	float EffortReachHeadroom = 1.25f;

	/** Audible reach (cm) for Effort. Shout returns 0 = the attenuation asset's own range:
	 *  the top band is unbounded, so it is the anchor every other effort scales down from
	 *  (author the actor's attenuation for shout reach). */
	float GetEffortReachDistance(ENPCVoiceEffort Effort) const {
		switch (Effort) {
			case ENPCVoiceEffort::Whisper: return WhisperMaxDistance * EffortReachHeadroom;
			case ENPCVoiceEffort::Conversational: return ConversationalMaxDistance * EffortReachHeadroom;
			case ENPCVoiceEffort::Raised: return RaisedMaxDistance * EffortReachHeadroom;
			default: return 0.f;
		}
	}

	// ── Content Contexts ──────────────────────────────────────────────────────
	// Thresholds that pick WHICH acoustic situation the listener is in, so the bank can carry
	// a line specific to it. Purely content selection — none of this reaches gain or path
	// math. Any context with no line in the bank falls back to the generic Clear/Occluded
	// entry for that half, so partial banks degrade instead of breaking.

	/** Occlusion at or above which a still-visible listener selects PartiallyOccluded content
	 *  instead of Clear. 0 = off (the visible half stays one band).
	 *
	 *  Sits well below OcclusionShiftThreshold on purpose: those two thresholds answer different
	 *  questions. OcclusionShiftThreshold asks "can they still see me at all", this asks "is the
	 *  path actually unobstructed" — and the answer to the second turns false as soon as any
	 *  offset sample blocks, which is a long way below hidden. Default 0.15 catches roughly one
	 *  blocked sample out of the five-point ring: a pillar or railing clipping the centre line
	 *  while the sound still arrives nearly intact. Without it the NPC narrates "nothing between
	 *  us, a perfectly straight line" at a listener standing behind a crate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PartialOcclusionThreshold = 0.15f;

	/** Hidden listeners within this straight-line distance (cm) AND past
	 *  BehindWallMinDetourRatio select BehindWall content: physically close, acoustically far. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "0.0"))
	float BehindWallMaxDirectDistance = 800.f;

	/** How many times longer than the straight line the sound's route must be to count as
	 *  "all the way around". 2 = the path is twice the direct distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "1.0"))
	float BehindWallMinDetourRatio = 2.0f;

	/** Hidden listeners whose route is no longer than this multiple of the straight line
	 *  select AroundCorner content — out of sight, but only just. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "1.0"))
	float AroundCornerMaxDetourRatio = 1.35f;

	/** Seconds after sight is lost or regained during which LostSight / SightRegained content
	 *  outranks the spatial contexts, so the NPC reacts to the change before describing the
	 *  new state. An upper bound only — the reaction is also spent the moment one of those
	 *  lines plays, so raising this lets a reaction wait out a long in-flight line without
	 *  ever letting the NPC remark on the same crossing twice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Content Contexts",
		meta = (ClampMin = "0.0"))
	float SightChangeReactionWindow = 6.f;

	// ── Effort Gain ───────────────────────────────────────────────────────────
	// How much louder the effort is AT THE SOURCE, sent to the voice MetaSound's EffortGainDb
	// input at each line start. Reach (above) decides where a sound dies; this decides how
	// loud it is at a given distance — orthogonal, and both are real: a shout is louder AND
	// carries further. Without this, crossing a band boundary changes almost nothing, because
	// two falloff curves that end near each other differ by only a decibel or two where they
	// overlap.
	//
	// This is what the bank's single LUFS target was FOR: normalizing the renders strips the
	// TTS's accidental loudness variation so that deliberate, designed variation can be
	// applied here instead. Effort is still timbre in the asset; level is the engine's job.
	//
	// Values are dB, unlike reach these are NOT derivable from the distance bands (loudness at
	// the source has nothing to do with where the band edges sit). Shout anchors at 0 and
	// everything scales DOWN: the sources share one mixing bus, so boosting above 0 risks
	// clipping it when several sounds sum, and it mirrors how reach anchors on shout too.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float WhisperGainDb = -12.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float ConversationalGainDb = -6.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Gain",
		meta = (ClampMax = "0.0"))
	float RaisedGainDb = -3.f;

	/** The anchor — leave at 0 and tune the others down against it. */
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

	// ── Barge-in ──────────────────────────────────────────────────────────────
	// A playing line is normally never modified mid-flight. The exceptions are the three
	// moments worth reacting to: direct line of sight breaking, sight coming back, and the
	// committed effort drifting far from what the playing line was rendered at. The line is
	// cut with a short declick fade, a line from the matching category fires, and the next
	// full line follows quickly.
	//
	// Visibility outranks effort drift when both land on the same tick, which is the common
	// case: losing sight inflates the acoustic path, which climbs the effort bands. Without
	// that priority the NPC reports a listener "moving away" who only stepped behind a wall.

	/** Minimum bucket-step jump (|committed − playing line's bucket|) that triggers an
	 *  effort barge-in. 1 = any band change interrupts (dwell time + cooldown keep it from
	 *  chattering); 2 = only whisper↔raised-scale jumps. 0 disables effort barge-ins only —
	 *  the sight-lost and sight-regained triggers are unaffected. */
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

	// ── Debug ─────────────────────────────────────────────────────────────────

	/** Size multiplier for the voice component's on-screen debug lines. 1 = the engine's
	 *  default. Worth raising for video capture, where the default is unreadable once the
	 *  footage is scaled down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Debug",
		meta = (ClampMin = "0.5", ClampMax = "4.0"))
	float DebugTextScale = 2.f;
};
