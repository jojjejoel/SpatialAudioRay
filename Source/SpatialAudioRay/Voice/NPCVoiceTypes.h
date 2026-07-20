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

UENUM(BlueprintType)
enum class ENPCVoiceCategory : uint8 {
	/** Playable while the listener has line of sight to the NPC. */
	Clear,
	/** Occlusion-keyed content ("I can hear you back there") — selected while occluded. */
	Occluded,
	/** Barge-in lines for dramatic bucket jumps; never picked by normal scheduling. */
	Transition
};

/** One take of one line at one effort level. Rows are produced by
 *  Tools/VoiceGen/export_to_unreal.py — import its CSV with this row type. */
USTRUCT(BlueprintType)
struct FNPCVoiceLineRow : public FTableRowBase {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName LineId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceEffort Bucket = ENPCVoiceEffort::Conversational;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	ENPCVoiceCategory Category = ENPCVoiceCategory::Clear;

	/** Lines sharing a group go on cooldown together after one of them plays. None = no group. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName CooldownGroup;

	/** Render length in seconds (from the generation manifest) — the scheduler's only
	 *  end-of-line signal, so it must match the actual asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	float Duration = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	TSoftObjectPtr<USoundWave> Sound;

	/** Transcript — debug HUD / future subtitles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FString Text;
};
