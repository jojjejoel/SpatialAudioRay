// Lightweight wrapper for cached-edge related helpers.
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
	// Round-robins through Points starting at Cursor, skipping entries ShouldSkip rejects, and
	// advances Cursor past whatever it returns. INDEX_NONE if every entry is skipped. Shared by
	// the recheck and promotion round-robins below, which differ only in their skip condition.
	static int32 SelectRoundRobinEdge(const TArray<FCachedEdgePoint>& Points, int32& Cursor,
	                                  TFunctionRef<bool(const FCachedEdgePoint&)> ShouldSkip);
	// Submits forward+reverse async traces for every segment of Path (pulled in by Pull at both
	// ends to avoid corner-clipping false positives; short segments <= 2*Pull+1 are skipped)
	// into Component.PathRecheck, alongside the pulled endpoints for the readback's debug draw.
	static void SubmitPolylineRecheckTraces(USpatialAudioComponent& Component, UWorld* World,
	                                        const TArray<FVector>& Path, float Pull);
	// Reads back an in-flight polyline recheck; waits (returns) while any trace is still in
	// flight. A blocked segment source-side-evicts the checked entry, re-found by exact
	// EdgePoint match — an entry rewritten or removed since submission drops the result.
	static void TickShortestPathReadback(USpatialAudioComponent& Component, UWorld* World,
	                                     const FVector& SrcPos, const USpatialAudioSettings& Settings);
	// Ranks non-evicting points by movement delta (most stale first) and spreads their eviction
	// thresholds across [MoveThresh/N, MoveThresh] so a shared movement doesn't evict all of them
	// on the same frame.
	static TArray<float> ComputeProgressiveMoveThresholds(const USpatialAudioComponent& Component,
	                                                       const FVector& SrcPos, float MoveThresh);

	static bool TickEvictionFade(FCachedEdgePoint& EP, float DeltaTime, float EvictFadeTime);
	static void TickPhase0Readback(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                               const FVector& SrcPos, const FVector& LisPos,
	                               const USpatialAudioSettings& Settings);
	static void SubmitPhase0OffsetFan(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                  const FVector& LisPos, float OffsetR);
	static void TickPhase0OffsetReadback(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                     const FVector& SrcPos, const FVector& LisPos);
	static bool TickMovementThresholdEviction(USpatialAudioComponent& Component, FCachedEdgePoint& EP,
	                                          const FVector& SrcPos, float EffectiveMoveThresh);
	static void TickPhase0Submission(const USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                 const FVector& LisPos, bool bIntervalFired);
	static void StartEviction(USpatialAudioComponent& Component, FCachedEdgePoint& EP, const FVector& SrcPos,
	                          bool bSourceSide = false);
	static bool TryRelayRescue(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                           const FVector& LisPos);
	// Forward+reverse listener LoS probe at P, drawing a green/red result sphere under the
	// shortest-path view (key 0). Shared by inner-anchor promotion and relay→edge conversion.
	static bool ProbeListenerLoSPoint(USpatialAudioComponent& Component, UWorld* World,
	                                  const FVector& LisPos, const FVector& P);
	// Bisects between a listener-blocked and a listener-clear segment end for the LoS transition
	// point (the diffraction corner on that segment). Returns the innermost traced-clear point,
	// or ClearEnd unchanged when no midpoint cleared (bOutFoundClear false).
	// ExplicitSteps 0 derives the count from the bracket length so the result lands within half
	// CachedEdgeMergeRadius of the true corner regardless of segment length — a fixed count made
	// accuracy scale with the segment, which left relay conversions off the same corner too far
	// apart to ever merge. Only the promotion refinement overrides it
	// (ShortestPathPromotionBisectSteps), to pin its per-check trace cost.
	static FVector BisectListenerLoS(USpatialAudioComponent& Component, UWorld* World, const FVector& LisPos,
	                                 const FVector& BlockedEnd, const FVector& ClearEnd, bool& bOutFoundClear,
	                                 int32 ExplicitSteps = 0);
	// bAllowSubSegmentRefine: when the previous vertex is blocked, additionally binary-search the
	// final (verified) segment for the LoS transition point. Only the slow opportunistic
	// round-robin passes true — the Phase 0 rescue site re-fires every interval while blocked
	// and must not pay the bisection traces each time.
	static bool TryPromoteToInnerAnchor(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                    const FVector& LisPos, bool bAllowSubSegmentRefine);
	static void TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                 const FVector& SrcPos, const FVector& LisPos, bool bIntervalFired);
	static void TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
	                                    const FVector& SrcPos, float DeltaTime,
	                                    const USpatialAudioSettings& Settings);
	static void TickInnerAnchorPromotion(USpatialAudioComponent& Component, UWorld* World,
	                                     const FVector& LisPos, float DeltaTime,
	                                     const USpatialAudioSettings& Settings);
	// Collapses entries that have drifted within CachedEdgeMergeRadius of each other, keeping the
	// one that travelled least to get there. The sweep's own merge only matches INCOMING finds
	// against the cache; nothing reconciles cached entries against each other, and three sites
	// move an entry's EdgePoint after admission (relay→edge conversion, inner-anchor promotion,
	// sub-segment refinement).
	static void MergeCoincidentEdges(USpatialAudioComponent& Component,
	                                 const USpatialAudioSettings& Settings);
};
