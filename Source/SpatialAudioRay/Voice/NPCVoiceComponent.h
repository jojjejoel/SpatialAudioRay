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
 * Drives an NPC's voice from the acoustic state of its USpatialAudioComponent:
 * the effective acoustic distance to the listener (straight line while clear, diffraction
 * path length while occluded — GetEffectiveAcousticDistance) selects a vocal-effort bucket
 * (with dwell hysteresis), and a scheduler plays bank lines at that effort through the
 * shared spatial bus. Occlusion itself never shifts the effort — path length already encodes
 * how hard the NPC is to hear — it only decides whether the listener counts as hidden, which
 * picks the content half (Clear vs Occluded lines) and, on the tick it flips, drives the
 * sight reactions.
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
 * with one exception: a barge-in cuts the line short with a declick fade and replaces it,
 * then resumes normal scheduling at the new effort. Three moments qualify, ranked: sight
 * lost, sight regained, and an effort jump of ≥ TransitionBucketDelta buckets from the
 * playing line's effort. When a sight change finds nothing playing there is no line to cut,
 * so the next one is pulled forward instead — otherwise the reaction would arrive after the
 * window its content is gated on had already closed.
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
	 *  applied INSIDE the graph, ahead of the Audio Bus Writer — a component volume multiplier
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

	// Scheduler state — see NPCVoiceTypes.h. Held as structs so the pure decisions in
	// NPCVoiceLogic.h can take them by reference.
	FNPCVoiceBucketHysteresis BucketState;
	FNPCVoicePlaybackState Playback;
	FNPCVoiceTransitionState Transition;
	FNPCVoiceSightState SightState;
	TMap<FName, float> CooldownUntil;

	/** Cached at load: which barge-in reasons the bank can actually replace a line for. */
	FNPCVoiceBargeInAvailability BargeInAvailability;
};
