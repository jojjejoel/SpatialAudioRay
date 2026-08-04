#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Voice/NPCVoiceLogic.h"
#include "Voice/NPCVoiceSettings.h"

namespace {
	FNPCVoiceRuntimeLine MakeVoiceLine(const TCHAR* Id, ENPCVoiceEffort Effort, ENPCVoiceCategory Category,
	                                   const TCHAR* Group = TEXT(""),
	                                   ENPCVoiceTransitionDir Dir = ENPCVoiceTransitionDir::None) {
		FNPCVoiceRuntimeLine Line;
		Line.Row.LineId = FName(Id);
		Line.Row.Bucket = Effort;
		Line.Row.Category = Category;
		Line.Row.CooldownGroup = FName(Group);
		Line.Row.Direction = Dir;
		return Line;
	}

	FNPCVoiceRuntimeLine MakeVoiceTransitionLine(const TCHAR* Id, ENPCVoiceEffort Effort,
	                                             ENPCVoiceTransitionDir Dir,
	                                             const TCHAR* Group = TEXT("")) {
		return MakeVoiceLine(Id, Effort, ENPCVoiceCategory::Transition, Group, Dir);
	}

	FNPCVoiceAcousticState MakeVoiceAcoustic(float Occlusion, float DirectCm = 500.f,
	                                         float EffectiveCm = 500.f) {
		FNPCVoiceAcousticState Acoustic;
		Acoustic.Occlusion = Occlusion;
		Acoustic.DirectDistanceCm = DirectCm;
		Acoustic.EffectiveDistanceCm = EffectiveCm;
		return Acoustic;
	}

	FNPCVoiceAcousticState MakeVoiceGenericOccluded() {
		return MakeVoiceAcoustic(1.f, /*DirectCm=*/1500.f, /*EffectiveCm=*/3000.f);
	}

	FNPCVoicePlaybackState MakeVoicePlayingState(ENPCVoiceEffort ActiveEffort, float EndTime,
	                                             bool bIsTransition = false) {
		FNPCVoicePlaybackState Playback;
		Playback.bPlaying = true;
		Playback.ActiveEffort = ActiveEffort;
		Playback.EndTime = EndTime;
		Playback.bActiveIsBargeIn = bIsTransition;
		return Playback;
	}

