#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "SpatialAudioSettings.h"
#include "SpatialAudioTypes.h"

class USpatialAudioComponent;
class AActor;
class APawn;

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

	// One sub-cycle's share of a sweep: every CycleCount-th direction starting at StartIndex, so
	// the cycles of a full sweep sequence partition the sphere with no direction cast twice.
	// OutIndices carries each direction's index in the whole set — MakeBiasStream seeds off it,
	// so it must stay the full-set index, not the position within the slice.
	static void SelectCycleDirections(const TArray<FVector>& AllDirections, int32 StartIndex, int32 CycleCount,
	                                  TArray<FVector>& OutDirections, TArray<int32>& OutIndices);

	// How many leading waypoints string pulling may use as anchors. Anything at or past the LoS
	// origin's travelled distance lies beyond the edge point the pull starts from, so it cannot
	// be on the source side of the path being shortened.
	static int32 CountPrefixAnchorWaypoints(const TArray<FSpatialRayState::FBounceWaypoint>& Waypoints,
	                                        float LoSCumulativeDistance);

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
	// ── ReadbackFinalizeBatch phases ─────────────────────────────────────────
	static bool TryDiscardStaleSweep(USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings);
	static void AccumulateRefineProbesIntoCycle(USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings);
	static void MergeStoredPathsIntoCache(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void AdvanceSweepCycleAndIdleState(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);

	// A ray with no more pending async LoS probes is complete; one with probes still in flight
	// must wait one more tick (bTerminalLoSPending picks it up on the early-out path next tick).
	// The same shape recurs at every ray-lifecycle exit point in this file.
	static void FinishOrDefer(FSpatialRayState& Ray, bool& bAllDone);

	// ── StartAsyncFullCast phases ────────────────────────────────────────────
	static void CaptureSweepPositions(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                  const AActor& Owner, const APawn& Pawn);
	static void ResolveSweepRayBudget(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void BuildCachedEdgeExclusionDirs(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void UpdateActiveMissDirs(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void ResetCycleAccumulator(USpatialAudioComponent& Component);
	static void SubmitSweepRays(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings, UWorld* World,
	                            const FVector& ToListenerDir, float FullCastDistance, int32 CycleCount);

	// What the known-miss filter decided about one candidate sweep direction.
	struct FMissDirResolution {
		FVector Dir = FVector::ZeroVector;
		/** Direction is a re-probe of a recorded miss; the ray records that and skips crawling. */
		bool bIsMissDir = false;
		/** Dir was deliberately overridden, so the lateral-band bias must leave it alone. */
		bool bDirFixed = false;
		/** Direction dropped from this sweep entirely. */
		bool bSkip = false;
	};

	static FMissDirResolution ResolveMissDirection(const USpatialAudioComponent& Component, const FVector& Dir,
	                                              float MissMinDot, const USpatialAudioSettings& Settings);
	static FVector ApplyLateralBandBias(const USpatialAudioComponent& Component, const FVector& Dir,
	                                    const FVector& ToListenerDir, float FullCastDistance, int32 DirectionIndex,
	                                    const USpatialAudioSettings& Settings);

	// ── TickAsyncCast per-ray phases ─────────────────────────────────────────
	static void TickSingleRay(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                          bool bBias, float Budget, bool& bAllDone, const USpatialAudioSettings& Settings);
	static bool TryProcessMidAirTurn(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                 bool bBias, float Budget, bool bSegMissed, bool& bAllDone,
	                                 const USpatialAudioSettings& Settings);
	static void ProcessRayTermination(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                  const FTraceDatum& SegData, bool bSegMissed, float Budget, bool& bAllDone,
	                                  const USpatialAudioSettings& Settings);
	static void ProcessRayBounceContinuation(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                         const FHitResult& Hit, bool bBias, float Budget, bool& bAllDone,
	                                         const USpatialAudioSettings& Settings);
	static bool TrySetupSurfaceCrawl(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                 const FHitResult& Hit, const USpatialAudioSettings& Settings);

	static void DrainPendingLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World, const FVector& ListenerPos);

	// Flight paths belong to both the bounce-ray and surface-crawl views: a crawl's incoming and
	// outgoing legs are flight, so hiding them whenever bounce rays are off would leave the crawl
	// picture with no way in or out.
	static bool ShouldDrawFlightPaths(const USpatialAudioComponent& Component);
	static void DrawFlightSegment(const USpatialAudioComponent& Component, UWorld* World, const FVector& From,
	                              const FVector& To, const USpatialAudioSettings& Settings);

	// Submits one point→listener LoS probe, gated on Math::IsWithinPathBudget. Returns false when
	// the gate rejected it — which, since that sum only grows along a ray, also tells a caller
	// walking outward that every later point on the same segment is out of budget too.
	static bool TryAddListenerLoSProbe(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
	                                   UWorld* World, const FVector& SamplePos, float CumDist, float Budget,
	                                   const USpatialAudioSettings& Settings);

	static void SubmitFlightSegment(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
	                                UWorld* World, float SegmentLength);

	// ── ProcessCrawlBatch phases ─────────────────────────────────────────────
	struct FCrawlStepResult {
		bool bSucceeded = false;
		bool bPerpWallHit = false;
		FVector EdgePoint = FVector::ZeroVector;
		FVector EdgeDir = FVector::ZeroVector;
		FVector PerpNormal = FVector::ZeroVector;
		float CrawlDist = 0.f;
		int32 FoundAtStep = -1;
	};

	static bool AreCrawlTracesReady(UWorld* World, FSpatialRayState& Ray, FTraceDatum& OutRangeData);
	static FCrawlStepResult EvaluateCrawlSteps(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                           int32 Limit, const USpatialAudioSettings& Settings);
	static void DrawCrawlDebugVisualization(const FSpatialRayState& Ray, const UWorld* World,
	                                        const FCrawlStepResult& Result, int32 Limit, const USpatialAudioSettings& Settings);
	static void ApplyCrawlResult(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
	                             const FCrawlStepResult& Result, bool bBias, const USpatialAudioSettings& Settings);

	static void ProcessCrawlBatch(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                              bool bBias, float Budget, bool& bAllDone,
	                              const USpatialAudioSettings& Settings);
	static TArray<FVector> BuildEdgeDirHints(const TArray<FStoredLoSPath>& StoredPaths, const FVector& SourcePos);
	static void SubmitSegmentLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                   const FVector& SegOrigin, const FVector& SegDir,
	                                   float SegLen, float Budget,
	                                   const USpatialAudioSettings& Settings);
	static bool HasClearShortcut(const USpatialAudioComponent& Component, const UWorld* World,
	                             const FVector& Edge, const FVector& Anchor);
	static int32 FindFirstVisibleAnchor(const USpatialAudioComponent& Component, const UWorld* World,
	                                    const FVector& FromPoint,
	                                    const TArray<FSpatialRayState::FBounceWaypoint>& Waypoints,
	                                    int32 SearchLimit);
	static float ComputeStringPulledLeg1(const USpatialAudioComponent& Component, const UWorld* World,
	                                     const FSpatialRayState& Ray, const FVector& SourcePos,
	                                     TArray<FVector>& OutPath, TArray<bool>& OutSegmentVerified);
};
