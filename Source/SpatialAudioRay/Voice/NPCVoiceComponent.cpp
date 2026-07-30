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

		// The wave overrules the manifest: Row.Duration is the scheduler's only end-of-line
		// signal, so a re-render that never made it back into the CSV would silently truncate
		// every following line or leave dead air — invisible until it ruins a take.
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
	// Same threshold that decides the listener is hidden, so effort and content agree about
	// whether the detour is real: while the NPC is still saying "I can see you" lines, its
	// effort must not be driven by a route around a corner the listener can see past.
	Acoustic.EffectiveDistanceCm =
		Spatial->GetEffectiveAcousticDistance(ListenerPos, S.OcclusionShiftThreshold);

	VoiceLogic::AdvanceBucketHysteresis(
		BucketState, VoiceLogic::MapToBucket(Acoustic.EffectiveDistanceCm, S), Now, S.BucketDwellTime);

	// Crossings fire instantly rather than dwelled: the occlusion behind them is already
	// smoothed and threshold-gated, and a delayed reaction misses the moment it is reacting to.
	// The barge-in rate limit absorbs any residual chatter. Stamped before the acoustic state
	// reads the timer, so a crossing this tick opens its own reaction window immediately.
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
		// Nothing was interrupted. If sight just changed and the NPC happens to be between
		// lines, the reaction has to come from scheduling one sooner — the normal silence
		// outlasts the window the LostSight / SightRegained content is gated on.
		if (SightChange != ENPCVoiceSightChange::None && !Playback.bPlaying && !Transition.bPending) {
			VoiceLogic::PullInNextLine(Playback, Now, S);
		}
		return;
	}
	// Nothing usable for that reason (all on cooldown, or the bank lacks it) — leave the line
	// running rather than cutting to silence.
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
	// Effort sets audible reach, derived from its own distance band (whisper carries meters,
	// shout carries the map) — applied before Play so the new line never starts a frame at
	// the previous line's range.
	if (USpatialAudioComponent* Spatial = SpatialAudio.Get()) {
		Spatial->SetAttenuationOuterRadius(S.GetEffortReachDistance(Line.Row.Bucket));
	}
	// Both params must land before Play so MetaSound initialization picks them up (same
	// contract as the spatial component's wave override). Gain is set once per line rather
	// than per frame — a line's effort never changes mid-flight.
	Audio->SetWaveParameter(WaveParameterName, Line.Wave);
	Audio->SetFloatParameter(EffortGainParameterName, S.GetEffortGainDb(Line.Row.Bucket));
	Audio->Play();

	VoiceLogic::BeginLine(Playback, Line.Row, Now, S.LineEndPadding, bAsBargeIn);
	VoiceLogic::StampCooldown(CooldownUntil, Line.Row.CooldownGroup, Now, S.CooldownGroupSeconds);
	// Both scheduling paths land here, so a reaction settles its crossing whether it arrived as
	// a barge-in or as an ordinary line — the NPC remarks on losing or regaining sight once.
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
	// The spoken words get their own row: it is by far the longest field, and inlining it
	// pushed the numbers off the right edge at any text scale worth reading on video. Added
	// only while a line is actually playing — these messages last a single frame unless
	// re-added, so it clears itself the moment the NPC goes quiet.
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
	// Only while it is actually gating content. Once the reaction has been spoken the flag is
	// what the ladder reads, so showing the raw age would explain the wrong thing — spent says
	// "the window is open but the NPC has already remarked on it".
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
	// Reach comes from the spatial component, not from the settings: it is the post-clamp
	// distance the sound actually dies at (the inner radius floors it, the asset's own range
	// caps it), which is what a mis-tuned band needs to show.
	const USpatialAudioComponent* Spatial = SpatialAudio.Get();
	// The spoken words are deliberately not here — DrawDebugText gives them their own row.
	return FString::Printf(
		TEXT("  playing %s @%s%s reach %.1fm %+.0fdB (%.1fs left)"),
		*Playback.ActiveLineId.ToString(),
		*EffortEnum->GetNameStringByValue(static_cast<int64>(Playback.ActiveBucket)),
		Playback.bActiveIsBargeIn ? TEXT(" [barge-in]") : TEXT(""),
		Spatial ? Spatial->GetAttenuationOuterRadius() / 100.f : 0.f,
		GetSettings().GetEffortGainDb(Playback.ActiveBucket),
		Playback.EndTime - Now);
}
