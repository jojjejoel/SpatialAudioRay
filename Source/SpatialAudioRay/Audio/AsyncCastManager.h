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
		float MaxRayDistance,
		const USpatialAudioSettings& Settings);

	struct FRayAccumulatorInput {
		int32 RaysReached = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPos = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float MaxRayDistance = 0.f;
		bool bDirectLoSFound = false;
	};

	struct FRayAccumulatorOutput {
		FVector VirtualSourcePos = FVector::ZeroVector;
		bool bHasVirtualSource = false;
		float MinLoSDist = 0.f;
	};

	static FRayAccumulatorOutput ComputeAudioFromRayAccumulator(const FRayAccumulatorInput& In);

	// Seeded per (source, listener, ray index) so the lateral-band bias resampling in
	// StartAsyncFullCast is reproducible while the player and source are stationary.
	static FRandomStream MakeBiasStream(const FVector& SourcePos, const FVector& ListenerPos, int32 RayIndex);

	// Entries that stand in for a ray this sweep. A relay is an audible stopgap rather than a found
	// path and an evicting entry is on its way out, so neither counts and the sweep keeps searching
	// at full budget until a real path displaces them.

	// The cycles of a full sweep sequence partition the sphere with no direction cast twice.
	// OutIndices must stay the whole-set index, since MakeBiasStream seeds off it.
	static void SelectCycleDirections(const TArray<FVector>& AllDirections, int32 StartIndex, int32 CycleCount,
	                                  TArray<FVector>& OutDirections, TArray<int32>& OutIndices);

	// Anything at or past the LoS origin's travelled distance lies beyond the edge point the pull
	// starts from, so it cannot be an anchor.
	static int32 CountPrefixAnchorWaypoints(const TArray<FSpatialRayState::FBounceWaypoint>& Waypoints,
	                                        float LoSCumulativeDistance);

	// Mid-air counterpart of ComputeBouncedDirection. No surface normal exists, so the current
	// direction takes the reflected direction's role and scatter uses the full sphere. At zero
	// roughness and zero bias it turns 90 degrees instead of returning InDir unchanged.
	static FVector ComputeMidAirTurnDirection(const FVector& InDir, const FVector& TurnPoint,
	                                          const FVector& ListenerPos, bool bApplyBias,
	                                          float SurfaceRoughness, float BounceListenerBias);

