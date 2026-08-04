#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceSettings.h"
#include "NPCVoiceState.h"

namespace VoiceLogic {
	constexpr float TransitionPlayMargin = 0.03f;

	constexpr float DurationMismatchTolerance = 0.05f;

	inline ENPCVoiceCategory BargeInCategory(ENPCVoiceBargeInReason Reason) {
		switch (Reason) {
		case ENPCVoiceBargeInReason::SightLost: return ENPCVoiceCategory::LostSight;
		case ENPCVoiceBargeInReason::SightGained: return ENPCVoiceCategory::SightRegained;
		default: return ENPCVoiceCategory::Transition;
		}
	}

	inline FNPCVoiceBargeInAvailability ResolveBargeInAvailability(
		const TArray<FNPCVoiceRuntimeLine>& Lines) {
		FNPCVoiceBargeInAvailability Available;
		for (const FNPCVoiceRuntimeLine& Line : Lines) {
			switch (Line.Row.Category) {
			case ENPCVoiceCategory::Transition: Available.bTransition = true;
				break;
			case ENPCVoiceCategory::LostSight: Available.bLostSight = true;
				break;
			case ENPCVoiceCategory::SightRegained: Available.bSightRegained = true;
				break;
			default: break;
			}
		}
		return Available;
	}

	inline float ResolveLineDuration(float RowDuration, float WaveDuration) {
		return WaveDuration > 0.f ? WaveDuration : RowDuration;
	}

	inline ENPCVoiceEffort MapToEffort(float EffectiveDistanceCm, const UNPCVoiceSettings& Settings) {
		if (EffectiveDistanceCm <= Settings.WhisperMaxDistance) {
			return ENPCVoiceEffort::Whisper;
		}
		if (EffectiveDistanceCm <= Settings.ConversationalMaxDistance) {
			return ENPCVoiceEffort::Conversational;
		}
		if (EffectiveDistanceCm <= Settings.RaisedMaxDistance) {
			return ENPCVoiceEffort::Raised;
		}
		return ENPCVoiceEffort::Shout;
	}

	inline void AdvanceEffortHysteresis(FNPCVoiceEffortHysteresis& State, ENPCVoiceEffort Mapped,
	                                    float Now, float DwellTime) {
		if (!State.bInitialized) {
			State.bInitialized = true;
			State.Committed = Mapped;
			State.Candidate = Mapped;
			State.CandidateSince = Now;
			return;
		}
		if (Mapped != State.Candidate) {
			State.Candidate = Mapped;
			State.CandidateSince = Now;
		}
		if (State.Candidate != State.Committed && Now - State.CandidateSince >= DwellTime) {
			State.Committed = State.Candidate;
		}
	}

	inline bool IsListenerHidden(const FNPCVoiceAcousticState& Acoustic,
	                             const UNPCVoiceSettings& Settings) {
		return Acoustic.Occlusion >= Settings.OcclusionShiftThreshold;
	}

	inline bool IsPathPartiallyBlocked(const FNPCVoiceAcousticState& Acoustic,
	                                   const UNPCVoiceSettings& Settings) {
		return Settings.PartialOcclusionThreshold > 0.f &&
			Acoustic.Occlusion >= Settings.PartialOcclusionThreshold;
	}

	inline ENPCVoiceSightChange AdvanceSightState(FNPCVoiceSightState& State, bool bHidden,
	                                              float Now) {
		if (!State.bInitialized) {
			State.bInitialized = true;
			State.bHidden = bHidden;
			return ENPCVoiceSightChange::None;
		}
		if (bHidden == State.bHidden) {
			return ENPCVoiceSightChange::None;
		}
		State.bHidden = bHidden;
		State.LastChangeTime = Now;
		State.bReactionDelivered = false;
		return bHidden ? ENPCVoiceSightChange::Lost : ENPCVoiceSightChange::Gained;
	}

	inline bool IsSightReactionCategory(ENPCVoiceCategory Category) {
		return Category == ENPCVoiceCategory::LostSight ||
			Category == ENPCVoiceCategory::SightRegained;
	}

	inline bool IsSightReactionPending(const FNPCVoiceSightState& State, float Now,
	                                   const UNPCVoiceSettings& Settings) {
		return !State.bReactionDelivered &&
			Now - State.LastChangeTime <= Settings.SightChangeReactionWindow;
	}

	inline void MarkSightReactionDelivered(FNPCVoiceSightState& State, ENPCVoiceCategory Category) {
		if (IsSightReactionCategory(Category)) {
			State.bReactionDelivered = true;
		}
	}

	inline bool IsCooldownBlocked(FName Group, float Now, const TMap<FName, float>& Cooldowns) {
		if (Group.IsNone()) {
			return false;
		}
		const float* Until = Cooldowns.Find(Group);
		return Until && Now < *Until;
	}

