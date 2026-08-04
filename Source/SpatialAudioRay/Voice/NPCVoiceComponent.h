#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NPCVoiceState.h"
#include "NPCVoiceComponent.generated.h"

class UAudioComponent;
class UDataTable;
class UNPCVoiceSettings;
class USpatialAudioComponent;

/**
 * Drives an NPC's voice from its USpatialAudioComponent: effective acoustic distance selects a
 * vocal effort, and a scheduler plays bank lines at that effort through the spatial bus.
 *
 * Needs a USpatialAudioComponent, a UAudioComponent tagged both "AudioComponentSource" and
 * VoiceAudioComponentTag, and a VoiceBank DataTable of FNPCVoiceLineRow. The MetaSound must apply
 * EffortGainParameterName inside the graph, ahead of the Audio Bus Writer.
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

	/** Tag on the owner's voice UAudioComponent, which must also carry "AudioComponentSource". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName VoiceAudioComponentTag = TEXT("NPCVoiceAudio");

	/** Wave input name on the voice MetaSound that each line's SoundWave is injected into. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName WaveParameterName = TEXT("SoundWave");

	/** Float input on the voice MetaSound carrying the effort's source gain in dB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice")
	FName EffortGainParameterName = TEXT("EffortGainDb");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NPC Voice|Debug")
	bool bShowDebugText = false;

private:
	void ResolveOwnerComponents();
	void LoadBank();
	FNPCVoiceAcousticState SampleAcousticState(const USpatialAudioComponent& Spatial,
	                                           const FVector& ListenerPos) const;
	void TickSightReaction(float Now, ENPCVoiceSightChange SightChange);
	void TickScheduler(float Now, const FNPCVoiceAcousticState& Acoustic);
	void SelectAndPlayLine(float Now, const FNPCVoiceAcousticState& Acoustic);
	void PlayLine(const FNPCVoiceRuntimeLine& Line, float Now, bool bAsBargeIn);
	void ApplyEffortParametersBeforePlay(UAudioComponent& Audio, ENPCVoiceEffort Effort,
	                                     USoundWave* Wave);
	void DrawDebugText(const FNPCVoiceAcousticState& Acoustic, float Now) const;
	FString BuildDebugInputsLine(const FNPCVoiceAcousticState& Acoustic, float Now) const;
	FString BuildDebugStateLine(float Now) const;

	UPROPERTY()
	TArray<FNPCVoiceRuntimeLine> Lines;

	TWeakObjectPtr<UAudioComponent> VoiceAudio;
	TWeakObjectPtr<USpatialAudioComponent> SpatialAudio;

	FNPCVoiceEffortHysteresis EffortState;
	FNPCVoicePlaybackState Playback;
	FNPCVoiceTransitionState Transition;
	FNPCVoiceSightState SightState;
	TMap<FName, float> CooldownUntil;
	FNPCVoiceBargeInAvailability BargeInAvailability;
};
