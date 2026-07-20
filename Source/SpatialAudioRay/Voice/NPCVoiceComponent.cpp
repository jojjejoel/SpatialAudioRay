#include "NPCVoiceComponent.h"

#include "Components/AudioComponent.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "NPCVoiceSettings.h"
#include "Sound/SoundWave.h"
#include "SpatialAudioComponent.h"
#include "SpatialAudioRayModule.h"

UNPCVoiceComponent::UNPCVoiceComponent() {
	PrimaryComponentTick.bCanEverTick = true;
}

const UNPCVoiceSettings& UNPCVoiceComponent::GetSettings() const {
	return _Settings ? *_Settings : *GetMutableDefault<UNPCVoiceSettings>();
}

void UNPCVoiceComponent::BeginPlay() {
	Super::BeginPlay();
	ResolveOwnerComponents();
	LoadBank();

	const UNPCVoiceSettings& S = GetSettings();
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	NextLineTime = Now + FMath::RandRange(S.LineIntervalMin, S.LineIntervalMax);
}

void UNPCVoiceComponent::ResolveOwnerComponents() {
	AActor* Owner = GetOwner();
	if (!Owner) {
		return;
	}
	SpatialAudio = Owner->FindComponentByClass<USpatialAudioComponent>();
	VoiceAudio = Owner->FindComponentByTag<UAudioComponent>(VoiceAudioComponentTag);

	if (!SpatialAudio.IsValid() || !VoiceAudio.IsValid()) {
		UE_LOG(LogSpatialAudio, Warning,
		       TEXT("NPCVoice on %s: missing %s — voice disabled"),
		       *Owner->GetName(),
		       !SpatialAudio.IsValid() ? TEXT("USpatialAudioComponent") : *VoiceAudioComponentTag.ToString());
	}
}

void UNPCVoiceComponent::LoadBank() {
	Lines.Reset();
	if (!VoiceBank) {
		return;
	}
	// Sync load is fine at this scale (a small VO bank); rows without a resolvable wave are
	// dropped here so the scheduler never has to null-check mid-selection.
	for (const TPair<FName, uint8*>& Pair : VoiceBank->GetRowMap()) {
		const FNPCVoiceLineRow* Row = reinterpret_cast<const FNPCVoiceLineRow*>(Pair.Value);
		if (!Row) {
			continue;
		}
		USoundWave* Wave = Row->Sound.LoadSynchronous();
		if (!Wave) {
			UE_LOG(LogSpatialAudio, Warning, TEXT("NPCVoice: row %s has no loadable Sound — skipped"),
			       *Pair.Key.ToString());
			continue;
		}
		FNPCVoiceRuntimeLine& Line = Lines.AddDefaulted_GetRef();
		Line.Row = *Row;
		Line.Wave = Wave;
	}
}

ENPCVoiceEffort UNPCVoiceComponent::MapToBucket(float DistanceCm, float Occlusion,
                                                const UNPCVoiceSettings& S) {
	ENPCVoiceEffort Bucket = ENPCVoiceEffort::Shout;
	if (DistanceCm <= S.WhisperMaxDistance) {
		Bucket = ENPCVoiceEffort::Whisper;
	}
	else if (DistanceCm <= S.ConversationalMaxDistance) {
		Bucket = ENPCVoiceEffort::Conversational;
	}
	else if (DistanceCm <= S.RaisedMaxDistance) {
		Bucket = ENPCVoiceEffort::Raised;
	}
	if (Occlusion >= S.OcclusionShiftThreshold) {
		Bucket = static_cast<ENPCVoiceEffort>(
			FMath::Min(static_cast<int32>(Bucket) + 1, static_cast<int32>(ENPCVoiceEffort::Shout)));
	}
	return Bucket;
}

void UNPCVoiceComponent::UpdateBucket(float Now, float DistanceCm, float Occlusion) {
	const ENPCVoiceEffort Mapped = MapToBucket(DistanceCm, Occlusion, GetSettings());
	if (!bBucketInitialized) {
		CurrentBucket = Mapped;
		CandidateBucket = Mapped;
		bBucketInitialized = true;
		return;
	}
	if (Mapped != CandidateBucket) {
		CandidateBucket = Mapped;
		CandidateSince = Now;
	}
	if (CandidateBucket != CurrentBucket && Now - CandidateSince >= GetSettings().BucketDwellTime) {
		CurrentBucket = CandidateBucket;
	}
}

void UNPCVoiceComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                       FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	USpatialAudioComponent* Spatial = SpatialAudio.Get();
	if (!World || !Owner || !Spatial || !VoiceAudio.IsValid() || Lines.Num() == 0) {
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		return;
	}

	const float Now = World->GetTimeSeconds();
	const float DistanceCm =
		static_cast<float>(FVector::Dist(Owner->GetActorLocation(), PC->GetPawn()->GetActorLocation()));
	const float Occlusion = Spatial->CurrentOcclusion;

	UpdateBucket(Now, DistanceCm, Occlusion);

	if (bLinePlaying && Now >= LineEndTime) {
		bLinePlaying = false;
		ActiveLineId = NAME_None;
		ActiveText.Reset();
		const UNPCVoiceSettings& S = GetSettings();
		NextLineTime = Now + FMath::RandRange(S.LineIntervalMin, S.LineIntervalMax);
	}
	if (!bLinePlaying && Now >= NextLineTime) {
		SelectAndPlayLine(Now, Occlusion);
	}

	if (bShowDebugText) {
		DrawDebugText(DistanceCm, Occlusion, Now);
	}
}

void UNPCVoiceComponent::SelectAndPlayLine(float Now, float Occlusion) {
	const UNPCVoiceSettings& S = GetSettings();
	const ENPCVoiceCategory Desired =
		Occlusion >= S.OcclusionShiftThreshold ? ENPCVoiceCategory::Occluded : ENPCVoiceCategory::Clear;
	const ENPCVoiceCategory Fallback =
		Desired == ENPCVoiceCategory::Clear ? ENPCVoiceCategory::Occluded : ENPCVoiceCategory::Clear;

	auto Gather = [&](ENPCVoiceCategory Category, bool bAllowRepeat) {
		TArray<int32> Pool;
		for (int32 i = 0; i < Lines.Num(); ++i) {
			const FNPCVoiceLineRow& Row = Lines[i].Row;
			if (Row.Bucket != CurrentBucket || Row.Category != Category) {
				continue;
			}
			if (!bAllowRepeat && Row.LineId == LastLineId) {
				continue;
			}
			if (const float* Until = CooldownUntil.Find(Row.CooldownGroup);
				Until && !Row.CooldownGroup.IsNone() && Now < *Until) {
				continue;
			}
			Pool.Add(i);
		}
		return Pool;
	};

	// Category preference, then no-repeat, are soft constraints — a one-line bucket should
	// still speak rather than fall silent.
	TArray<int32> Pool = Gather(Desired, false);
	if (Pool.IsEmpty()) { Pool = Gather(Fallback, false); }
	if (Pool.IsEmpty()) { Pool = Gather(Desired, true); }
	if (Pool.IsEmpty()) { Pool = Gather(Fallback, true); }
	if (Pool.IsEmpty()) {
		NextLineTime = Now + FMath::RandRange(S.LineIntervalMin, S.LineIntervalMax);
		return;
	}

	const FNPCVoiceRuntimeLine& Line = Lines[Pool[FMath::RandRange(0, Pool.Num() - 1)]];
	UAudioComponent* Audio = VoiceAudio.Get();
	// Wave param must land before Play so MetaSound initialization picks it up (same
	// contract as the spatial component's wave override).
	Audio->SetWaveParameter(WaveParameterName, Line.Wave);
	Audio->Play();

	bLinePlaying = true;
	LineEndTime = Now + Line.Row.Duration + S.LineEndPadding;
	ActiveLineId = Line.Row.LineId;
	ActiveText = Line.Row.Text;
	LastLineId = Line.Row.LineId;
	if (!Line.Row.CooldownGroup.IsNone()) {
		CooldownUntil.Add(Line.Row.CooldownGroup, Now + S.CooldownGroupSeconds);
	}
}

void UNPCVoiceComponent::DrawDebugText(float DistanceCm, float Occlusion, float Now) const {
	if (!GEngine) {
		return;
	}
	const UEnum* EffortEnum = StaticEnum<ENPCVoiceEffort>();
	const uint64 Key = static_cast<uint64>(GetUniqueID()) * 10ull;

	FString Inputs = FString::Printf(
		TEXT("VOICE dist=%.1fm occ=%.2f -> bucket=%s"),
		DistanceCm / 100.f, Occlusion,
		*EffortEnum->GetNameStringByValue(static_cast<int64>(CurrentBucket)));
	if (CandidateBucket != CurrentBucket) {
		Inputs += FString::Printf(
			TEXT(" (candidate=%s dwell %.1f/%.1fs)"),
			*EffortEnum->GetNameStringByValue(static_cast<int64>(CandidateBucket)),
			Now - CandidateSince, GetSettings().BucketDwellTime);
	}
	GEngine->AddOnScreenDebugMessage(Key, 0.f, FColor::Cyan, Inputs);

	const FString State = bLinePlaying
		? FString::Printf(TEXT("  playing %s (%.1fs left) \"%s\""),
		                  *ActiveLineId.ToString(), LineEndTime - Now, *ActiveText)
		: FString::Printf(TEXT("  idle, next line in %.1fs"), FMath::Max(0.f, NextLineTime - Now));
	GEngine->AddOnScreenDebugMessage(Key + 1, 0.f, FColor::Cyan, State);
}
