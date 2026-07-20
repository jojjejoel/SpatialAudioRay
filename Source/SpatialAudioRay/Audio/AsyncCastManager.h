#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "SpatialAudioSettings.h"
#include "SpatialAudioTypes.h"

class USpatialAudioComponent;

class FAsyncCastManager {
public:
	static void StartAsyncFullCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void TickAsyncCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void SubmitFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void ReadbackFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);

	struct FCachedPointAccum {
		int32 RaysReached = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPos = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float WeightedDist = 0.f;
	};

	static FCachedPointAccum AccumulateCachedPoints(
		const TArray<FCachedEdgePoint>& Points,
		const FVector& ListenerPos,
		const USpatialAudioSettings& Settings);

	static void UpdateMissDirState(
		const FSpatialRayState& Ray,
		const FVector& SourcePos,
		const FVector& ListenerPos,
		const TArray<FVector>& CachedEdgeDirs,
		TArray<FCachedMissDir>& InOutMissDirs,
		bool& bGeometryChangeDetected,
		const USpatialAudioSettings& Settings);

	struct FRayAccumulatorInput {
		int32 RaysReached = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPos = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float DirectDist = 0.f;
		float MaxRayDistance = 0.f;
		bool bDirectLoSFound = false;
		/** Needed to derive the straight-line source→virtual distance (Leg1Geom) for
		 *  PathAttenuationGeomBlend — unused otherwise. */
		FVector SourcePos = FVector::ZeroVector;
	};

	struct FRayAccumulatorOutput {
		float OcclusionValue = 1.f;
		float PathAttenuation = 0.f;
		FVector VirtualSourcePos = FVector::ZeroVector;
		bool bHasVirtualSource = false;
		float MinLoSDist = 0.f;
	};

	static FRayAccumulatorOutput ComputeAudioFromRayAccumulator(
		const FRayAccumulatorInput& In,
		const USpatialAudioSettings& Settings);

	// Seeded per (source, listener, ray index) so the lateral-band bias resampling in
	// StartAsyncFullCast is reproducible while the player and source are stationary.
	static FRandomStream MakeBiasStream(const FVector& SourcePos, const FVector& ListenerPos, int32 RayIndex);

	// Mid-air counterpart of ComputeBouncedDirection for MaxStraightFlightDistance turns:
	// no surface normal exists, so the current direction takes the reflected direction's role
	// and scatter uses the full sphere. At zero roughness AND zero listener bias the scatter
	// formula would return InDir unchanged (a wasted straight "turn"), so it instead turns 90°
	// at an angle deterministically seeded from the turn point — stationary scenes replay the
	// same direction every sweep. Shared by the async pipeline and the sync sweeps.
	static FVector ComputeMidAirTurnDirection(const FVector& InDir, const FVector& TurnPoint,
	                                          const FVector& ListenerPos, bool bApplyBias,
	                                          float SurfaceRoughness, float BounceListenerBias);

private:
	static void DrainPendingLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World, const FVector& ListenerPos);
	static void ProcessCrawlBatch(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                              bool bBias, float Budget, bool& bAllDone,
	                              const USpatialAudioSettings& Settings);
	static TArray<FVector> BuildEdgeDirHints(const TArray<FStoredLoSPath>& StoredPaths, const FVector& SourcePos);
	static void SubmitSegmentLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                   const FVector& SegOrigin, const FVector& SegDir,
	                                   float SegLen, float Budget,
	                                   const USpatialAudioSettings& Settings);
	static FVector ComputeBouncedDirection(const FVector& InDir, const FVector& SurfaceNormal,
	                                       bool bApplyBias, const FVector& HitLocation,
	                                       const FVector& ListenerPos, float SurfaceRoughness,
	                                       float BounceListenerBias);
	static bool HasClearShortcut(const USpatialAudioComponent& Component, const UWorld* World,
	                             const FVector& Edge, const FVector& Anchor);
	static float ComputeStringPulledLeg1(const USpatialAudioComponent& Component, const UWorld* World,
	                                     const FSpatialRayState& Ray, const FVector& SourcePos,
	                                     TArray<FVector>& OutPath, int32& OutVerifiedFrom);
};
