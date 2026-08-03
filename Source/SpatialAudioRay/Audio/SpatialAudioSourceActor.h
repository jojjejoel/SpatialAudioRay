// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpatialAudioSourceActor.generated.h"

class UAudioComponent;
class USpatialAudioComponent;
class USoundWave;

/**
 * A self-contained spatial audio source that can be spawned at runtime.
 * Pre-wires UAudioComponent + USpatialAudioComponent so the same ray-casting
 * calculations used by manually placed actors apply automatically.
 *
 * Derive a Blueprint from this class, assign SC_Diffraction to the AudioComponent,
 * and set SoundWaveOverride to the specific wave you want this source to play.
 */
UCLASS(Blueprintable)
class SPATIALAUDIORAY_API ASpatialAudioSourceActor : public AActor {
	GENERATED_BODY()

public:
	ASpatialAudioSourceActor();

	/**
	 * The raw sound wave this source plays.
	 * Injected into the shared Sound Cue via its Wave Parameter node at runtime.
	 * Set this per Blueprint subclass to give each source a different sound
	 * while sharing a single Sound Cue (e.g. SC_Diffraction).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Spatial Audio")
	USoundWave* SoundWaveOverride;

	/** Name of the Wave Parameter node inside the shared Sound Cue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Spatial Audio")
	FName WaveParameterName = TEXT("SoundWave");

	/** When true, the actor destroys itself when the audio finishes playing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Spatial Audio")
	bool bDestroyOnAudioFinished = true;

private:
	UPROPERTY(VisibleAnywhere)
	USceneComponent* RootSceneComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	UAudioComponent* AudioCompSource;

	/** Never plays. A config template (MetaSound + attenuation) the USpatialAudioComponent
	 *  virtual voice pool clones its runtime components from. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	UAudioComponent* AudioCompVirtual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	USpatialAudioComponent* SpatialComp;

	UPROPERTY(VisibleAnywhere)
	UStaticMeshComponent* CubeMesh;


	UFUNCTION()
	void OnAudioFinished();

protected:
	virtual void BeginPlay() override;
};
