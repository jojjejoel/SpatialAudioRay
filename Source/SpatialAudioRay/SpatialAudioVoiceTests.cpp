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
	         UNPCVoiceComponent::MapToBucket(100.f, 0.f, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Whisper band edge is inclusive"),
	         UNPCVoiceComponent::MapToBucket(S->WhisperMaxDistance, 0.f, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Past whisper edge = conversational"),
	         UNPCVoiceComponent::MapToBucket(S->WhisperMaxDistance + 1.f, 0.f, *S) == ENPCVoiceEffort::Conversational);
	TestTrue(TEXT("Mid range = raised"),
	         UNPCVoiceComponent::MapToBucket(S->ConversationalMaxDistance + 1.f, 0.f, *S) == ENPCVoiceEffort::Raised);
	TestTrue(TEXT("Far = shout"),
	         UNPCVoiceComponent::MapToBucket(S->RaisedMaxDistance + 1.f, 0.f, *S) == ENPCVoiceEffort::Shout);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceMapToBucket_OcclusionShift,
	"SpatialAudio.Voice.MapToBucket.OcclusionShift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVoiceMapToBucket_OcclusionShift::RunTest(const FString& Parameters) {
	const UNPCVoiceSettings* S = GetDefault<UNPCVoiceSettings>();

	TestTrue(TEXT("Occluded shifts one bucket toward shout"),
	         UNPCVoiceComponent::MapToBucket(100.f, S->OcclusionShiftThreshold, *S) == ENPCVoiceEffort::Conversational);
	TestTrue(TEXT("Below threshold does not shift"),
	         UNPCVoiceComponent::MapToBucket(100.f, S->OcclusionShiftThreshold - 0.01f, *S) == ENPCVoiceEffort::Whisper);
	TestTrue(TEXT("Shout does not shift past the top"),
	         UNPCVoiceComponent::MapToBucket(S->RaisedMaxDistance + 1.f, 1.f, *S) == ENPCVoiceEffort::Shout);

	return true;
}
