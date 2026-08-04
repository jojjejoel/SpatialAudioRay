#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Audio/SpatialAudioSettings.h"


class USpatialAudioComponent;
struct FCachedEdgePoint;

class FEdgeCache {
public:
	static void TickCachedEdgeEviction(USpatialAudioComponent& Component, float DeltaTime,
	                                   const USpatialAudioSettings& Settings);

private:
	static float PerEdgeInterval(const USpatialAudioComponent& Component, float Interval);
	static float EdgeCheckSlice(const USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static int32 SelectRoundRobinEdge(const TArray<FCachedEdgePoint>& Points, int32& Cursor,
	                                  const TFunctionRef<bool(const FCachedEdgePoint&)>& ShouldSkip);
	static void SubmitPolylineRecheckTraces(USpatialAudioComponent& Component, UWorld* World,
	                                        const TArray<FVector>& Path, const TArray<bool>& SegmentVerified,
	                                        float EndInset);
	static void ApplyRecheckReanchor(FCachedEdgePoint& Edge, const FVector& LiveSourcePos);
	static void TickShortestPathReadback(USpatialAudioComponent& Component, UWorld* World,
	                                     const FVector& SourcePos, const USpatialAudioSettings& Settings);

	static void ClearStalePendingChecks(USpatialAudioComponent& Component);
	static bool AdvancePhase0Timer(USpatialAudioComponent& Component, float DeltaTime,
	                               const USpatialAudioSettings& Settings);
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
	static bool ReadOffsetFanTraces(FCachedEdgePoint& Edge, UWorld* World, bool (&OutFanClear)[4]);
	static void DrawOffsetFan(const USpatialAudioComponent& Component, const FCachedEdgePoint& Edge,
	                          const UWorld* World,
	                          const bool (&FanClear)[4]);
	static void RestoreFromListenerSideEviction(FCachedEdgePoint& Edge, const FVector& SourcePos,
	                                            const FVector& ListenerPos);
	static void RescueOrEvict(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                          const FVector& SourcePos, const FVector& ListenerPos);
	static void TickPhase0Submission(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                 const FVector& ListenerPos, bool bIntervalFired);
	static void StartEviction(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const FVector& SourcePos,
	                          bool bSourceSide = false);
	static bool SubmitRelayRescueTraces(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                    UWorld* World, const FVector& ListenerPos);
	static void TickRelayRescueReadback(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                    const FVector& SourcePos, const FVector& ListenerPos);
	static bool ProbeListenerLoSPoint(const USpatialAudioComponent& Component, const UWorld* World,
	                                  const FVector& ListenerPos, const FVector& Point);
	static void DrawProbeResult(const USpatialAudioComponent& Component, const UWorld* World,
	                            const FVector& Point, bool bClear);
	static int32 ResolveStepsForMergeRadius(const USpatialAudioComponent& Component, float Span);
	static FVector BisectListenerLoS(const USpatialAudioComponent& Component, const UWorld* World,
	                                 const FVector& ListenerPos,
	                                 const FVector& BlockedEnd, const FVector& ClearEnd, bool& bOutFoundClear,
	                                 int32 ExplicitSteps = 0);
	static bool TryPromoteToInnerAnchor(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                    const UWorld* World,
	                                    const FVector& ListenerPos, bool bAllowSubSegmentRefine);
	static bool TryJumpToPreviousVertex(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                    const UWorld* World, const FVector& ListenerPos,
	                                    const FVector& InnerAnchor);
	static bool TryRefineAlongFinalSegment(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                       const UWorld* World,
	                                       const FVector& ListenerPos, const FVector& InnerAnchor);
	static void TickRelayMaintenance(USpatialAudioComponent& Component, FCachedEdgePoint& Edge, UWorld* World,
	                                 const FVector& SourcePos, const FVector& ListenerPos, bool bIntervalFired);
	static bool ReadRelayCheckTraces(FCachedEdgePoint& Edge, UWorld* World, FTraceDatum (&OutData)[4]);
	static void SubmitRelayCheckTraces(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge,
	                                   UWorld* World, const FVector& ListenerPos);
	static void ConvertRelayToEdge(const USpatialAudioComponent& Component, FCachedEdgePoint& Edge, const UWorld* World,
	                               const FVector& ListenerPos);
	static TArray<FVector> ResolveRecheckPath(const FCachedEdgePoint& Edge);
	static int32 FindFirstBlockedSegment(const TArray<FTraceDatum>& Data);
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
	static bool IsMergeCandidate(const FCachedEdgePoint& Edge);
	static bool TravelledFurther(const FCachedEdgePoint& Edge, const FCachedEdgePoint& Other);
};