private:
	// ── ReadbackFinalizeBatch phases ─────────────────────────────────────────
	static bool TryDiscardStaleSweep(USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings);
	static void AccumulateRefineProbesIntoCycle(USpatialAudioComponent& Component, const UWorld* World, const USpatialAudioSettings& Settings);
	static void MergeStoredPathsIntoCache(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void AdvanceSweepCycleAndIdleState(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void PublishSweepAudioTargets(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);

	// ── MergeStoredPathsIntoCache phases ─────────────────────────────────────
	// Mirrors the cluster priority: source-path falloff over listener-proximity falloff, the
	// listener term inert while ListenerDistanceFalloff is 0.
	static float RankScore(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                       float PathDist, const FVector& Point);
	static bool OutranksIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                              const FStoredLoSPath& Found, const FCachedEdgePoint& Incumbent);
	static void WriteEntry(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const FStoredLoSPath& Found);
	static int32 FindMergeCandidate(const USpatialAudioComponent& Component, const FVector& Point, float MergeRadiusSq);
	static bool IsWorseIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                             const FCachedEdgePoint& Candidate, const FCachedEdgePoint& Worst);
	static int32 FindWorstIncumbent(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                const TArray<bool>& bMatchedThisCycle);
	static void MergeIntoSameCorner(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                const FStoredLoSPath& Found);
	static bool TryDisplaceWorstIncumbent(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                      const FStoredLoSPath& Found, TArray<bool>& bMatchedThisCycle);

	// A ray with probes still in flight waits one more tick, picked up via bTerminalLoSPending.
	static void FinishOrDefer(FSpatialRayState& Ray, bool& bAllDone);

	// ── StartAsyncFullCast phases ────────────────────────────────────────────
	static void CaptureSweepPositions(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
	                                  const AActor& Owner, const APawn& Pawn);
	static void ResolveSweepRayBudget(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static int32 CountHeldEdges(const TArray<FCachedEdgePoint>& Points);
	// Scales the sweep budget down as the cache fills, never below MinFullSweepRayCount.
	static int32 ApplyCacheFullnessRayScale(const USpatialAudioComponent& Component, int32 RayCount,
	                                        const USpatialAudioSettings& Settings);
	static void ResetCycleAccumulator(USpatialAudioComponent& Component);
	static void SubmitSweepRays(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings, UWorld* World,
	                            const FVector& ToListenerDir, float FullCastDistance, int32 CycleCount);

	static FVector ApplyLateralBandBias(const USpatialAudioComponent& Component, const FVector& Dir,
	                                    const FVector& ToListenerDir, float FullCastDistance, int32 DirectionIndex,
	                                    const USpatialAudioSettings& Settings);

	// ── TickAsyncCast per-ray phases ─────────────────────────────────────────
	static void TickSingleRay(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                          float Budget, bool& bAllDone, const USpatialAudioSettings& Settings);
	static bool TryProcessMidAirTurn(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                 float Budget, bool bSegMissed, bool& bAllDone,
	                                 const USpatialAudioSettings& Settings);
	static void ProcessRayTermination(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                  const FTraceDatum& SegData, bool bSegMissed, float Budget, bool& bAllDone,
	                                  const USpatialAudioSettings& Settings);
	static void ProcessRayBounceContinuation(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                         const FHitResult& Hit, float Budget, bool& bAllDone,
	                                         const USpatialAudioSettings& Settings);
	static bool TrySetupSurfaceCrawl(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                 const FHitResult& Hit, const USpatialAudioSettings& Settings);

	static void DrainPendingLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World, const FVector& ListenerPos);

	// A crawl's incoming and outgoing legs are flight, so the crawl view needs them too.
	static bool ShouldDrawFlightPaths(const USpatialAudioComponent& Component);
	static void DrawFlightSegment(const USpatialAudioComponent& Component, const UWorld* World, const FVector& From,
	                              const FVector& To, const USpatialAudioSettings& Settings);

	// False when the budget gate rejected it. Since that sum only grows along a ray, it also tells
	// a caller walking outward that every later point is out of budget too.
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
	// The three questions each crawl step asks: has the listener come into view, has a wall closed
	// off the crawl, and has the surface fallen away behind us (the diffraction edge being hunted).
	static void TryConfirmLoSAtCrawlStep(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
	                                     UWorld* World,
	                                     const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe,
	                                     const USpatialAudioSettings& Settings);
	static bool TryTakePerpWallExit(const FSpatialRayState& Ray, UWorld* World,
	                                const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe,
	                                int32 StepIdx, FCrawlStepResult& OutResult);
	static bool TryTakeFreeEdgeExit(const USpatialAudioComponent& Component, const FSpatialRayState& Ray,
	                                UWorld* World,
	                                const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe,
	                                int32 StepIdx, FCrawlStepResult& OutResult);
	static FCrawlStepResult EvaluateCrawlSteps(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                                           int32 Limit, const USpatialAudioSettings& Settings);
	static void DrawCrawlDebugVisualization(const FSpatialRayState& Ray, const UWorld* World,
	                                        const FCrawlStepResult& Result, int32 Limit, const USpatialAudioSettings& Settings);
	static void ApplyCrawlResult(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
	                             const FCrawlStepResult& Result, const USpatialAudioSettings& Settings);

	static void ProcessCrawlBatch(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
	                              float Budget, bool& bAllDone,
	                              const USpatialAudioSettings& Settings);
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
