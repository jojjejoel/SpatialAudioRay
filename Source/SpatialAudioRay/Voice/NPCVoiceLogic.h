#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceSettings.h"
#include "NPCVoiceTypes.h"

/** Every scheduling decision as pure functions over explicit state: no component, world, audio
 *  device or engine singleton, mirroring Audio/Math.h. Mutating functions take that state by
 *  reference as the first parameter and touch nothing else. */
namespace VoiceLogic {

	/** Margin past the declick fade before the barge-in line starts, so Play cannot race the
	 *  fade-out's stop. An ordering guard on the audio component, not a design knob. */
	constexpr float TransitionPlayMargin = 0.03f;

	/** How far a row's Duration may disagree with its wave before it is worth warning about.
	 *  Below this it is manifest rounding, not a stale export. */
	constexpr float DurationMismatchTolerance = 0.05f;

	// ── Bank queries ──────────────────────────────────────────────────────────

	/** The category a barge-in draws its replacement line from. */
	inline ENPCVoiceCategory BargeInCategory(ENPCVoiceBargeInReason Reason) {
		switch (Reason) {
			case ENPCVoiceBargeInReason::SightLost: return ENPCVoiceCategory::LostSight;
			case ENPCVoiceBargeInReason::SightGained: return ENPCVoiceCategory::SightRegained;
			default: return ENPCVoiceCategory::Transition;
		}
	}

	/** Cached at load. A reason with no content never fires, so it cannot claim a tick away from
	 *  a reason that does have content. */
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

	/** The wave is ground truth: Duration is the only end-of-line signal there is, so a stale
	 *  manifest would truncate every following line. Falls back to the manifest only when the
	 *  wave reports nothing usable. */
	inline float ResolveLineDuration(float RowDuration, float WaveDuration) {
		return WaveDuration > 0.f ? WaveDuration : RowDuration;
	}

	// ── Effort selection ──────────────────────────────────────────────────────

