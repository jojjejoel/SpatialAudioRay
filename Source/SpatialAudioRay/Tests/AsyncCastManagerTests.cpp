#include "CoreMinimal.h"
#include "SpatialAudioTypes.h"
#include "Misc/AutomationTest.h"
#include "Audio/AsyncCastManager.h"

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
	Accumulate_SinglePoint_Alpha1,
	"SpatialAudioRay.Async.Accumulate.SinglePoint.Alpha1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool Accumulate_SinglePoint_Alpha1::RunTest(const FString& Parameters) {
	constexpr float geomDist = 100.0f;
	constexpr float maxDist = 100.0f;

	FCachedEdgePoint Point;
	Point.EvictionAlpha = 1;
	Point.GeomDist = geomDist;

	const TArray Array({Point});

	const auto Settings = NewObject<USpatialAudioSettings>();
	Settings->CandidateDistanceFalloff = 0.5f;

	FAsyncCastManager::FCachedPointAccum Accum = FAsyncCastManager::AccumulateCachedPoints(
		Array,
		maxDist,
		*Settings);

	TestTrue(
		TEXT("Single point with alpha 1 contributes full weight"),
		Accum.TotalWeight == 1.f / (1.f + Settings->CandidateDistanceFalloff * geomDist / maxDist));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	Accumulate_SinglePoint_AlphaHalf,
	"SpatialAudioRay.Async.Accumulate.SinglePoint.AlphaHalf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool Accumulate_SinglePoint_AlphaHalf::RunTest(const FString& Parameters) {
	constexpr float geomDist = 100.0f;
	constexpr float maxDist = 100.0f;

	FCachedEdgePoint Point;
	Point.EvictionAlpha = 0.5f;
	Point.GeomDist = geomDist;

	const TArray Array({Point});

	const auto Settings = NewObject<USpatialAudioSettings>();
	Settings->CandidateDistanceFalloff = 0.5f;

	FAsyncCastManager::FCachedPointAccum Accum = FAsyncCastManager::AccumulateCachedPoints(
		Array,
		maxDist,
		*Settings);

	TestTrue(
		TEXT("Single point with alpha 0.5 contributes half weight"),
		Accum.TotalWeight == 0.5f * (1.f / (1.f + Settings->CandidateDistanceFalloff * geomDist / maxDist)));

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
	FSelectCycleDirections_PartitionsTheSet,
	"SpatialAudioRay.Async.SelectCycleDirections.PartitionsTheSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FSelectCycleDirections_PartitionsTheSet::RunTest(const FString& Parameters) {
	constexpr int32 Total = 10;
	constexpr int32 CycleCount = 3;

	TArray<FVector> All;
	for (int32 i = 0; i < Total; ++i) {
		All.Add(FVector(static_cast<double>(i), 0.0, 0.0));
	}

	TArray<int32> Seen;
	for (int32 Cycle = 0; Cycle < CycleCount; ++Cycle) {
		TArray<FVector> Dirs;
		TArray<int32> Indices;
		FAsyncCastManager::SelectCycleDirections(All, Cycle, CycleCount, Dirs, Indices);

		TestEqual(TEXT("Every selected direction carries its index"), Dirs.Num(), Indices.Num());
		for (int32 i = 0; i < Dirs.Num(); ++i) {
			TestTrue(TEXT("Index refers to the direction it was taken from"),
				Dirs[i].Equals(All[Indices[i]]));
			Seen.Add(Indices[i]);
		}
	}

	Seen.Sort();
	TestEqual(TEXT("The cycles together cover the whole set"), Seen.Num(), Total);
	for (int32 i = 0; i < Seen.Num(); ++i) {
		TestEqual(TEXT("Each direction is cast exactly once across the sequence"), Seen[i], i);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSelectCycleDirections_ResetsOutputs,
	"SpatialAudioRay.Async.SelectCycleDirections.ResetsOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FSelectCycleDirections_ResetsOutputs::RunTest(const FString& Parameters) {
	const TArray<FVector> All = {FVector::ForwardVector, FVector::RightVector};

	TArray<FVector> Dirs = {FVector::UpVector, FVector::UpVector, FVector::UpVector};
	TArray<int32> Indices = {99, 99, 99};
	FAsyncCastManager::SelectCycleDirections(All, 0, 1, Dirs, Indices);

	TestEqual(TEXT("Leftover directions from a previous cycle are cleared"), Dirs.Num(), 2);
	TestEqual(TEXT("Leftover indices from a previous cycle are cleared"), Indices.Num(), 2);

	FAsyncCastManager::SelectCycleDirections(All, 5, 1, Dirs, Indices);
	TestEqual(TEXT("A start index past the end selects nothing"), Dirs.Num(), 0);

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
