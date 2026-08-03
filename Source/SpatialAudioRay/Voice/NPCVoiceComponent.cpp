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
	// Rows without a wave are dropped here so the scheduler never null-checks mid-selection.
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

		// Duration is the only end-of-line signal the scheduler has, so a stale CSV would
		// silently truncate every following line or leave dead air.
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
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();

	const UNPCVoiceSettings& S = GetSettings();

	FNPCVoiceAcousticState Acoustic;
	Acoustic.Occlusion = Spatial->CurrentOcclusion;
	Acoustic.DirectDistanceCm =
		static_cast<float>(FVector::Dist(Owner->GetActorLocation(), ListenerPos));
	// Same threshold that calls the listener hidden, so effort and content never disagree
	// about whether the detour is real.
	Acoustic.EffectiveDistanceCm =
		Spatial->GetEffectiveAcousticDistance(ListenerPos, S.OcclusionShiftThreshold);

	VoiceLogic::AdvanceBucketHysteresis(
		BucketState, VoiceLogic::MapToBucket(Acoustic.EffectiveDistanceCm, S), Now, S.BucketDwellTime);

	// Not dwelled: a delayed reaction misses the moment it reacts to, and the occlusion
	// behind it is already smoothed. Stamped before the state below reads the timer, so a
	// crossing this tick opens its own window.
	const ENPCVoiceSightChange SightChange = VoiceLogic::AdvanceSightState(
		SightState, VoiceLogic::IsListenerHidden(Acoustic, S), Now);
	Acoustic.bSightReactionPending = VoiceLogic::IsSightReactionPending(SightState, Now, S);

	TickSightReaction(Now, SightChange);
	TickScheduler(Now, Acoustic);

	if (bShowDebugText) {
		DrawDebugText(Acoustic, Now);
	}
}

void UNPCVoiceComponent::TickSightReaction(float Now, ENPCVoiceSightChange SightChange) {
	const UNPCVoiceSettings& S = GetSettings();
	const VoiceLogic::FBargeInDecision Decision = VoiceLogic::EvaluateBargeIn(
		Playback, Transition, BucketState.Committed, SightChange, BargeInAvailability, Now, S);
	if (!Decision.ShouldBargeIn()) {
		// Nothing to interrupt. Normal silence outlasts the reaction window, so a crossing
		// between lines has to pull the next one forward instead.
		if (SightChange != ENPCVoiceSightChange::None && !Playback.bPlaying && !Transition.bPending) {
			VoiceLogic::PullInNextLine(Playback, Now, S);
		}
		return;
	}
	// No usable line for that reason, so leave the current one running rather than cut out.
	const int32 LineIdx = VoiceLogic::FindBargeInLine(
		Lines, VoiceLogic::BargeInCategory(Decision.Reason), Decision.Dir, BucketState.Committed,
		Now, CooldownUntil);
	if (LineIdx == INDEX_NONE) {
		return;
	}
	if (UAudioComponent* Audio = VoiceAudio.Get()) {
		Audio->FadeOut(S.TransitionFadeOutTime, 0.f);
	}
	VoiceLogic::BeginBargeIn(Playback, Transition, LineIdx, Now, S);
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
	// The pending check keeps a cut line's stale NextLineTime (already in the past) from
	// racing a normal line in ahead of the barge-in it was cut for.
	if (!Playback.bPlaying && !Transition.bPending && Now >= Playback.NextLineTime) {
		SelectAndPlayLine(Now, Acoustic);
	}
}

void UNPCVoiceComponent::SelectAndPlayLine(float Now, const FNPCVoiceAcousticState& Acoustic) {
	const UNPCVoiceSettings& S = GetSettings();
	const int32 LineIdx = VoiceLogic::SelectLineIndex(Lines, BucketState.Committed, Acoustic,
	                                                  Playback.LastLineId, Now, CooldownUntil, S);
	if (LineIdx == INDEX_NONE) {
		Playback.NextLineTime = Now + VoiceLogic::ResolveNextLineDelay(false, S);
		return;
	}
	PlayLine(Lines[LineIdx], Now, /*bAsBargeIn=*/false);
}

void UNPCVoiceComponent::PlayLine(const FNPCVoiceRuntimeLine& Line, float Now, bool bAsBargeIn) {
	UAudioComponent* Audio = VoiceAudio.Get();
	if (!Audio) {
		return;
	}
	const UNPCVoiceSettings& S = GetSettings();
	// Before Play, or the line starts a frame at the previous effort's reach.
	if (USpatialAudioComponent* Spatial = SpatialAudio.Get()) {
		Spatial->SetAttenuationOuterRadius(S.GetEffortReachDistance(Line.Row.Bucket));
	}
	// Both must land before Play for MetaSound initialization to see them. Once per line,
	// not per frame: a line's effort never changes mid-flight.
	Audio->SetWaveParameter(WaveParameterName, Line.Wave);
	Audio->SetFloatParameter(EffortGainParameterName, S.GetEffortGainDb(Line.Row.Bucket));
	Audio->Play();

	VoiceLogic::BeginLine(Playback, Line.Row, Now, S.LineEndPadding, bAsBargeIn);
	VoiceLogic::StampCooldown(CooldownUntil, Line.Row.CooldownGroup, Now, S.CooldownGroupSeconds);
	// Both scheduling paths land here, so the NPC remarks on a crossing exactly once.
	VoiceLogic::MarkSightReactionDelivered(SightState, Line.Row.Category);
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
	// Own row: inlining the longest field pushed the numbers off screen at video-readable
	// scales. Single-frame messages, so it clears itself when the NPC goes quiet.
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
	const TArray<ENPCVoiceCategory, TInlineAllocator<4>> Preference =
		VoiceLogic::ResolveCategoryPreference(Acoustic, GetSettings());

	FString Line = FString::Printf(
		TEXT("VOICE dist=%.1fm eff=%.1fm detour=%.2fx occ=%.2f -> %s / %s"),
		Acoustic.DirectDistanceCm / 100.f, Acoustic.EffectiveDistanceCm / 100.f,
		Acoustic.DetourRatio(), Acoustic.Occlusion,
		*EffortEnum->GetNameStringByValue(static_cast<int64>(BucketState.Committed)),
		*CategoryEnum->GetNameStringByValue(static_cast<int64>(Preference[0])));
	if (BucketState.Candidate != BucketState.Committed) {
		Line += FString::Printf(
			TEXT(" (candidate=%s dwell %.1f/%.1fs)"),
			*EffortEnum->GetNameStringByValue(static_cast<int64>(BucketState.Candidate)),
			Now - BucketState.CandidateSince, GetSettings().BucketDwellTime);
	}
	// "spent" marks a window the NPC has already remarked on, which is what the ladder
	// reads. Raw age alone would explain the wrong thing.
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
	// Post-clamp reach from the component, not the settings band: a mis-tuned band needs
	// the distance the sound actually dies at.
	const USpatialAudioComponent* Spatial = SpatialAudio.Get();
	return FString::Printf(
		TEXT("  playing %s @%s%s reach %.1fm %+.0fdB (%.1fs left)"),
		*Playback.ActiveLineId.ToString(),
		*EffortEnum->GetNameStringByValue(static_cast<int64>(Playback.ActiveBucket)),
		Playback.bActiveIsBargeIn ? TEXT(" [barge-in]") : TEXT(""),
		Spatial ? Spatial->GetAttenuationOuterRadius() / 100.f : 0.f,
		GetSettings().GetEffortGainDb(Playback.ActiveBucket),
		Playback.EndTime - Now);
}
