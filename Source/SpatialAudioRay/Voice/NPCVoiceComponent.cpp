#include "NPCVoiceComponent.h"

#include "Components/AudioComponent.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "NPCVoiceLogic.h"
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

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	Playback.NextLineTime = Now + VoiceLogic::ResolveNextLineDelay(false, GetSettings());
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

		Line.Row.Duration = VoiceLogic::ResolveLineDuration(Row->Duration, Wave->Duration);
		if (!FMath::IsNearlyEqual(Line.Row.Duration, Row->Duration,
		                          VoiceLogic::DurationMismatchTolerance)) {
			UE_LOG(LogSpatialAudio, Warning,
			       TEXT("NPCVoice: row %s declares %.2fs but its wave is %.2fs — using the wave. "
				       "Re-export the bank CSV."),
			       *Pair.Key.ToString(), Row->Duration, Line.Row.Duration);
		}
	}
	BargeInAvailability = VoiceLogic::ResolveBargeInAvailability(Lines);
}

void UNPCVoiceComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                       FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	USpatialAudioComponent* Spatial = SpatialAudio.Get();
	if (!World || !Owner || !Spatial || !VoiceAudio.IsValid() || Lines.IsEmpty()) {
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		return;
	}

	const float Now = World->GetTimeSeconds();
	const UNPCVoiceSettings& Settings = GetSettings();

	FNPCVoiceAcousticState Acoustic =
		SampleAcousticState(*Spatial, PC->GetPawn()->GetActorLocation());

	VoiceLogic::AdvanceEffortHysteresis(EffortState,
	                                    VoiceLogic::MapToEffort(Acoustic.EffectiveDistanceCm, Settings),
	                                    Now, Settings.EffortDwellTime);

	const ENPCVoiceSightChange SightChange = VoiceLogic::AdvanceSightState(
		SightState, VoiceLogic::IsListenerHidden(Acoustic, Settings), Now);
	Acoustic.bSightReactionPending = VoiceLogic::IsSightReactionPending(SightState, Now, Settings);

	TickSightReaction(Now, SightChange);
	TickScheduler(Now, Acoustic);

	if (bShowDebugText) {
		DrawDebugText(Acoustic, Now);
	}
}

FNPCVoiceAcousticState UNPCVoiceComponent::SampleAcousticState(const USpatialAudioComponent& Spatial,
                                                               const FVector& ListenerPos) const {
	FNPCVoiceAcousticState Acoustic;
	Acoustic.Occlusion = Spatial.CurrentOcclusion;
	Acoustic.DirectDistanceCm =
		static_cast<float>(FVector::Dist(GetOwner()->GetActorLocation(), ListenerPos));
	Acoustic.EffectiveDistanceCm =
		Spatial.GetEffectiveAcousticDistance(ListenerPos, GetSettings().OcclusionShiftThreshold);
	return Acoustic;
}

void UNPCVoiceComponent::TickSightReaction(float Now, ENPCVoiceSightChange SightChange) {
	const UNPCVoiceSettings& Settings = GetSettings();
	const VoiceLogic::FBargeInDecision Decision = VoiceLogic::EvaluateBargeIn(
		Playback, Transition, EffortState.Committed, SightChange, BargeInAvailability, Now, Settings);
	if (!Decision.ShouldBargeIn()) {
		if (SightChange != ENPCVoiceSightChange::None && !Playback.bPlaying && !Transition.bPending) {
			VoiceLogic::PullInNextLine(Playback, Now, Settings);
		}
		return;
	}
	const int32 LineIdx = VoiceLogic::FindBargeInLine(
		Lines, VoiceLogic::BargeInCategory(Decision.Reason), Decision.Dir, EffortState.Committed,
		Now, CooldownUntil);
	if (LineIdx == INDEX_NONE) {
		return;
	}
	if (UAudioComponent* Audio = VoiceAudio.Get()) {
		Audio->FadeOut(Settings.TransitionFadeOutTime, 0.f);
	}
	VoiceLogic::BeginBargeIn(Playback, Transition, LineIdx, Now, Settings);
}

void UNPCVoiceComponent::TickScheduler(float Now, const FNPCVoiceAcousticState& Acoustic) {
	if (Transition.bPending && Now >= Transition.PlayTime) {
		Transition.bPending = false;
		if (Lines.IsValidIndex(Transition.PendingLine)) {
			PlayLine(Lines[Transition.PendingLine], Now, /*bAsBargeIn=*/true);
		}
		Transition.PendingLine = INDEX_NONE;
	}
	if (Playback.bPlaying && Now >= Playback.EndTime) {
		VoiceLogic::EndLine(Playback, Now, GetSettings());
	}
	if (!Playback.bPlaying && !Transition.bPending && Now >= Playback.NextLineTime) {
		SelectAndPlayLine(Now, Acoustic);
	}
}

void UNPCVoiceComponent::SelectAndPlayLine(float Now, const FNPCVoiceAcousticState& Acoustic) {
	const UNPCVoiceSettings& Settings = GetSettings();
	const int32 LineIdx = VoiceLogic::SelectLineIndex(Lines, EffortState.Committed, Acoustic,
	                                                  Playback.LastLineId, Now, CooldownUntil, Settings);
	if (LineIdx == INDEX_NONE) {
		Playback.NextLineTime = Now + VoiceLogic::ResolveNextLineDelay(false, Settings);
		return;
	}
	PlayLine(Lines[LineIdx], Now, /*bAsBargeIn=*/false);
}

