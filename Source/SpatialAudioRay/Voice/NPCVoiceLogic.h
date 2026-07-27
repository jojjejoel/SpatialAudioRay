#pragma once

#include "CoreMinimal.h"
#include "NPCVoiceSettings.h"
#include "NPCVoiceTypes.h"

/**
 * Every scheduling decision the NPC voice system makes, as pure functions over explicit
 * state — no component, no world, no audio device, no engine singletons. Mirrors the
 * Audio/Math.h convention: UNPCVoiceComponent is left holding only engine wiring (resolving
 * components, loading the bank, calling Play/FadeOut) and delegates each decision here, so
 * the decisions are unit-testable in isolation.
 *
 * Functions that mutate state take it by reference as the first parameter; nothing here
 * reads or writes anything else.
 */
namespace VoiceLogic {

	/** Margin past the declick fade before the barge-in line starts, so Play cannot race the
	 *  fade-out's stop. Not a design knob — an ordering guard on the audio component. */
	constexpr float TransitionPlayMargin = 0.03f;

	// ── Bank queries ──────────────────────────────────────────────────────────

	/** The category a barge-in draws its replacement line from. */
	inline ENPCVoiceCategory BargeInCategory(ENPCVoiceBargeInReason Reason) {
		switch (Reason) {
			case ENPCVoiceBargeInReason::SightLost: return ENPCVoiceCategory::LostSight;
			case ENPCVoiceBargeInReason::SightGained: return ENPCVoiceCategory::SightRegained;
			default: return ENPCVoiceCategory::Transition;
		}
	}

	/** Whether the bank can service a barge-in at all. Cached at load: with none of the
	 *  barge-in categories present the feature stays dormant rather than cutting lines it
	 *  can't replace. */
	inline bool BankHasBargeInContent(const TArray<FNPCVoiceRuntimeLine>& Lines) {
		return Lines.ContainsByPredicate([](const FNPCVoiceRuntimeLine& Line) {
			return Line.Row.Category == ENPCVoiceCategory::Transition ||
				Line.Row.Category == ENPCVoiceCategory::LostSight ||
				Line.Row.Category == ENPCVoiceCategory::SightRegained;
		});
	}

	// ── Effort selection ──────────────────────────────────────────────────────

	/** Effort band for an effective acoustic distance, near = quiet. Occlusion enters only
	 *  through that distance (a bent path is longer), so a listener just around a small
	 *  corner steps up one band instead of jumping to Shout. */
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

	/** Commits Mapped once it has persisted for DwellTime. A candidate that flips back to the
	 *  committed bucket before the dwell expires never commits, so a player loitering on a
	 *  band edge doesn't change the NPC's delivery. */
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

	// ── Line selection ────────────────────────────────────────────────────────

	/** Whether Group is currently blocked. A None group is never on cooldown — ungrouped
	 *  lines are individually schedulable. */
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

	/** Whether the listener counts as hidden. The only thing occlusion still selects directly;
	 *  effort comes from path length. */
	inline bool IsListenerHidden(const FNPCVoiceAcousticState& Acoustic, const UNPCVoiceSettings& S) {
		return Acoustic.Occlusion >= S.OcclusionShiftThreshold;
	}

	/** Indices of every line playable at Bucket/Category right now. bAllowRepeat waives the
	 *  no-immediate-repeat rule. Transition rows never match — they carry their own category
	 *  and are reachable only through FindBargeInLine. */
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

