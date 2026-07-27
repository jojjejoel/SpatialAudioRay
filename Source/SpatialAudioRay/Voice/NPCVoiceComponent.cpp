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
	bBankHasTransitions = Lines.ContainsByPredicate([](const FNPCVoiceRuntimeLine& L) {
		return L.Row.Category == ENPCVoiceCategory::Transition;
	});
}

ENPCVoiceEffort UNPCVoiceComponent::MapToBucket(float EffectiveDistanceCm,
                                                const UNPCVoiceSettings& S) {
	if (EffectiveDistanceCm <= S.WhisperMaxDistance) {
		return ENPCVoiceEffort::Whisper;
	}
	if (EffectiveDistanceCm <= S.ConversationalMaxDistance) {
		return ENPCVoiceEffort::Conversational;
	}
	if (EffectiveDistanceCm <= S.RaisedMaxDistance) {
		return ENPCVoiceEffort::Raised;
	}
	return ENPCVoiceEffort::Shout;
}

void UNPCVoiceComponent::UpdateBucket(float Now, float EffectiveDistanceCm) {
	const ENPCVoiceEffort Mapped = MapToBucket(EffectiveDistanceCm, GetSettings());
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
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();
	const float DistanceCm = static_cast<float>(FVector::Dist(Owner->GetActorLocation(), ListenerPos));
	const float EffectiveDistanceCm = Spatial->GetEffectiveAcousticDistance(ListenerPos);
	const float Occlusion = Spatial->CurrentOcclusion;

	UpdateBucket(Now, EffectiveDistanceCm);
	TickTransitionBargeIn(Now);

	if (bTransitionPending && Now >= TransitionPlayTime) {
		bTransitionPending = false;
		if (Lines.IsValidIndex(PendingTransitionLine)) {
			PlayLine(Lines[PendingTransitionLine], Now);
		}
		PendingTransitionLine = INDEX_NONE;
	}

	if (bLinePlaying && Now >= LineEndTime) {
		bLinePlaying = false;
		ActiveLineId = NAME_None;
		ActiveText.Reset();
		const UNPCVoiceSettings& S = GetSettings();
		// A transition announces a change — the full line at the new effort follows quickly
		// instead of waiting out the normal interval.
		NextLineTime = bActiveLineIsTransition
			? Now + S.PostTransitionLineDelay
			: Now + FMath::RandRange(S.LineIntervalMin, S.LineIntervalMax);
	}
	if (!bLinePlaying && !bTransitionPending && Now >= NextLineTime) {
		SelectAndPlayLine(Now, Occlusion);
	}

	if (bShowDebugText) {
		DrawDebugText(DistanceCm, EffectiveDistanceCm, Occlusion, Now);
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

	PlayLine(Lines[Pool[FMath::RandRange(0, Pool.Num() - 1)]], Now);
}

void UNPCVoiceComponent::PlayLine(const FNPCVoiceRuntimeLine& Line, float Now) {
	UAudioComponent* Audio = VoiceAudio.Get();
	if (!Audio) {
		return;
	}
	const UNPCVoiceSettings& S = GetSettings();
	// Effort sets audible reach (whisper carries meters, shout carries the map) — applied
	// before Play so the new line never starts a frame at the previous line's range.
	if (USpatialAudioComponent* Spatial = SpatialAudio.Get()) {
		Spatial->SetAttenuationFalloffScale(S.GetFalloffScale(Line.Row.Bucket));
	}
	// Wave param must land before Play so MetaSound initialization picks it up (same
	// contract as the spatial component's wave override).
	Audio->SetWaveParameter(WaveParameterName, Line.Wave);
	Audio->Play();
	bLinePlaying = true;
	LineEndTime = Now + Line.Row.Duration + S.LineEndPadding;
	ActiveLineId = Line.Row.LineId;
	ActiveText = Line.Row.Text;
	LastLineId = Line.Row.LineId;
	ActiveLineBucket = Line.Row.Bucket;
	bActiveLineIsTransition = Line.Row.Category == ENPCVoiceCategory::Transition;
	if (!Line.Row.CooldownGroup.IsNone()) {
		CooldownUntil.Add(Line.Row.CooldownGroup, Now + S.CooldownGroupSeconds);
	}
}

void UNPCVoiceComponent::TickTransitionBargeIn(float Now) {
	const UNPCVoiceSettings& S = GetSettings();
	if (!bLinePlaying || bActiveLineIsTransition || !bBankHasTransitions ||
		S.TransitionBucketDelta <= 0) {
		return;
	}
	const int32 Jump = FMath::Abs(
		static_cast<int32>(CurrentBucket) - static_cast<int32>(ActiveLineBucket));
	if (Jump < S.TransitionBucketDelta ||
		Now - LastTransitionTime < S.TransitionCooldownSeconds ||
		LineEndTime - Now < S.TransitionMinRemainingTime) {
		return;
	}
	const ENPCVoiceTransitionDir Dir =
		static_cast<int32>(CurrentBucket) > static_cast<int32>(ActiveLineBucket)
			? ENPCVoiceTransitionDir::Farther
			: ENPCVoiceTransitionDir::Closer;
	const int32 LineIdx = FindTransitionLine(Lines, Dir, CurrentBucket, Now, CooldownUntil);
	if (LineIdx == INDEX_NONE) {
		return;
	}

	if (UAudioComponent* Audio = VoiceAudio.Get()) {
		Audio->FadeOut(S.TransitionFadeOutTime, 0.f);
	}
	bLinePlaying = false;
	ActiveLineId = NAME_None;
	ActiveText.Reset();
	bTransitionPending = true;
	PendingTransitionLine = LineIdx;
	// Small margin past the declick fade so Play doesn't race the fade-stop.
	TransitionPlayTime = Now + S.TransitionFadeOutTime + 0.03f;
	// Stamped at trigger (not at playback) so a failed race can't re-fire every tick.
	LastTransitionTime = Now;
}

int32 UNPCVoiceComponent::FindTransitionLine(const TArray<FNPCVoiceRuntimeLine>& InLines,
                                             ENPCVoiceTransitionDir Dir, ENPCVoiceEffort TargetBucket,
                                             float Now, const TMap<FName, float>& Cooldowns) {
	int32 BestDelta = MAX_int32;
	TArray<int32> Pool;
	for (int32 i = 0; i < InLines.Num(); ++i) {
		const FNPCVoiceLineRow& Row = InLines[i].Row;
		if (Row.Category != ENPCVoiceCategory::Transition || Row.Direction != Dir) {
			continue;
		}
		if (const float* Until = Cooldowns.Find(Row.CooldownGroup);
			Until && !Row.CooldownGroup.IsNone() && Now < *Until) {
			continue;
		}
		const int32 Delta = FMath::Abs(
			static_cast<int32>(Row.Bucket) - static_cast<int32>(TargetBucket));
		if (Delta < BestDelta) {
			BestDelta = Delta;
			Pool.Reset();
		}
		if (Delta == BestDelta) {
			Pool.Add(i);
		}
	}
	return Pool.IsEmpty() ? INDEX_NONE : Pool[FMath::RandRange(0, Pool.Num() - 1)];
}

void UNPCVoiceComponent::DrawDebugText(float DistanceCm, float EffectiveDistanceCm,
                                       float Occlusion, float Now) const {
	if (!GEngine) {
		return;
	}
	const UEnum* EffortEnum = StaticEnum<ENPCVoiceEffort>();
	const uint64 Key = static_cast<uint64>(GetUniqueID()) * 10ull;

	FString Inputs = FString::Printf(
		TEXT("VOICE dist=%.1fm eff=%.1fm occ=%.2f -> bucket=%s"),
		DistanceCm / 100.f, EffectiveDistanceCm / 100.f, Occlusion,
		*EffortEnum->GetNameStringByValue(static_cast<int64>(CurrentBucket)));
	if (CandidateBucket != CurrentBucket) {
		Inputs += FString::Printf(
			TEXT(" (candidate=%s dwell %.1f/%.1fs)"),
			*EffortEnum->GetNameStringByValue(static_cast<int64>(CandidateBucket)),
			Now - CandidateSince, GetSettings().BucketDwellTime);
	}
	GEngine->AddOnScreenDebugMessage(Key, 0.f, FColor::Cyan, Inputs);

	const FString State = bTransitionPending
		? TEXT("  transition barge-in pending")
		: bLinePlaying
		? FString::Printf(TEXT("  playing %s @%s%s reach x%.2f (%.1fs left) \"%s\""),
		                  *ActiveLineId.ToString(),
		                  *EffortEnum->GetNameStringByValue(static_cast<int64>(ActiveLineBucket)),
		                  bActiveLineIsTransition ? TEXT(" [transition]") : TEXT(""),
		                  GetSettings().GetFalloffScale(ActiveLineBucket),
		                  LineEndTime - Now, *ActiveText)
		: FString::Printf(TEXT("  idle, next line in %.1fs"), FMath::Max(0.f, NextLineTime - Now));
	GEngine->AddOnScreenDebugMessage(Key + 1, 0.f, FColor::Cyan, State);
}