void UNPCVoiceComponent::PlayLine(const FNPCVoiceRuntimeLine& Line, float Now, bool bAsBargeIn) {
	UAudioComponent* Audio = VoiceAudio.Get();
	if (!Audio) {
		return;
	}
	const UNPCVoiceSettings& Settings = GetSettings();
	ApplyEffortParametersBeforePlay(*Audio, Line.Row.Bucket, Line.Wave);
	Audio->Play();

	VoiceLogic::BeginLine(Playback, Line.Row, Now, Settings.LineEndPadding, bAsBargeIn);
	VoiceLogic::StampCooldown(CooldownUntil, Line.Row.CooldownGroup, Now,
	                          Settings.CooldownGroupSeconds);
	VoiceLogic::MarkSightReactionDelivered(SightState, Line.Row.Category);
}

void UNPCVoiceComponent::ApplyEffortParametersBeforePlay(UAudioComponent& Audio,
                                                         ENPCVoiceEffort Effort,
                                                         USoundWave* Wave) {
	const UNPCVoiceSettings& Settings = GetSettings();
	if (USpatialAudioComponent* Spatial = SpatialAudio.Get()) {
		Spatial->SetAttenuationOuterRadius(Settings.GetEffortReachDistance(Effort));
	}
	Audio.SetWaveParameter(WaveParameterName, Wave);
	Audio.SetFloatParameter(EffortGainParameterName, Settings.GetEffortGainDb(Effort));
}

void UNPCVoiceComponent::DrawDebugText(const FNPCVoiceAcousticState& Acoustic, float Now) const {
	if (!GEngine) {
		return;
	}
	const uint64 Key = static_cast<uint64>(GetUniqueID()) * 10ull;
	const float Scale = GetSettings().DebugTextScale;
	const FVector2D TextScale(Scale, Scale);
	GEngine->AddOnScreenDebugMessage(Key, 0.f, FColor::Cyan, BuildDebugInputsLine(Acoustic, Now),
	                                 /*bNewerOnTop=*/true, TextScale);
	GEngine->AddOnScreenDebugMessage(Key + 1, 0.f, FColor::Cyan, BuildDebugStateLine(Now),
	                                 /*bNewerOnTop=*/true, TextScale);
	if (Playback.bPlaying && !Playback.ActiveText.IsEmpty()) {
		GEngine->AddOnScreenDebugMessage(Key + 2, 0.f, FColor::White,
		                                 FString::Printf(TEXT("  \"%s\""), *Playback.ActiveText),
		                                 /*bNewerOnTop=*/true, TextScale);
	}
}

FString UNPCVoiceComponent::BuildDebugInputsLine(const FNPCVoiceAcousticState& Acoustic,
                                                 float Now) const {
	const UEnum* EffortEnum = StaticEnum<ENPCVoiceEffort>();
	const UEnum* CategoryEnum = StaticEnum<ENPCVoiceCategory>();
	const VoiceLogic::FCategoryPreference Preference =
		VoiceLogic::ResolveCategoryPreference(Acoustic, GetSettings());

	FString Line = FString::Printf(
		TEXT("VOICE dist=%.1fm eff=%.1fm detour=%.2fx occ=%.2f -> %s / %s"),
		Acoustic.DirectDistanceCm / 100.f, Acoustic.EffectiveDistanceCm / 100.f,
		Acoustic.DetourRatio(), Acoustic.Occlusion,
		*EffortEnum->GetNameStringByValue(static_cast<int64>(EffortState.Committed)),
		*CategoryEnum->GetNameStringByValue(static_cast<int64>(Preference[0])));
	if (EffortState.Candidate != EffortState.Committed) {
		Line += FString::Printf(
			TEXT(" (candidate=%s dwell %.1f/%.1fs)"),
			*EffortEnum->GetNameStringByValue(static_cast<int64>(EffortState.Candidate)),
			Now - EffortState.CandidateSince, GetSettings().EffortDwellTime);
	}
	const float SinceChange = Now - SightState.LastChangeTime;
	if (SinceChange <= GetSettings().SightChangeReactionWindow) {
		Line += FString::Printf(TEXT(" (%s %.1fs ago%s)"),
		                        SightState.bHidden ? TEXT("hidden") : TEXT("seen"), SinceChange,
		                        Acoustic.bSightReactionPending ? TEXT("") : TEXT(", spent"));
	}
	return Line;
}

FString UNPCVoiceComponent::BuildDebugStateLine(float Now) const {
	if (Transition.bPending) {
		return TEXT("  transition barge-in pending");
	}
	if (!Playback.bPlaying) {
		return FString::Printf(TEXT("  idle, next line in %.1fs"),
		                       FMath::Max(0.f, Playback.NextLineTime - Now));
	}
	const UEnum* EffortEnum = StaticEnum<ENPCVoiceEffort>();
	const USpatialAudioComponent* Spatial = SpatialAudio.Get();
	return FString::Printf(
		TEXT("  playing %s @%s%s reach %.1fm %+.0fdB (%.1fs left)"),
		*Playback.ActiveLineId.ToString(),
		*EffortEnum->GetNameStringByValue(static_cast<int64>(Playback.ActiveEffort)),
		Playback.bActiveIsBargeIn ? TEXT(" [barge-in]") : TEXT(""),
		Spatial ? Spatial->GetAttenuationOuterRadius() / 100.f : 0.f,
		GetSettings().GetEffortGainDb(Playback.ActiveEffort),
		Playback.EndTime - Now);
}