	inline void StampCooldown(TMap<FName, float>& Cooldowns, FName Group, float Now, float Seconds) {
		if (!Group.IsNone()) {
			Cooldowns.Add(Group, Now + Seconds);
		}
	}

	inline TArray<int32> GatherCandidates(const TArray<FNPCVoiceRuntimeLine>& Lines,
	                                      ENPCVoiceEffort Effort, ENPCVoiceCategory Category,
	                                      bool bAllowRepeat, FName LastLineId, float Now,
	                                      const TMap<FName, float>& Cooldowns) {
		TArray<int32> Pool;
		for (int32 i = 0; i < Lines.Num(); ++i) {
			const FNPCVoiceLineRow& Row = Lines[i].Row;
			if (Row.Bucket != Effort || Row.Category != Category) {
				continue;
			}
			if (!bAllowRepeat && Row.LineId == LastLineId) {
				continue;
			}
			if (IsCooldownBlocked(Row.CooldownGroup, Now, Cooldowns)) {
				continue;
			}
			Pool.Add(i);
		}
		return Pool;
	}

	using FCategoryPreference = TArray<ENPCVoiceCategory, TInlineAllocator<4>>;

	inline void AppendVisibleCategories(FCategoryPreference& Allowed,
	                                    const FNPCVoiceAcousticState& Acoustic,
	                                    const UNPCVoiceSettings& Settings) {
		if (Acoustic.bSightReactionPending) {
			Allowed.Add(ENPCVoiceCategory::SightRegained);
		}
		if (IsPathPartiallyBlocked(Acoustic, Settings)) {
			Allowed.Add(ENPCVoiceCategory::PartiallyOccluded);
		}
		Allowed.Add(ENPCVoiceCategory::Clear);
	}

	inline void AppendHiddenCategories(FCategoryPreference& Allowed,
	                                   const FNPCVoiceAcousticState& Acoustic,
	                                   const UNPCVoiceSettings& Settings) {
		if (Acoustic.bSightReactionPending) {
			Allowed.Add(ENPCVoiceCategory::LostSight);
		}
		const float Detour = Acoustic.DetourRatio();
		if (Acoustic.DirectDistanceCm <= Settings.BehindWallMaxDirectDistance &&
			Detour >= Settings.BehindWallMinDetourRatio) {
			Allowed.Add(ENPCVoiceCategory::BehindWall);
		}
		else if (Detour <= Settings.AroundCornerMaxDetourRatio) {
			Allowed.Add(ENPCVoiceCategory::AroundCorner);
		}
		Allowed.Add(ENPCVoiceCategory::Occluded);
	}

	inline FCategoryPreference ResolveCategoryPreference(const FNPCVoiceAcousticState& Acoustic,
	                                                     const UNPCVoiceSettings& Settings) {
		FCategoryPreference Allowed;
		if (IsListenerHidden(Acoustic, Settings)) {
			AppendHiddenCategories(Allowed, Acoustic, Settings);
		}
		else {
			AppendVisibleCategories(Allowed, Acoustic, Settings);
		}
		return Allowed;
	}

	inline int32 SelectLineIndex(const TArray<FNPCVoiceRuntimeLine>& Lines, ENPCVoiceEffort Effort,
	                             const FNPCVoiceAcousticState& Acoustic, FName LastLineId, float Now,
	                             const TMap<FName, float>& Cooldowns,
	                             const UNPCVoiceSettings& Settings) {
		const FCategoryPreference Allowed = ResolveCategoryPreference(Acoustic, Settings);

		for (const bool bAllowRepeat : {false, true}) {
			for (const ENPCVoiceCategory Category : Allowed) {
				const TArray<int32> Pool =
					GatherCandidates(Lines, Effort, Category, bAllowRepeat, LastLineId, Now, Cooldowns);
				if (!Pool.IsEmpty()) {
					return Pool[FMath::RandRange(0, Pool.Num() - 1)];
				}
			}
		}
		return INDEX_NONE;
	}