	/** Categories this acoustic state may draw from, most specific first, ending in the
	 *  generic entry for its half.
	 *
	 *  Selection NEVER crosses between the visible and occluded halves. Every line asserts
	 *  something about the world, so an occluded line played to a listener standing in the
	 *  open — or a "nothing between us but air" line played to one behind a wall —
	 *  contradicts what they can see. Silence is the correct failure mode; the bank is
	 *  expected to carry a generic Clear and Occluded line at every effort, and any missing
	 *  specific context simply falls through to those. */
	inline TArray<ENPCVoiceCategory, TInlineAllocator<4>> ResolveCategoryPreference(
		const FNPCVoiceAcousticState& Acoustic, const UNPCVoiceSettings& S) {
		TArray<ENPCVoiceCategory, TInlineAllocator<4>> Allowed;

		if (!IsListenerHidden(Acoustic, S)) {
			// Reacting to the change outranks describing the new state, but only briefly.
			if (Acoustic.LoSHeldDuration <= S.SightChangeReactionWindow) {
				Allowed.Add(ENPCVoiceCategory::SightRegained);
			}
			if (Acoustic.DirectDistanceCm >= S.FarVisibleMinDistance) {
				Allowed.Add(ENPCVoiceCategory::FarVisible);
			}
			Allowed.Add(ENPCVoiceCategory::Clear);
			return Allowed;
		}

		if (Acoustic.TimeSinceLoSLost <= S.SightChangeReactionWindow) {
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

	/** The line to speak, or INDEX_NONE when the bank offers nothing playable. Category
	 *  preference and no-immediate-repeat are SOFT constraints — a bucket holding a single
	 *  line should still speak rather than fall silent, so the ladder relaxes them in turn
	 *  before giving up. Cooldowns, and the ban on occluded content while visible, are HARD. */
	inline int32 SelectLineIndex(const TArray<FNPCVoiceRuntimeLine>& Lines, ENPCVoiceEffort Bucket,
	                             const FNPCVoiceAcousticState& Acoustic, FName LastLineId, float Now,
	                             const TMap<FName, float>& Cooldowns, const UNPCVoiceSettings& S) {
		const TArray<ENPCVoiceCategory, TInlineAllocator<4>> Allowed =
			ResolveCategoryPreference(Acoustic, S);

		// Exhaust every allowed category before relaxing the no-repeat rule: a slightly less
		// apt line the player hasn't just heard beats repeating the one they have.
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

	/** Replacement line for a barge-in: rows in Category matching Dir with cooldowns
	 *  respected, exact TargetBucket preferred then the nearest rendered bucket (a
	 *  slightly-off effort still beats refusing to react), random among ties. INDEX_NONE when
	 *  nothing is usable, which aborts the barge-in rather than cutting to silence.
	 *  Dir discriminates the two effort directions; sight reasons pass None, which is what
	 *  their rows carry. */
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

	/** Whether to cut the playing line, why, and — for effort drift — which way it jumped. */
	struct FBargeInDecision {
		ENPCVoiceBargeInReason Reason = ENPCVoiceBargeInReason::None;
		ENPCVoiceTransitionDir Dir = ENPCVoiceTransitionDir::None;

		bool ShouldBargeIn() const { return Reason != ENPCVoiceBargeInReason::None; }
	};

	/** Decision only — the caller owns the fade-out and the state writes. Shared gates first:
	 *  something is playing, it isn't itself a barge-in (barge-ins are never interrupted), the
	 *  bank has replacement content, the rate limit has elapsed, and enough of the line remains
	 *  that cutting it reads as deliberate rather than as a glitch.
	 *
	 *  Then the triggers, and their ORDER is the point. Losing sight inflates the acoustic path,
	 *  which climbs the effort bands, so a visibility change almost always arrives together with
	 *  effort drift. Ranking visibility first keeps the NPC from reporting a listener "moving
	 *  away" who never moved at all — they stepped behind a wall. */
	inline FBargeInDecision EvaluateBargeIn(const FNPCVoicePlaybackState& Playback,
	                                        const FNPCVoiceTransitionState& Transition,
	                                        ENPCVoiceEffort Committed, ENPCVoiceSightChange SightChange,
	                                        bool bBankHasBargeInContent, float Now,
	                                        const UNPCVoiceSettings& S) {
		if (!Playback.bPlaying || Playback.bActiveIsBargeIn || !bBankHasBargeInContent) {
			return {};
		}
		if (Now - Transition.LastTime < S.TransitionCooldownSeconds ||
			Playback.EndTime - Now < S.TransitionMinRemainingTime) {
			return {};
		}
		if (SightChange == ENPCVoiceSightChange::Lost) {
			return {ENPCVoiceBargeInReason::SightLost};
		}
		if (SightChange == ENPCVoiceSightChange::Gained) {
			return {ENPCVoiceBargeInReason::SightGained};
		}
		if (S.TransitionBucketDelta <= 0) {
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

	/** Silence before the next line. A transition announces a change, so the full line at the
	 *  new effort follows quickly instead of waiting out the normal random interval. */
	inline float ResolveNextLineDelay(bool bAfterTransition, const UNPCVoiceSettings& S) {
		return bAfterTransition ? S.PostTransitionLineDelay
		                        : FMath::RandRange(S.LineIntervalMin, S.LineIntervalMax);
	}

	/** Drops the playing line without scheduling a replacement. Deliberately leaves
	 *  bActiveIsBargeIn alone: EndLine reads it afterwards to choose the next delay, and the
	 *  barge-in path relies on it to refuse interrupting a barge-in it just started. */
	inline void ClearActiveLine(FNPCVoicePlaybackState& Playback) {
		Playback.bPlaying = false;
		Playback.ActiveLineId = NAME_None;
		Playback.ActiveText.Reset();
	}

	/** Records a line as started: when it ends, its identity for the no-repeat rule and HUD,
	 *  and the effort and barge-in flags EvaluateBargeIn compares against. bAsBargeIn comes
	 *  from HOW the line was scheduled, not from its category — LostSight rows are played both
	 *  as ordinary lines and as interruptions, and only the latter must be uninterruptible. */
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
		// Stamped at trigger rather than at playback so a barge-in whose line fails to start
		// can't re-fire every tick.
		Transition.LastTime = Now;
	}

} // namespace VoiceLogic
