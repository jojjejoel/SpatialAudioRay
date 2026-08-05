#include "CoreMinimal.h"
#include "SpatialAudioTypes.h"
#include "Misc/AutomationTest.h"
#include "Audio/AsyncCastManager.h"

namespace {
	FCacheMergeContext MakeMergeContext(int32 MaxEdgeCount = 4,
	                                    const FVector& ListenerPos = FVector::ZeroVector) {
		FCacheMergeContext Ctx;
		Ctx.SourcePos = FVector::ZeroVector;
		Ctx.ListenerPos = ListenerPos;
		Ctx.MaxRayDistance = 5000.f;
		Ctx.MaxEdgeCount = MaxEdgeCount;
		return Ctx;
	}

	USpatialAudioSettings* MakeMergeSettings(float MergeRadius = 100.f) {
		USpatialAudioSettings* Settings = NewObject<USpatialAudioSettings>();
		Settings->CachedEdgeMergeRadius = MergeRadius;
		Settings->CandidateDistanceFalloff = 1.f;
		Settings->ListenerDistanceFalloff = 0.f;
		return Settings;
	}

	FStoredLoSPath MakeFound(const FVector& Origin, float PathDist) {
		FStoredLoSPath Path;
		Path.LoSOrigin = Origin;
		Path.PathDist = PathDist;
		Path.LoSCumulativeDistance = PathDist;
		return Path;
	}

	FCachedEdgePoint MakeCached(const FVector& Point, float PathDist) {
		FCachedEdgePoint Edge;
		Edge.EdgePoint = Point;
		Edge.PathDist = PathDist;
		Edge.GeomDist = PathDist;
		return Edge;
	}
}

namespace {
	const FVector PullSource(0, 0, 0);
	const FVector PullWaypoint0(1000, 0, 0);
	const FVector PullWaypoint1(1000, 1000, 0);
	const FVector PullEdge(1000, 2000, 0);