	inline int32 FindBargeInLine(const TArray<FNPCVoiceRuntimeLine>& Lines,
	                             ENPCVoiceCategory Category, ENPCVoiceTransitionDir Dir,
	                             ENPCVoiceEffort TargetEffort, float Now,
	                             const TMap<FName, float>& Cooldowns) {
		int32 BestDelta = MAX_int32;
		TArray<int32> Pool;
		for (int32 i = 0; i < Lines.Num(); ++i) {
			const FNPCVoiceLineRow& Row = Lines[i].Row;
			if (Row.Category != Category || Row.Direction != Dir) {
				continue;
			}
			if (IsCooldownBlocked(Row.CooldownGroup, Now, Cooldowns)) {
				continue;
			}
			const int32 Delta = FMath::Abs(
				static_cast<int32>(Row.Bucket) - static_cast<int32>(TargetEffort));
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

	struct FBargeInDecision {
		ENPCVoiceBargeInReason Reason = ENPCVoiceBargeInReason::None;
		ENPCVoiceTransitionDir Dir = ENPCVoiceTransitionDir::None;

		bool ShouldBargeIn() const { return Reason != ENPCVoiceBargeInReason::None; }
	};

	inline bool CanInterruptPlayingLine(const FNPCVoicePlaybackState& Playback,
	                                    const FNPCVoiceTransitionState& Transition, float Now,
	                                    const UNPCVoiceSettings& Settings) {
		return Playback.bPlaying && !Playback.bActiveIsBargeIn &&
			Now - Transition.LastTime >= Settings.TransitionCooldownSeconds &&
			Playback.EndTime - Now >= Settings.TransitionMinRemainingTime;
	}

	inline FBargeInDecision EvaluateBargeIn(const FNPCVoicePlaybackState& Playback,
	                                        const FNPCVoiceTransitionState& Transition,
	                                        ENPCVoiceEffort Committed, ENPCVoiceSightChange SightChange,
	                                        const FNPCVoiceBargeInAvailability& Available, float Now,
	                                        const UNPCVoiceSettings& Settings) {
		if (!CanInterruptPlayingLine(Playback, Transition, Now, Settings)) {
			return {};
		}
		if (SightChange == ENPCVoiceSightChange::Lost && Available.bLostSight) {
			return {ENPCVoiceBargeInReason::SightLost};
		}
		if (SightChange == ENPCVoiceSightChange::Gained && Available.bSightRegained) {
			return {ENPCVoiceBargeInReason::SightGained};
		}
		if (Settings.TransitionEffortDelta <= 0 || !Available.bTransition) {
			return {};
		}
		const int32 Jump = FMath::Abs(
			static_cast<int32>(Committed) - static_cast<int32>(Playback.ActiveEffort));
		if (Jump < Settings.TransitionEffortDelta) {
			return {};
		}
		const bool bRose = static_cast<int32>(Committed) > static_cast<int32>(Playback.ActiveEffort);
		return {
			ENPCVoiceBargeInReason::EffortDrift,
			bRose ? ENPCVoiceTransitionDir::Farther : ENPCVoiceTransitionDir::Closer
		};
	}

	inline float ResolveNextLineDelay(bool bAfterTransition, const UNPCVoiceSettings& Settings) {
		return bAfterTransition
			       ? Settings.PostTransitionLineDelay
			       : FMath::RandRange(Settings.LineIntervalMin, Settings.LineIntervalMax);
	}

	inline void PullInNextLine(FNPCVoicePlaybackState& Playback, float Now,
	                           const UNPCVoiceSettings& Settings) {
		Playback.NextLineTime =
			FMath::Min(Playback.NextLineTime, Now + Settings.PostTransitionLineDelay);
	}

	inline void ClearActiveLine(FNPCVoicePlaybackState& Playback) {
		Playback.bPlaying = false;
		Playback.ActiveLineId = NAME_None;
		Playback.ActiveText.Reset();
	}

	inline void BeginLine(FNPCVoicePlaybackState& Playback, const FNPCVoiceLineRow& Row, float Now,
	                      float EndPadding, bool bAsBargeIn) {
		Playback.bPlaying = true;
		Playback.EndTime = Now + Row.Duration + EndPadding;
		Playback.ActiveLineId = Row.LineId;
		Playback.ActiveText = Row.Text;
		Playback.LastLineId = Row.LineId;
		Playback.ActiveEffort = Row.Bucket;
		Playback.bActiveIsBargeIn = bAsBargeIn;
	}

	inline void EndLine(FNPCVoicePlaybackState& Playback, float Now,
	                    const UNPCVoiceSettings& Settings) {
		const bool bAfterBargeIn = Playback.bActiveIsBargeIn;
		ClearActiveLine(Playback);
		Playback.NextLineTime = Now + ResolveNextLineDelay(bAfterBargeIn, Settings);
	}

	inline void BeginBargeIn(FNPCVoicePlaybackState& Playback, FNPCVoiceTransitionState& Transition,
	                         int32 LineIdx, float Now, const UNPCVoiceSettings& Settings) {
		ClearActiveLine(Playback);
		Transition.bPending = true;
		Transition.PendingLine = LineIdx;
		Transition.PlayTime = Now + Settings.TransitionFadeOutTime + TransitionPlayMargin;
		Transition.LastTime = Now;
	}
}
