#include "CoreMinimal.h"
#include "SpatialAudioTypes.h"
#include "Misc/AutomationTest.h"
#include "Audio/Math.h"

// ─── ReflectDirection ─────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReflectDirection_HeadOn,
	"SpatialAudioRay.Math.ReflectDirection.HeadOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReflectDirection_HeadOn::RunTest(const FString& Parameters) {
	const FVector Dir(1.f, 0.f, 0.f);
	const FVector Normal(-1.f, 0.f, 0.f);

	const FVector Reflected = Math::ReflectDirection(Dir, Normal);

	TestTrue(
		TEXT("Head-on reflection reverses direction"),
		Reflected.Equals(FVector(-1.f, 0.f, 0.f), KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReflectDirection_Tangent,
	"SpatialAudioRay.Math.ReflectDirection.Tangent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReflectDirection_Tangent::RunTest(const FString& Parameters) {
	const FVector Dir(1.f, 0.f, 0.f);
	const FVector Normal(0.f, 1.f, 0.f);

	const FVector Reflected = Math::ReflectDirection(Dir, Normal);

	TestTrue(
		TEXT("Direction parallel to plane remains unchanged"),
		Reflected.Equals(Dir, KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReflectDirection_PreservesMagnitude,
	"SpatialAudioRay.Math.ReflectDirection.PreservesMagnitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReflectDirection_PreservesMagnitude::RunTest(const FString& Parameters) {
	const FVector Dir(3.f, -4.f, 2.f);
	const FVector Normal = FVector(0.f, 1.f, 0.f);

	const FVector Reflected = Math::ReflectDirection(Dir, Normal);

	TestTrue(
		TEXT("Reflection preserves vector magnitude"),
		FMath::IsNearlyEqual(
			Reflected.Size(),
			Dir.Size(),
			KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReflectDirection_DoubleReflection,
	"SpatialAudioRay.Math.ReflectDirection.DoubleReflection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReflectDirection_DoubleReflection::RunTest(const FString& Parameters) {
	const FVector Dir(1.f, 2.f, 3.f);
	const FVector Normal = FVector(0.f, 1.f, 0.f);

	const FVector Once = Math::ReflectDirection(Dir, Normal);
	const FVector Twice = Math::ReflectDirection(Once, Normal);

	TestTrue(
		TEXT("Reflecting twice returns original vector"),
		Twice.Equals(Dir, KINDA_SMALL_NUMBER));

	return true;
}


// ─── ComputePathAttenuation ───────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPathAttenuation_NoExcess,
	"SpatialAudioRay.Math.PathAttenuation.NoExcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FPathAttenuation_NoExcess::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();
	Settings->PathAttenuationStrength = 1.f; // full strength for testing;
	// Path equal to direct distance → no attenuation. Leg1Geom == AvgPathDist so
	// PathAttenuationGeomBlend (default 0) can't change the result either way.
	const float Result = Math::ComputePathAttenuation(1000.f, 1000.f, 1000.f, *Settings);
	TestEqual(TEXT("Equal path and max distance gives full attenuation"), Result, 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPathAttenuation_FullExcess,
	"SpatialAudioRay.Math.PathAttenuation.FullExcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FPathAttenuation_FullExcess::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();
	// Path at max distance → full attenuation. Leg1Geom == AvgPathDist, same reasoning as above.
	const float Result = Math::ComputePathAttenuation(2000.f, 2000.f, 1000.f, *Settings);
	TestTrue(TEXT("Path exceeding max distance gives attenuation > 0"), Result > 0.f);
	TestTrue(TEXT("Attenuation is clamped to 1"), Result <= 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPathAttenuation_GeomBlend,
	"SpatialAudioRay.Math.PathAttenuation.GeomBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FPathAttenuation_GeomBlend::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();
	Settings->PathAttenuationStrength = 1.f;

	// A long, complex traveled path (2000) vs a much shorter straight-line distance (500),
	// MaxRayDistance 2000.
	Settings->PathAttenuationGeomBlend = 0.f;
	const float PureTraveled = Math::ComputePathAttenuation(2000.f, 500.f, 2000.f, *Settings);
	TestEqual(TEXT("Blend 0 ignores Leg1Geom entirely"), PureTraveled, 1.f);

	Settings->PathAttenuationGeomBlend = 1.f;
	const float PureGeom = Math::ComputePathAttenuation(2000.f, 500.f, 2000.f, *Settings);
	TestEqual(TEXT("Blend 1 ignores traveled distance entirely"), PureGeom, 0.25f);

	Settings->PathAttenuationGeomBlend = 0.5f;
	const float Blended = Math::ComputePathAttenuation(2000.f, 500.f, 2000.f, *Settings);
	TestTrue(TEXT("Blend 0.5 lands strictly between the two pure results"),
	          Blended > PureGeom && Blended < PureTraveled);
	return true;
}

// ─── GenerateFibonacciDirections ──────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_OutputCount,
	"SpatialAudioRay.Math.Fibonacci.OutputCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_OutputCount::RunTest(const FString& Parameters) {
	constexpr int32 RequestedCount = 64;
	const TArray<FVector> Dirs = Math::GenerateFibonacciDirections(RequestedCount);
	TestEqual(TEXT("Output count matches requested count"), Dirs.Num(), RequestedCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_DirectionsNormalized,
	"SpatialAudioRay.Math.Fibonacci.DirectionsNormalized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_DirectionsNormalized::RunTest(const FString& Parameters) {
	const TArray<FVector> Dirs = Math::GenerateFibonacciDirections(32);
	for (int32 i = 0; i < Dirs.Num(); ++i) {
		const float Len = Dirs[i].Size();
		TestTrue(
			FString::Printf(TEXT("Direction %d is normalized (length %.4f)"), i, Len),
			FMath::IsNearlyEqual(Len, 1.f, KINDA_SMALL_NUMBER)
		);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_ZeroRays,
	"SpatialAudioRay.Math.Fibonacci.ZeroRays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_ZeroRays::RunTest(const FString& Parameters) {
	const TArray<FVector> Dirs = Math::GenerateFibonacciDirections(0);

	TestEqual(TEXT("Zero rays returns empty array"), Dirs.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_SingleRay,
	"SpatialAudioRay.Math.Fibonacci.SingleRay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_SingleRay::RunTest(const FString& Parameters) {
	const TArray<FVector> Dirs = Math::GenerateFibonacciDirections(1);

	TestEqual(TEXT("One ray generated"), Dirs.Num(), 1);

	TestTrue(
		TEXT("Single ray points at north pole"),
		Dirs[0].Equals(FVector(0, 0, 1), KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_PolesPresent,
	"SpatialAudioRay.Math.Fibonacci.PolesPresent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_PolesPresent::RunTest(const FString& Parameters) {
	const TArray<FVector> Dirs = Math::GenerateFibonacciDirections(64);

	TestTrue(
		TEXT("First point is north pole"),
		Dirs[0].Equals(FVector(0, 0, 1), KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("Last point is south pole"),
		Dirs.Last().Equals(FVector(0, 0, -1), KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_CustomPoleAligned,
	"SpatialAudioRay.Math.Fibonacci.CustomPoleAligned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_CustomPoleAligned::RunTest(const FString& Parameters) {
	const FVector PoleDir = FVector(1.f, 1.f, 0.f).GetSafeNormal();
	const TArray<FVector> Dirs = Math::GenerateFibonacciDirections(64, PoleDir);

	TestTrue(
		TEXT("First point sits at the custom pole (toward listener)"),
		Dirs[0].Equals(PoleDir, KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("Last point sits at the opposite pole (away from listener)"),
		Dirs.Last().Equals(-PoleDir, KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFibonacci_DeterministicForSamePole,
	"SpatialAudioRay.Math.Fibonacci.DeterministicForSamePole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFibonacci_DeterministicForSamePole::RunTest(const FString& Parameters) {
	// Simulates a stationary source/listener: the same pole direction, recomputed on
	// repeated calls (as happens every full sweep), must produce identical ray sets.
	const FVector PoleDir = FVector(0.3f, -0.6f, 0.74f).GetSafeNormal();
	const TArray<FVector> DirsA = Math::GenerateFibonacciDirections(64, PoleDir);
	const TArray<FVector> DirsB = Math::GenerateFibonacciDirections(64, PoleDir);

	bool bAllMatch = DirsA.Num() == DirsB.Num();
	for (int32 i = 0; bAllMatch && i < DirsA.Num(); ++i) {
		bAllMatch &= DirsA[i].Equals(DirsB[i], KINDA_SMALL_NUMBER);
	}

	TestTrue(TEXT("Repeated calls with the same pole produce identical directions"), bAllMatch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOcclusionRatio_DirectPath,
	"SpatialAudioRay.Math.OcclusionRatio.DirectPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FOcclusionRatio_DirectPath::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	const float Result = Math::ComputeOcclusionFromPathRatio(1000.f, 1000.f, *Settings);
	TestEqual(TEXT("Direct path gives zero occlusion"), Result, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOcclusionRatio_Clamped,
	"SpatialAudioRay.Math.OcclusionRatio.Clamped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FOcclusionRatio_Clamped::RunTest(const FString& Parameters) {
	// Extremely long path → occlusion clamped to 1
	const auto Settings = NewObject<USpatialAudioSettings>();
	const float Result = Math::ComputeOcclusionFromPathRatio(99999.f, 100.f, *Settings);
	TestTrue(TEXT("Extreme path excess is clamped to 1"), Result <= 1.f);
	TestTrue(TEXT("Extreme path excess gives high occlusion"), Result > 0.9f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRayDirectionWeight_Perpendicular,
	"SpatialAudioRay.Math.RayDirectionWeight.Perpendicular",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FRayDirectionWeight_Perpendicular::RunTest(const FString& Parameters) {
	const float Weight = Math::ComputeRayDirectionWeight(
		FVector(1.f, 0.f, 0.f),
		FVector(0.f, 0.f, 1.f),
		0.f,
		0.f,
		1000.f);

	TestEqual(
		TEXT("Perpendicular direction gets full weight"),
		Weight,
		1.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRayDirectionWeight_ForwardPenalized,
	"SpatialAudioRay.Math.RayDirectionWeight.ForwardPenalized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FRayDirectionWeight_ForwardPenalized::RunTest(const FString& Parameters) {
	const float Weight = Math::ComputeRayDirectionWeight(
		FVector(0.f, 0.f, 1.f),
		FVector(0.f, 0.f, 1.f),
		0.f,
		0.f,
		1000.f);

	TestEqual(
		TEXT("Forward direction receives zero weight"),
		Weight,
		0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRayDirectionWeight_DirectLoSFraction,
	"SpatialAudioRay.Math.RayDirectionWeight.DirectLoSFraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FRayDirectionWeight_DirectLoSFraction::RunTest(const FString& Parameters) {
	const FVector Dir = FVector(0.7f, 0.f, 0.7f).GetSafeNormal();
	const FVector S2L = FVector(0.f, 0.f, 1.f);

	const float NoLoS = Math::ComputeRayDirectionWeight(
		Dir, S2L, 0.f, 0.f, 1000.f);

	const float FullLoS = Math::ComputeRayDirectionWeight(
		Dir, S2L, 1.f, 0.f, 1000.f);

	TestTrue(
		TEXT("Higher DirectLoSFraction increases weight"),
		FullLoS > NoLoS);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOcclusionRatio_ZeroDirectDistance,
	"SpatialAudioRay.Math.OcclusionRatio.ZeroDirectDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FOcclusionRatio_ZeroDirectDistance::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	const float Result =
		Math::ComputeOcclusionFromPathRatio(
			1000.f,
			0.f,
			*Settings);

	TestEqual(
		TEXT("Zero direct distance returns zero occlusion"),
		Result,
		0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHasAnyDirectLoS_Empty,
	"SpatialAudioRay.Math.HasAnyDirectLoS.Empty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHasAnyDirectLoS_Empty::RunTest(const FString& Parameters)
{
	TArray<FSpatialRayState> Rays;
	TestFalse(TEXT("Empty array"), Math::HasAnyDirectLoS(Rays));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHasAnyDirectLoS_IndirectOnly,
	"SpatialAudioRay.Math.HasAnyDirectLoS.IndirectOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHasAnyDirectLoS_IndirectOnly::RunTest(const FString& Parameters)
{
	TArray<FSpatialRayState> Rays;
	FSpatialRayState& R = Rays.AddDefaulted_GetRef();
	R.bLoSFound  = true;
	R.LoSBounces = 1;
	TestFalse(TEXT("Indirect LoS only"), Math::HasAnyDirectLoS(Rays));
	return true;
}


// ─── ComputeVirtualAudioParams ────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualAudio_Gain_Formula,
	"SpatialAudioRay.Math.VirtualAudio.Gain.Formula",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualAudio_Gain_Formula::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	// VirtualGain = VirtualCrossfade * (1 - PathAttenuation)
	const Math::FVirtualAudioParams VAP = Math::ComputeVirtualAudioParams(
		0.5f,   // VirtualCrossfade
		0.2f,   // PathAttenuation
		100.f,  // Leg1Geom
		100.f,  // Leg1Traveled
		5000.f, // MaxRayDistance
		*Settings);

	constexpr float Expected = 0.5f * (1.f - 0.2f);
	TestTrue(TEXT("VirtualGain matches formula"), FMath::IsNearlyEqual(VAP.VirtualGain, Expected, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualAudio_PathAttenuation_ReducesGain,
	"SpatialAudioRay.Math.VirtualAudio.PathAttenuation.ReducesGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualAudio_PathAttenuation_ReducesGain::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	const Math::FVirtualAudioParams NoneAtten = Math::ComputeVirtualAudioParams(
		1.f, 0.f, 100.f, 100.f, 5000.f, *Settings);

	const Math::FVirtualAudioParams HalfAtten = Math::ComputeVirtualAudioParams(
		1.f, 0.5f, 100.f, 100.f, 5000.f, *Settings);

	TestTrue(TEXT("Higher path attenuation reduces virtual gain"), HalfAtten.VirtualGain < NoneAtten.VirtualGain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualAudio_PathBend_NoLeg1Excess_IsZero,
	"SpatialAudioRay.Math.VirtualAudio.PathBend.NoLeg1Excess_IsZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualAudio_PathBend_NoLeg1Excess_IsZero::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	// Leg1Traveled == Leg1Geom → the ray reached the virtual position via a straight line,
	// no detour → zero path bend.
	const Math::FVirtualAudioParams VAP = Math::ComputeVirtualAudioParams(
		1.f, 0.f,
		/*Leg1Geom=*/200.f, /*Leg1Traveled=*/200.f,
		/*MaxRayDistance=*/5000.f,
		*Settings);

	TestEqual(TEXT("No Leg1 excess gives zero path bend"), VAP.VirtualPathBend, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualAudio_PathBend_WithLeg1Excess_NonZero,
	"SpatialAudioRay.Math.VirtualAudio.PathBend.WithLeg1Excess_NonZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualAudio_PathBend_WithLeg1Excess_NonZero::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	// Leg1Traveled (300) > Leg1Geom (200) → the ray had to crawl/bounce to reach the edge.
	const Math::FVirtualAudioParams VAP = Math::ComputeVirtualAudioParams(
		1.f, 0.f,
		/*Leg1Geom=*/200.f, /*Leg1Traveled=*/300.f,
		/*MaxRayDistance=*/5000.f,
		*Settings);

	TestTrue(TEXT("Leg1 excess produces non-zero path bend"), VAP.VirtualPathBend > 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualAudio_PathBend_FullExcess_SetsSaturationPoint,
	"SpatialAudioRay.Math.VirtualAudio.PathBend.FullExcess_SetsSaturationPoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualAudio_PathBend_FullExcess_SetsSaturationPoint::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	// Excess ratio 0.5 (300 traveled / 200 geom): at the default FullExcess of 1 the bend is
	// halfway; lowering FullExcess to 0.5 moves the saturation point to 1.5x path → full bend.
	const Math::FVirtualAudioParams Default = Math::ComputeVirtualAudioParams(
		1.f, 0.f, /*Leg1Geom=*/200.f, /*Leg1Traveled=*/300.f, /*MaxRayDistance=*/5000.f, *Settings);
	TestTrue(TEXT("Default FullExcess=1: excess 0.5 gives bend 0.5"),
		FMath::IsNearlyEqual(Default.VirtualPathBend, 0.5f, 0.0001f));

	Settings->VirtualPathBendFullExcess = 0.5f;
	const Math::FVirtualAudioParams Halved = Math::ComputeVirtualAudioParams(
		1.f, 0.f, 200.f, 300.f, 5000.f, *Settings);
	TestEqual(TEXT("FullExcess=0.5: excess 0.5 saturates bend at 1"), Halved.VirtualPathBend, 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualAudio_PathBend_DistanceStrength_AddsTraveledDistanceTerm,
	"SpatialAudioRay.Math.VirtualAudio.PathBend.DistanceStrength_AddsTraveledDistanceTerm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualAudio_PathBend_DistanceStrength_AddsTraveledDistanceTerm::RunTest(const FString& Parameters) {
	const auto Settings = NewObject<USpatialAudioSettings>();

	// Straight single-corner path (traveled == geom → zero detour bend), traveled distance at
	// half of MaxRayDistance. Default strength 0 keeps the old behavior; strength 1 adds
	// traveled/MaxRay = 0.5 bend from distance alone.
	const Math::FVirtualAudioParams Off = Math::ComputeVirtualAudioParams(
		1.f, 0.f, /*Leg1Geom=*/2500.f, /*Leg1Traveled=*/2500.f, /*MaxRayDistance=*/5000.f, *Settings);
	TestEqual(TEXT("Strength 0 (default): straight path stays at zero bend"), Off.VirtualPathBend, 0.f);

	Settings->VirtualPathBendDistanceStrength = 1.f;
	const Math::FVirtualAudioParams On = Math::ComputeVirtualAudioParams(
		1.f, 0.f, 2500.f, 2500.f, 5000.f, *Settings);
	TestTrue(TEXT("Strength 1: traveled at half MaxRay gives bend 0.5"),
		FMath::IsNearlyEqual(On.VirtualPathBend, 0.5f, 0.0001f));

	// Detour and distance terms stack, clamped to 1: excess ratio 0.5 + distance 0.5 = 1.0.
	const Math::FVirtualAudioParams Stacked = Math::ComputeVirtualAudioParams(
		1.f, 0.f, /*Leg1Geom=*/1666.67f, /*Leg1Traveled=*/2500.f, 5000.f, *Settings);
	TestEqual(TEXT("Detour + distance terms stack and clamp at 1"), Stacked.VirtualPathBend, 1.f);
	return true;
}


// ─── ComputeVirtualCrossfadeTarget ──────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeTarget_StartAtOne_IsBinaryGate,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeTarget.StartAtOne_IsBinaryGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeTarget_StartAtOne_IsBinaryGate::RunTest(const FString& Parameters) {
	// StartOcclusion = 1 disables the ramp: the gate follows bHasDirectLoS alone regardless of
	// how high smoothed occlusion sits.
	TestEqual(TEXT("Ramp disabled at Start = 1 — always zero"),
		Math::ComputeVirtualCrossfadeRamp(/*CurrentOcclusion=*/0.9f, /*StartOcclusion=*/1.f), 0.f);
	TestEqual(TEXT("LoS held, zero ramp — gate stays closed"),
		Math::ComputeVirtualCrossfadeTarget(/*bHasDirectLoS=*/true, /*bSuppressHardGate=*/false, /*SmoothedRamp=*/0.f), 0.f);
	TestEqual(TEXT("LoS lost — gate fully open"),
		Math::ComputeVirtualCrossfadeTarget(false, false, 0.f), 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeTarget_Ramp_MapsBandToZeroOne,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeTarget.Ramp_MapsBandToZeroOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeTarget_Ramp_MapsBandToZeroOne::RunTest(const FString& Parameters) {
	// With Start = 0.75 the band [0.75, 1.0] maps linearly onto [0, 1]; below the band the
	// ramp stays zero.
	TestEqual(TEXT("Below the band — zero"),
		Math::ComputeVirtualCrossfadeRamp(0.5f, 0.75f), 0.f);
	TestEqual(TEXT("At the band start — still zero"),
		Math::ComputeVirtualCrossfadeRamp(0.75f, 0.75f), 0.f);
	TestTrue(TEXT("Pinhole state (0.8) — partial at (0.8-0.75)/0.25"),
		FMath::IsNearlyEqual(Math::ComputeVirtualCrossfadeRamp(0.8f, 0.75f), 0.2f, 0.0001f));
	TestEqual(TEXT("Full occlusion — fully open"),
		Math::ComputeVirtualCrossfadeRamp(1.f, 0.75f), 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeTarget_LoSLoss_ForcesFullOpenBelowBand,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeTarget.LoSLoss_ForcesFullOpenBelowBand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeTarget_LoSLoss_ForcesFullOpenBelowBand::RunTest(const FString& Parameters) {
	// Smoothed occlusion lags a sudden break — the hard LoS-loss term must win over the ramp so
	// the virtual's entrance isn't delayed by occlusion smoothing.
	TestEqual(TEXT("LoS lost while the smoothed ramp is still low — gate fully open"),
		Math::ComputeVirtualCrossfadeTarget(false, false, /*SmoothedRamp=*/0.1f), 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeTarget_StationarySuppression_RampGovernsAlone,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeTarget.StationarySuppression_RampGovernsAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeTarget_StationarySuppression_RampGovernsAlone::RunTest(const FString& Parameters) {
	// Stationary scene with the ramp enabled: a marginal pinhole blanking one full sampling
	// rotation must not pump the gate to full — the hard LoS-loss term is suppressed and the
	// smoothed ramp governs alone.
	TestTrue(TEXT("LoS lost but suppressed — gate holds at the ramp level"),
		FMath::IsNearlyEqual(Math::ComputeVirtualCrossfadeTarget(false, /*bSuppressHardGate=*/true, 0.2f), 0.2f, 0.0001f));
	return true;
}


// ─── ComputeVirtualCrossfadeSlew ────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeSlew_FadeIn_RampsAtFadeInRate,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeSlew.FadeIn_RampsAtFadeInRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeSlew_FadeIn_RampsAtFadeInRate::RunTest(const FString& Parameters) {
	// Target on (1.0), starting from 0 — one step should move by DeltaTime/FadeInTime, not snap.
	const float FadeInTime = 0.5f;
	const float DeltaTime = 0.1f;
	const float Result = Math::ComputeVirtualCrossfadeSlew(
		/*CurrentCrossfade=*/0.f, /*TargetCrossfade=*/1.f, FadeInTime, /*FadeOutTime=*/0.5f, DeltaTime);

	const float Expected = DeltaTime / FadeInTime;
	TestTrue(TEXT("Fades in linearly at the fade-in rate"), FMath::IsNearlyEqual(Result, Expected, 0.0001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeSlew_FadeOut_RampsAtFadeOutRate,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeSlew.FadeOut_RampsAtFadeOutRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeSlew_FadeOut_RampsAtFadeOutRate::RunTest(const FString& Parameters) {
	// Target off (0.0), starting from 1 — one step should move down by DeltaTime/FadeOutTime.
	const float FadeOutTime = 0.5f;
	const float DeltaTime = 0.1f;
	const float Result = Math::ComputeVirtualCrossfadeSlew(
		/*CurrentCrossfade=*/1.f, /*TargetCrossfade=*/0.f, /*FadeInTime=*/0.5f, FadeOutTime, DeltaTime);

	const float Expected = 1.f - DeltaTime / FadeOutTime;
	TestTrue(TEXT("Fades out linearly at the fade-out rate"), FMath::IsNearlyEqual(Result, Expected, 0.0001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeSlew_AsymmetricTimes_UseCorrectDirection,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeSlew.AsymmetricTimes_UseCorrectDirection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeSlew_AsymmetricTimes_UseCorrectDirection::RunTest(const FString& Parameters) {
	// Fast fade-in (0.1s), slow fade-out (2.0s) — fading in should move much further in one
	// step than fading out would, confirming each direction reads its own time constant.
	const float DeltaTime = 0.05f;
	const float FadingIn = Math::ComputeVirtualCrossfadeSlew(0.f, 1.f, /*FadeInTime=*/0.1f, /*FadeOutTime=*/2.0f, DeltaTime);
	const float FadingOut = Math::ComputeVirtualCrossfadeSlew(1.f, 0.f, /*FadeInTime=*/0.1f, /*FadeOutTime=*/2.0f, DeltaTime);

	TestTrue(TEXT("Fast fade-in moves further per step than slow fade-out"),
		FadingIn > (1.f - FadingOut));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeSlew_ZeroFadeTime_IsInstant,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeSlew.ZeroFadeTime_IsInstant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeSlew_ZeroFadeTime_IsInstant::RunTest(const FString& Parameters) {
	const float Result = Math::ComputeVirtualCrossfadeSlew(
		/*CurrentCrossfade=*/0.f, /*TargetCrossfade=*/1.f, /*FadeInTime=*/0.f, /*FadeOutTime=*/0.f, /*DeltaTime=*/0.016f);
	TestEqual(TEXT("Zero fade time snaps instantly to target"), Result, 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVirtualCrossfadeSlew_DoesNotOvershoot,
	"SpatialAudioRay.Math.VirtualAudio.CrossfadeSlew.DoesNotOvershoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FVirtualCrossfadeSlew_DoesNotOvershoot::RunTest(const FString& Parameters) {
	// A large DeltaTime relative to FadeInTime must clamp at the target, not overshoot past 1.
	const float Result = Math::ComputeVirtualCrossfadeSlew(
		/*CurrentCrossfade=*/0.f, /*TargetCrossfade=*/1.f, /*FadeInTime=*/0.1f, /*FadeOutTime=*/0.1f, /*DeltaTime=*/5.f);
	TestEqual(TEXT("Large DeltaTime clamps at target rather than overshooting"), Result, 1.f);
	return true;
}


// ─── ClusterEdgePoints ────────────────────────────────────────────────────────

namespace {
	FCachedEdgePoint MakeEdgePoint(const FVector& Pos, float PathDist = 500.f,
	                               float GeomDist = 400.f, float EvictionAlpha = 1.f) {
		FCachedEdgePoint Ep;
		Ep.EdgePoint = Pos;
		Ep.PathDist = PathDist;
		Ep.GeomDist = GeomDist;
		Ep.EvictionAlpha = EvictionAlpha;
		return Ep;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_TightGroup_YieldsSingleCluster,
	"SpatialAudioRay.Math.ClusterEdgePoints.TightGroup_YieldsSingleCluster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_TightGroup_YieldsSingleCluster::RunTest(const FString& Parameters) {
	TArray<FCachedEdgePoint> Points;
	Points.Add(MakeEdgePoint(FVector(0, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(100, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(0, 100, 0)));

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, /*ClusterRadius=*/250.f, /*Falloff=*/0.f,
	                        /*ListenerPos=*/FVector::ZeroVector, /*ListenerFalloff=*/0.f,
	                        /*MaxRayDistance=*/5000.f, /*EmitterPullback=*/0.f, /*MaxClusters=*/4, Clusters);

	TestEqual(TEXT("Points within the radius form one cluster"), Clusters.Num(), 1);
	if (Clusters.Num() == 1) {
		// Falloff 0 and equal alphas → equal weights → plain average of the three positions.
		TestTrue(TEXT("Centroid is the average of member points"),
			Clusters[0].Centroid.Equals(FVector(100.f / 3.f, 100.f / 3.f, 0.f), 0.1f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_SeparatedGroups_YieldTwoClusters,
	"SpatialAudioRay.Math.ClusterEdgePoints.SeparatedGroups_YieldTwoClusters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_SeparatedGroups_YieldTwoClusters::RunTest(const FString& Parameters) {
	TArray<FCachedEdgePoint> Points;
	Points.Add(MakeEdgePoint(FVector(0, 0, 0), /*PathDist=*/400.f));
	Points.Add(MakeEdgePoint(FVector(50, 0, 0), /*PathDist=*/600.f));
	Points.Add(MakeEdgePoint(FVector(2000, 0, 0), /*PathDist=*/1000.f));

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, 250.f, 0.f, FVector::ZeroVector, 0.f, 5000.f, /*EmitterPullback=*/0.f, 4, Clusters);

	TestEqual(TEXT("Groups beyond the radius stay separate clusters"), Clusters.Num(), 2);
	if (Clusters.Num() == 2) {
		// Heavier (two-point) cluster sorts first; equal weights → PathDist is the plain average.
		TestTrue(TEXT("Heavier cluster is first"), Clusters[0].TotalWeight > Clusters[1].TotalWeight);
		TestTrue(TEXT("Cluster PathDist is the weighted average of its members"),
			FMath::IsNearlyEqual(Clusters[0].PathDist, 500.f, 0.1f));
		TestTrue(TEXT("Isolated point keeps its own PathDist"),
			FMath::IsNearlyEqual(Clusters[1].PathDist, 1000.f, 0.1f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_DriftedCentroids_AreMerged,
	"SpatialAudioRay.Math.ClusterEdgePoints.DriftedCentroids_AreMerged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_DriftedCentroids_AreMerged::RunTest(const FString& Parameters) {
	// Seeds at 0 and 260 start just past the radius (250) so they form two clusters; the point
	// at 120 then joins the first and drags its centroid to 60 — within 250 of centroid 260 —
	// so only the merge pass can combine them into one.
	TArray<FCachedEdgePoint> Points;
	Points.Add(MakeEdgePoint(FVector(0, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(260, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(120, 0, 0)));

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, 250.f, 0.f, FVector::ZeroVector, 0.f, 5000.f, /*EmitterPullback=*/0.f, 4, Clusters);

	TestEqual(TEXT("Clusters whose centroids drift within the radius merge into one"),
		Clusters.Num(), 1);
	if (Clusters.Num() == 1) {
		TestTrue(TEXT("Merged centroid is the average of all points"),
			Clusters[0].Centroid.Equals(FVector(380.f / 3.f, 0, 0), 0.1f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_MaxClusters_KeepsHeaviest,
	"SpatialAudioRay.Math.ClusterEdgePoints.MaxClusters_KeepsHeaviest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_MaxClusters_KeepsHeaviest::RunTest(const FString& Parameters) {
	// Three well-separated groups: sizes 3, 2, 1 → truncating to 2 keeps the two heaviest.
	TArray<FCachedEdgePoint> Points;
	Points.Add(MakeEdgePoint(FVector(0, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(50, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(0, 50, 0)));
	Points.Add(MakeEdgePoint(FVector(5000, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(5050, 0, 0)));
	Points.Add(MakeEdgePoint(FVector(0, 5000, 0)));

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, 250.f, 0.f, FVector::ZeroVector, 0.f, 10000.f, /*EmitterPullback=*/0.f, /*MaxClusters=*/2, Clusters);

	TestEqual(TEXT("Output truncates to MaxClusters"), Clusters.Num(), 2);
	if (Clusters.Num() == 2) {
		TestTrue(TEXT("Kept clusters are ordered heaviest first"),
			Clusters[0].TotalWeight >= Clusters[1].TotalWeight);
		TestTrue(TEXT("Heaviest cluster is the three-point group near the origin"),
			Clusters[0].Centroid.Size() < 100.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_ListenerFalloff_RanksWithoutTouchingGain,
	"SpatialAudioRay.Math.ClusterEdgePoints.ListenerFalloff_RanksWithoutTouchingGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_ListenerFalloff_RanksWithoutTouchingGain::RunTest(const FString& Parameters) {
	// Two separated points with identical source-side stats; the listener sits on the second.
	// Listener falloff must promote the near cluster to first place (selection/position), while
	// TotalWeight — the per-voice gain share — must stay source-side and therefore equal.
	TArray<FCachedEdgePoint> Points;
	Points.Add(MakeEdgePoint(FVector(0, 0, 0), /*PathDist=*/500.f));
	Points.Add(MakeEdgePoint(FVector(2000, 0, 0), /*PathDist=*/500.f));

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, 250.f, 0.f, /*ListenerPos=*/FVector(2000, 0, 0),
	                        /*ListenerFalloff=*/5.f, 5000.f, /*EmitterPullback=*/0.f, 4, Clusters);

	TestEqual(TEXT("Both points form clusters"), Clusters.Num(), 2);
	if (Clusters.Num() == 2) {
		TestTrue(TEXT("Listener-closer cluster ranks first"),
			Clusters[0].Centroid.Equals(FVector(2000, 0, 0), 0.1f));
		TestTrue(TEXT("TotalWeight (gain share) ignores listener distance — equal for both"),
			FMath::IsNearlyEqual(Clusters[0].TotalWeight, Clusters[1].TotalWeight, KINDA_SMALL_NUMBER));
		TestTrue(TEXT("PathDist average ignores listener distance"),
			FMath::IsNearlyEqual(Clusters[0].PathDist, Clusters[1].PathDist, 0.1f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_EvictionAlpha_ScalesWeight,
	"SpatialAudioRay.Math.ClusterEdgePoints.EvictionAlpha_ScalesWeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_EvictionAlpha_ScalesWeight::RunTest(const FString& Parameters) {
	// Two separated single-point clusters, one fading out (alpha 0.25): the confident one must
	// carry the higher weight, and a fully evicted point (alpha 0) must not create a cluster.
	TArray<FCachedEdgePoint> Points;
	Points.Add(MakeEdgePoint(FVector(0, 0, 0), 500.f, 400.f, /*EvictionAlpha=*/0.25f));
	Points.Add(MakeEdgePoint(FVector(2000, 0, 0), 500.f, 400.f, /*EvictionAlpha=*/1.f));
	Points.Add(MakeEdgePoint(FVector(4000, 0, 0), 500.f, 400.f, /*EvictionAlpha=*/0.f));

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, 250.f, 0.f, FVector::ZeroVector, 0.f, 5000.f, /*EmitterPullback=*/0.f, 4, Clusters);

	TestEqual(TEXT("Zero-alpha point creates no cluster"), Clusters.Num(), 2);
	if (Clusters.Num() == 2) {
		TestTrue(TEXT("Full-alpha cluster outweighs the fading one"),
			Clusters[0].Centroid.Equals(FVector(2000, 0, 0), 0.1f));
		TestTrue(TEXT("Weights reflect the 4x alpha ratio"),
			FMath::IsNearlyEqual(Clusters[0].TotalWeight / Clusters[1].TotalWeight, 4.f, 0.01f));
	}
	return true;
}

// ─── FCachedEdgePoint::EmitterPoint ───────────────────────────────────────────

namespace {
	// Path: source (0,0,0) → anchor (1000,0,0) → edge (1000,500,0).
	FCachedEdgePoint MakePathedEdgePoint() {
		FCachedEdgePoint Ep;
		Ep.EdgePoint = FVector(1000, 500, 0);
		Ep.ShortestPath = {FVector::ZeroVector, FVector(1000, 0, 0), FVector(1000, 500, 0)};
		Ep.ShortestPathSegmentVerified = {true, true};
		return Ep;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmitterPoint_ZeroPullback_IsEffectivePoint,
	"SpatialAudioRay.Math.EmitterPoint.ZeroPullback_IsEffectivePoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEmitterPoint_ZeroPullback_IsEffectivePoint::RunTest(const FString& Parameters) {
	const FCachedEdgePoint Ep = MakePathedEdgePoint();
	TestTrue(TEXT("Pullback 0 sits exactly at the edge point"),
		Ep.EmitterPoint(0.f).Equals(Ep.EdgePoint, 0.01f));
	FCachedEdgePoint NoPath;
	NoPath.EdgePoint = FVector(300, 0, 0);
	TestTrue(TEXT("Empty polyline stays at the edge point regardless of pullback"),
		NoPath.EmitterPoint(500.f).Equals(NoPath.EdgePoint, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmitterPoint_Pullback_WalksAlongPath,
	"SpatialAudioRay.Math.EmitterPoint.Pullback_WalksAlongPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEmitterPoint_Pullback_WalksAlongPath::RunTest(const FString& Parameters) {
	const FCachedEdgePoint Ep = MakePathedEdgePoint();
	TestTrue(TEXT("Pullback within the last segment stays on it"),
		Ep.EmitterPoint(200.f).Equals(FVector(1000, 300, 0), 0.01f));
	// 700 = the full 500 edge→anchor leg + 200 down the anchor→source leg.
	TestTrue(TEXT("Pullback spanning a corner continues along the next segment"),
		Ep.EmitterPoint(700.f).Equals(FVector(800, 0, 0), 0.01f));
	TestTrue(TEXT("Pullback beyond the whole path clamps at the source"),
		Ep.EmitterPoint(99999.f).Equals(FVector::ZeroVector, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmitterPoint_Pullback_StopsAtVerifiedBoundary,
	"SpatialAudioRay.Math.EmitterPoint.Pullback_StopsAtVerifiedBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEmitterPoint_Pullback_StopsAtVerifiedBoundary::RunTest(const FString& Parameters) {
	FCachedEdgePoint Ep = MakePathedEdgePoint();
	// Source→anchor is unverified traveled route: the walk must not enter it, however large
	// the pullback — a point on an unverified segment can sit inside geometry.
	Ep.ShortestPathSegmentVerified = {false, true};
	TestTrue(TEXT("Walk clamps at the first verified anchor"),
		Ep.EmitterPoint(99999.f).Equals(FVector(1000, 0, 0), 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmitterPoint_Pullback_DoesNotResumePastGap,
	"SpatialAudioRay.Math.EmitterPoint.Pullback_DoesNotResumePastGap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEmitterPoint_Pullback_DoesNotResumePastGap::RunTest(const FString& Parameters) {
	// Path: source (0,0,0) -> mid (500,0,0) -> anchor (1000,0,0) -> edge (1000,500,0).
	// Verified/unverified segments interleave: source->mid verified, mid->anchor unverified
	// (a corner the string pull couldn't shortcut past), anchor->edge verified again.
	FCachedEdgePoint Ep;
	Ep.EdgePoint = FVector(1000, 500, 0);
	Ep.ShortestPath = {
		FVector::ZeroVector, FVector(500, 0, 0), FVector(1000, 0, 0), FVector(1000, 500, 0)
	};
	Ep.ShortestPathSegmentVerified = {true, false, true};
	// The walk must clamp at the near side of the unverified gap (anchor) even though a
	// verified stretch exists further back — it must not skip over the gap to reach it.
	TestTrue(TEXT("Walk clamps at the gap, not the far verified stretch"),
		Ep.EmitterPoint(99999.f).Equals(FVector(1000, 0, 0), 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmitterPoint_Relayed_WalksRelayLegFirst,
	"SpatialAudioRay.Math.EmitterPoint.Relayed_WalksRelayLegFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEmitterPoint_Relayed_WalksRelayLegFirst::RunTest(const FString& Parameters) {
	FCachedEdgePoint Ep = MakePathedEdgePoint();
	Ep.bRelayed = true;
	Ep.RelayPoint = FVector(1000, 1000, 0); // 500 from the edge, straight extension of leg 2
	TestTrue(TEXT("Small pullback stays on the relay leg"),
		Ep.EmitterPoint(200.f).Equals(FVector(1000, 800, 0), 0.01f));
	// 700 = full 500 relay→edge leg + 200 along edge→anchor.
	TestTrue(TEXT("Larger pullback continues past the edge down the polyline"),
		Ep.EmitterPoint(700.f).Equals(FVector(1000, 300, 0), 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClusterEdgePoints_EmitterPullback_MovesCentroidOnly,
	"SpatialAudioRay.Math.ClusterEdgePoints.EmitterPullback_MovesCentroidOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FClusterEdgePoints_EmitterPullback_MovesCentroidOnly::RunTest(const FString& Parameters) {
	// One pathed edge: the cluster's output position must be the pulled-back point while
	// PathDist/TotalWeight (gain inputs) stay exactly as without pullback.
	TArray<FCachedEdgePoint> Points;
	FCachedEdgePoint Ep = MakePathedEdgePoint();
	Ep.PathDist = 1500.f;
	Ep.GeomDist = 1200.f;
	Points.Add(Ep);

	TArray<FEdgeCluster> Clusters;
	Math::ClusterEdgePoints(Points, 250.f, 0.f, FVector::ZeroVector, 0.f, 5000.f,
	                        /*EmitterPullback=*/200.f, 4, Clusters);

	TestEqual(TEXT("One cluster forms"), Clusters.Num(), 1);
	if (Clusters.Num() == 1) {
		TestTrue(TEXT("Centroid is the pulled-back point on the arrival path"),
			Clusters[0].Centroid.Equals(FVector(1000, 300, 0), 0.01f));
		TestTrue(TEXT("PathDist is untouched by the pullback"),
			FMath::IsNearlyEqual(Clusters[0].PathDist, 1500.f, 0.1f));
	}
	return true;
}

// ─── ComputeEffectiveAcousticDistance ─────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEffectiveAcousticDistance_BlendsByOcclusion,
	"SpatialAudioRay.Math.EffectiveAcousticDistance.BlendsByOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEffectiveAcousticDistance_BlendsByOcclusion::RunTest(const FString& Parameters) {
	TestTrue(TEXT("Clear LoS returns the straight-line distance"),
		FMath::IsNearlyEqual(Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 0.f), 500.f));
	TestTrue(TEXT("Fully occluded returns the path distance"),
		FMath::IsNearlyEqual(Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 1.f), 2000.f));
	TestTrue(TEXT("Partial occlusion interpolates"),
		FMath::IsNearlyEqual(Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 0.5f), 1250.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEffectiveAcousticDistance_FloorsPathAtDirect,
	"SpatialAudioRay.Math.EffectiveAcousticDistance.FloorsPathAtDirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEffectiveAcousticDistance_FloorsPathAtDirect::RunTest(const FString& Parameters) {
	// Independently smoothed legs can momentarily sum below the straight line; no physical
	// path can, so the result must never dip under the direct distance.
	TestTrue(TEXT("Path shorter than direct clamps to direct"),
		FMath::IsNearlyEqual(Math::ComputeEffectiveAcousticDistance(500.f, 300.f, 1.f), 500.f));
	TestTrue(TEXT("Occlusion outside [0,1] is clamped"),
		FMath::IsNearlyEqual(Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 1.5f), 2000.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEffectiveAcousticDistance_FloorDelaysTheDetour,
	"SpatialAudioRay.Math.EffectiveAcousticDistance.FloorDelaysTheDetour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FEffectiveAcousticDistance_FloorDelaysTheDetour::RunTest(const FString& Parameters) {
	// Edge caching finds routes well before the source is hidden (the pre-sweep band pre-warms
	// the cache during partial LoS). Counting them while most of the sound still arrives
	// straight overstates the distance, so the floor holds the result at the straight line
	// until the listener is actually hidden.
	const float Floor = 0.75f;

	TestTrue(TEXT("Below the floor the detour is ignored entirely"),
		FMath::IsNearlyEqual(
			Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 0.5f, Floor), 500.f));
	TestTrue(TEXT("At the floor it is still exactly the straight line — no jump on crossing"),
		FMath::IsNearlyEqual(
			Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, Floor, Floor), 500.f));
	TestTrue(TEXT("Halfway past the floor is halfway to the route"),
		FMath::IsNearlyEqual(
			Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 0.875f, Floor), 1250.f));
	TestTrue(TEXT("Full occlusion still reaches the whole route, floor or not"),
		FMath::IsNearlyEqual(
			Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 1.f, Floor), 2000.f));

	// Floor 0 is the identity, so callers that don't opt in are unaffected.
	TestTrue(TEXT("A zero floor reproduces the plain blend"),
		FMath::IsNearlyEqual(Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 0.5f, 0.f),
		                     Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 0.5f)));
	// A floor of 1 would divide by zero; it clamps instead of exploding.
	TestTrue(TEXT("A floor of 1 is clamped, not a division by zero"),
		FMath::IsFinite(Math::ComputeEffectiveAcousticDistance(500.f, 2000.f, 1.f, 1.f)));
	return true;
}

// ─── ComputeFalloffScaleForOuterRadius ────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFalloffScaleForOuterRadius_HitsTheTarget,
	"SpatialAudioRay.Math.FalloffScaleForOuterRadius.HitsTheTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFalloffScaleForOuterRadius_HitsTheTarget::RunTest(const FString& Parameters) {
	// Inner 100 + falloff 3900 = 4000cm authored range. Asking for 2050 should use half the
	// falloff: 100 + 0.5 * 3900 = 2050.
	TestTrue(TEXT("Scale places the audible edge at the requested distance"),
		FMath::IsNearlyEqual(Math::ComputeFalloffScaleForOuterRadius(2050.f, 100.f, 3900.f), 0.5f, 0.001f));
	TestTrue(TEXT("Asking for the full authored range yields scale 1"),
		FMath::IsNearlyEqual(Math::ComputeFalloffScaleForOuterRadius(4000.f, 100.f, 3900.f), 1.f, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFalloffScaleForOuterRadius_Clamps,
	"SpatialAudioRay.Math.FalloffScaleForOuterRadius.Clamps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FFalloffScaleForOuterRadius_Clamps::RunTest(const FString& Parameters) {
	// Inside the inner radius the engine plays at full volume, so a target there cannot be
	// honored — it yields the shortest legal falloff rather than silence.
	TestTrue(TEXT("Target inside the inner radius floors at the minimum scale"),
		FMath::IsNearlyEqual(Math::ComputeFalloffScaleForOuterRadius(50.f, 400.f, 3600.f),
		                     Math::MinFalloffScale, 0.001f));

	// Never past the authored range: the ray/LoS search is sized at base scale, so a sound
	// audible beyond it would have no diffraction paths to play through.
	TestTrue(TEXT("Target beyond the authored range caps at scale 1"),
		FMath::IsNearlyEqual(Math::ComputeFalloffScaleForOuterRadius(99000.f, 100.f, 3900.f), 1.f, 0.001f));

	TestTrue(TEXT("Zero target means the asset's own range"),
		FMath::IsNearlyEqual(Math::ComputeFalloffScaleForOuterRadius(0.f, 100.f, 3900.f), 1.f, 0.001f));
	TestTrue(TEXT("No attenuation asset (zero base falloff) means the asset's own range"),
		FMath::IsNearlyEqual(Math::ComputeFalloffScaleForOuterRadius(2000.f, 100.f, 0.f), 1.f, 0.001f));
	return true;
}