	/** Effort band for an effective acoustic distance, near = quiet. Occlusion enters only through
	 *  that distance, so a listener just around a corner steps up one band instead of to Shout. */
	inline ENPCVoiceEffort MapToBucket(float EffectiveDistanceCm, const UNPCVoiceSettings& S) {
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

	/** Commits Mapped once it has persisted for DwellTime, so a player loitering on a band edge
	 *  never changes the delivery. */
	inline void AdvanceBucketHysteresis(FNPCVoiceBucketHysteresis& State, ENPCVoiceEffort Mapped,
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
	inline bool IsListenerHidden(const FNPCVoiceAcousticState& Acoustic, const UNPCVoiceSettings& S) {
		return Acoustic.Occlusion >= S.OcclusionShiftThreshold;
	}

	/** Only meaningful on the visible half. "Hidden" is a separate, much higher threshold: these
	 *  answer different questions, can they see me at all versus is the path unobstructed. */
	inline bool IsPathPartiallyBlocked(const FNPCVoiceAcousticState& Acoustic,
	                                   const UNPCVoiceSettings& S) {
		return S.PartialOcclusionThreshold > 0.f && Acoustic.Occlusion >= S.PartialOcclusionThreshold;
	}

	/** The voice layer's ONLY sight signal, derived from the same IsListenerHidden predicate that
	 *  content selection uses. Keying off the spatial component's LoS timers instead lets the two
	 *  disagree both ways: that flag drops on a single grazing trace, announcing a break nobody
	 *  heard, and in a pinhole state it never breaks at all, which pinned LostSight at the head of
	 *  the ladder for as long as the listener stood in the doorway. */
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
		// A new crossing is a new thing to remark on, whatever was said about the last one.
		State.bReactionDelivered = false;
		return bHidden ? ENPCVoiceSightChange::Lost : ENPCVoiceSightChange::Gained;
	}

	/** Playing one settles the debt for that crossing, however it was scheduled. */
	inline bool IsSightReactionCategory(ENPCVoiceCategory Category) {
		return Category == ENPCVoiceCategory::LostSight ||
			Category == ENPCVoiceCategory::SightRegained;
	}

	/** Recent enough to remark on, and not already remarked on. The window is generous so the
	 *  reaction survives an in-flight line finishing, but a reaction states an event once rather
	 *  than describing a state, so it must stop being offered while the window is still open.
	 *  Left purely temporal the next line re-announces the same crossing, and since an effort-band
	 *  change is what usually schedules it: "there you are!" shouted, then again at raised. */
	inline bool IsSightReactionPending(const FNPCVoiceSightState& State, float Now,
	                                   const UNPCVoiceSettings& S) {
		return !State.bReactionDelivered &&
			Now - State.LastChangeTime <= S.SightChangeReactionWindow;
	}

	/** Settles the reaction debt when Category is one of the reaction categories. */
	inline void MarkSightReactionDelivered(FNPCVoiceSightState& State, ENPCVoiceCategory Category) {
		if (IsSightReactionCategory(Category)) {
			State.bReactionDelivered = true;
		}
	}

	// ── Line selection ────────────────────────────────────────────────────────

	/** A None group is never on cooldown, since ungrouped lines are individually schedulable. */
	inline bool IsCooldownBlocked(FName Group, float Now, const TMap<FName, float>& Cooldowns) {
		if (Group.IsNone()) {
			return false;
		}
		const float* Until = Cooldowns.Find(Group);
		return Until && Now < *Until;
	}

	/** Blocks Group until Seconds from now. Ungrouped lines stamp nothing. */
	inline void StampCooldown(TMap<FName, float>& Cooldowns, FName Group, float Now, float Seconds) {
		if (!Group.IsNone()) {
			Cooldowns.Add(Group, Now + Seconds);
		}
	}

	/** Indices of every line playable at Bucket/Category now. bAllowRepeat waives the no-repeat
	 *  rule. Transition rows never match; they are reachable only through FindBargeInLine. */
	inline TArray<int32> GatherCandidates(const TArray<FNPCVoiceRuntimeLine>& Lines,
	                                      ENPCVoiceEffort Bucket, ENPCVoiceCategory Category,
	                                      bool bAllowRepeat, FName LastLineId, float Now,
	                                      const TMap<FName, float>& Cooldowns) {
		TArray<int32> Pool;
		for (int32 i = 0; i < Lines.Num(); ++i) {
			const FNPCVoiceLineRow& Row = Lines[i].Row;
			if (Row.Bucket != Bucket || Row.Category != Category) {
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

	/** Categories this state may draw from, most specific first, ending in the generic entry for
	 *  its half. Selection NEVER crosses between the visible and occluded halves: every line
	 *  asserts something about the world, so an occluded line played to a listener standing in the
	 *  open contradicts what they can see. Silence is the correct failure mode. */
	inline TArray<ENPCVoiceCategory, TInlineAllocator<4>> ResolveCategoryPreference(
		const FNPCVoiceAcousticState& Acoustic, const UNPCVoiceSettings& S) {
		TArray<ENPCVoiceCategory, TInlineAllocator<4>> Allowed;

		// Reacting to the crossing outranks describing the new state, but only until it has been
		// reacted to. Which reaction the flag opens follows from the half the listener is in now.
		const bool bJustChanged = Acoustic.bSightReactionPending;

		if (!IsListenerHidden(Acoustic, S)) {
			if (bJustChanged) {
				Allowed.Add(ENPCVoiceCategory::SightRegained);
			}
			// No "visible but far" context: for a visible listener the effective distance is
			// essentially the straight line, so the effort bucket already partitions the visible
			// half by distance. A separate category restated the bucket and needed its own
			// threshold hand-synced to a band edge. Generic Clear stays the last resort.
			if (IsPathPartiallyBlocked(Acoustic, S)) {
				Allowed.Add(ENPCVoiceCategory::PartiallyOccluded);
			}
			Allowed.Add(ENPCVoiceCategory::Clear);
			return Allowed;
		}

		if (bJustChanged) {
			Allowed.Add(ENPCVoiceCategory::LostSight);
		}
		const float Detour = Acoustic.DetourRatio();
		if (Acoustic.DirectDistanceCm <= S.BehindWallMaxDirectDistance &&
			Detour >= S.BehindWallMinDetourRatio) {
			Allowed.Add(ENPCVoiceCategory::BehindWall);
		}
		else if (Detour <= S.AroundCornerMaxDetourRatio) {
			Allowed.Add(ENPCVoiceCategory::AroundCorner);
		}
		Allowed.Add(ENPCVoiceCategory::Occluded);
		return Allowed;
	}

	/** The line to speak, or INDEX_NONE when nothing is playable. Category preference and
	 *  no-immediate-repeat are SOFT: a bucket holding one line should still speak, so the ladder
	 *  relaxes them in turn. Cooldowns and the ban on occluded content while visible are HARD. */
	inline int32 SelectLineIndex(const TArray<FNPCVoiceRuntimeLine>& Lines, ENPCVoiceEffort Bucket,
	                             const FNPCVoiceAcousticState& Acoustic, FName LastLineId, float Now,
	                             const TMap<FName, float>& Cooldowns, const UNPCVoiceSettings& S) {
		const TArray<ENPCVoiceCategory, TInlineAllocator<4>> Allowed =
			ResolveCategoryPreference(Acoustic, S);

		// Exhaust every allowed category before relaxing no-repeat: a less apt line the player has
		// not just heard beats repeating the one they have.
		for (const bool bAllowRepeat : {false, true}) {
			for (const ENPCVoiceCategory Category : Allowed) {
				const TArray<int32> Pool =
					GatherCandidates(Lines, Bucket, Category, bAllowRepeat, LastLineId, Now, Cooldowns);
				if (!Pool.IsEmpty()) {
					return Pool[FMath::RandRange(0, Pool.Num() - 1)];
				}
			}
		}
		return INDEX_NONE;
	}

	/** Rows in Category matching Dir, cooldowns respected, exact TargetBucket preferred then the
	 *  nearest rendered one, random among ties. INDEX_NONE aborts the barge-in rather than cutting
	 *  to silence. Sight reasons pass Dir None, which is what their rows carry. */
	inline int32 FindBargeInLine(const TArray<FNPCVoiceRuntimeLine>& Lines,
	                             ENPCVoiceCategory Category, ENPCVoiceTransitionDir Dir,
	                             ENPCVoiceEffort TargetBucket, float Now,
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

	// ── Barge-in ──────────────────────────────────────────────────────────────

	/** Whether to cut the playing line, why, and for effort drift which way it jumped. */
	struct FBargeInDecision {
		ENPCVoiceBargeInReason Reason = ENPCVoiceBargeInReason::None;
		ENPCVoiceTransitionDir Dir = ENPCVoiceTransitionDir::None;

		bool ShouldBargeIn() const { return Reason != ENPCVoiceBargeInReason::None; }
	};

	/** Decision only; the caller owns the fade-out and the state writes. Shared gates first:
	 *  something is playing, it is not itself a barge-in, the rate limit has elapsed, and enough
	 *  of the line remains that cutting reads as deliberate. Then trigger ORDER, which is the
	 *  point: losing sight inflates the path and climbs the effort bands, so ranking visibility
	 *  first stops the NPC reporting a listener "moving away" who stepped behind a wall. Each
	 *  trigger is gated on its own replacement content. */
	inline FBargeInDecision EvaluateBargeIn(const FNPCVoicePlaybackState& Playback,
	                                        const FNPCVoiceTransitionState& Transition,
	                                        ENPCVoiceEffort Committed, ENPCVoiceSightChange SightChange,
	                                        const FNPCVoiceBargeInAvailability& Available, float Now,
	                                        const UNPCVoiceSettings& S) {
		if (!Playback.bPlaying || Playback.bActiveIsBargeIn) {
			return {};
		}
		if (Now - Transition.LastTime < S.TransitionCooldownSeconds ||
			Playback.EndTime - Now < S.TransitionMinRemainingTime) {
			return {};
		}
		if (SightChange == ENPCVoiceSightChange::Lost && Available.bLostSight) {
			return {ENPCVoiceBargeInReason::SightLost};
		}
		if (SightChange == ENPCVoiceSightChange::Gained && Available.bSightRegained) {
			return {ENPCVoiceBargeInReason::SightGained};
		}
		if (S.TransitionBucketDelta <= 0 || !Available.bTransition) {
			return {};
		}
		const int32 Jump = FMath::Abs(
			static_cast<int32>(Committed) - static_cast<int32>(Playback.ActiveBucket));
		if (Jump < S.TransitionBucketDelta) {
			return {};
		}
		const bool bRose = static_cast<int32>(Committed) > static_cast<int32>(Playback.ActiveBucket);
		return {ENPCVoiceBargeInReason::EffortDrift,
		        bRose ? ENPCVoiceTransitionDir::Farther : ENPCVoiceTransitionDir::Closer};
	}

	// ── Playback state transitions ────────────────────────────────────────────

	/** A transition announces a change, so the full line at the new effort follows quickly. */
	inline float ResolveNextLineDelay(bool bAfterTransition, const UNPCVoiceSettings& S) {
		return bAfterTransition ? S.PostTransitionLineDelay
		                        : FMath::RandRange(S.LineIntervalMin, S.LineIntervalMax);
	}

	/** A sight change with nothing playing produces no barge-in, and the normal silence outlasts
	 *  SightChangeReactionWindow, so the NPC would sit through the break and never mention it.
	 *  Only ever moves the next line earlier. */
	inline void PullInNextLine(FNPCVoicePlaybackState& Playback, float Now,
	                           const UNPCVoiceSettings& S) {
		Playback.NextLineTime = FMath::Min(Playback.NextLineTime, Now + S.PostTransitionLineDelay);
	}

	/** Leaves bActiveIsBargeIn alone: EndLine reads it afterwards to choose the next delay, and
	 *  the barge-in path relies on it to refuse interrupting a barge-in it just started. */
	inline void ClearActiveLine(FNPCVoicePlaybackState& Playback) {
		Playback.bPlaying = false;
		Playback.ActiveLineId = NAME_None;
		Playback.ActiveText.Reset();
	}

	/** bAsBargeIn comes from HOW the line was scheduled, not from its category: LostSight rows
	 *  play both as ordinary lines and as interruptions, and only the latter is uninterruptible. */
	inline void BeginLine(FNPCVoicePlaybackState& Playback, const FNPCVoiceLineRow& Row, float Now,
	                      float EndPadding, bool bAsBargeIn) {
		Playback.bPlaying = true;
		Playback.EndTime = Now + Row.Duration + EndPadding;
		Playback.ActiveLineId = Row.LineId;
		Playback.ActiveText = Row.Text;
		Playback.LastLineId = Row.LineId;
		Playback.ActiveBucket = Row.Bucket;
		Playback.bActiveIsBargeIn = bAsBargeIn;
	}

	/** Ends a line that played to completion and schedules the next. */
	inline void EndLine(FNPCVoicePlaybackState& Playback, float Now, const UNPCVoiceSettings& S) {
		const bool bAfterBargeIn = Playback.bActiveIsBargeIn;
		ClearActiveLine(Playback);
		Playback.NextLineTime = Now + ResolveNextLineDelay(bAfterBargeIn, S);
	}

	/** Cuts the playing line and queues LineIdx to start once the declick fade has run. */
	inline void BeginBargeIn(FNPCVoicePlaybackState& Playback, FNPCVoiceTransitionState& Transition,
	                         int32 LineIdx, float Now, const UNPCVoiceSettings& S) {
		ClearActiveLine(Playback);
		Transition.bPending = true;
		Transition.PendingLine = LineIdx;
		Transition.PlayTime = Now + S.TransitionFadeOutTime + TransitionPlayMargin;
		// Stamped at trigger rather than at playback, so a barge-in whose line fails to start
		// cannot re-fire every tick.
		Transition.LastTime = Now;
	}

} // namespace VoiceLogic
