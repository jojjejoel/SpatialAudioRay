#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Voice/NPCVoiceLogic.h"
#include "Voice/NPCVoiceSettings.h"

namespace {
	FNPCVoiceRuntimeLine MakeVoiceLine(const TCHAR* Id, ENPCVoiceEffort Bucket, ENPCVoiceCategory Category,
	                              const TCHAR* Group = TEXT(""),
	                              ENPCVoiceTransitionDir Dir = ENPCVoiceTransitionDir::None) {
		FNPCVoiceRuntimeLine Line;
		Line.Row.LineId = FName(Id);
		Line.Row.Bucket = Bucket;
		Line.Row.Category = Category;
		Line.Row.CooldownGroup = FName(Group);
		Line.Row.Direction = Dir;
		return Line;
	}

	FNPCVoiceRuntimeLine MakeVoiceTransitionLine(const TCHAR* Id, ENPCVoiceEffort Bucket,
	                                        ENPCVoiceTransitionDir Dir,
	                                        const TCHAR* Group = TEXT("")) {
		return MakeVoiceLine(Id, Bucket, ENPCVoiceCategory::Transition, Group, Dir);
	}

	/** Acoustic state with no detour and no recent sight crossing — the plain visible/hidden
	 *  case. An unobstructed visible listener always resolves to Clear. */
	FNPCVoiceAcousticState MakeVoiceAcoustic(float Occlusion, float DirectCm = 500.f,
	                                         float EffectiveCm = 500.f) {
		FNPCVoiceAcousticState Acoustic;
		Acoustic.Occlusion = Occlusion;
		Acoustic.DirectDistanceCm = DirectCm;
		Acoustic.EffectiveDistanceCm = EffectiveCm;
		return Acoustic;
	}

	/** Hidden, far enough out and detoured enough to miss both BehindWall and AroundCorner,
	 *  so the preference is the generic Occluded entry alone. */
	FNPCVoiceAcousticState MakeVoiceGenericOccluded() {
		return MakeVoiceAcoustic(1.f, /*DirectCm=*/1500.f, /*EffectiveCm=*/3000.f);
	}

	/** Playback state as it looks mid-line, ready for a barge-in evaluation. */
	FNPCVoicePlaybackState MakeVoicePlayingState(ENPCVoiceEffort ActiveBucket, float EndTime,
	                                        bool bIsTransition = false) {
		FNPCVoicePlaybackState Playback;
		Playback.bPlaying = true;
		Playback.ActiveBucket = ActiveBucket;
		Playback.EndTime = EndTime;
		Playback.bActiveIsBargeIn = bIsTransition;
		return Playback;
	}

	/** A bank that can service every barge-in reason — the gates under test are the other ones. */
	FNPCVoiceBargeInAvailability MakeVoiceFullBank() {
		return {/*bTransition=*/true, /*bLostSight=*/true, /*bSightRegained=*/true};
	}
}

// ─── MapToBucket ──────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceMapToBucket_DistanceBands,
	"SpatialAudio.Voice.MapToBucket.DistanceBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceMapToBucket_DistanceBands::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	TestTrue(TEXT("Close = whisper"),
	         VoiceLogic::MapToBucket(100.f, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Whisper band edge is inclusive"),
	         VoiceLogic::MapToBucket(S->WhisperMaxDistance, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Past whisper edge = conversational"),
	         VoiceLogic::MapToBucket(S->WhisperMaxDistance + 1.f, *S) == ENPCVoiceEffort::Conversational);
	TestTrue(TEXT("Mid range = raised"),
	         VoiceLogic::MapToBucket(S->ConversationalMaxDistance + 1.f, *S) == ENPCVoiceEffort::Raised);
	TestTrue(TEXT("Far = shout"),
	         VoiceLogic::MapToBucket(S->RaisedMaxDistance + 1.f, *S) == ENPCVoiceEffort::Shout);

	return true;
}

// ─── AdvanceBucketHysteresis ──────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceHysteresis_FirstSampleCommitsInstantly,
	"SpatialAudio.Voice.Hysteresis.FirstSampleCommitsInstantly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceHysteresis_FirstSampleCommitsInstantly::RunTest(const FString& Parameters) {
	// Nothing to smooth against on the first sample, and the default bucket would otherwise
	// mis-deliver the opening line.
	FNPCVoiceBucketHysteresis State;
	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Shout, /*Now=*/10.f, /*Dwell=*/1.f);

	TestTrue(TEXT("First sample commits without waiting out the dwell"),
	         State.Committed == ENPCVoiceEffort::Shout);
	TestTrue(TEXT("Candidate matches the commit"), State.Candidate == ENPCVoiceEffort::Shout);
	TestTrue(TEXT("State is seeded"), State.bInitialized);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceHysteresis_CommitsOnlyAfterDwell,
	"SpatialAudio.Voice.Hysteresis.CommitsOnlyAfterDwell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceHysteresis_CommitsOnlyAfterDwell::RunTest(const FString& Parameters) {
	FNPCVoiceBucketHysteresis State;
	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Whisper, 0.f, 1.f);

	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Shout, 10.f, 1.f);
	TestTrue(TEXT("A new candidate does not commit immediately"),
	         State.Committed == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Candidate is recorded"), State.Candidate == ENPCVoiceEffort::Shout);

	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Shout, 10.9f, 1.f);
	TestTrue(TEXT("Still short of the dwell time"), State.Committed == ENPCVoiceEffort::Whisper);

	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Shout, 11.f, 1.f);
	TestTrue(TEXT("Commits once the candidate has persisted for the dwell time"),
	         State.Committed == ENPCVoiceEffort::Shout);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceHysteresis_FlipBackCancelsCommit,
	"SpatialAudio.Voice.Hysteresis.FlipBackCancelsCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceHysteresis_FlipBackCancelsCommit::RunTest(const FString& Parameters) {
	// A player loitering on a band edge must not change the NPC's delivery.
	FNPCVoiceBucketHysteresis State;
	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Whisper, 0.f, 1.f);
	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Shout, 10.f, 1.f);
	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Whisper, 10.5f, 1.f);
	VoiceLogic::AdvanceBucketHysteresis(State, ENPCVoiceEffort::Whisper, 20.f, 1.f);

	TestTrue(TEXT("Flipping back before the dwell expires never commits the intruder"),
	         State.Committed == ENPCVoiceEffort::Whisper);
	return true;
}

