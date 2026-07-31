#include "CoreMinimal.h"
#include "SpatialAudioTypes.h"
#include "Misc/AutomationTest.h"
#include "Audio/EdgeCache.h"

// ─── Merge rules ──────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeCandidate_ExcludesStopgapsAndLeavers,
	"SpatialAudioRay.EdgeCache.MergeCandidate.ExcludesStopgapsAndLeavers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMergeCandidate_ExcludesStopgapsAndLeavers::RunTest(const FString& Parameters) {
	FCachedEdgePoint Direct;
	TestTrue(TEXT("A plain confirmed edge can merge"), FEdgeCache::IsMergeCandidate(Direct));

	// Two relays rescued through the same fan point present the same position while their real
	// edges are distinct, so merging them would delete a genuine path.
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

	// Both sit on the same corner, so the listener leg cancels and only the travelled distance
	// separates them. The 3-bounce short route is the better one.
	TestTrue(TEXT("The longer route loses regardless of bounce count"),
		FEdgeCache::TravelledFurther(Long, Short));
	TestFalse(TEXT("The shorter route survives"), FEdgeCache::TravelledFurther(Short, Long));

	// Equal distances keep the incumbent rather than churning the cache.
	FCachedEdgePoint Tied = Short;
	TestFalse(TEXT("A tie does not displace"), FEdgeCache::TravelledFurther(Short, Tied));

	// A relay's frozen leg counts toward the distance it presents.
	FCachedEdgePoint WithRelay;
	WithRelay.PathDist = 800.f;
	WithRelay.RelayDist = 5000.f;
	TestTrue(TEXT("The relay leg counts toward the travelled distance"),
		FEdgeCache::TravelledFurther(WithRelay, Short));

	return true;
}
