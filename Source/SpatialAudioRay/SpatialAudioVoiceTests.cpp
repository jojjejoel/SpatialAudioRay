#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Voice/NPCVoiceComponent.h"
#include "Voice/NPCVoiceSettings.h"

// ─── MapToBucket ──────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceMapToBucket_DistanceBands,
	"SpatialAudio.Voice.MapToBucket.DistanceBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceMapToBucket_DistanceBands::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	TestTrue(TEXT("Close = whisper"),
	         UNPCVoiceComponent::MapToBucket(100.f, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Whisper band edge is inclusive"),
	         UNPCVoiceComponent::MapToBucket(S->WhisperMaxDistance, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Past whisper edge = conversational"),
	         UNPCVoiceComponent::MapToBucket(S->WhisperMaxDistance + 1.f, *S) == ENPCVoiceEffort::Conversational);
	TestTrue(TEXT("Mid range = raised"),
	         UNPCVoiceComponent::MapToBucket(S->ConversationalMaxDistance + 1.f, *S) == ENPCVoiceEffort::Raised);
	TestTrue(TEXT("Far = shout"),
	         UNPCVoiceComponent::MapToBucket(S->RaisedMaxDistance + 1.f, *S) == ENPCVoiceEffort::Shout);

	return true;
}

// ─── FindTransitionLine ───────────────────────────────────────────────────────

namespace {
	FNPCVoiceRuntimeLine MakeTransitionLine(const TCHAR* Id, ENPCVoiceEffort Bucket,
	                                        ENPCVoiceTransitionDir Dir,
	                                        const TCHAR* Group = TEXT("")) {
		FNPCVoiceRuntimeLine Line;
		Line.Row.LineId = FName(Id);
		Line.Row.Category = ENPCVoiceCategory::Transition;
		Line.Row.Bucket = Bucket;
		Line.Row.Direction = Dir;
		Line.Row.CooldownGroup = FName(Group);
		return Line;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceFindTransitionLine_DirectionAndBucket,
	"SpatialAudio.Voice.FindTransitionLine.DirectionAndBucket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceFindTransitionLine_DirectionAndBucket::RunTest(const FString& Parameters) {
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeTransitionLine(TEXT("T_CloserWhisper"), ENPCVoiceEffort::Whisper,
	                             ENPCVoiceTransitionDir::Closer));
	Lines.Add(MakeTransitionLine(TEXT("T_FartherRaised"), ENPCVoiceEffort::Raised,
	                             ENPCVoiceTransitionDir::Farther));
	Lines.Add(MakeTransitionLine(TEXT("T_FartherShout"), ENPCVoiceEffort::Shout,
	                             ENPCVoiceTransitionDir::Farther));
	const TMap<FName, float> NoCooldowns;

	const int32 Exact = UNPCVoiceComponent::FindTransitionLine(
		Lines, ENPCVoiceTransitionDir::Farther, ENPCVoiceEffort::Shout, 0.f, NoCooldowns);
	TestEqual(TEXT("Exact target bucket wins in its direction"), Exact, 2);

	const int32 Nearest = UNPCVoiceComponent::FindTransitionLine(
		Lines, ENPCVoiceTransitionDir::Farther, ENPCVoiceEffort::Conversational, 0.f, NoCooldowns);
	TestEqual(TEXT("No exact bucket: nearest rendered one in the direction"), Nearest, 1);

	const int32 WrongDir = UNPCVoiceComponent::FindTransitionLine(
		Lines, ENPCVoiceTransitionDir::Closer, ENPCVoiceEffort::Shout, 0.f, NoCooldowns);
	TestEqual(TEXT("Direction filters even when buckets fit better elsewhere"), WrongDir, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceFindTransitionLine_CooldownAndEmpty,
	"SpatialAudio.Voice.FindTransitionLine.CooldownAndEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceFindTransitionLine_CooldownAndEmpty::RunTest(const FString& Parameters) {
	TArray<FNPCVoiceRuntimeLine> Lines;
	Lines.Add(MakeTransitionLine(TEXT("T_A"), ENPCVoiceEffort::Shout,
	                             ENPCVoiceTransitionDir::Farther, TEXT("trans")));
	Lines.Add(MakeTransitionLine(TEXT("T_B"), ENPCVoiceEffort::Raised,
	                             ENPCVoiceTransitionDir::Farther));

	TMap<FName, float> Cooldowns;
	Cooldowns.Add(FName(TEXT("trans")), 100.f);

	const int32 Picked = UNPCVoiceComponent::FindTransitionLine(
		Lines, ENPCVoiceTransitionDir::Farther, ENPCVoiceEffort::Shout, /*Now=*/50.f, Cooldowns);
	TestEqual(TEXT("Cooldown-blocked exact match yields to the nearest free line"), Picked, 1);

	const int32 None = UNPCVoiceComponent::FindTransitionLine(
		Lines, ENPCVoiceTransitionDir::Closer, ENPCVoiceEffort::Whisper, 0.f, Cooldowns);
	TestEqual(TEXT("No line in the direction: INDEX_NONE"), None, static_cast<int32>(INDEX_NONE));

	return true;
}
