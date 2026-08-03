#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceSettings.h"
#include "NPCVoiceState.h"

/** Every scheduling decision as pure functions over explicit state: no component, world, audio
 *  device or engine singleton, mirroring Audio/Math.h. Mutating functions take that state by
 *  reference as the first parameter and touch nothing else. */
namespace VoiceLogic {

	/** Margin past the declick fade before the barge-in line starts, so Play cannot race the
	 *  fade-out's stop. An ordering guard on the audio component, not a design knob. */
	constexpr float TransitionPlayMargin = 0.03f;

	/** Below this a row's Duration disagreeing with its wave is manifest rounding, not a stale
	 *  export worth warning about. */
	constexpr float DurationMismatchTolerance = 0.05f;

	// ── Bank queries ──────────────────────────────────────────────────────────

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
				case ENPCVoiceCategory::Transition: Available.bTransition = true; break;
				case ENPCVoiceCategory::LostSight: Available.bLostSight = true; break;
				case ENPCVoiceCategory::SightRegained: Available.bSightRegained = true; break;
				default: break;
			}
		}
		return Available;
	}

	/** The wave is ground truth: Duration is the scheduler's only end-of-line signal, so a stale
	 *  manifest would truncate every following line. */
	inline float ResolveLineDuration(float RowDuration, float WaveDuration) {
		return WaveDuration > 0.f ? WaveDuration : RowDuration;
	}

	// ── Effort selection ──────────────────────────────────────────────────────

	/** Bands run inverted, near = quiet, and occlusion reaches this only by lengthening the
	 *  distance, so a listener just around a corner steps up one band instead of to Shout. */
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

	// ── Sight ─────────────────────────────────────────────────────────────────

	/** The only thing occlusion still selects directly; effort comes from path length. */
	inline bool IsListenerHidden(const FNPCVoiceAcousticState& Acoustic,
	                             const UNPCVoiceSettings& Settings) {
		return Acoustic.Occlusion >= Settings.OcclusionShiftThreshold;
	}

	inline bool IsPathPartiallyBlocked(const FNPCVoiceAcousticState& Acoustic,
	                                   const UNPCVoiceSettings& Settings) {
		return Settings.PartialOcclusionThreshold > 0.f &&
			Acoustic.Occlusion >= Settings.PartialOcclusionThreshold;
	}

	/** Derived from the same IsListenerHidden predicate content selection uses. Keying off the
	 *  spatial component's LoS timers instead lets the two disagree both ways: that flag drops on a
	 *  single grazing trace, announcing a break nobody heard, and in a pinhole state it never breaks
	 *  at all, which pinned LostSight at the head of the ladder for as long as the listener stood in
	 *  the doorway. */
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

	/** The window is generous so a reaction survives an in-flight line finishing; the delivered
	 *  flag is what stops it being offered twice while that window is still open. */
	inline bool IsSightReactionPending(const FNPCVoiceSightState& State, float Now,
	                                   const UNPCVoiceSettings& Settings) {
		return !State.bReactionDelivered &&
			Now - State.LastChangeTime <= Settings.SightChangeReactionWindow;
	}

	/** Playing one settles the debt for that crossing, however it was scheduled. */
	inline void MarkSightReactionDelivered(FNPCVoiceSightState& State, ENPCVoiceCategory Category) {
		if (IsSightReactionCategory(Category)) {
			State.bReactionDelivered = true;
		}
	}

	// ── Line selection ────────────────────────────────────────────────────────

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

	/** Transition rows match no category the ladder offers, so they stay reachable only through
	 *  FindBargeInLine. */
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

	/** No "visible but far" context: for a visible listener the effective distance is essentially
	 *  the straight line, so the effort already partitions this half by distance. */
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

	/** Categories this state may draw from, most specific first, ending in the generic entry for its
	 *  half. Reacting to a crossing outranks describing the new state, but only until it has been
	 *  reacted to. Selection NEVER crosses between the two halves: every line asserts something
	 *  about the world, so an occluded line played to a listener standing in the open contradicts
	 *  what they can see. Silence is the correct failure mode. */
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

	/** The line to speak, or INDEX_NONE when nothing is playable. Category preference and
	 *  no-immediate-repeat are SOFT, since an effort holding one line should still speak, so the
	 *  ladder relaxes them in turn and exhausts every category before repeating a line the player
	 *  just heard. Cooldowns and the half the categories came from are HARD. */
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

	/** Exact TargetEffort preferred, then the nearest rendered one, random among ties. INDEX_NONE
	 *  aborts the barge-in rather than cutting to silence. Sight reasons pass Dir None, which is
	 *  what their rows carry. */
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

	// ── Barge-in ──────────────────────────────────────────────────────────────

	/** Whether to cut the playing line, why, and for effort drift which way it jumped. */
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

	/** Decision only; the caller owns the fade-out and the state writes. Trigger ORDER is the point:
	 *  losing sight inflates the path and climbs the effort bands, so ranking visibility first stops
	 *  the NPC reporting a listener "moving away" who stepped behind a wall. Each trigger is gated on
	 *  its own replacement content. */
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
		return {ENPCVoiceBargeInReason::EffortDrift,
		        bRose ? ENPCVoiceTransitionDir::Farther : ENPCVoiceTransitionDir::Closer};
	}

	// ── Playback state transitions ────────────────────────────────────────────

	/** A transition announces a change, so the full line at the new effort follows quickly. */
	inline float ResolveNextLineDelay(bool bAfterTransition, const UNPCVoiceSettings& Settings) {
		return bAfterTransition
			? Settings.PostTransitionLineDelay
			: FMath::RandRange(Settings.LineIntervalMin, Settings.LineIntervalMax);
	}

	/** A sight change with nothing playing produces no barge-in, and the normal silence outlasts
	 *  SightChangeReactionWindow, so the NPC would sit through the break and never mention it. */
	inline void PullInNextLine(FNPCVoicePlaybackState& Playback, float Now,
	                           const UNPCVoiceSettings& Settings) {
		Playback.NextLineTime =
			FMath::Min(Playback.NextLineTime, Now + Settings.PostTransitionLineDelay);
	}

	/** Leaves bActiveIsBargeIn alone: EndLine reads it afterwards to choose the next delay, and the
	 *  barge-in path relies on it to refuse interrupting a barge-in it just started. */
	inline void ClearActiveLine(FNPCVoicePlaybackState& Playback) {
		Playback.bPlaying = false;
		Playback.ActiveLineId = NAME_None;
		Playback.ActiveText.Reset();
	}

	/** bAsBargeIn comes from HOW the line was scheduled, not from its category: LostSight rows play
	 *  both as ordinary lines and as interruptions, and only the latter is uninterruptible. */
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
		// Stamped at trigger rather than at playback, so a barge-in whose line fails to start
		// cannot re-fire every tick.
		Transition.LastTime = Now;
	}

} // namespace VoiceLogic
