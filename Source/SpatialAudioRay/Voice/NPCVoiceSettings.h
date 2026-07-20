#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
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
	// Distance bands select effort inversely: whisper when the listener is close,
	// shout when far. Perceived loudness contrast is deliberately left to timbre —
	// the engine's attenuation owns loudness (far shout and close whisper land at
	// similar levels; the difference is the performance).

	/** Listener distance (cm) at or below which the NPC whispers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float WhisperMaxDistance = 600.f;

	/** Listener distance (cm) at or below which the NPC talks conversationally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float ConversationalMaxDistance = 1500.f;

	/** Listener distance (cm) at or below which the NPC projects (raised). Beyond it: shout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float RaisedMaxDistance = 3000.f;

	/** At or above this occlusion the bucket shifts one step toward Shout (the NPC raises its
	 *  voice because it can't see you) and line selection switches to Occluded-category content. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OcclusionShiftThreshold = 0.75f;

	/** Seconds a differing candidate bucket must persist before the committed bucket follows.
	 *  Keeps players dancing on a band edge from flip-flopping the NPC's delivery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Effort Buckets",
		meta = (ClampMin = "0.0"))
	float BucketDwellTime = 1.0f;

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