// ─── AdvanceSightState ────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSight_ReportsCrossingsOnly,
	"SpatialAudio.Voice.Sight.ReportsCrossingsOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSight_ReportsCrossingsOnly::RunTest(const FString& Parameters) {
	FNPCVoiceSightState State;

	// The listener's starting side is not something the NPC just watched happen.
	TestTrue(TEXT("The seeding sample reports no crossing"),
	         VoiceLogic::AdvanceSightState(State, /*bHidden=*/true, 10.f) ==
	         ENPCVoiceSightChange::None);
	TestTrue(TEXT("...but does record the side"), State.bHidden && State.bInitialized);
	TestEqual(TEXT("...and leaves the reaction window shut"), State.LastChangeTime, -1e9f);

	TestTrue(TEXT("Staying hidden reports nothing"),
	         VoiceLogic::AdvanceSightState(State, true, 11.f) == ENPCVoiceSightChange::None);
	TestEqual(TEXT("A non-crossing never stamps the timer"), State.LastChangeTime, -1e9f);

	TestTrue(TEXT("Becoming visible reports a gain"),
	         VoiceLogic::AdvanceSightState(State, /*bHidden=*/false, 12.f) ==
	         ENPCVoiceSightChange::Gained);
	TestEqual(TEXT("The crossing stamps the reaction window"), State.LastChangeTime, 12.f);

	TestTrue(TEXT("Becoming hidden reports a loss"),
	         VoiceLogic::AdvanceSightState(State, true, 20.f) == ENPCVoiceSightChange::Lost);
	TestEqual(TEXT("...and re-stamps"), State.LastChangeTime, 20.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSight_WindowGatesReactionContentOnBothSides,
	"SpatialAudio.Voice.Sight.WindowGatesReactionContentOnBothSides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSight_WindowGatesReactionContentOnBothSides::RunTest(const FString& Parameters) {
	// One pending flag serves both reactions; the half the listener is in now decides which it
	// opens. A settled scene must offer neither — this is what a listener standing still in a
	// doorway used to break, because the reaction was keyed off direct line of sight instead,
	// which in that state never breaks at all and pinned LostSight at the head of the ladder.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	FNPCVoiceAcousticState Settled = MakeVoiceGenericOccluded();
	TestFalse(TEXT("A settled hidden listener is offered no LostSight content"),
	          VoiceLogic::ResolveCategoryPreference(Settled, *S)
	              .Contains(ENPCVoiceCategory::LostSight));

	FNPCVoiceAcousticState SettledVisible = MakeVoiceAcoustic(0.f);
	TestFalse(TEXT("A settled visible listener is offered no SightRegained content"),
	          VoiceLogic::ResolveCategoryPreference(SettledVisible, *S)
	              .Contains(ENPCVoiceCategory::SightRegained));

	Settled.bSightReactionPending = true;
	TestTrue(TEXT("A pending reaction on the hidden half opens LostSight"),
	         VoiceLogic::ResolveCategoryPreference(Settled, *S)[0] == ENPCVoiceCategory::LostSight);

	SettledVisible.bSightReactionPending = true;
	TestTrue(TEXT("The same flag opens SightRegained on the visible half"),
	         VoiceLogic::ResolveCategoryPreference(SettledVisible, *S)[0] ==
	         ENPCVoiceCategory::SightRegained);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSight_ReactionIsSpokenOncePerCrossing,
	"SpatialAudio.Voice.Sight.ReactionIsSpokenOncePerCrossing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSight_ReactionIsSpokenOncePerCrossing::RunTest(const FString& Parameters) {
	// A reaction is a one-time statement about an event, not a description of a state. The
	// window has to be generous enough to survive an in-flight line finishing, so on time alone
	// the line AFTER a reaction re-announces the same crossing — and since what typically
	// schedules that line is the listener crossing an effort band, it re-announces it in a
	// different voice ("there you are!" shouted, then again at raised effort).
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	FNPCVoiceSightState State;
	VoiceLogic::AdvanceSightState(State, /*bHidden=*/true, /*Now=*/0.f);   // seed hidden
	VoiceLogic::AdvanceSightState(State, /*bHidden=*/false, /*Now=*/10.f); // regained sight

	TestTrue(TEXT("The crossing owes a reaction"),
	         VoiceLogic::IsSightReactionPending(State, 10.f, *S));

	// A non-reaction line leaves the debt outstanding.
	VoiceLogic::MarkSightReactionDelivered(State, ENPCVoiceCategory::AroundCorner);
	TestTrue(TEXT("An unrelated line does not settle the crossing"),
	         VoiceLogic::IsSightReactionPending(State, 10.5f, *S));

	VoiceLogic::MarkSightReactionDelivered(State, ENPCVoiceCategory::SightRegained);
	TestFalse(TEXT("Once spoken the reaction stops being offered, window still open"),
	          VoiceLogic::IsSightReactionPending(State, 10.5f, *S));

	// The window still expires on its own for a crossing that was never reacted to.
	FNPCVoiceSightState Unreacted;
	VoiceLogic::AdvanceSightState(Unreacted, true, 0.f);
	VoiceLogic::AdvanceSightState(Unreacted, false, 10.f);
	TestTrue(TEXT("Still owed at the window edge"),
	         VoiceLogic::IsSightReactionPending(Unreacted, 10.f + S->SightChangeReactionWindow, *S));
	TestFalse(TEXT("Expired past it"),
	          VoiceLogic::IsSightReactionPending(Unreacted,
	                                             10.f + S->SightChangeReactionWindow + 0.01f, *S));

	// A fresh crossing is a fresh thing to remark on, whatever was said about the last one.
	TestTrue(TEXT("Losing sight again reports the crossing"),
	         VoiceLogic::AdvanceSightState(State, /*bHidden=*/true, 12.f) ==
	         ENPCVoiceSightChange::Lost);
	TestTrue(TEXT("...and re-arms the reaction"),
	         VoiceLogic::IsSightReactionPending(State, 12.f, *S));
	return true;
}

