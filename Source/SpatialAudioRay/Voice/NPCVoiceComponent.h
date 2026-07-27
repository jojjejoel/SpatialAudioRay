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
 * the effective acoustic distance to the listener (straight line while clear, diffraction
 * path length while occluded — GetEffectiveAcousticDistance) selects a vocal-effort bucket
 * (with dwell hysteresis), and a scheduler plays bank lines at that effort through the
 * shared spatial bus. Occlusion itself only switches content category (Clear vs Occluded
 * lines) — the path length already encodes how hard the NPC is to hear.
 *
 * Setup on the NPC actor:
 *  - a USpatialAudioComponent (the acoustic state source),
 *  - a UAudioComponent tagged with BOTH "AudioComponentSource" (joins the spatial
 *    pipeline: bus, attenuation, per-frame occlusion) and VoiceAudioComponentTag
 *    (marks it as this component's mouth), bAutoActivate off, playing an MS_Source-style
 *    MetaSound whose wave input matches WaveParameterName,
 *  - this component, with a VoiceBank DataTable of FNPCVoiceLineRow.
 *
 * A playing line is never modified mid-flight — bucket changes apply to the NEXT line —
 * with one exception: a dramatic effort jump (≥ TransitionBucketDelta buckets from the
 * playing line's effort) barges in with a short Transition-category line in the jump's
 * direction, then resumes normal scheduling at the new effort.
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

	/** Pure bucket mapping: effective-acoustic-distance band, near = quiet. Occlusion enters
	 *  only through the distance (a bent path is longer), so a listener just around a small
	 *  corner gets a small step up, not a jump to Shout. Static for unit testing. */
	static ENPCVoiceEffort MapToBucket(float EffectiveDistanceCm, const UNPCVoiceSettings& S);

	/** Pure transition-line pick for a barge-in: Transition-category rows matching Dir with
	 *  cooldowns respected; exact TargetBucket preferred, then the nearest rendered bucket
	 *  (a slightly-off effort still beats refusing to react), random among ties.
	 *  INDEX_NONE when the bank has nothing usable. Static for unit testing. */
	static int32 FindTransitionLine(const TArray<FNPCVoiceRuntimeLine>& InLines,
	                                ENPCVoiceTransitionDir Dir, ENPCVoiceEffort TargetBucket,
	                                float Now, const TMap<FName, float>& Cooldowns);

private:
	void ResolveOwnerComponents();
	void LoadBank();
	void UpdateBucket(float Now, float EffectiveDistanceCm);
	void SelectAndPlayLine(float Now, float Occlusion);
	void PlayLine(const FNPCVoiceRuntimeLine& Line, float Now);
	void TickTransitionBargeIn(float Now);
	void DrawDebugText(float DistanceCm, float EffectiveDistanceCm, float Occlusion, float Now) const;

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

	/** Effort the playing line was rendered at — the barge-in trigger compares the committed
	 *  bucket against this, not against the previous frame's bucket, so drift accumulated
	 *  over a long line still trips it. */
	ENPCVoiceEffort ActiveLineBucket = ENPCVoiceEffort::Conversational;
	bool bActiveLineIsTransition = false;
	bool bBankHasTransitions = false;
	bool bTransitionPending = false;
	int32 PendingTransitionLine = INDEX_NONE;
	float TransitionPlayTime = 0.f;
	float LastTransitionTime = -1e9f;
};
