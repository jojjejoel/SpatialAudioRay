#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NPCVoiceTypes.h"
#include "NPCVoiceComponent.generated.h"

class UAudioComponent;
class UDataTable;
class UNPCVoiceSettings;
class USpatialAudioComponent;

/**
 * Drives an NPC's voice from the acoustic state of its USpatialAudioComponent. Effective
 * acoustic distance (straight while clear, diffraction path length while occluded) selects a
 * vocal-effort bucket with dwell hysteresis, and a scheduler plays bank lines at that effort
 * through the shared spatial bus. Occlusion never shifts the effort, since path length already
 * encodes it; it only decides whether the listener counts as hidden, which picks the content
 * half and drives the sight reactions on the tick it flips.
 *
 * Setup on the NPC actor: a USpatialAudioComponent, a UAudioComponent tagged with BOTH
 * "AudioComponentSource" and VoiceAudioComponentTag (bAutoActivate off, playing an
 * MS_Source-style MetaSound whose wave input matches WaveParameterName), and this component
 * with a VoiceBank DataTable of FNPCVoiceLineRow.
 *
 * A playing line is never modified mid-flight; bucket changes apply to the NEXT line. The one
 * exception is a barge-in, which cuts the line with a declick fade and replaces it. Three
 * moments qualify, ranked: sight lost, sight regained, and an effort jump of at least
 * TransitionBucketDelta buckets. With nothing playing there is no line to cut, so the next is
 * pulled forward instead, or the reaction would land after its window had closed.
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

	/** Float input on the voice MetaSound carrying the effort's source gain in dB. Must be
 *  applied INSIDE the graph, ahead of the Audio Bus Writer: a component volume multiplier
	 *  would only affect direct playback, leaving occluded playback (which replays the bus
	 *  through the virtual emitters) at the wrong level. Graphs without the input ignore it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName EffortGainParameterName = TEXT("EffortGainDb");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Debug")
	bool bShowDebugText = false;

private:
	void ResolveOwnerComponents();
	void LoadBank();
	void TickSightReaction(float Now, ENPCVoiceSightChange SightChange);
	void TickScheduler(float Now, const FNPCVoiceAcousticState& Acoustic);
	void SelectAndPlayLine(float Now, const FNPCVoiceAcousticState& Acoustic);
	void PlayLine(const FNPCVoiceRuntimeLine& Line, float Now, bool bAsBargeIn);
	void DrawDebugText(const FNPCVoiceAcousticState& Acoustic, float Now) const;
	FString BuildDebugInputsLine(const FNPCVoiceAcousticState& Acoustic, float Now) const;
	FString BuildDebugStateLine(float Now) const;

	UPROPERTY()
	TArray<FNPCVoiceRuntimeLine> Lines;

	TWeakObjectPtr<UAudioComponent> VoiceAudio;
	TWeakObjectPtr<USpatialAudioComponent> SpatialAudio;

	// Scheduler state, see NPCVoiceTypes.h. Held as structs so the pure decisions in
	// NPCVoiceLogic.h can take them by reference.
	FNPCVoiceBucketHysteresis BucketState;
	FNPCVoicePlaybackState Playback;
	FNPCVoiceTransitionState Transition;
	FNPCVoiceSightState SightState;
	TMap<FName, float> CooldownUntil;

	/** Cached at load: which barge-in reasons the bank can actually replace a line for. */
	FNPCVoiceBargeInAvailability BargeInAvailability;
};
