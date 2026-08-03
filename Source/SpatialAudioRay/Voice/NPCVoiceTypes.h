#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "NPCVoiceTypes.generated.h"

class USoundWave;

/** Vocal effort, ordered quiet → loud so drift between two of them is an integer step. */
UENUM(BlueprintType)
enum class ENPCVoiceEffort : uint8 {
	Whisper,
	Conversational,
	Raised,
	Shout
};

/** Content class for a line, picked by VoiceLogic::ResolveCategoryPreference. Append only. */
UENUM(BlueprintType)
enum class ENPCVoiceCategory : uint8 {
	/** Visible fallback. */
	Clear,
	/** Occluded fallback. */
	Occluded,
	/** Barge-in only, never picked by normal scheduling. */
	Transition,
	/** Occluded, but the path is barely longer than the straight line. */
	AroundCorner,
	/** Occluded and physically close, yet the sound travels far to arrive. */
	BehindWall,
	/** Direct sight just broke. Outranks the spatial contexts briefly. */
	LostSight,
	/** Direct sight just came back. */
	SightRegained,
	/** Visible but partly obstructed, while the sound still arrives nearly intact. */
	PartiallyOccluded
};

/** Why a playing line was cut short. Each reason has its own category, see BargeInCategory. */
UENUM(BlueprintType)
enum class ENPCVoiceBargeInReason : uint8 {
	None,
	/** The committed effort drifted far from what the playing line was rendered at. */
	EffortDrift,
	SightLost,
	SightGained
};

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
	/** Effort dropped: the listener closed in. */
	Closer,
	/** Effort rose: the listener is getting away. */
	Farther
};

/** One take of one line at one effort. Rows come from Tools/VoiceGen/export_to_unreal.py. */
USTRUCT(BlueprintType)
struct FNPCVoiceLineRow : public FTableRowBase {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName LineId;

	/** The line's effort. Named for the CSV column it imports from; renaming means re-exporting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceEffort Bucket = ENPCVoiceEffort::Conversational;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceCategory Category = ENPCVoiceCategory::Clear;

	/** Transition rows only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceTransitionDir Direction = ENPCVoiceTransitionDir::None;

	/** Lines sharing a group go on cooldown together after one of them plays. None = no group. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName CooldownGroup;

	/** Render length in seconds, from the generation manifest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	float Duration = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	TSoftObjectPtr<USoundWave> Sound;

	/** Transcript for the debug HUD and future subtitles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FString Text;
};

/** A bank row with its wave resolved. UPROPERTY so loaded waves stay GC-rooted. */
USTRUCT()
struct FNPCVoiceRuntimeLine {
	GENERATED_BODY()

	UPROPERTY()
	FNPCVoiceLineRow Row;

	UPROPERTY()
	TObjectPtr<USoundWave> Wave = nullptr;
};