	/** Travelled 5000cm to reach an edge 2236cm away in a straight line, turning at two waypoints. */
	FSpatialRayState MakePulledRay() {
		FSpatialRayState Ray;
		Ray.LoSOrigin = PullEdge;
		Ray.LoSCumulativeDistance = 5000.f;
		Ray.BounceWaypoints.Add({PullWaypoint0, 2000.f});
		Ray.BounceWaypoints.Add({PullWaypoint1, 3500.f});
		return Ray;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStringPull_DirectSightCollapsesToOneSegment,
	"SpatialAudioRay.Async.StringPull.DirectSightCollapsesToOneSegment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FStringPull_DirectSightCollapsesToOneSegment::RunTest(const FString& Parameters) {
	auto AlwaysClear = [](const FVector&, const FVector&) { return true; };

	TArray<FVector> Path;
	TArray<bool> Verified;
	const float Pulled = FAsyncCastManager::ComputeStringPulledLeg1(
		AlwaysClear, MakePulledRay(), PullSource, Path, Verified);

	TestTrue(TEXT("An unobstructed edge measures the straight line, not the travelled route"),
	         FMath::IsNearlyEqual(Pulled, FVector::Dist(PullEdge, PullSource), 0.1f));
	TestEqual(TEXT("The polyline collapses to source and edge"), Path.Num(), 2);
	TestTrue(TEXT("The path reads source first"), Path[0].Equals(PullSource, 0.1f));
	TestTrue(TEXT("and ends at the edge"), Path.Last().Equals(PullEdge, 0.1f));
	TestEqual(TEXT("Its single segment is verified"), Verified.Num(), 1);
	TestTrue(TEXT("Verified means traced clear"), Verified[0]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStringPull_HopsThroughTheFirstVisibleAnchor,
	"SpatialAudioRay.Async.StringPull.HopsThroughTheFirstVisibleAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FStringPull_HopsThroughTheFirstVisibleAnchor::RunTest(const FString& Parameters) {
	auto CornerBlocked = [](const FVector& From, const FVector& To) {
		const bool bEdgeToSource = From.Equals(PullEdge, 0.1f) && To.Equals(PullSource, 0.1f);
		return !bEdgeToSource;
	};

	TArray<FVector> Path;
	TArray<bool> Verified;
	const float Pulled = FAsyncCastManager::ComputeStringPulledLeg1(
		CornerBlocked, MakePulledRay(), PullSource, Path, Verified);

	const float Expected = FVector::Dist(PullEdge, PullWaypoint0) + FVector::Dist(PullWaypoint0, PullSource);
	TestTrue(TEXT("The pulled distance is the two straight legs, not the travelled 5000"),
	         FMath::IsNearlyEqual(Pulled, Expected, 0.1f));
	TestEqual(TEXT("The later waypoint is skipped entirely"), Path.Num(), 3);
	TestTrue(TEXT("The surviving anchor is the first visible one"), Path[1].Equals(PullWaypoint0, 0.1f));
	TestTrue(TEXT("Both spliced segments are verified"), Verified.Num() == 2 && Verified[0] && Verified[1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStringPull_BlindFallbackKeepsTravelledRoute,
	"SpatialAudioRay.Async.StringPull.BlindFallbackKeepsTravelledRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FStringPull_BlindFallbackKeepsTravelledRoute::RunTest(const FString& Parameters) {
	auto NeverClear = [](const FVector&, const FVector&) { return false; };

	TArray<FVector> Path;
	TArray<bool> Verified;
	const float Pulled = FAsyncCastManager::ComputeStringPulledLeg1(
		NeverClear, MakePulledRay(), PullSource, Path, Verified);

	TestTrue(TEXT("With nothing visible the distance falls back to what the ray actually travelled"),
	         FMath::IsNearlyEqual(Pulled, 5000.f, 0.1f));
	TestEqual(TEXT("Every waypoint is retained"), Path.Num(), 4);
	TestEqual(TEXT("with a segment flag for each"), Verified.Num(), 3);
	TestTrue(TEXT("None of them is verified, so the recheck will skip them"),
	         !Verified[0] && !Verified[1] && !Verified[2]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStringPull_NeverExceedsTheTravelledDistance,
	"SpatialAudioRay.Async.StringPull.NeverExceedsTheTravelledDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FStringPull_NeverExceedsTheTravelledDistance::RunTest(const FString& Parameters) {
	auto AlwaysClear = [](const FVector&, const FVector&) { return true; };

	FSpatialRayState Ray = MakePulledRay();
	Ray.LoSCumulativeDistance = 100.f;

	TArray<FVector> Path;
	TArray<bool> Verified;
	const float Pulled = FAsyncCastManager::ComputeStringPulledLeg1(
		AlwaysClear, Ray, PullSource, Path, Verified);

	TestTrue(TEXT("A pulled path is clamped to the distance the ray actually flew"),
	         Pulled <= 100.f + 0.1f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCacheMerge_SameCornerKeepsShorterRoute,
	"SpatialAudioRay.Async.CacheMerge.SameCornerKeepsShorterRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCacheMerge_SameCornerKeepsShorterRoute::RunTest(const FString& Parameters) {
	const FCacheMergeContext Ctx = MakeMergeContext();
	const auto Settings = MakeMergeSettings();

	TArray<FCachedEdgePoint> Edges{MakeCached(FVector(1000, 0, 0), 2000.f)};
	FAsyncCastManager::MergeStoredPaths(Edges, {MakeFound(FVector(1010, 0, 0), 1200.f)}, Ctx, *Settings);

	TestEqual(TEXT("A find inside the merge radius does not add an entry"), Edges.Num(), 1);
	TestTrue(TEXT("The shorter route overwrites the incumbent"),
	         FMath::IsNearlyEqual(Edges[0].PathDist, 1200.f));

	FAsyncCastManager::MergeStoredPaths(Edges, {MakeFound(FVector(1010, 0, 0), 5000.f)}, Ctx, *Settings);
	TestTrue(TEXT("A longer route at the same corner is rejected"),
	         FMath::IsNearlyEqual(Edges[0].PathDist, 1200.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCacheMerge_FillsToCapacityBeforeDisplacing,
	"SpatialAudioRay.Async.CacheMerge.FillsToCapacityBeforeDisplacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCacheMerge_FillsToCapacityBeforeDisplacing::RunTest(const FString& Parameters) {
	const FCacheMergeContext Ctx = MakeMergeContext(/*MaxEdgeCount=*/2);
	const auto Settings = MakeMergeSettings();

	TArray<FCachedEdgePoint> Edges;
	const TArray<FStoredLoSPath> Found{
		MakeFound(FVector(1000, 0, 0), 1000.f),
		MakeFound(FVector(0, 1000, 0), 1100.f),
		MakeFound(FVector(0, 0, 1000), 1200.f)
	};
	FAsyncCastManager::MergeStoredPaths(Edges, Found, Ctx, *Settings);

	TestEqual(TEXT("The cache never exceeds CachedEdgeMaxCount"), Edges.Num(), 2);
	TestTrue(TEXT("Newly added entries are marked new for the cache fill"),
	         Edges[0].bNewSinceFillArm && Edges[1].bNewSinceFillArm);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCacheMerge_DisplacesAtMostOnePerSweep,
	"SpatialAudioRay.Async.CacheMerge.DisplacesAtMostOnePerSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCacheMerge_DisplacesAtMostOnePerSweep::RunTest(const FString& Parameters) {
	const FCacheMergeContext Ctx = MakeMergeContext(/*MaxEdgeCount=*/2);
	const auto Settings = MakeMergeSettings();

	TArray<FCachedEdgePoint> Edges{
		MakeCached(FVector(1000, 0, 0), 4000.f),
		MakeCached(FVector(0, 1000, 0), 4000.f)
	};
	const TArray<FStoredLoSPath> Found{
		MakeFound(FVector(0, 0, 1000), 100.f),
		MakeFound(FVector(0, 0, 2000), 100.f)
	};
	FAsyncCastManager::MergeStoredPaths(Edges, Found, Ctx, *Settings);

	int32 Replaced = 0;
	for (const FCachedEdgePoint& Edge : Edges) {
		if (FMath::IsNearlyEqual(Edge.PathDist, 100.f)) {
			++Replaced;
		}
	}
	TestEqual(TEXT("Two far better finds still displace only one incumbent"), Replaced, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCacheMerge_HysteresisRejectsMarginalFinds,
	"SpatialAudioRay.Async.CacheMerge.HysteresisRejectsMarginalFinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCacheMerge_HysteresisRejectsMarginalFinds::RunTest(const FString& Parameters) {
	const FCacheMergeContext Ctx = MakeMergeContext();
	const auto Settings = MakeMergeSettings();

	const FCachedEdgePoint Incumbent = MakeCached(FVector(1000, 0, 0), 1000.f);

	TestFalse(TEXT("A find a hair better than the incumbent does not displace it"),
	          FAsyncCastManager::OutranksIncumbent(
		          Ctx, *Settings, MakeFound(FVector(0, 1000, 0), 999.f), Incumbent));
	TestTrue(TEXT("A clearly shorter path does displace it"),
	         FAsyncCastManager::OutranksIncumbent(
		         Ctx, *Settings, MakeFound(FVector(0, 1000, 0), 500.f), Incumbent));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCacheMerge_RelayedEntriesAlwaysLose,
	"SpatialAudioRay.Async.CacheMerge.RelayedEntriesAlwaysLose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCacheMerge_RelayedEntriesAlwaysLose::RunTest(const FString& Parameters) {
	const FCacheMergeContext Ctx = MakeMergeContext();
	const auto Settings = MakeMergeSettings();

	FCachedEdgePoint Relayed = MakeCached(FVector(1000, 0, 0), 100.f);
	Relayed.bRelayed = true;
	const FCachedEdgePoint Direct = MakeCached(FVector(0, 1000, 0), 4000.f);

	TestTrue(TEXT("A relay is displaced even by a far worse find"),
	         FAsyncCastManager::OutranksIncumbent(
		         Ctx, *Settings, MakeFound(FVector(0, 0, 1000), 4900.f), Relayed));
	TestTrue(TEXT("A relay ranks as the worst incumbent regardless of its path"),
	         FAsyncCastManager::IsWorseIncumbent(Ctx, *Settings, Relayed, Direct));
	TestFalse(TEXT("A direct entry never ranks worse than a relay"),
	          FAsyncCastManager::IsWorseIncumbent(Ctx, *Settings, Direct, Relayed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCacheMerge_WorstIncumbentSkipsMatchedEntries,
	"SpatialAudioRay.Async.CacheMerge.WorstIncumbentSkipsMatchedEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCacheMerge_WorstIncumbentSkipsMatchedEntries::RunTest(const FString& Parameters) {
	const FCacheMergeContext Ctx = MakeMergeContext();
	const auto Settings = MakeMergeSettings();

	const TArray<FCachedEdgePoint> Edges{
		MakeCached(FVector(1000, 0, 0), 4000.f),
		MakeCached(FVector(0, 1000, 0), 500.f)
	};

	TArray<bool> NoneMatched{false, false};
	TestEqual(TEXT("The longest path is the worst incumbent"),
	          FAsyncCastManager::FindWorstIncumbent(Edges, Ctx, *Settings, NoneMatched), 0);

	TArray<bool> WorstMatched{true, false};
	TestEqual(TEXT("An entry confirmed this sweep is not eligible for displacement"),
	          FAsyncCastManager::FindWorstIncumbent(Edges, Ctx, *Settings, WorstMatched), 1);

	TArray<bool> AllMatched{true, true};
	TestEqual(TEXT("Nothing is displaced when every entry was confirmed"),
	          FAsyncCastManager::FindWorstIncumbent(Edges, Ctx, *Settings, AllMatched), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	Accumulate_EmptyArray,
	"SpatialAudioRay.Async.Accumulate.EmptyArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool Accumulate_EmptyArray::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	FAsyncCastManager::FCachedPointAccum Accum = FAsyncCastManager::AccumulateCachedPoints(
		TArray<FCachedEdgePoint>(),
		1000.f,
		*Settings);

	TestTrue(
		TEXT("Accumulating empty array returns zeroed struct"),
		Accum.RaysReached == 0 &&
		Accum.MinLoSDist == TNumericLimits<float>::Max() &&
		Accum.WeightedPos.IsZero() &&
		Accum.TotalWeight == 0.f &&
		Accum.WeightedDist == 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	Accumulate_SinglePoint_DistanceFalloff,
	"SpatialAudioRay.Async.Accumulate.SinglePoint.DistanceFalloff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool Accumulate_SinglePoint_DistanceFalloff::RunTest(const FString& Parameters) {
	constexpr float geomDist = 100.0f;
	constexpr float maxDist = 100.0f;

	FCachedEdgePoint Point;
	Point.GeomDist = geomDist;

	const TArray Array({Point});

	const auto Settings = NewObject<USpatialAudioSettings>();
	Settings->CandidateDistanceFalloff = 0.5f;

	FAsyncCastManager::FCachedPointAccum Accum = FAsyncCastManager::AccumulateCachedPoints(
		Array,
		maxDist,
		*Settings);

	TestTrue(
		TEXT("Single point is weighted by its distance falloff"),
		Accum.TotalWeight == 1.f / (1.f + Settings->CandidateDistanceFalloff * geomDist / maxDist));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FComputeAudio_VirtualSource_WeightedAverage,
	"SpatialAudioRay.Async.ComputeAudio.VirtualSource.WeightedAverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FComputeAudio_VirtualSource_WeightedAverage::RunTest(const FString& Parameters) {
	FAsyncCastManager::FRayAccumulatorInput In;
	In.bDirectLoSFound = false;
	In.WeightedPos = FVector(200.f, 0.f, 0.f);
	In.TotalWeight = 2.f;
	In.MaxRayDistance = 1000.f;
	const FAsyncCastManager::FRayAccumulatorOutput Out = FAsyncCastManager::ComputeAudioFromRayAccumulator(In);

	TestTrue(TEXT("Virtual source is set when weight > 0"), Out.bHasVirtualSource);
	TestTrue(TEXT("Virtual source equals WeightedPos / TotalWeight"),
	         Out.VirtualSourcePos.Equals(FVector(100.f, 0.f, 0.f), 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FComputeAudio_DirectLoS_NoVirtualSource,
	"SpatialAudioRay.Async.ComputeAudio.DirectLoS.NoVirtualSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FComputeAudio_DirectLoS_NoVirtualSource::RunTest(const FString& Parameters) {
	FAsyncCastManager::FRayAccumulatorInput In;
	In.bDirectLoSFound = true;
	In.WeightedPos = FVector(200.f, 0.f, 0.f);
	In.TotalWeight = 2.f;
	In.MaxRayDistance = 1000.f;
	const FAsyncCastManager::FRayAccumulatorOutput Out = FAsyncCastManager::ComputeAudioFromRayAccumulator(In);

	TestFalse(TEXT("Direct LoS suppresses virtual source"), Out.bHasVirtualSource);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMakeBiasStream_SameInputsSameSequence,
	"SpatialAudioRay.Async.MakeBiasStream.SameInputsSameSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMakeBiasStream_SameInputsSameSequence::RunTest(const FString& Parameters) {
	const FVector SourcePos(100.f, 200.f, 50.f);
	const FVector ListenerPos(400.f, -150.f, 80.f);

	FRandomStream StreamA = FAsyncCastManager::MakeBiasStream(SourcePos, ListenerPos, 7);
	FRandomStream StreamB = FAsyncCastManager::MakeBiasStream(SourcePos, ListenerPos, 7);

	bool bAllMatch = true;
	for (int32 i = 0; i < 10; ++i) {
		bAllMatch &= FMath::IsNearlyEqual(StreamA.FRand(), StreamB.FRand());
		bAllMatch &= StreamA.VRand().Equals(StreamB.VRand(), KINDA_SMALL_NUMBER);
	}

	TestTrue(TEXT("Identical source/listener/ray-index draw an identical sequence"), bAllMatch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMakeBiasStream_DifferentRayIndexDiffers,
	"SpatialAudioRay.Async.MakeBiasStream.DifferentRayIndexDiffers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMakeBiasStream_DifferentRayIndexDiffers::RunTest(const FString& Parameters) {
	const FVector SourcePos(100.f, 200.f, 50.f);
	const FVector ListenerPos(400.f, -150.f, 80.f);

	FRandomStream StreamA = FAsyncCastManager::MakeBiasStream(SourcePos, ListenerPos, 0);
	FRandomStream StreamB = FAsyncCastManager::MakeBiasStream(SourcePos, ListenerPos, 1);

	TestNotEqual(TEXT("Different ray indices seed different streams"),
	             StreamA.GetInitialSeed(), StreamB.GetInitialSeed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMakeBiasStream_DifferentPositionDiffers,
	"SpatialAudioRay.Async.MakeBiasStream.DifferentPositionDiffers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMakeBiasStream_DifferentPositionDiffers::RunTest(const FString& Parameters) {
	FRandomStream StreamA = FAsyncCastManager::MakeBiasStream(
		FVector(100.f, 200.f, 50.f), FVector(400.f, -150.f, 80.f), 3);
	FRandomStream StreamB = FAsyncCastManager::MakeBiasStream(
		FVector(101.f, 200.f, 50.f), FVector(400.f, -150.f, 80.f), 3);

	TestNotEqual(TEXT("Moved source seeds a different stream"),
	             StreamA.GetInitialSeed(), StreamB.GetInitialSeed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMidAirTurn_ZeroRoughnessZeroBias_TurnsPerpendicular,
	"SpatialAudioRay.Async.MidAirTurn.ZeroRoughnessZeroBias.TurnsPerpendicular",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMidAirTurn_ZeroRoughnessZeroBias_TurnsPerpendicular::RunTest(const FString& Parameters) {
	const FVector InDir(1.f, 0.f, 0.f);
	const FVector Result = FAsyncCastManager::ComputeMidAirTurnDirection(
		InDir, FVector(120.f, -40.f, 60.f), FVector(0.f, 500.f, 0.f),
		/*bApplyBias*/ false, /*Roughness*/ 0.f, /*ListenerBias*/ 0.f);

	TestTrue(TEXT("Zero-scatter zero-bias turn is exactly perpendicular to the flight direction"),
	         FMath::Abs(FVector::DotProduct(Result, InDir)) < 1e-3f && Result.IsNormalized());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMidAirTurn_ZeroRoughnessZeroBias_IsDeterministic,
	"SpatialAudioRay.Async.MidAirTurn.ZeroRoughnessZeroBias.IsDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMidAirTurn_ZeroRoughnessZeroBias_IsDeterministic::RunTest(const FString& Parameters) {
	const FVector InDir(0.f, 1.f, 0.f);
	const FVector TurnPoint(300.f, 75.f, -20.f);
	const FVector ListenerPos(-100.f, 250.f, 40.f);

	const FVector First = FAsyncCastManager::ComputeMidAirTurnDirection(
		InDir, TurnPoint, ListenerPos, false, 0.f, 0.f);
	const FVector Second = FAsyncCastManager::ComputeMidAirTurnDirection(
		InDir, TurnPoint, ListenerPos, false, 0.f, 0.f);

	TestTrue(TEXT("Identical turn point and listener replay the identical direction"),
	         First.Equals(Second, 1e-6f));

	const FVector OtherPoint = FAsyncCastManager::ComputeMidAirTurnDirection(
		InDir, TurnPoint + FVector(50.f, 0.f, 0.f), ListenerPos, false, 0.f, 0.f);
	TestTrue(TEXT("A different turn point seeds a different direction"),
	         !First.Equals(OtherPoint, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMidAirTurn_FullListenerBias_PointsAtListener,
	"SpatialAudioRay.Async.MidAirTurn.FullListenerBias.PointsAtListener",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMidAirTurn_FullListenerBias_PointsAtListener::RunTest(const FString& Parameters) {
	const FVector TurnPoint(100.f, 0.f, 0.f);
	const FVector ListenerPos(100.f, 300.f, 0.f);
	const FVector Result = FAsyncCastManager::ComputeMidAirTurnDirection(
		FVector(1.f, 0.f, 0.f), TurnPoint, ListenerPos,
		/*bApplyBias*/ false, /*Roughness*/ 0.f, /*ListenerBias*/ 1.f);

	TestTrue(TEXT("Full listener bias turns the ray straight at the listener"),
	         Result.Equals((ListenerPos - TurnPoint).GetSafeNormal(), 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMidAirTurn_ResultIsNormalized,
	"SpatialAudioRay.Async.MidAirTurn.ResultIsNormalized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMidAirTurn_ResultIsNormalized::RunTest(const FString& Parameters) {
	for (int32 i = 0; i < 32; ++i) {
		const FVector Result = FAsyncCastManager::ComputeMidAirTurnDirection(
			FVector(0.f, 0.f, 1.f), FVector(50.f, -20.f, 10.f), FVector(-300.f, 400.f, 90.f),
			/*bApplyBias*/ true, /*Roughness*/ 0.7f, /*ListenerBias*/ 0.4f);
		if (!TestTrue(TEXT("Scattered turn direction is unit length"), Result.IsNormalized())) {
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCountPrefixAnchorWaypoints_StopsAtTheEdge,
	"SpatialAudioRay.Async.CountPrefixAnchorWaypoints.StopsAtTheEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCountPrefixAnchorWaypoints_StopsAtTheEdge::RunTest(const FString& Parameters) {
	TArray<FSpatialRayState::FBounceWaypoint> Waypoints;
	Waypoints.Add({FVector(100.f, 0.f, 0.f), 100.f});
	Waypoints.Add({FVector(200.f, 0.f, 0.f), 250.f});
	Waypoints.Add({FVector(300.f, 0.f, 0.f), 400.f});

	TestEqual(TEXT("Only waypoints before the LoS origin are anchors"),
	          FAsyncCastManager::CountPrefixAnchorWaypoints(Waypoints, 260.f), 2);

	TestEqual(TEXT("A waypoint exactly at the LoS distance is not an anchor"),
	          FAsyncCastManager::CountPrefixAnchorWaypoints(Waypoints, 250.f), 1);

	TestEqual(TEXT("An LoS origin before every waypoint leaves no anchors"),
	          FAsyncCastManager::CountPrefixAnchorWaypoints(Waypoints, 50.f), 0);

	TestEqual(TEXT("An LoS origin past every waypoint takes them all"),
	          FAsyncCastManager::CountPrefixAnchorWaypoints(Waypoints, 9000.f), 3);

	const TArray<FSpatialRayState::FBounceWaypoint> NoWaypoints;
	TestEqual(TEXT("A ray that never turned has no anchors"),
	          FAsyncCastManager::CountPrefixAnchorWaypoints(NoWaypoints, 500.f), 0);

	return true;
}
