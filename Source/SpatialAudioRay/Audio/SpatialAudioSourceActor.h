// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpatialAudioSourceActor.generated.h"

class UAudioComponent;
class USpatialAudioComponent;
class USoundWave;

/**
 * A self-contained spatial audio source that can be spawned at runtime, pre-wiring a
 * UAudioComponent and a USpatialAudioComponent. Derive a Blueprint from this class, assign
 * SC_Diffraction to the AudioComponent, and set SoundWaveOverride to the wave it should play.
 */
UCLASS(Blueprintable)
class SPATIALAUDIORAY_API ASpatialAudioSourceActor : public AActor {
	GENERATED_BODY()

public:
	ASpatialAudioSourceActor();

	/** The raw sound wave this source plays, injected into the shared Sound Cue via its Wave
	 *  Parameter node at runtime. Set per Blueprint subclass to vary the sound per source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Spatial Audio")
	TObjectPtr<USoundWave> SoundWaveOverride;

	/** Name of the Wave Parameter node inside the shared Sound Cue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Spatial Audio")
	FName WaveParameterName = TEXT("SoundWave");

	/** When true, the actor destroys itself when the audio finishes playing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Spatial Audio")
	bool bDestroyOnAudioFinished = true;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> RootSceneComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UAudioComponent> AudioCompSource;

	/** Never plays. A config template (MetaSound + attenuation) the USpatialAudioComponent
	 *  virtual voice pool clones its runtime components from. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UAudioComponent> AudioCompVirtual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<USpatialAudioComponent> SpatialComp;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> CubeMesh;


	UFUNCTION()
	void OnAudioFinished();

protected:
	virtual void BeginPlay() override;
};