	FNPCVoiceBargeInAvailability MakeVoiceFullBank() {
		return {
			/*bTransition=*/true, /*bLostSight=*/true, /*bSightRegained=*/true
		};
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceMapToEffort_DistanceBands,
	"SpatialAudioRay.Voice.MapToEffort.DistanceBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceMapToEffort_DistanceBands::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	TestTrue(TEXT("Close = whisper"),
	         VoiceLogic::MapToEffort(100.f, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Whisper band edge is inclusive"),
	         VoiceLogic::MapToEffort(S->WhisperMaxDistance, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Past whisper edge = conversational"),
	         VoiceLogic::MapToEffort(S->WhisperMaxDistance + 1.f, *S) == ENPCVoiceEffort::Conversational);
	TestTrue(TEXT("Mid range = raised"),
	         VoiceLogic::MapToEffort(S->ConversationalMaxDistance + 1.f, *S) == ENPCVoiceEffort::Raised);
	TestTrue(TEXT("Far = shout"),
	         VoiceLogic::MapToEffort(S->RaisedMaxDistance + 1.f, *S) == ENPCVoiceEffort::Shout);

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceHysteresis_FirstSampleCommitsInstantly,
	"SpatialAudioRay.Voice.Hysteresis.FirstSampleCommitsInstantly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceHysteresis_FirstSampleCommitsInstantly::RunTest(const FString& Parameters) {
	FNPCVoiceEffortHysteresis State;
	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Shout, /*Now=*/10.f, /*Dwell=*/1.f);

	TestTrue(TEXT("First sample commits without waiting out the dwell"),
	         State.Committed == ENPCVoiceEffort::Shout);
	TestTrue(TEXT("Candidate matches the commit"), State.Candidate == ENPCVoiceEffort::Shout);
	TestTrue(TEXT("State is seeded"), State.bInitialized);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceHysteresis_CommitsOnlyAfterDwell,
	"SpatialAudioRay.Voice.Hysteresis.CommitsOnlyAfterDwell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceHysteresis_CommitsOnlyAfterDwell::RunTest(const FString& Parameters) {
	FNPCVoiceEffortHysteresis State;
	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Whisper, 0.f, 1.f);

	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Shout, 10.f, 1.f);
	TestTrue(TEXT("A new candidate does not commit immediately"),
	         State.Committed == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Candidate is recorded"), State.Candidate == ENPCVoiceEffort::Shout);

	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Shout, 10.9f, 1.f);
	TestTrue(TEXT("Still short of the dwell time"), State.Committed == ENPCVoiceEffort::Whisper);

	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Shout, 11.f, 1.f);
	TestTrue(TEXT("Commits once the candidate has persisted for the dwell time"),
	         State.Committed == ENPCVoiceEffort::Shout);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceHysteresis_FlipBackCancelsCommit,
	"SpatialAudioRay.Voice.Hysteresis.FlipBackCancelsCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceHysteresis_FlipBackCancelsCommit::RunTest(const FString& Parameters) {
	FNPCVoiceEffortHysteresis State;
	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Whisper, 0.f, 1.f);
	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Shout, 10.f, 1.f);
	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Whisper, 10.5f, 1.f);
	VoiceLogic::AdvanceEffortHysteresis(State, ENPCVoiceEffort::Whisper, 20.f, 1.f);

	TestTrue(TEXT("Flipping back before the dwell expires never commits the intruder"),
	         State.Committed == ENPCVoiceEffort::Whisper);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSight_ReportsCrossingsOnly,
	"SpatialAudioRay.Voice.Sight.ReportsCrossingsOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSight_ReportsCrossingsOnly::RunTest(const FString& Parameters) {
	FNPCVoiceSightState State;

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
	"SpatialAudioRay.Voice.Sight.WindowGatesReactionContentOnBothSides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSight_WindowGatesReactionContentOnBothSides::RunTest(const FString& Parameters) {
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
	"SpatialAudioRay.Voice.Sight.ReactionIsSpokenOncePerCrossing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSight_ReactionIsSpokenOncePerCrossing::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	FNPCVoiceSightState State;
	VoiceLogic::AdvanceSightState(State, /*bHidden=*/true, /*Now=*/0.f);
	VoiceLogic::AdvanceSightState(State, /*bHidden=*/false, /*Now=*/10.f);

	TestTrue(TEXT("The crossing owes a reaction"),
	         VoiceLogic::IsSightReactionPending(State, 10.f, *S));

	VoiceLogic::MarkSightReactionDelivered(State, ENPCVoiceCategory::AroundCorner);
	TestTrue(TEXT("An unrelated line does not settle the crossing"),
	         VoiceLogic::IsSightReactionPending(State, 10.5f, *S));

	VoiceLogic::MarkSightReactionDelivered(State, ENPCVoiceCategory::SightRegained);
	TestFalse(TEXT("Once spoken the reaction stops being offered, window still open"),
	          VoiceLogic::IsSightReactionPending(State, 10.5f, *S));

	FNPCVoiceSightState Unreacted;
	VoiceLogic::AdvanceSightState(Unreacted, true, 0.f);
	VoiceLogic::AdvanceSightState(Unreacted, false, 10.f);
	TestTrue(TEXT("Still owed at the window edge"),
	         VoiceLogic::IsSightReactionPending(Unreacted, 10.f + S->SightChangeReactionWindow, *S));
	TestFalse(TEXT("Expired past it"),
	          VoiceLogic::IsSightReactionPending(Unreacted,
	                                             10.f + S->SightChangeReactionWindow + 0.01f, *S));

	TestTrue(TEXT("Losing sight again reports the crossing"),
	         VoiceLogic::AdvanceSightState(State, /*bHidden=*/true, 12.f) ==
	         ENPCVoiceSightChange::Lost);
	TestTrue(TEXT("...and re-arms the reaction"),
	         VoiceLogic::IsSightReactionPending(State, 12.f, *S));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceCooldown_BlocksAndExpires,
	"SpatialAudioRay.Voice.Cooldown.BlocksAndExpires",
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

	VoiceLogic::StampCooldown(Cooldowns, NAME_None, 10.f, 5.f);
	TestFalse(TEXT("None group is never blocked"), VoiceLogic::IsCooldownBlocked(NAME_None, 11.f, Cooldowns));
	TestFalse(TEXT("None group is never stamped into the map"), Cooldowns.Contains(NAME_None));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_EffortAndCategory,
	"SpatialAudioRay.Voice.SelectLine.EffortAndCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_EffortAndCategory::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceLine(TEXT("ClearShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	Lines.Add(MakeVoiceLine(TEXT("OccludedShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Occluded));
	Lines.Add(MakeVoiceLine(TEXT("ClearWhisper"), ENPCVoiceEffort::Whisper, ENPCVoiceCategory::Clear));
	const TMap<FName, float> NoCooldowns;

	TestEqual(TEXT("Clear LoS picks the Clear line in the committed effort"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S), 0);
	TestEqual(TEXT("Occluded picks the occlusion-keyed line in the same effort"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Shout, MakeVoiceGenericOccluded(),
	                                      NAME_None, 0.f, NoCooldowns, *S), 1);
	TestEqual(TEXT("A different committed effort selects that effort's line"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Whisper, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S), 2);
	TestEqual(TEXT("Effort with no lines at all yields INDEX_NONE"),
	          VoiceLogic::SelectLineIndex(Lines, ENPCVoiceEffort::Raised, MakeVoiceAcoustic(0.f),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_SoftConstraintsRelax,
	"SpatialAudioRay.Voice.SelectLine.SoftConstraintsRelax",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_SoftConstraintsRelax::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const TMap<FName, float> NoCooldowns;

	TArray<FNPCVoiceRuntimeLine> GenericOccludedOnly;
	GenericOccludedOnly.Add(
		MakeVoiceLine(TEXT("OccShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Occluded));
	FNPCVoiceAcousticState BehindWall = MakeVoiceAcoustic(1.f, /*DirectCm=*/300.f, /*EffectiveCm=*/2400.f);
	TestEqual(TEXT("A context with no lines falls through to its half's generic entry"),
	          VoiceLogic::SelectLineIndex(GenericOccludedOnly, ENPCVoiceEffort::Shout, BehindWall,
	                                      NAME_None, 0.f, NoCooldowns, *S), 0);

	TArray<FNPCVoiceRuntimeLine> ClearOnly;
	ClearOnly.Add(MakeVoiceLine(TEXT("ClearShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	TestEqual(TEXT("A single-line effort repeats rather than falling silent"),
	          VoiceLogic::SelectLineIndex(ClearOnly, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      FName(TEXT("ClearShout")), 0.f, NoCooldowns, *S), 0);

	TArray<FNPCVoiceRuntimeLine> Pair = ClearOnly;
	Pair.Add(MakeVoiceLine(TEXT("ClearShoutB"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	TestEqual(TEXT("No-repeat is honored when an alternative exists"),
	          VoiceLogic::SelectLineIndex(Pair, ENPCVoiceEffort::Shout, MakeVoiceAcoustic(0.f),
	                                      FName(TEXT("ClearShout")), 0.f, NoCooldowns, *S), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_CooldownIsHard,
	"SpatialAudioRay.Voice.SelectLine.CooldownIsHard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_CooldownIsHard::RunTest(const FString& Parameters) {
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceFindTransitionLine_DirectionAndEffort,
	"SpatialAudioRay.Voice.FindTransitionLine.DirectionAndEffort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceFindTransitionLine_DirectionAndEffort::RunTest(const FString& Parameters) {
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_CloserWhisper"), ENPCVoiceEffort::Whisper,
	                                  ENPCVoiceTransitionDir::Closer));
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_FartherRaised"), ENPCVoiceEffort::Raised,
	                                  ENPCVoiceTransitionDir::Farther));
	Lines.Add(MakeVoiceTransitionLine(TEXT("T_FartherShout"), ENPCVoiceEffort::Shout,
	                                  ENPCVoiceTransitionDir::Farther));
	const TMap<FName, float> NoCooldowns;

	TestEqual(TEXT("Exact target effort wins in its direction"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Farther,
	                                      ENPCVoiceEffort::Shout, 0.f, NoCooldowns), 2);
	TestEqual(TEXT("No exact effort: nearest rendered one in the direction"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Farther,
	                                      ENPCVoiceEffort::Conversational, 0.f, NoCooldowns), 1);
	TestEqual(TEXT("Direction filters even when efforts fit better elsewhere"),
	          VoiceLogic::FindBargeInLine(Lines, ENPCVoiceCategory::Transition, ENPCVoiceTransitionDir::Closer,
	                                      ENPCVoiceEffort::Shout, 0.f, NoCooldowns), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceFindTransitionLine_CooldownAndEmpty,
	"SpatialAudioRay.Voice.FindTransitionLine.CooldownAndEmpty",
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeInAvailability_ResolvesPerCategory,
	"SpatialAudioRay.Voice.BargeInAvailability.ResolvesPerCategory",
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
	"SpatialAudioRay.Voice.BargeIn.UnserviceableReasonYieldsToOneWithContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_UnserviceableReasonYieldsToOneWithContent::RunTest(const FString& Parameters) {
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_FiresOnDriftWithDirection,
	"SpatialAudioRay.Voice.BargeIn.FiresOnDriftWithDirection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_FiresOnDriftWithDirection::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;
	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, /*EndTime=*/100.f);

	const VoiceLogic::FBargeInDecision Away =
		VoiceLogic::EvaluateBargeIn(Playing, Fresh, ENPCVoiceEffort::Shout, ENPCVoiceSightChange::None,
		                            MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Effort rising mid-line barges in"), Away.ShouldBargeIn());
	TestTrue(TEXT("Rising effort means the listener is getting away"),
	         Away.Dir == ENPCVoiceTransitionDir::Farther);

	const FNPCVoicePlaybackState Shouting = MakeVoicePlayingState(ENPCVoiceEffort::Shout, 100.f);
	const VoiceLogic::FBargeInDecision Closer =
		VoiceLogic::EvaluateBargeIn(Shouting, Fresh, ENPCVoiceEffort::Whisper, ENPCVoiceSightChange::None,
		                            MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Effort dropping mid-line barges in"), Closer.ShouldBargeIn());
	TestTrue(TEXT("Falling effort means the listener closed in"),
	         Closer.Dir == ENPCVoiceTransitionDir::Closer);

	TestFalse(TEXT("No drift, no barge-in"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, ENPCVoiceEffort::Whisper, ENPCVoiceSightChange::None,
	                                      MakeVoiceFullBank(), 0.f, *S)
	          .ShouldBargeIn());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_Gates,
	"SpatialAudioRay.Voice.BargeIn.Gates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_Gates::RunTest(const FString& Parameters) {
	UNPCVoiceSettings* S = NewObject<UNPCVoiceSettings>();
	const FNPCVoiceTransitionState Fresh;
	const FNPCVoicePlaybackState Playing = MakeVoicePlayingState(ENPCVoiceEffort::Whisper, /*EndTime=*/100.f);
	const ENPCVoiceEffort Drifted = ENPCVoiceEffort::Shout;

	TestFalse(TEXT("Nothing playing: nothing to interrupt"),
	          VoiceLogic::EvaluateBargeIn(FNPCVoicePlaybackState(), Fresh, Drifted, ENPCVoiceSightChange::None,
	                                      MakeVoiceFullBank(), 0.f, *S)
	          .ShouldBargeIn());

	const FNPCVoicePlaybackState PlayingTransition =
		MakeVoicePlayingState(ENPCVoiceEffort::Whisper, 100.f, /*bIsBargeIn=*/true);
	TestFalse(TEXT("A transition line is never itself interrupted"),
	          VoiceLogic::EvaluateBargeIn(PlayingTransition, Fresh, Drifted, ENPCVoiceSightChange::None,
	                                      MakeVoiceFullBank(), 0.f, *S)
	          .ShouldBargeIn());

	TestFalse(TEXT("A bank with no transition content stays dormant"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::None,
	                                      FNPCVoiceBargeInAvailability(), 0.f, *S)
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
	          VoiceLogic::EvaluateBargeIn(NearlyDone, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(),
	                                      0.f, *S).ShouldBargeIn());

	S->TransitionEffortDelta = 0;
	TestFalse(TEXT("Delta 0 disables effort barge-ins"),
	          VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f,
	                                      *S).ShouldBargeIn());
	TestTrue(TEXT("Delta 0 leaves the sight triggers working"),
	         VoiceLogic::EvaluateBargeIn(Playing, Fresh, Drifted, ENPCVoiceSightChange::Lost, MakeVoiceFullBank(), 0.f,
	                                     *S)
	         .ShouldBargeIn());

	S->TransitionEffortDelta = 2;
	const FNPCVoicePlaybackState OneStepOff = MakeVoicePlayingState(ENPCVoiceEffort::Conversational, 100.f);
	TestFalse(TEXT("A one-step drift is below a delta of 2"),
	          VoiceLogic::EvaluateBargeIn(OneStepOff, Fresh, ENPCVoiceEffort::Raised, ENPCVoiceSightChange::None,
	                                      MakeVoiceFullBank(), 0.f, *S)
	          .ShouldBargeIn());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoicePlayback_BeginAndEndLine,
	"SpatialAudioRay.Voice.Playback.BeginAndEndLine",
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
	TestTrue(TEXT("Active effort is what the line was rendered at"),
	         Playback.ActiveEffort == ENPCVoiceEffort::Raised);
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
	"SpatialAudioRay.Voice.Playback.TransitionIsFollowedQuickly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoicePlayback_TransitionIsFollowedQuickly::RunTest(const FString& Parameters) {
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
	"SpatialAudioRay.Voice.Playback.BargeInQueuesPastTheFade",
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
	"SpatialAudioRay.Voice.Playback.IdleReactionBeatsTheReactionWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoicePlayback_IdleReactionBeatsTheReactionWindow::RunTest(const FString& Parameters) {
	UNPCVoiceSettings* S = NewObject<UNPCVoiceSettings>();
	S->LineIntervalMax = 9.f;
	S->SightChangeReactionWindow = 6.f;
	S->PostTransitionLineDelay = 0.272f;

	FNPCVoicePlaybackState Playback;
	Playback.NextLineTime = 100.f + S->LineIntervalMax;
	VoiceLogic::PullInNextLine(Playback, /*Now=*/100.f, *S);
	TestTrue(TEXT("A wait that outlasted the window now lands inside it"),
	         Playback.NextLineTime < 100.f + S->SightChangeReactionWindow);
	TestEqual(TEXT("The next line moves to the post-transition delay"),
	          Playback.NextLineTime, 100.f + S->PostTransitionLineDelay);

	Playback.NextLineTime = 100.f;
	VoiceLogic::PullInNextLine(Playback, 100.f, *S);
	TestEqual(TEXT("An imminent line is left alone"), Playback.NextLineTime, 100.f);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceReach_DerivesFromBands,
	"SpatialAudioRay.Voice.Reach.DerivesFromBands",
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
	"SpatialAudioRay.Voice.Reach.HeadroomOneDiesAtTheBandEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceReach_HeadroomOneDiesAtTheBandEdge::RunTest(const FString& Parameters) {
	UNPCVoiceSettings* S = NewObject<UNPCVoiceSettings>();
	S->EffortReachHeadroom = 1.f;

	TestEqual(TEXT("Whisper reach equals the whisper/conversational boundary"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Whisper), S->WhisperMaxDistance);
	TestEqual(TEXT("Conversational reach equals its own boundary"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Conversational), S->ConversationalMaxDistance);

	S->WhisperMaxDistance = 1000.f;
	TestEqual(TEXT("Moving a band moves that effort's reach with it"),
	          S->GetEffortReachDistance(ENPCVoiceEffort::Whisper), 1000.f);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceGain_RisesWithEffortAndAnchorsAtShout,
	"SpatialAudioRay.Voice.Gain.RisesWithEffortAndAnchorsAtShout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceGain_RisesWithEffortAndAnchorsAtShout::RunTest(const FString& Parameters) {
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

	TestEqual(TEXT("Shout is the 0 dB anchor"), S->GetEffortGainDb(ENPCVoiceEffort::Shout), 0.f);
	TestTrue(TEXT("No effort is boosted above the anchor"),
	         S->GetEffortGainDb(ENPCVoiceEffort::Whisper) <= 0.f &&
	         S->GetEffortGainDb(ENPCVoiceEffort::Conversational) <= 0.f &&
	         S->GetEffortGainDb(ENPCVoiceEffort::Raised) <= 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceSelectLine_OccludedContentNeverLeaksIntoClearLoS,
	"SpatialAudioRay.Voice.SelectLine.OccludedContentNeverLeaksIntoClearLoS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceSelectLine_OccludedContentNeverLeaksIntoClearLoS::RunTest(const FString& Parameters) {
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

	TestEqual(TEXT("Below the occlusion threshold counts as clear line of sight"),
	          VoiceLogic::SelectLineIndex(OccludedOnly, ENPCVoiceEffort::Shout,
	                                      MakeVoiceAcoustic(S->OcclusionShiftThreshold - 0.01f),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));

	TArray<FNPCVoiceRuntimeLine> ClearOnly;
	ClearOnly.Add(MakeVoiceLine(TEXT("ClearShout"), ENPCVoiceEffort::Shout, ENPCVoiceCategory::Clear));
	TestEqual(TEXT("Occluded listeners never fall back to visible content"),
	          VoiceLogic::SelectLineIndex(ClearOnly, ENPCVoiceEffort::Shout, MakeVoiceGenericOccluded(),
	                                      NAME_None, 0.f, NoCooldowns, *S),
	          static_cast<int32>(INDEX_NONE));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceContext_PicksTheAcousticSituation,
	"SpatialAudioRay.Voice.Context.PicksTheAcousticSituation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceContext_PicksTheAcousticSituation::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	TestTrue(TEXT("Near and visible resolves to Clear"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.f, 300.f, 300.f), *S)[0] ==
	         ENPCVoiceCategory::Clear);
	TestTrue(TEXT("Far and visible resolves to Clear as well"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.f, 5000.f, 5000.f), *S)[0] ==
	         ENPCVoiceCategory::Clear);

	FNPCVoiceAcousticState BehindWall = MakeVoiceAcoustic(1.f, /*DirectCm=*/300.f, /*EffectiveCm=*/2400.f);
	TestTrue(TEXT("Close but heavily detoured resolves to BehindWall"),
	         VoiceLogic::ResolveCategoryPreference(BehindWall, *S)[0] == ENPCVoiceCategory::BehindWall);

	FNPCVoiceAcousticState Corner = MakeVoiceAcoustic(1.f, /*DirectCm=*/1000.f, /*EffectiveCm=*/1100.f);
	TestTrue(TEXT("Barely detoured resolves to AroundCorner"),
	         VoiceLogic::ResolveCategoryPreference(Corner, *S)[0] == ENPCVoiceCategory::AroundCorner);

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
	"SpatialAudioRay.Voice.Context.VisibleHalfSplitsOnObstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceContext_VisibleHalfSplitsOnObstruction::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();
	const float Far = 5000.f;

	TestTrue(TEXT("An unobstructed listener resolves to Clear at any distance"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.f, Far, Far), *S)[0] ==
	         ENPCVoiceCategory::Clear);

	const VoiceLogic::FCategoryPreference Obstructed =
		VoiceLogic::ResolveCategoryPreference(
			MakeVoiceAcoustic(S->PartialOcclusionThreshold, Far, Far), *S);
	TestTrue(TEXT("A blocked sample switches the visible half's context"),
	         Obstructed[0] == ENPCVoiceCategory::PartiallyOccluded);
	TestTrue(TEXT("Generic Clear stays the last resort so a partial bank still speaks"),
	         Obstructed.Last() == ENPCVoiceCategory::Clear);

	TestTrue(TEXT("Just short of hidden is still the visible half, partially blocked"),
	         VoiceLogic::ResolveCategoryPreference(
		         MakeVoiceAcoustic(S->OcclusionShiftThreshold - 0.01f), *S)[0] ==
	         ENPCVoiceCategory::PartiallyOccluded);
	TestTrue(TEXT("At the hidden threshold it crosses to the occluded half"),
	         !VoiceLogic::ResolveCategoryPreference(
		         MakeVoiceAcoustic(S->OcclusionShiftThreshold), *S)
	         .Contains(ENPCVoiceCategory::PartiallyOccluded));

	UNPCVoiceSettings* Off = NewObject<UNPCVoiceSettings>();
	Off->PartialOcclusionThreshold = 0.f;
	TestTrue(TEXT("Disabled, a partially blocked listener is Clear again"),
	         VoiceLogic::ResolveCategoryPreference(MakeVoiceAcoustic(0.5f), *Off)[0] ==
	         ENPCVoiceCategory::Clear);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceContext_NeverCrossesTheVisibilitySplit,
	"SpatialAudioRay.Voice.Context.NeverCrossesTheVisibilitySplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceContext_NeverCrossesTheVisibilitySplit::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	const VoiceLogic::FCategoryPreference Visible =
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
	const VoiceLogic::FCategoryPreference Occluded =
		VoiceLogic::ResolveCategoryPreference(Hidden, *S);
	TestTrue(TEXT("An occluded ladder ends in Occluded"),
	         Occluded.Last() == ENPCVoiceCategory::Occluded);
	TestFalse(TEXT("An occluded ladder never offers visible content"),
	          Occluded.Contains(ENPCVoiceCategory::Clear) ||
	          Occluded.Contains(ENPCVoiceCategory::PartiallyOccluded));

	TestFalse(TEXT("Transition content is never offered to normal scheduling"),
	          Visible.Contains(ENPCVoiceCategory::Transition) ||
	          Occluded.Contains(ENPCVoiceCategory::Transition));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_VisibilityOutranksEffortDrift,
	"SpatialAudioRay.Voice.BargeIn.VisibilityOutranksEffortDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceBargeIn_VisibilityOutranksEffortDrift::RunTest(const FString& Parameters) {
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

	const VoiceLogic::FBargeInDecision Drift = VoiceLogic::EvaluateBargeIn(
		Playing, Fresh, Drifted, ENPCVoiceSightChange::None, MakeVoiceFullBank(), 0.f, *S);
	TestTrue(TEXT("Effort drift still fires when visibility is steady"),
	         Drift.Reason == ENPCVoiceBargeInReason::EffortDrift);
	TestTrue(TEXT("And still reports its direction"), Drift.Dir == ENPCVoiceTransitionDir::Farther);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceBargeIn_SightTriggersShareTheCommonGates,
	"SpatialAudioRay.Voice.BargeIn.SightTriggersShareTheCommonGates",
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
	"SpatialAudioRay.Voice.BargeIn.ReasonPicksItsCategory",
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
