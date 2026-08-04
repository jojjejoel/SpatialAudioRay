#include "Audio/SpatialAudioSourceActor.h"
#include "Components/AudioComponent.h"
#include "Audio/SpatialAudioComponent.h"

ASpatialAudioSourceActor::ASpatialAudioSourceActor() {
	RootSceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	RootComponent = RootSceneComponent;

	CubeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualRepresentation"));
	CubeMesh->SetupAttachment(RootSceneComponent);

	AudioCompSource = CreateDefaultSubobject<UAudioComponent>(TEXT("AudioComponentSource"));
	AudioCompSource->bAutoActivate = false;
	AudioCompSource->ComponentTags.Add(TEXT("AudioComponentSource"));
	AudioCompSource->SetupAttachment(RootSceneComponent);

	AudioCompVirtual = CreateDefaultSubobject<UAudioComponent>(TEXT("AudioComponentVirtual"));
	AudioCompVirtual->bAutoActivate = false;
	AudioCompVirtual->ComponentTags.Add(TEXT("AudioComponentVirtual"));
	AudioCompVirtual->SetupAttachment(RootSceneComponent);

	SpatialComp = CreateDefaultSubobject<USpatialAudioComponent>(TEXT("SpatialAudioComponent"));
}

void ASpatialAudioSourceActor::BeginPlay() {
	Super::BeginPlay();

	if (bDestroyOnAudioFinished) {
		AudioCompSource->OnAudioFinished.AddDynamic(this, &ASpatialAudioSourceActor::OnAudioFinished);
	}

	if (SoundWaveOverride) {
		AudioCompSource->SetWaveParameter(WaveParameterName, SoundWaveOverride);
	}

	AudioCompSource->Play();
}

void ASpatialAudioSourceActor::OnAudioFinished() {
	Destroy();
}
