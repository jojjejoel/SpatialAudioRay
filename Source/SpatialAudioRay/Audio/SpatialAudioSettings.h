// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpatialAudioSettings.generated.h"

/**
 * Shared, designer-tunable settings for USpatialAudioComponent. Create one asset in the Content
 * Browser and assign it to every component instance, so the whole project is tuned from one place.
 * If no asset is assigned, the component falls back to the class defaults (CDO).
 */
UCLASS(BlueprintType)
class SPATIALAUDIORAY_API USpatialAudioSettings : public UDataAsset {
	GENERATED_BODY()

public:
	/** How diffusely rays scatter on a surface. 0 = perfect mirror, 1 = fully random hemisphere.
	 *  Diffuse scattering helps rays find the listener around corners and angled walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SurfaceRoughness = 0.7f;

	/** How strongly a bounce direction is nudged toward the listener after roughness scatter.
	 *  0.15-0.3 leans rays toward diffraction edges without collapsing them onto one path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BounceListenerBias = 0.f;

	/** How strongly the crawl direction is pulled toward the listener. 0 = the ray's own slide
	 *  direction, 1 = straight at the listener projected onto the wall plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CrawlListenerBias = 0.5f;

	/** Distance (cm) relaunched ray origins, crawl paths and edge points are lifted off the surface
	 *  they interact with. Must stay small: larger values bury points inside neighbouring geometry
	 *  at corners and thin walls, which reads as LoS through walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float RaySurfaceBias = 1.f;

	/** Maximum distance (cm) a ray may travel in one direction before changing course. Airborne
	 *  misses turn mid-air instead of terminating and crawls bounce off the wall, each consuming a
	 *  bounce. 0 = off: misses terminate and crawl range falls back to the full ray range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0"))
	float MaxStraightFlightDistance = 0.f;

	/** Step size (cm) for every diffraction sample: line-of-sight points along a flight segment,
	 *  and the steps a surface crawl takes along a wall. Counts are derived from this and the
	 *  distance available, capped by Math::MaxDiffractionSamples. Smaller finds narrower openings
	 *  and locates edges more precisely, at three traces per crawl step. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "5.0", ClampMax = "1000.0"))
	float DiffractionEdgeSampleStep = 30.f;

	/** Seconds of movement to lead ray steering by, so sweep results are less stale by the time
	 *  they are consumed. Within this long of losing LoS, steering aims at the PAST position
	 *  instead. Steering only: every probe and gate still verifies against real positions. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float SteeringPredictionLeadTime = 0.f;


	/** Number of rays fired in the full async sweep (Fibonacci sphere distribution). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget")
	int32 FullSweepRayCount = 64;

	/** Minimum full-sweep ray count when distance scaling is active. Set at or above
	 *  FullSweepRayCount to disable ray scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget",
		meta = (ClampMin = "4"))
	int32 MinFullSweepRayCount = 16;

	/** Maximum number of wall bounces a ray can take before being discarded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget",
		meta = (ClampMin = "0", ClampMax = "16"))
	int32 MaxBounces = 4;

	/** Minimum bounces at maximum distance. Scales linearly from MaxBounces; set equal to it to
	 *  disable bounce scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget",
		meta = (ClampMin = "0", ClampMax = "16"))
	int32 MinMaxBounces = 1;

	/** Multiplier on MaxRayDistance setting the total travel budget across all bounces. Each
	 *  individual segment is still capped to MaxRayDistance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget",
		meta = (ClampMin = "0.0"))
	float TotalPathBudgetMultiplier = 1.5f;

	/** Shape of the distance priority falloff, which scales ray count, bounce count and the sweep
	 *  interval together. 1 = linear; higher holds priority longer then drops steeply near max
	 *  distance, sparing medium-range sources. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget",
		meta = (ClampMin = "0.5", ClampMax = "8.0"))
	float DistancePriorityExponent = 2.0f;

	/** Ray-count multiplier once the edge cache is full, interpolated by how full it is. A saturated
	 *  cache can only improve by displacement, so searching into one is worth less. Floors at one
	 *  ray, not at MinFullSweepRayCount, which is the distance-scaling floor. 1 = no reduction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Budget",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FullCacheRayScale = 1.f;


	/** Seconds between full async sweeps at closest range. Everything else in this category
	 *  modulates it at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.05"))
	float FullSweepInterval = 0.5f;

	/** Seconds between full async sweeps at maximum distance. Scales linearly from
	 *  FullSweepInterval; set equal to it to disable interval scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.05"))
	float MaxFullSweepInterval = 2.0f;

	/** Combined source + listener speed (cm/s) at which velocity interval scaling reaches full
	 *  effect. Typical single-mover values: walk 200, jog 350, sprint 600. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.0"))
	float VelocityScaleMaxSpeed = 400.f;

	/** Smallest factor the sweep interval is ever multiplied by, reached at VelocityScaleMaxSpeed
	 *  while moving and applied whole while a post-movement cache fill is outstanding. The two
	 *  states cannot overlap: a cache fill only runs once both ends are stationary. 0.5 = sweeps
	 *  fire twice as often. 1.0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float MinSweepIntervalScale = 0.5f;

	/** Factor on the sweep interval once a sweep has completed with both ends stationary. Takes
	 *  precedence over an outstanding cache fill. 20 = sweeps fire 20x less often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float StationaryIdleMultiplier = 20.0f;

	/** Distance (cm) either end must move from where idle mode was entered to resume normal pacing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "1.0", ClampMax = "200.0"))
	float StationaryIdleBreakDist = 25.0f;

	/** Distance (cm) the listener must move since the last sweep to request an early one, bypassing
	 *  the interval timer. 0 = disable movement-triggered sweeps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float MovementSweepTriggerDist = 200.f;

	/** Minimum seconds between movement-triggered sweeps. Does not gate interval-triggered sweeps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float MovementSweepCooldown = 0.3f;

	/** After a movement-triggered sweep, keep sweeping at MinSweepIntervalScale speed for up to this
	 *  many completed sweeps, or until a new edge is cached. Edges re-confirmed at the same spot and
	 *  relays do not count: the burst exists to survey the NEW position. Covers arriving occluded
	 *  somewhere new, where velocity scaling has already stopped. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0", ClampMax = "20"))
	int32 MovementCacheFillMaxSweeps = 0;

	/** Smoothed occlusion at which sweeps start pre-warming the edge cache while the source is
	 *  still partly visible, so the voices start from a populated cache at full occlusion. The
	 *  5-sample ring quantizes to 0/80/100%, so keep this at or below 0.8. 1 = disabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Sweep Scheduling",
		meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float PreSweepOcclusionThreshold = 0.75f;


	/** Radius (cm) of the ring sample points around the listener for the direct LoS check. Larger
	 *  values widen the occlusion transition zone in listener-travel terms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Line Of Sight Sampling",
		meta = (ClampMin = "0.0"))
	float DirectLoSSampleRadius = 50.f;

	/** Scales the attenuation inner radius into the LoS target sphere around the source, so seeing
	 *  any of its surface counts as seeing the source. Models source extent at no extra trace cost.
	 *  1 = sphere at the inner radius, 0 = point source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Line Of Sight Sampling",
		meta = (ClampMin = "0.0"))
	float SourceLoSSampleRadiusScale = 1.f;

	/** Checks per full ring rotation (step = 90 degrees / N). The fraction averages over exactly one
	 *  pattern, so a stationary scene yields a constant value. 1 = fixed ring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Line Of Sight Sampling",
		meta = (ClampMin = "1", ClampMax = "8"))
	int32 OffsetRingRotationSteps = 4;

	/** Exponent shaping the annulus radius ladder: check k samples at (k/RotationSteps)^this x full
	 *  radius. 0.5 = equal-area annuli, 1 = evenly spaced radii, 0 = rim only. Costs nothing: only
	 *  the sample radii move. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Line Of Sight Sampling",
		meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float OffsetRingRadiusExponent = 0.5f;

	/** Seconds between direct LoS samples. Scaled down with speed by OffsetLoSVelocityScale.
	 *  0 = every frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Line Of Sight Sampling",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OffsetLoSCheckInterval = 0.f;

	/** Minimum multiplier on OffsetLoSCheckInterval at VelocityScaleMaxSpeed. Separate from
	 *  MinSweepIntervalScale, so sampling and sweeps can accelerate by different amounts.
	 *  0.25 = 4x as often at full speed. 1.0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Line Of Sight Sampling",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float OffsetLoSVelocityScale = 0.25f;


	/** MetaSound float parameter that receives the occlusion value (0 = open, 1 = fully blocked). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion")
	FName OcclusionParamName = "Occlusion";

	/** Shape of the occlusion curve applied before driving audio effects. 1 = linear; higher pushes
	 *  the audible effect later in the range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float OcclusionCurveExponent = 1.5f;

	/** Seconds for occlusion to follow its target, in either direction. 0 = instant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float OcclusionBlendTime = 0.25f;

	/** Seconds direct LoS must hold before the virtual source snaps back and the edge cache clears.
	 *  Debounces single-frame flickers at a shadow boundary. Occlusion itself still reacts
	 *  immediately. 0 = no debounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DirectLoSConfirmTime = 0.1f;


	/** Maximum number of edge points cached simultaneously. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "1", ClampMax = "32"))
	int32 CachedEdgeMaxCount = 4;

	/** Distance (cm) within which two diffraction points count as the same corner and consolidate
	 *  into one entry. Applies both to sweep arrivals and to entries that drift together. Whichever
	 *  route travelled least survives. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "5.0", ClampMax = "200.0"))
	float CachedEdgeMergeRadius = 25.f;

	/** Seconds a cached edge fades out after losing listener line-of-sight, still contributing at a
	 *  decaying weight so the virtual source does not jump. A clear check during the fade cancels
	 *  it. 0 = instant removal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float CachedEdgeEvictionFadeTime = 0.3f;

	/** Seconds between checks on one cached edge, PER CACHED EDGE: the interval is divided by cache
	 *  size and one entry is taken per slice, so cost spreads instead of arriving as a burst of N.
	 *  Scales down with listener speed. Drives all three passes, which each take their own turn:
	 *  the listener-to-edge trace, the source-side polyline recheck, and the promotion step that
	 *  walks an edge toward its true corner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.01", ClampMax = "5.0"))
	float CachedEdgeCheckInterval = 0.1f;

	/** Binary-search steps the promotion spends locating the corner on the final polyline segment.
	 *  Each costs 2 traces and halves the bracket. 0 = derive the count from the segment, stopping
	 *  inside half CachedEdgeMergeRadius so two entries on one corner still merge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0", ClampMax = "12"))
	int32 ShortestPathPromotionBisectSteps = 0;


	/** Maximum simultaneously audible virtual voices. The component pool is 2x this so a voice
	 *  fading out during a handoff can coexist with its replacement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Voices",
		meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxVirtualVoices = 3;

	/** Maximum distance (cm) a voice may move in one update while keeping its audio component.
	 *  Beyond it the old voice fades out where it stands and a new one fades in at the new
	 *  position, over VirtualVoiceHandoffFadeTime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Voices",
		meta = (ClampMin = "0.0", ClampMax = "5000.0"))
	float VirtualVoiceMaxMoveDistance = 150.f;

	/** Seconds for a voice's fade envelope on position handoffs and appear/disappear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Voices",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualVoiceHandoffFadeTime = 0.25f;

	/** How strongly path distance reduces a candidate's contribution to the virtual source
	 *  position. 0 = all candidates weighted equally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Voices",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float CandidateDistanceFalloff = 1.f;

	/** How strongly listener-to-edge distance reduces a cached edge's priority when placing the
	 *  virtual source and choosing which clusters win voice slots. Position and selection only,
	 *  deliberately out of every gain formula. 0 = source-side weighting only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Voices",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ListenerDistanceFalloff = 0.f;

	/** Per-bounce multiplier on a ray's contribution to the virtual source position, applied as
	 *  falloff^bounces. 0.5 = each extra bounce halves the influence. 1 = off, only path distance
	 *  matters. Above 1 favours the more indirect routes instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Voices",
		meta = (ClampMin = "0.05", ClampMax = "10.0"))
	float BounceCountFalloff = 1.f;


	/** Smoothed occlusion at which the virtual voices begin fading in, reaching full level at total
	 *  occlusion, letting diffracted sound bleed in through pinhole states. Set at or above
	 *  PreSweepOcclusionThreshold so the cache is warm first. 1 = hard gate at total occlusion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VirtualCrossfadeStartOcclusion = 1.f;

	/** Seconds of smoothing on the occlusion-keyed fade-in ramp. The ramp amplifies occlusion
	 *  sampling wobble by 1/(1-Start), so this keeps that out of the audible gain. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualCrossfadeSmoothingTime = 0.35f;

	/** Seconds for the virtual crossfade gate to ramp between silent and fully engaged, in either
	 *  direction. 0 = instant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualCrossfadeFadeTime = 0.15f;

	/** How strongly diffracted path distance attenuates VirtualGain. 0 = no effect, 1 = fully
	 *  attenuated when the path equals MaxRayDistance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float PathAttenuationStrength = 0.3f;

	/** Blends the travelled path distance toward the straight line before computing
	 *  PathAttenuation. A hedge for geometry where the sweep caches a longer substitute for the
	 *  true shortest path; it softens genuine long detours too. 0 = pure travelled distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PathAttenuationGeomBlend = 0.f;

	/** Seconds for the path attenuation value to follow its target. 0 = instant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float PathAttenuationBlendTime = 0.2f;

	/** Path excess ratio (travelled/straight-line - 1) at which VirtualPathBend saturates at 1. The
	 *  single saturation point for everything the MetaSound derives from bend. Large values mute
	 *  the detour term, leaving VirtualPathBendDistanceStrength as the sole driver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.01", ClampMax = "100.0"))
	float VirtualPathBendFullExcess = 1.f;

	/** Adds a source-to-cluster distance term to VirtualPathBend, so far diffraction points sound
	 *  duller even on straight single-corner paths. 0 = detour ratio only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Sound",
		meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float VirtualPathBendDistanceStrength = 0.f;


	/** Caps how many sources draw at once before CycleDebugSourceKey has collapsed the set to a
	 *  single selection; only the closest to the listener draw. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	int32 MaxUncycledDebugSources = 0;

	/** How long debug lines and spheres persist. Matching the sweep interval avoids clutter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (ClampMin = "0.01"))
	float DebugLineDuration = 0.5f;
};
