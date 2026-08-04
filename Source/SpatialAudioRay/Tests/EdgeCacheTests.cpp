#include "CoreMinimal.h"
#include "SpatialAudioTypes.h"
#include "Misc/AutomationTest.h"
#include "Audio/EdgeCache.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeCandidate_ExcludesStopgapsAndLeavers,
	"SpatialAudioRay.EdgeCache.MergeCandidate.ExcludesStopgapsAndLeavers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMergeCandidate_ExcludesStopgapsAndLeavers::RunTest(const FString& Parameters) {
	FCachedEdgePoint Direct;
	TestTrue(TEXT("A plain confirmed edge can merge"), FEdgeCache::IsMergeCandidate(Direct));

	FCachedEdgePoint Relayed;
	Relayed.bRelayed = true;
	TestFalse(TEXT("A relayed entry is not a merge candidate"), FEdgeCache::IsMergeCandidate(Relayed));

	FCachedEdgePoint Evicting;
	Evicting.bEvicting = true;
	TestFalse(TEXT("An evicting entry is not a merge candidate"), FEdgeCache::IsMergeCandidate(Evicting));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTravelledFurther_IgnoresBounceCount,
	"SpatialAudioRay.EdgeCache.TravelledFurther.IgnoresBounceCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FTravelledFurther_IgnoresBounceCount::RunTest(const FString& Parameters) {
	FCachedEdgePoint Short;
	Short.PathDist = 800.f;
	Short.LoSBounces = 3;

	FCachedEdgePoint Long;
	Long.PathDist = 4000.f;
	Long.LoSBounces = 1;

	TestTrue(TEXT("The longer route loses regardless of bounce count"),
		FEdgeCache::TravelledFurther(Long, Short));
	TestFalse(TEXT("The shorter route survives"), FEdgeCache::TravelledFurther(Short, Long));

	FCachedEdgePoint Tied = Short;
	TestFalse(TEXT("A tie does not displace"), FEdgeCache::TravelledFurther(Short, Tied));

	FCachedEdgePoint WithRelay;
	WithRelay.PathDist = 800.f;
	WithRelay.RelayDist = 5000.f;
	TestTrue(TEXT("The relay leg counts toward the travelled distance"),
		FEdgeCache::TravelledFurther(WithRelay, Short));

	return true;
}
