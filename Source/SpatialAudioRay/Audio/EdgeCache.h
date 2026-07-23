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
	// Traces each segment of Path (pulled in by Pull at both ends to avoid corner-clipping
	// false positives). Returns true and fills OutBlockedA/B with the clipped endpoints of the
	// first blocked segment found; short segments (<= 2*Pull+1) are skipped entirely.
	static bool IsPolylineBlocked(const USpatialAudioComponent& Component, UWorld* World, const TArray<FVector>& Path,
	                              float Pull, FVector& OutBlockedA, FVector& OutBlockedB);
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
	static bool HasOtherDirectEdge(const USpatialAudioComponent& Component, const FCachedEdgePoint& Self);
	static bool TryRelayRescue(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                           const FVector& LisPos);
	static bool TryPromoteToInnerAnchor(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                    const FVector& LisPos);
	static void TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                 const FVector& SrcPos, const FVector& LisPos, bool bIntervalFired);
	static void TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
	                                    const FVector& SrcPos, float DeltaTime,
	                                    const USpatialAudioSettings& Settings);
	static void TickInnerAnchorPromotion(USpatialAudioComponent& Component, UWorld* World,
	                                     const FVector& LisPos, float DeltaTime,
	                                     const USpatialAudioSettings& Settings);
};
