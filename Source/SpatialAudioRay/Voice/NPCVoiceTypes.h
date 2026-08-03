#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "NPCVoiceTypes.generated.h"

class USoundWave;

/** Vocal effort, ordered quiet → loud so bucket shifts are integer steps. */
UENUM(BlueprintType)
enum class ENPCVoiceEffort : uint8 {
	Whisper,
	Conversational,
	Raised,
	Shout
};

/** Content class, selected from the acoustic situation the listener is actually in.
 *
 *  Values split into a VISIBLE half and an OCCLUDED half, and selection never crosses
 *  between them (see VoiceLogic::ResolveCategoryPreference): each line asserts something
 *  about the world, so playing an occluded line to a listener standing in the open, or a
 *  "nothing between us" line to one behind a wall, contradicts what they can see.
 *  Appended in order; existing DataTable rows keep their meaning. */
UENUM(BlueprintType)
enum class ENPCVoiceCategory : uint8 {
	/** Visible, generic. The fallback for every visible context. */
	Clear,
	/** Occluded, generic. The fallback for every occluded context. */
	Occluded,
	/** Barge-in lines for dramatic bucket jumps; never picked by normal scheduling. */
	Transition,
	/** Occluded, but the path is barely longer than the straight line: one corner away. */
	AroundCorner,
	/** Occluded, physically close, yet the sound has to travel far to arrive. The signature
	 *  diffraction state, and the one where effort and proximity openly disagree, since
	 *  effort follows the path length while the listener is near enough to touch. */
	BehindWall,
	/** The moment direct line of sight broke. Temporal, not spatial: outranks the spatial
	 *  contexts briefly so the NPC can react to the change before describing the new state. */
	LostSight,
	/** The mirror of LostSight: direct line of sight just came back. */
	SightRegained,
	/** Visible, but something is genuinely in the way (a pillar, a railing, a crate clipping
	 *  the centre line) while enough of the source stays exposed that the sound arrives nearly
	 *  intact. The state Clear lies about: it asserts an unobstructed straight path, which
	 *  stops being true the moment any of the offset samples blocks, long before occlusion is
	 *  high enough to count as hidden. */
	PartiallyOccluded
};

/** Why a playing line was cut short. Each reason draws its replacement from a different
 *  category (see VoiceLogic::BargeInCategory). */
UENUM(BlueprintType)
enum class ENPCVoiceBargeInReason : uint8 {
	None,
	/** The committed effort drifted far from what the playing line was rendered at. */
	EffortDrift,
	/** Direct line of sight just broke. */
	SightLost,
	/** Direct line of sight just returned. */
	SightGained
};

/** Visibility delta for this tick. */
UENUM(BlueprintType)
enum class ENPCVoiceSightChange : uint8 {
	None,
	Lost,
	Gained
};

/** Which way the effort jumped for a Transition-category line. None on normal lines. */
UENUM(BlueprintType)
enum class ENPCVoiceTransitionDir : uint8 {
	None,
	/** Effort dropped: the listener closed in ("oh, you're right here"). */
	Closer,
	/** Effort rose: the listener is getting away ("hey, where are you going!"). */
	Farther
};

/** One take of one line at one effort level. Rows are produced by
 *  Tools/VoiceGen/export_to_unreal.py. Import its CSV with this row type. */
USTRUCT(BlueprintType)
struct FNPCVoiceLineRow : public FTableRowBase {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName LineId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceEffort Bucket = ENPCVoiceEffort::Conversational;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceCategory Category = ENPCVoiceCategory::Clear;

	/** Transition lines only: which way the effort jumped. None on normal lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceTransitionDir Direction = ENPCVoiceTransitionDir::None;

	/** Lines sharing a group go on cooldown together after one of them plays. None = no group. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName CooldownGroup;

	/** Render length in seconds (from the generation manifest), the scheduler's only
	 *  end-of-line signal, so it must match the actual asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	float Duration = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	TSoftObjectPtr<USoundWave> Sound;

	/** Transcript for the debug HUD and future subtitles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FString Text;
};

/** A bank row with its wave resolved. UPROPERTY so loaded waves stay GC-rooted
 *  (the DataTable itself only holds soft references). */
USTRUCT()
struct FNPCVoiceRuntimeLine {
	GENERATED_BODY()

	UPROPERTY()
	FNPCVoiceLineRow Row;

	UPROPERTY()
	TObjectPtr<USoundWave> Wave = nullptr;
};

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
	/** Straight line, source to listener. */
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

// The voice scheduler's entire mutable state. Plain C++ and held here rather than as loose
// component members so the pure decisions in NPCVoiceLogic.h can take them as parameters, which
// is what makes them testable without a component, world, or audio device.

/** Edge detector for the listener crossing between visible and hidden. The voice layer's
 *  SOLE sight signal: both the content ladder's reaction window and the sight barge-in
 *  triggers read it, so the two cannot disagree about whether a break happened. */
struct FNPCVoiceSightState {
	/** Whether the listener counted as hidden at the last sample. */
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
	/** Post-hysteresis effort: what the next line plays at. */
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
