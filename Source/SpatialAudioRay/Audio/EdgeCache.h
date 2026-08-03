#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Audio/SpatialAudioSettings.h"


class USpatialAudioComponent;
struct FCachedEdgePoint;

class FEdgeCache {
public:
	static void TickCachedEdgeEviction(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);

private:
	// INDEX_NONE if every entry is skipped.
	// Interval divided by cache size, so a setting means the period per edge rather than per cache.
	static float PerEdgeInterval(const USpatialAudioComponent& Component, float Interval);
	static int32 SelectRoundRobinEdge(const TArray<FCachedEdgePoint>& Points, int32& Cursor,
	                                  const TFunctionRef<bool(const FCachedEdgePoint&)>& ShouldSkip);
	// Segments shorter than 2*EndInset+1 are skipped.
	// Unverified segments are skipped: they were blocked at discovery by design, so tracing them
	// would evict every multi-corner edge.
	static void SubmitPolylineRecheckTraces(USpatialAudioComponent& Component, UWorld* World,
	                                        const TArray<FVector>& Path, const TArray<bool>& SegmentVerified,
	                                        float EndInset);
	// Moves the entry's source anchor to the live position and re-measures PathDist/GeomDist.
	static void ApplyRecheckReanchor(FCachedEdgePoint& Edge, const FVector& LiveSourcePos);
	static void TickShortestPathReadback(USpatialAudioComponent& Component, UWorld* World,
	                                     const FVector& SourcePos, const USpatialAudioSettings& Settings);

	// A handle from before a pipeline reset is meaningless, so drop the whole generation unread.
	static void ClearStalePendingChecks(USpatialAudioComponent& Component);
	// True on an interval boundary. Scaled by speed, so a moving player re-validates more often.
	static bool AdvancePhase0Timer(USpatialAudioComponent& Component, float DeltaTime,
	                               const USpatialAudioSettings& Settings);
	// True means the entry has faded out and the caller should drop it.
	static bool TickSingleEdge(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                           const FVector& SourcePos, const FVector& ListenerPos, float DeltaTime,
	                           bool bIntervalFired, const USpatialAudioSettings& Settings);

	static bool TickEvictionFade(FCachedEdgePoint& Edge, float DeltaTime, float EvictFadeTime);
	static void TickPhase0Readback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                               const FVector& SourcePos, const FVector& ListenerPos,
	                               const USpatialAudioSettings& Settings);
	static void SubmitPhase0OffsetFan(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                  const FVector& ListenerPos, float OffsetRadius);
	static void TickPhase0OffsetReadback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                     const FVector& SourcePos, const FVector& ListenerPos);
	// False while any of the four fan traces is still in flight. World stays non-const because
	// UWorld::QueryTraceData is not a const method.
	static bool ReadOffsetFanTraces(FCachedEdgePoint& Edge, UWorld* World, bool (&OutFanClear)[4]);
	static void DrawOffsetFan(const USpatialAudioComponent& Component, const FCachedEdgePoint& Edge, const UWorld* World,
	                          const bool (&FanClear)[4]);
	// A source-side eviction is left alone, since this check cannot speak for that side.
	static void RestoreFromListenerSideEviction(FCachedEdgePoint& Edge, const FVector& SourcePos,
	                                            const FVector& ListenerPos);
	static void RescueOrEvict(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                          const FVector& SourcePos, const FVector& ListenerPos);
	static void TickPhase0Submission(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                 const FVector& ListenerPos, bool bIntervalFired);
	static void StartEviction(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const FVector& SourcePos,
	                          bool bSourceSide = false);
	// False when no rescue is possible at all, which is the caller's cue to evict now. True means
	// traces are in flight and the verdict lands in TickRelayRescueReadback next tick.
	static bool SubmitRelayRescueTraces(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                    UWorld* World, const FVector& ListenerPos);
	static void TickRelayRescueReadback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                    const FVector& SourcePos, const FVector& ListenerPos);
	static bool ProbeListenerLoSPoint(const USpatialAudioComponent& Component, const UWorld* World,
	                                  const FVector& ListenerPos, const FVector& Point);
	static void DrawProbeResult(const USpatialAudioComponent& Component, const UWorld* World,
	                            const FVector& Point, bool bClear);
	static int32 ResolveStepsForMergeRadius(const USpatialAudioComponent& Component, float Span);
	// Returns ClearEnd when no midpoint cleared (bOutFoundClear false). ExplicitSteps 0 derives the
	// count from the bracket; only the promotion refinement pins it, to bound its trace cost.
	static FVector BisectListenerLoS(const USpatialAudioComponent& Component, const UWorld* World, const FVector& ListenerPos,
	                                 const FVector& BlockedEnd, const FVector& ClearEnd, bool& bOutFoundClear,
	                                 int32 ExplicitSteps = 0);
	// Only the slow round-robin passes bAllowSubSegmentRefine. The Phase 0 rescue site re-fires
	// every interval while blocked and must not pay the bisection traces each time.
	static bool TryPromoteToInnerAnchor(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
	                                    const FVector& ListenerPos, bool bAllowSubSegmentRefine);
	// Converges the emitter onto the real corner instead of the step size discovery quantized it
	// to, so it slides along the wall as the listener moves.
	static bool TryJumpToPreviousVertex(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                    const UWorld* World, const FVector& ListenerPos,
	                                    const FVector& InnerAnchor);
	static bool TryRefineAlongFinalSegment(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
	                                       const FVector& ListenerPos, const FVector& InnerAnchor);
	static void TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                 const FVector& SourcePos, const FVector& ListenerPos, bool bIntervalFired);
	// [0..1] listener to edge, [2..3] edge to relay, each forward and reverse.
	static bool ReadRelayCheckTraces(FCachedEdgePoint& Edge, UWorld* World, FTraceDatum (&OutData)[4]);
	static void SubmitRelayCheckTraces(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                   UWorld* World, const FVector& ListenerPos);
	// Leaves bRelayed a transitional state rather than somewhere an entry rests.
	static void ConvertRelayToEdge(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
	                               const FVector& ListenerPos);
	// Promotion pops a vertex per jump, so an entry can be left without a usable polyline. Its
	// straight source-to-edge line stands in, the same fallback ConvertRelayToEdge seeds.
	static TArray<FVector> ResolveRecheckPath(const FCachedEdgePoint& Edge);
	static int32 FindFirstBlockedSegment(const TArray<FTraceDatum>& Data);
	// Null when the entry was rewritten or removed since its recheck was submitted.
	static FCachedEdgePoint* FindEntryByExactEdgePoint(USpatialAudioComponent& Component,
	                                                   const FVector& EdgePoint);
	static void TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
	                                    const FVector& SourcePos, float DeltaTime,
	                                    const USpatialAudioSettings& Settings);
	static void TickInnerAnchorPromotion(USpatialAudioComponent& Component, const UWorld* World,
	                                     const FVector& ListenerPos, float DeltaTime,
	                                     const USpatialAudioSettings& Settings);
	static void MergeCoincidentEdges(USpatialAudioComponent& Component,
	                                 const USpatialAudioSettings& Settings);

public:
	// A relay is a stopgap and an evicting entry is already leaving, so neither may be merged into.
	static bool IsMergeCandidate(const FCachedEdgePoint& Edge);
	// Bounce count deliberately stays out of it, unlike the sweep's admission test: a 3-bounce 8m
	// route beats a 1-bounce 40m one to the same corner. Ties keep the incumbent.
	static bool TravelledFurther(const FCachedEdgePoint& Edge, const FCachedEdgePoint& Other);
};