// ─── Cooldowns ────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceCooldown_BlocksAndExpires,
	"SpatialAudio.Voice.Cooldown.BlocksAndExpires",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceCooldown_BlocksAndExpires::RunTest(const FString& Parameters) {
	TMap<FName, float> Cooldowns;
	VoiceLogic::StampCooldown(Cooldowns, FName(TEXT("taunt")), /*Now=*/10.f, /*Seconds=*/5.f);

	TestTrue(TEXT("Stamped group is blocked before it expires"),
	         VoiceLogic::IsCooldownBlocked(FName(TEXT("taunt")), 14.f, Cooldowns));
	TestFalse(TEXT("Blocked no longer at the expiry instant"),
	          VoiceLogic::IsCooldownBlocked(FName(TEXT("taunt")), 15.f, Cooldowns));
	TestFalse(TEXT("Unknown group is never blocked"),
	          VoiceLogic::IsCooldownBlocked(FName(TEXT("other")), 11.f, Cooldowns));

	// Ungrouped lines are individually schedulable, so None must never stamp or block.
	VoiceLogic::StampCooldown(Cooldowns, NAME_None, 10.f, 5.f);
	TestFalse(TEXT("None group is never blocked"), VoiceLogic::IsCooldownBlocked(NAME_None, 11.f, Cooldowns));
	TestFalse(TEXT("None group is never stamped into the map"), Cooldowns.Contains(NAME_None));
	return true;
}

