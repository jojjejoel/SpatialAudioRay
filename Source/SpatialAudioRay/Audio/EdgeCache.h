// Lightweight wrapper for cached-edge related helpers.
#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioSettings.h"


class USpatialAudioComponent;
struct FCachedEdgePoint;

class FEdgeCache {
public:
	static void TickCachedEdgeEviction(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);

private:
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
	static void TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& EP, UWorld* World,
	                                 const FVector& SrcPos, const FVector& LisPos, bool bIntervalFired);
	static void TickShortestPathRecheck(USpatialAudioComponent& Component, UWorld* World,
	                                    const FVector& SrcPos, float DeltaTime,
	                                    const USpatialAudioSettings& Settings);
};
