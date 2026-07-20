#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NPCVoiceTypes.h"
#include "NPCVoiceComponent.generated.h"

class UAudioComponent;
class UDataTable;
class UNPCVoiceSettings;
class USoundWave;
class USpatialAudioComponent;

/** A bank row with its wave resolved — UPROPERTY so loaded waves stay GC-rooted
 *  (the DataTable itself only holds soft references). */
USTRUCT()
struct FNPCVoiceRuntimeLine {
	GENERATED_BODY()

	UPROPERTY()
	FNPCVoiceLineRow Row;

	UPROPERTY()
	TObjectPtr<USoundWave> Wave = nullptr;
};

/**
 * Drives an NPC's voice from the acoustic state of its USpatialAudioComponent:
 * listener distance + occlusion select a vocal-effort bucket (with dwell hysteresis),
 * and a scheduler plays bank lines at that effort through the shared spatial bus.
 *
 * Setup on the NPC actor:
 *  - a USpatialAudioComponent (the acoustic state source),
 *  - a UAudioComponent tagged with BOTH "AudioComponentSource" (joins the spatial
 *    pipeline: bus, attenuation, per-frame occlusion) and VoiceAudioComponentTag
 *    (marks it as this component's mouth), bAutoActivate off, playing an MS_Source-style
 *    MetaSound whose wave input matches WaveParameterName,
 *  - this component, with a VoiceBank DataTable of FNPCVoiceLineRow.
 *
 * A playing line is never modified mid-flight: bucket changes apply to the NEXT line.
 */
UCLASS(ClassGroup=(Audio), meta=(BlueprintSpawnableComponent))
class SPATIALAUDIORAY_API UNPCVoiceComponent : public UActorComponent {
	GENERATED_BODY()

public:
	UNPCVoiceComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	/** Shared tunables; falls back to class defaults (CDO) when unassigned. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	TObjectPtr<UNPCVoiceSettings> _Settings = nullptr;

	const UNPCVoiceSettings& GetSettings() const;

	/** DataTable of FNPCVoiceLineRow (imported from the VoiceGen export CSV). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	TObjectPtr<UDataTable> VoiceBank = nullptr;

	/** Tag identifying the owner's voice UAudioComponent. That component must ALSO carry the
	 *  "AudioComponentSource" tag so the spatial pipeline feeds it bus + occlusion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName VoiceAudioComponentTag = TEXT("NPCVoiceAudio");

	/** Wave input name on the voice MetaSound that each line's SoundWave is injected into. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName WaveParameterName = TEXT("SoundWave");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Debug")
	bool bShowDebugText = false;

	/** Committed effort bucket (post-hysteresis) — what the next line will play at. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NPC Voice|Debug")
	ENPCVoiceEffort CurrentBucket = ENPCVoiceEffort::Conversational;

	/** Pure bucket mapping: distance band (near = quiet), then the occlusion shift toward
	 *  Shout. Static for unit testing. */
	static ENPCVoiceEffort MapToBucket(float DistanceCm, float Occlusion, const UNPCVoiceSettings& S);

private:
	void ResolveOwnerComponents();
	void LoadBank();
	void UpdateBucket(float Now, float DistanceCm, float Occlusion);
	void SelectAndPlayLine(float Now, float Occlusion);
	void DrawDebugText(float DistanceCm, float Occlusion, float Now) const;

	UPROPERTY()
	TArray<FNPCVoiceRuntimeLine> Lines;

	TWeakObjectPtr<UAudioComponent> VoiceAudio;
	TWeakObjectPtr<USpatialAudioComponent> SpatialAudio;

	ENPCVoiceEffort CandidateBucket = ENPCVoiceEffort::Conversational;
	float CandidateSince = 0.f;
	bool bBucketInitialized = false;

	bool bLinePlaying = false;
	float LineEndTime = 0.f;
	float NextLineTime = 0.f;
	FName ActiveLineId;
	FString ActiveText;
	FName LastLineId;
	TMap<FName, float> CooldownUntil;
};
