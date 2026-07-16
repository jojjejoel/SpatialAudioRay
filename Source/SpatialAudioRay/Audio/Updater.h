#pragma once

#include "CoreMinimal.h"

class USpatialAudioComponent;
class USpatialAudioSettings;
class UWorld;
struct FEdgeCluster;

class FUpdater {
public:
	/** Offset-LoS sampling, fraction smoothing and the occlusion target derived from it.
	 *  Called every frame — including while a full cast is in flight, where the update cast
	 *  doesn't run — so occlusion keeps draining instead of stalling for the sweep duration. */
	static void TickDirectLoSSampling(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);
	static void PerformUpdateRayCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);
	static void UpdateAudioParameters(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings);
	static void PerformLoSBreakSweep(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings);

	// If the listener->candidate trace is blocked, clamps the offset point to the hit location
	// (nudged back toward the listener so the next trace doesn't start inside geometry) instead
	// of discarding it.
	static FVector ResolveOffsetPoint(USpatialAudioComponent& Component, UWorld* World,
	                                  const FVector& ListenerPos, const FVector& CandidatePoint);

	/** Synchronous source-visibility fraction (0, 0.2 … 1) over 5 samples: listener center plus a
	 *  4-point ring of radius OffsetR around it, each paired with a same-world-direction point at
	 *  lateral radius SourceRingR around the source, lifted toward the listener onto the SourceR
	 *  sphere (source extent — seeing any surface of the full-volume sphere counts as seeing the
	 *  source; samples inside it are clear without tracing). Head-on the source targets read as a
	 *  disc of radius SourceR; from the side, its listener-facing cap. Rotates RingStepRad per
	 *  call (pattern repeats every 90°/RingStepRad calls). Both ring radii <= 0 runs the center
	 *  trace only (fraction 0 or 1); SourceR 0 = point source (traces reach the exact center). */
	static float SyncOffsetLoSFraction(USpatialAudioComponent& Component, UWorld* World,
	                                   const FVector& SourcePos, const FVector& ListenerPos,
	                                   float OffsetR, float SourceR, float SourceRingR,
	                                   float RingStepRad);

private:
	static void UpdateDualModeAudio(USpatialAudioComponent& Component, float DeltaTime, const USpatialAudioSettings& Settings,
	                                float CurvedOcclusion);

	/** Matches the desired voice set (edge clusters, or one aggregate voice when none exist)
	 *  to the active voices: within-glide-range matches keep their slot and glide; everything
	 *  else fades out in place while a replacement fades in on a fresh slot. */
	static void SyncVirtualVoicesToClusters(USpatialAudioComponent& Component,
	                                        const TArray<FEdgeCluster>& Clusters,
	                                        const USpatialAudioSettings& Settings);

};