// ─── SelectLineIndex ──────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_BucketAndCategory,
	"SpatialAudio.Voice.SelectLine.BucketAndCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_BucketAndCategory::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceLine(TEXT("ClearShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	Lines.Add(MakeVoiceLine(TEXT("OccludedShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Occluded));
	Lines.Add(MakeVoiceLine(TEXT("ClearWhisper"), ENPCVoiceEffort::Whisper, ENPCVoiceCategory::Clear));
	const TMap<FName, float> NoCooldowns;

	TestEqual(TEXT("Clear LoS picks the Clear line in the committed bucket"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S), 0);
	TestEqual(TEXT("Occluded picks the occlusion-keyed line in the same bucket"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Shout, MakeVoiceGenericOccluded(),
	                                      NAME_None, 0.f, NoCooldowns, *S), 1);
	TestEqual(TEXT("A different committed bucket selects that bucket's line"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Whisper, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S), 2);
	TestEqual(TEXT("Bucket with no lines at all yields INDEX_NONE"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Raised, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_SoftConstraintsRelax,
	"SpatialAudio.Voice.SelectLine.SoftConstraintsRelax",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_SoftConstraintsRelax::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const TMap<FName, float> NoCooldowns;

	// A specific context with no content of its own falls through to the generic entry for
	// its half rather than leaving the NPC silent.
	TArray<FNPCVoiceRuntimeLine> GenericOccludedOnly;
	GenericOccludedOnly.Add(
		MakeVoiceLine(TEXT("OccShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Occluded));
	FNPCVoiceAcousticState BehindWall = MakeVoiceAcoustic(1.f, /*DirectCm=*/300.f, /*EffectiveCm=*/2400.f);
	TestEqual(TEXT("A context with no lines falls through to its half's generic entry"),
	          VoiceLogic::SelectLineIndex(GenericOccludedOnly, ENPCVoiceEffort::Shout, BehindWall,
	                                      NAME_None, 0.f, NoCooldowns, *S), 0);

	// The only line in the bucket is also the one that just played: no-repeat must relax.
	TArray<FNPCVoiceRuntimeLine> ClearOnly;
	ClearOnly.Add(MakeVoiceLine(TEXT("ClearShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	TestEqual(TEXT("A single-line bucket repeats rather than falling silent"),
	          VoiceLogic::SelectLineIndex(ClearOnly, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      FName(TEXT("ClearShout")), 0.f, NoCooldowns, *S), 0);

	// With an alternative available, the no-repeat rule is honored.
	TArray<FNPCVoiceRuntimeLine> Pair = ClearOnly;
	Pair.Add(MakeVoiceLine(TEXT("ClearShoutB"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	TestEqual(TEXT("No-repeat is honored when an alternative exists"),
	          VoiceLogic::SelectLineIndex(Pair, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      FName(TEXT("ClearShout")), 0.f, NoCooldowns, *S), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_CooldownIsHard,
	"SpatialAudio.Voice.SelectLine.CooldownIsHard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_CooldownIsHard::RunTest(const FString& Parameters) {
	// Unlike category and no-repeat, a cooldown is never relaxed — every rung of the ladder
	// respects it, so an entirely cooled-down bucket stays silent.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceLine(TEXT("A"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear, TEXT("grp")));
	Lines.Add(MakeVoiceLine(TEXT("B"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Occluded, TEXT("grp")));

	TMap<FName, float> Cooldowns;
	Cooldowns.Add(FName(TEXT("grp")), 100.f);

	TestEqual(TEXT("Every candidate on cooldown yields INDEX_NONE"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      NAME_None, /*Now=*/50.f, Cooldowns, *S),
	          static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Same bank speaks once the cooldown has expired"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      NAME_None, /*Now=*/150.f, Cooldowns, *S), 0);
	return true;
}

// ─── FindBargeInLine ──────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceFindTransitionLine_DirectionAndBucket,
	"SpatialAudio.Voice.FindTransitionLine.DirectionAndBucket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceFindTransitionLine_DirectionAndBucket::RunTest(const FString& Parameters) {
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_CloserWhisper"), ENPCVoiceEffort::Whisper,
	                             ENPCVoiceTransitionDir::Closer));
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_FartherRaised"), ENPCVoiceEffort::Raised,
	                             ENPCVoiceTransitionDir::Farther));
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_FartherShout"), ENPCVoiceEffort::Shout,
	                             ENPCVoiceTransitionDir::Farther));
	const TMap<FName, float> NoCooldowns;

	TestEqual(TEXT("Exact target bucket wins in its direction"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Farther,
	                                         ENPCVoiceEffort::Shout, 0.f, NoCooldowns), 2);
	TestEqual(TEXT("No exact bucket: nearest rendered one in the direction"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Farther,
	                                         ENPCVoiceEffort::Conversational, 0.f, NoCooldowns), 1);
	TestEqual(TEXT("Direction filters even when buckets fit better elsewhere"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Closer,
	                                         ENPCVoiceEffort::Shout, 0.f, NoCooldowns), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceFindTransitionLine_CooldownAndEmpty,
	"SpatialAudio.Voice.FindTransitionLine.CooldownAndEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceFindTransitionLine_CooldownAndEmpty::RunTest(const FString& Parameters) {
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_A"), ENPCVoiceEffort::Shout,
	                             ENPCVoiceTransitionDir::Farther, TEXT("trans")));
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_B"), ENPCVoiceEffort::Raised,
	                             ENPCVoiceTransitionDir::Farther));

	TMap<FName, float> Cooldowns;
	Cooldowns.Add(FName(TEXT("trans")), 100.f);

	TestEqual(TEXT("Cooldown-blocked exact match yields to the nearest free line"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Farther,
	                                         ENPCVoiceEffort::Shout, /*Now=*/50.f, Cooldowns), 1);
	TestEqual(TEXT("No line in the direction: INDEX_NONE"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Closer,
	                                         ENPCVoiceEffort::Whisper, 0.f, Cooldowns),
	          static_cast<int32>(INDEX_NONE));
	return true;
}

// ─── ResolveBargeInAvailability ───────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeInAvailability_ResolvesPerCategory,
	"SpatialAudio.Voice.BargeInAvailability.ResolvesPerCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeInAvailability_ResolvesPerCategory::RunTest(const FString& Parameters) {
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceLine(TEXT("A"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	FNPCVoiceBargeInAvailability Available = VoiceLogic::ResolveBargeInAvailability(Lines);
	TestFalse(TEXT("Ordinary content services no barge-in reason"),
	          Available.bTransition || Available.bLostSight || Available.bSightRegained);

	Lines.Add(MakeVoiceTransitionLine(TEXT("T"), ENPCVoiceEffort::Shout, ENPCVoiceTransitionDir::Farther));
	Available = VoiceLogic::ResolveBargeInAvailability(Lines);
	TestTrue(TEXT("One Transition row enables effort drift"), Available.bTransition);
	TestFalse(TEXT("...and nothing else"), Available.bLostSight || Available.bSightRegained);

	Lines.Add(MakeVoiceLine(TEXT("L"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::LostSight));
	Available = VoiceLogic::ResolveBargeInAvailability(Lines);
	TestTrue(TEXT("Categories accumulate independently"),
	         Available.bTransition && Available.bLostSight);
	TestFalse(TEXT("An absent category stays absent"), Available.bSightRegained);

	TestTrue(TEXT("Has maps each reason to its own category"),
	         Available.Has(ENPCVoiceBargeInReason::EffortDrift) &&
	         Available.Has(ENPCVoiceBargeInReason::SightLost) &&
	         !Available.Has(ENPCVoiceBargeInReason::SightGained) &&
	         !Available.Has(ENPCVoiceBargeInReason::None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_UnserviceableReasonYieldsToOneWithContent,
	"SpatialAudio.Voice.BargeIn.UnserviceableReasonYieldsToOneWithContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_UnserviceableReasonYieldsToOneWithContent::RunTest(const FString& Parameters) {
	// A sight change and the effort drift it caused arrive on the same tick. If the bank cannot
	// service the sight reason, it must not claim the tick and abort — the drift reason has
	// content, and the sight edge is gone next tick while the playing line's bucket is not, so
	// the barge-in would be lost outright.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;
	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f);
	const ENPCVoiceEffort Drifted = ENPCVoiceEffort::Shout;

	FNPCVoiceBargeInAvailability TransitionOnly;
	TransitionOnly.bTransition = true;

	const VoiceLogic::FBargeInDecision Lost = VoiceLogic::EvaluateBargeIn(
		Playing, Fresh, Drifted, ENPCVoiceSightChange::Lost, TransitionOnly, 0.f, *S);
	TestTrue(TEXT("An unserviceable sight loss falls through to effort drift"),
	         Lost.Reason == ENPCVoiceBargeInReason::EffortDrift);

	// And the mirror: sight content present, transition content missing.
	FNPCVoiceBargeInAvailability SightOnly;
	SightOnly.bLostSight = true;
	TestTrue(TEXT("The serviceable sight reason still outranks drift"),
	         VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::Lost,
	                                     SightOnly, 0.f, *S)
	             .Reason == ENPCVoiceBargeInReason::SightLost);
	TestFalse(TEXT("Drift alone stays dormant without transition content"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::None,
	                                      SightOnly, 0.f, *S)
	              .ShouldBargeIn());
	return true;
}

// ─── EvaluateBargeIn ──────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_FiresOnDriftWithDirection,
	"SpatialAudio.Voice.BargeIn.FiresOnDriftWithDirection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_FiresOnDriftWithDirection::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;  // LastTime far in the past: rate limit clear
	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, /*EndTime=*/100.f);

	const VoiceLogic::FBargeInDecision Away =
		VoiceLogic::EvaluateBargeIn(Playing, Fresh, ENPCVoiceEffort::Shout, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Effort rising mid-line barges in"), Away.ShouldBargeIn());
	TestTrue(TEXT("Rising effort means the listener is getting away"),
	         Away.Dir == ENPCVoiceTransitionDir::Farther);

	const FNPCVoicePlaybackState Shouting = MakeVoicePlayingState(ENPCVoiceEffort::Shout, 100.f);
	const VoiceLogic::FBargeInDecision Closer =
		VoiceLogic::EvaluateBargeIn(Shouting, Fresh, ENPCVoiceEffort::Whisper, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Effort dropping mid-line barges in"), Closer.ShouldBargeIn());
	TestTrue(TEXT("Falling effort means the listener closed in"),
	         Closer.Dir == ENPCVoiceTransitionDir::Closer);

	TestFalse(TEXT("No drift, no barge-in"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, ENPCVoiceEffort::Whisper, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S)
	              .ShouldBargeIn());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_Gates,
	"SpatialAudio.Voice.BargeIn.Gates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_Gates::RunTest(const FString& Parameters) {
	UNPCVoiceSettings* S = NewObject<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;
	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, /*EndTime=*/100.f);
	const ENPCVoiceEffort Drifted = ENPCVoiceEffort::Shout;

	TestFalse(TEXT("Nothing playing: nothing to interrupt"),
	          VoiceLogic::EvaluateBargeIn(FNPCVoicePlaybackState(), Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S)
	              .ShouldBargeIn());

	const FNPCVoicePlaybackState PlayingTransition =
		MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f, /*bIsBargeIn=*/true);
	TestFalse(TEXT("A transition line is never itself interrupted"),
	          VoiceLogic::EvaluateBargeIn(PlayingTransition, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S)
	              .ShouldBargeIn());

	TestFalse(TEXT("A bank with no transition content stays dormant"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::None, FNPCVoiceBargeInAvailability(), 0.f, *S)
	              .ShouldBargeIn());

	FNPCVoiceTransitionState JustFired;
	JustFired.LastTime = 0.f;
	TestFalse(TEXT("Rate limit blocks a second barge-in"),
	          VoiceLogic::EvaluateBargeIn(Playing, JustFired, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(),
	                                      S->TransitionCooldownSeconds - 0.1f, *S)
	              .ShouldBargeIn());
	TestTrue(TEXT("Rate limit releases after the cooldown"),
	         VoiceLogic::EvaluateBargeIn(Playing, JustFired, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(),
	                                     S->TransitionCooldownSeconds, *S)
	             .ShouldBargeIn());

	const FNPCVoicePlaybackState NearlyDone =
		MakeVoicePlayingState(ENPCVoiceEffort::Whisper, S->TransitionMinRemainingTime - 0.01f);
	TestFalse(TEXT("A line about to end is left to finish"),
	          VoiceLogic::EvaluateBargeIn(NearlyDone, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S).ShouldBargeIn());

	S->TransitionBucketDelta = 0;
	TestFalse(TEXT("Delta 0 disables effort barge-ins"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S).ShouldBargeIn());
	TestTrue(TEXT("Delta 0 leaves the sight triggers working"),
	         VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::Lost, MakeVoiceFullBank(), 0.f, *S)
	             .ShouldBargeIn());

	S->TransitionBucketDelta = 2;
	const FNPCVoicePlaybackState OneStepOff = MakeVoicePlayingState(ENPCVoiceEffort::Conversational, 100.f);
	TestFalse(TEXT("A one-step drift is below a delta of 2"),
	          VoiceLogic::EvaluateBargeIn(OneStepOff, Fresh, ENPCVoiceEffort::Raised, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S)
	              .ShouldBargeIn());
	return true;
}

// ─── Playback state transitions ───────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoicePlayback_BeginAndEndLine,
	"SpatialAudio.Voice.Playback.BeginAndEndLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoicePlayback_BeginAndEndLine::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	FNPCVoicePlaybackState Playback;

	FNPCVoiceLineRow Row;
	Row.LineId = FName(TEXT("L001"));
	Row.Bucket = ENPCVoiceEffort::Raised;
	Row.Duration = 3.f;
	VoiceLogic::BeginLine(Playback, Row, /*Now=*/10.f, /*EndPadding=*/0.2f, /*bAsBargeIn=*/false);

	TestTrue(TEXT("Line is marked playing"), Playback.bPlaying);
	TestEqual(TEXT("End time is duration plus padding"), Playback.EndTime, 13.2f);
	TestTrue(TEXT("Active bucket is what the line was rendered at"),
	         Playback.ActiveBucket == ENPCVoiceEffort::Raised);
	TestTrue(TEXT("Line id feeds the no-repeat rule"), Playback.LastLineId == FName(TEXT("L001")));
	TestFalse(TEXT("A Clear-category line is not a transition"), Playback.bActiveIsBargeIn);

	VoiceLogic::EndLine(Playback, /*Now=*/13.2f, *S);
	TestFalse(TEXT("Line is no longer playing"), Playback.bPlaying);
	TestTrue(TEXT("LastLineId survives the line ending"), Playback.LastLineId == FName(TEXT("L001")));
	TestTrue(TEXT("Next line is scheduled inside the normal interval"),
	         Playback.NextLineTime >= 13.2f + S->LineIntervalMin &&
	         Playback.NextLineTime <= 13.2f + S->LineIntervalMax);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoicePlayback_TransitionIsFollowedQuickly,
	"SpatialAudio.Voice.Playback.TransitionIsFollowedQuickly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoicePlayback_TransitionIsFollowedQuickly::RunTest(const FString& Parameters) {
	// A transition announces a change, so the full line at the new effort must not wait out
	// the normal random interval.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	FNPCVoicePlaybackState Playback;

	FNPCVoiceLineRow Row;
	Row.LineId = FName(TEXT("T001"));
	Row.Category = ENPCVoiceCategory::Transition;
	Row.Duration = 2.f;
	VoiceLogic::BeginLine(Playback, Row, 0.f, 0.f, /*bAsBargeIn=*/true);
	TestTrue(TEXT("Transition category is recorded"), Playback.bActiveIsBargeIn);

	VoiceLogic::EndLine(Playback, /*Now=*/2.f, *S);
	TestEqual(TEXT("Follow-up uses the short post-transition delay"),
	          Playback.NextLineTime, 2.f + S->PostTransitionLineDelay);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoicePlayback_BargeInQueuesPastTheFade,
	"SpatialAudio.Voice.Playback.BargeInQueuesPastTheFade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoicePlayback_BargeInQueuesPastTheFade::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	FNPCVoicePlaybackState Playback = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f);
	Playback.ActiveLineId = FName(TEXT("L001"));
	FNPCVoiceTransitionState Transition;

	VoiceLogic::BeginBargeIn(Playback, Transition, /*LineIdx=*/3, /*Now=*/50.f, *S);

	TestFalse(TEXT("The cut line stops playing"), Playback.bPlaying);
	TestTrue(TEXT("Pending transition is armed"), Transition.bPending);
	TestEqual(TEXT("Queued line index is kept"), Transition.PendingLine, 3);
	TestTrue(TEXT("Playback starts only after the declick fade has run"),
	         Transition.PlayTime > 50.f + S->TransitionFadeOutTime);
	TestEqual(TEXT("Rate limit is stamped at trigger, not at playback"), Transition.LastTime, 50.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoicePlayback_IdleReactionBeatsTheReactionWindow,
	"SpatialAudio.Voice.Playback.IdleReactionBeatsTheReactionWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoicePlayback_IdleReactionBeatsTheReactionWindow::RunTest(const FString& Parameters) {
	// A sight change with nothing playing has no line to interrupt, so the only way to react is
	// to schedule the next one sooner. Left alone, the normal silence can outlast the window
	// LostSight / SightRegained content is gated on, and the NPC never mentions the break.
		const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	TestTrue(TEXT("The normal wait can indeed outlast the reaction window"),
	         S->LineIntervalMax > S->SightChangeReactionWindow);

	FNPCVoicePlaybackState Playback;
	Playback.NextLineTime = 100.f + S->LineIntervalMax;
	VoiceLogic::PullInNextLine(Playback, /*Now=*/100.f, *S);
	TestEqual(TEXT("The next line moves inside the window"),
	          Playback.NextLineTime, 100.f + S->PostTransitionLineDelay);

	// Never delays one already due — a line about to start is a faster reaction than this is.
	Playback.NextLineTime = 100.f;
	VoiceLogic::PullInNextLine(Playback, 100.f, *S);
	TestEqual(TEXT("An imminent line is left alone"), Playback.NextLineTime, 100.f);
	return true;
}

// ─── GetEffortReachDistance ───────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceReach_DerivesFromBands,
	"SpatialAudio.Voice.Reach.DerivesFromBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceReach_DerivesFromBands::RunTest(const FString& Parameters) {
	UNPCVoiceSettings* S = NewObject<UNPCVoiceSettings>();
	S->WhisperMaxDistance = 600.f;
	S->ConversationalMaxDistance = 1500.f;
	S->RaisedMaxDistance = 3000.f;
	S->EffortReachHeadroom = 1.25f;

	TestEqual(TEXT("Whisper carries its own band plus headroom"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Whisper), 750.f);
	TestEqual(TEXT("Conversational carries its own band plus headroom"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Conversational), 1875.f);
	TestEqual(TEXT("Raised carries its own band plus headroom"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Raised), 3750.f);
	TestEqual(TEXT("Shout defers to the attenuation asset's own range"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Shout), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceReach_HeadroomOneDiesAtTheBandEdge,
	"SpatialAudio.Voice.Reach.HeadroomOneDiesAtTheBandEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceReach_HeadroomOneDiesAtTheBandEdge::RunTest(const FString& Parameters) {
	// Headroom 1 is the tight configuration: each effort reaches silence exactly where the
	// next one takes over, so walking out of a band silences the line in flight.
	UNPCVoiceSettings* S = NewObject<UNPCVoiceSettings>();
	S->EffortReachHeadroom = 1.f;

	TestEqual(TEXT("Whisper reach equals the whisper/conversational boundary"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Whisper), S->WhisperMaxDistance);
	TestEqual(TEXT("Conversational reach equals its own boundary"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Conversational), S->ConversationalMaxDistance);

	// Reach follows the bands automatically — that is the point of deriving it.
	S->WhisperMaxDistance = 1000.f;
	TestEqual(TEXT("Moving a band moves that effort's reach with it"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Whisper), 1000.f);
	return true;
}

// ─── GetEffortGainDb ──────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceGain_RisesWithEffortAndAnchorsAtShout,
	"SpatialAudio.Voice.Gain.RisesWithEffortAndAnchorsAtShout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceGain_RisesWithEffortAndAnchorsAtShout::RunTest(const FString& Parameters) {
	// Source gain must climb monotonically with effort — this is the only lever that makes
	// crossing a band boundary audible, since two falloff curves that end near each other
	// differ by barely a decibel where they overlap.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	TestTrue(TEXT("Conversational is louder at the source than whisper"),
	         S->GetEffortGainDb(ENPCVoiceEffort::Conversational) >
	         S->GetEffortGainDb(ENPCVoiceEffort::Whisper));
	TestTrue(TEXT("Raised is louder than conversational"),
	         S->GetEffortGainDb(ENPCVoiceEffort::Raised) >
	         S->GetEffortGainDb(ENPCVoiceEffort::Conversational));
	TestTrue(TEXT("Shout is louder than raised"),
	         S->GetEffortGainDb(ENPCVoiceEffort::Shout) >
	         S->GetEffortGainDb(ENPCVoiceEffort::Raised));

	// Shout anchors at 0 and everything scales down, so the shared mixing bus cannot be
	// clipped by summing boosted sources.
	TestEqual(TEXT("Shout is the 0 dB anchor"), S->GetEffortGainDb(ENPCVoiceEffort::Shout), 0.f);
	TestTrue(TEXT("No effort is boosted above the anchor"),
	         S->GetEffortGainDb(ENPCVoiceEffort::Whisper) <= 0.f &&
	         S->GetEffortGainDb(ENPCVoiceEffort::Conversational) <= 0.f &&
	         S->GetEffortGainDb(ENPCVoiceEffort::Raised) <= 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_OccludedContentNeverLeaksIntoClearLoS,
	"SpatialAudio.Voice.SelectLine.OccludedContentNeverLeaksIntoClearLoS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_OccludedContentNeverLeaksIntoClearLoS::RunTest(const FString& Parameters) {
	// Selection never crosses between the visible and occluded halves: every line asserts
	// something about the world, so silence beats contradicting what the player can see.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const TMap<FName, float> NoCooldowns;

	TArray<FNPCVoiceRuntimeLine> OccludedOnly;
	OccludedOnly.Add(MakeVoiceLine(TEXT("OccShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Occluded));

	TestEqual(TEXT("Clear line of sight never falls back to occluded content"),
	          VoiceLogic::SelectLineIndex(OccludedOnly, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("The same bank speaks once the listener is actually occluded"),
	          VoiceLogic::SelectLineIndex(OccludedOnly, ENPCVoiceEffort::Shout, MakeVoiceGenericOccluded(),
	                                      NAME_None, 0.f, NoCooldowns, *S), 0);

	// Partial occlusion below the threshold still counts as visible.
	TestEqual(TEXT("Below the occlusion threshold counts as clear line of sight"),
	          VoiceLogic::SelectLineIndex(OccludedOnly, ENPCVoiceEffort::Shout,
	                                      MakeVoiceAcoustic(S->OcclusionShiftThreshold - 0.01f),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));

	// And the mirror: a visible-only bank stays silent while the listener is hidden.
	TArray<FNPCVoiceRuntimeLine> ClearOnly;
	ClearOnly.Add(MakeVoiceLine(TEXT("ClearShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	TestEqual(TEXT("Occluded listeners never fall back to visible content"),
	          VoiceLogic::SelectLineIndex(ClearOnly, ENPCVoiceEffort::Shout, MakeVoiceGenericOccluded(),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));
	return true;
}

// ─── ResolveCategoryPreference ────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceContext_PicksTheAcousticSituation,
	"SpatialAudio.Voice.Context.PicksTheAcousticSituation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceContext_PicksTheAcousticSituation::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	// Visible and unobstructed, near or far alike: nothing special to say about the acoustics,
	// because distance is already carried by the effort bucket rather than by a category.
	TestTrue(TEXT("Near and visible resolves to Clear"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.f, 300.f, 300.f), *S)[0] ==
	         ENPCVoiceCategory::Clear);
	TestTrue(TEXT("Far and visible resolves to Clear as well"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.f, 5000.f, 5000.f), *S)[0] ==
	         ENPCVoiceCategory::Clear);

	// The signature case: a couple of steps apart, but the sound travels many times further.
	FNPCVoiceAcousticState BehindWall = MakeVoiceAcoustic(1.f, /*DirectCm=*/300.f, /*EffectiveCm=*/2400.f);
	TestTrue(TEXT("Close but heavily detoured resolves to BehindWall"),
	         VoiceLogic::ResolveCategoryPreference(BehindWall, *S)[0] == ENPCVoiceCategory::BehindWall);

	// Hidden, but the path is barely longer than the straight line.
	FNPCVoiceAcousticState Corner = MakeVoiceAcoustic(1.f, /*DirectCm=*/1000.f, /*EffectiveCm=*/1100.f);
	TestTrue(TEXT("Barely detoured resolves to AroundCorner"),
	         VoiceLogic::ResolveCategoryPreference(Corner, *S)[0] == ENPCVoiceCategory::AroundCorner);

	// Same geometry as BehindWall, but sight was just lost — reacting outranks describing.
	FNPCVoiceAcousticState JustLost = BehindWall;
	JustLost.bSightReactionPending = true;
	TestTrue(TEXT("A fresh line-of-sight break outranks the spatial contexts"),
	         VoiceLogic::ResolveCategoryPreference(JustLost, *S)[0] == ENPCVoiceCategory::LostSight);
	TestTrue(TEXT("The spatial context is still available behind it"),
	         VoiceLogic::ResolveCategoryPreference(JustLost, *S).Contains(ENPCVoiceCategory::BehindWall));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceContext_VisibleHalfSplitsOnObstruction,
	"SpatialAudio.Voice.Context.VisibleHalfSplitsOnObstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceContext_VisibleHalfSplitsOnObstruction::RunTest(const FString& Parameters) {
	// Clear asserts an unobstructed straight path. That stops being true as soon as one offset
	// sample blocks — far below the occlusion that counts as hidden — so the visible half needs
	// a second context or the NPC narrates "nothing between us" at a listener behind a pillar.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const float Far = 5000.f;

	TestTrue(TEXT("An unobstructed listener resolves to Clear at any distance"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.f, Far, Far), *S)[0] ==
	         ENPCVoiceCategory::Clear);

	// Same geometry, just something clipping the line.
	const TArray<ENPCVoiceCategory, TInlineAllocator<4>> Obstructed =
		VoiceLogic::ResolveCategoryPreference(
			MakeVoiceAcoustic(S->PartialOcclusionThreshold, Far, Far), *S);
	TestTrue(TEXT("A blocked sample switches the visible half's context"),
	         Obstructed[0] == ENPCVoiceCategory::PartiallyOccluded);
	TestTrue(TEXT("Generic Clear stays the last resort so a partial bank still speaks"),
	         Obstructed.Last() == ENPCVoiceCategory::Clear);

	// The partial band reaches all the way up to hidden.
	TestTrue(TEXT("Just short of hidden is still the visible half, partially blocked"),
	         VoiceLogic::ResolveCategoryPreference(
		         MakeVoiceAcoustic(S->OcclusionShiftThreshold - 0.01f), *S)[0] ==
	         ENPCVoiceCategory::PartiallyOccluded);
	TestTrue(TEXT("At the hidden threshold it crosses to the occluded half"),
	         !VoiceLogic::ResolveCategoryPreference(
		          MakeVoiceAcoustic(S->OcclusionShiftThreshold), *S)
		          .Contains(ENPCVoiceCategory::PartiallyOccluded));

	// 0 = off collapses the visible half back to one band.
	UNPCVoiceSettings* Off = NewObject<UNPCVoiceSettings>();
	Off->PartialOcclusionThreshold = 0.f;
	TestTrue(TEXT("Disabled, a partially blocked listener is Clear again"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.5f), *Off)[0] ==
	         ENPCVoiceCategory::Clear);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceContext_NeverCrossesTheVisibilitySplit,
	"SpatialAudio.Voice.Context.NeverCrossesTheVisibilitySplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceContext_NeverCrossesTheVisibilitySplit::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	// Every ladder must end in the generic entry for its own half and contain nothing from
	// the other — that invariant is what keeps a line from contradicting what the player sees.
	const TArray<ENPCVoiceCategory, TInlineAllocator<4>> Visible =
		VoiceLogic::ResolveCategoryPreference(
			MakeVoiceAcoustic(0.f, 5000.f, 5000.f), *S);
	TestTrue(TEXT("A visible ladder ends in Clear"), Visible.Last() == ENPCVoiceCategory::Clear);
	TestFalse(TEXT("A visible ladder never offers occluded content"),
	          Visible.Contains(ENPCVoiceCategory::Occluded) ||
	          Visible.Contains(ENPCVoiceCategory::BehindWall) ||
	          Visible.Contains(ENPCVoiceCategory::AroundCorner) ||
	          Visible.Contains(ENPCVoiceCategory::LostSight));

	FNPCVoiceAcousticState Hidden = MakeVoiceAcoustic(1.f, 300.f, 2400.f);
	Hidden.bSightReactionPending = true;
	const TArray<ENPCVoiceCategory, TInlineAllocator<4>> Occluded =
		VoiceLogic::ResolveCategoryPreference(Hidden, *S);
	TestTrue(TEXT("An occluded ladder ends in Occluded"),
	         Occluded.Last() == ENPCVoiceCategory::Occluded);
	TestFalse(TEXT("An occluded ladder never offers visible content"),
	          Occluded.Contains(ENPCVoiceCategory::Clear) ||
	          Occluded.Contains(ENPCVoiceCategory::PartiallyOccluded));

	// Barge-in lines are reachable only through FindBargeInLine.
	TestFalse(TEXT("Transition content is never offered to normal scheduling"),
	          Visible.Contains(ENPCVoiceCategory::Transition) ||
	          Occluded.Contains(ENPCVoiceCategory::Transition));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_VisibilityOutranksEffortDrift,
	"SpatialAudio.Voice.BargeIn.VisibilityOutranksEffortDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_VisibilityOutranksEffortDrift::RunTest(const FString& Parameters) {
	// Losing sight inflates the acoustic path, which climbs the effort bands, so both
	// triggers fire on the same tick. Reporting effort drift there would tell the listener
	// they are "moving away" when they only stepped behind a wall a step from where they were.
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;
	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f);
	const ENPCVoiceEffort Drifted = ENPCVoiceEffort::Shout;

	const VoiceLogic::FBargeInDecision Lost = VoiceLogic::EvaluateBargeIn(
		Playing, Fresh, Drifted, ENPCVoiceSightChange::Lost, MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Sight loss wins over the effort drift it caused"),
	         Lost.Reason == ENPCVoiceBargeInReason::SightLost);

	const VoiceLogic::FBargeInDecision Gained = VoiceLogic::EvaluateBargeIn(
		Playing, Fresh, Drifted, ENPCVoiceSightChange::Gained, MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Regaining sight wins too"),
	         Gained.Reason == ENPCVoiceBargeInReason::SightGained);

	// With no visibility change the effort trigger is still reachable.
	const VoiceLogic::FBargeInDecision Drift = VoiceLogic::EvaluateBargeIn(
		Playing, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Effort drift still fires when visibility is steady"),
	         Drift.Reason == ENPCVoiceBargeInReason::EffortDrift);
	TestTrue(TEXT("And still reports its direction"), Drift.Dir == ENPCVoiceTransitionDir::Farther);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_SightTriggersShareTheCommonGates,
	"SpatialAudio.Voice.BargeIn.SightTriggersShareTheCommonGates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_SightTriggersShareTheCommonGates::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;
	const ENPCVoiceEffort Steady = ENPCVoiceEffort::Whisper;

	TestFalse(TEXT("Nothing playing: nothing to interrupt"),
	          VoiceLogic::EvaluateBargeIn(FNPCVoicePlaybackState(), Fresh, Steady,
	                                      ENPCVoiceSightChange::Lost, MakeVoiceFullBank(), 0.f, *S).ShouldBargeIn());

	const FNPCVoicePlaybackState PlayingBargeIn =
		MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f, /*bIsBargeIn=*/true);
	TestFalse(TEXT("A barge-in is never itself interrupted, even by a sight change"),
	          VoiceLogic::EvaluateBargeIn(PlayingBargeIn, Fresh, Steady,
	                                      ENPCVoiceSightChange::Lost, MakeVoiceFullBank(), 0.f, *S).ShouldBargeIn());

	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f);
	FNPCVoiceTransitionState JustFired;
	JustFired.LastTime = 0.f;
	TestFalse(TEXT("The rate limit covers sight triggers too"),
	          VoiceLogic::EvaluateBargeIn(Playing, JustFired, Steady, ENPCVoiceSightChange::Lost,
	                                      MakeVoiceFullBank(), S->TransitionCooldownSeconds - 0.1f, *S)
	              .ShouldBargeIn());

	const FNPCVoicePlaybackState NearlyDone =
		MakeVoicePlayingState(ENPCVoiceEffort::Whisper, S->TransitionMinRemainingTime - 0.01f);
	TestFalse(TEXT("A line about to end is left to finish"),
	          VoiceLogic::EvaluateBargeIn(NearlyDone, Fresh, Steady, ENPCVoiceSightChange::Lost,
	                                      MakeVoiceFullBank(), 0.f, *S).ShouldBargeIn());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_ReasonPicksItsCategory,
	"SpatialAudio.Voice.BargeIn.ReasonPicksItsCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_ReasonPicksItsCategory::RunTest(const FString& Parameters) {
	TestTrue(TEXT("Sight lost draws from LostSight"),
	         VoiceLogic::BargeInCategory(ENPCVoiceBargeInReason::SightLost) ==
	         ENPCVoiceCategory::LostSight);
	TestTrue(TEXT("Sight regained draws from SightRegained"),
	         VoiceLogic::BargeInCategory(ENPCVoiceBargeInReason::SightGained) ==
	         ENPCVoiceCategory::SightRegained);
	TestTrue(TEXT("Effort drift draws from Transition"),
	         VoiceLogic::BargeInCategory(ENPCVoiceBargeInReason::EffortDrift) ==
	         ENPCVoiceCategory::Transition);

	// Sight lines carry no direction, so the lookup must match on None rather than skip them.
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceLine(TEXT("L_Raised"), ENPCVoiceEffort::Raised, ENPCVoiceCategory::LostSight));
	const TMap<FName, float> NoCooldowns;
	TestEqual(TEXT("A LostSight line is reachable as a barge-in replacement"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::LostSight,
	                                      ENPCVoiceTransitionDir::None, ENPCVoiceEffort::Raised,
	                                      0.f, NoCooldowns), 0);
	TestEqual(TEXT("A bank without the reason's category aborts the barge-in"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::SightRegained,
	                                      ENPCVoiceTransitionDir::None, ENPCVoiceEffort::Raised,
	                                      0.f, NoCooldowns),
	          static_cast<int32>(INDEX_NONE));
	return true;
}
