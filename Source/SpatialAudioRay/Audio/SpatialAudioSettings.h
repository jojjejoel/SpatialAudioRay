// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpatialAudioSettings.generated.h"

/**
 * Shared, designer-tunable settings for USpatialAudioComponent.
 * Create one asset in the Content Browser (right-click → Miscellaneous → Data Asset →
 * SpatialAudioSettings) and assign it to every component instance.
 * Changing a value in the asset updates all components simultaneously.
 *
 * If no asset is assigned, the component falls back to the class defaults (CDO),
 * which carry the same values as the old inline defaults.
 */
UCLASS(BlueprintType)
class SPATIALAUDIORAY_API USpatialAudioSettings : public UDataAsset {
	GENERATED_BODY()

public:

	// ── Ray Casting ───────────────────────────────────────────────────────────
	// Core ray budget: how many rays, how far they travel, and how many bounces.

	/** Number of rays fired in the full async sweep (Fibonacci sphere distribution). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting")
	int32 FullSweepRayCount = 64;

	/**
	 * Divide the full sweep ray count across this many consecutive async casts.
	 * 1 = all rays fire in one cast (default). 2 = half per cast. 4 = quarter per cast.
	 * Each cast fires a strided slice of the Fibonacci sphere so spatial coverage
	 * stays even within each sub-sweep and the full sphere is covered once per cycle.
	 * Useful for spreading async trace cost across more frames at the expense of a
	 * proportionally longer total sweep latency (CycleCount × FullSweepInterval).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "1", ClampMax = "8"))
	int32 FullSweepCycleCount = 1;

	/** Maximum number of wall bounces a ray can take before being discarded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0", ClampMax = "16"))
	int32 MaxBounces = 4;

	/**
	 * Distance (cm) relaunched ray origins, crawl paths and edge points are lifted off the
	 * surface they interact with. Must stay small: it exists only to stop floating-point
	 * self-hits at the launch point — larger values bury points inside neighboring geometry
	 * at corners and thin walls, which reads as LoS through walls.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float RaySurfaceBias = 1.f;

	/**
	 * Seconds between full async sweeps at closest range (highest priority).
	 * Lower = more responsive, higher = cheaper. See Performance for distance- and
	 * velocity-based scaling that modulates this interval at runtime.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.05"))
	float FullSweepInterval = 0.5f;

	/**
	 * Scales the ray range, which is otherwise the audible range of the widest-range source's
	 * attenuation (inner extent + falloff), read at BeginPlay. That range is the right default
	 * because a diffraction path found beyond it belongs to a sound already attenuated to
	 * silence. Below 1 trades far-field paths for trace budget; above 1 searches past the
	 * audible range, which only pays off when a long detour still lands inside it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.1", ClampMax = "4.0"))
	float MaxRayDistanceScale = 1.f;

	/**
	 * Multiplier on MaxRayDistance that sets the total cumulative travel budget across all bounces.
	 * 2.0 = a ray may travel up to 2× the base distance total before being discarded.
	 * Each individual segment is still capped to MaxRayDistance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0"))
	float TotalPathBudgetMultiplier = 1.5f;

	/**
	 * Scales the maximum length of the first ray segment and the terminal segment.
	 * Intermediate bounce segments are always capped to MaxRayDistance.
	 * 1.0 = full length. 0.5 = half length, forcing more bounces at closer range.
	 * Does not affect the total path budget.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float RayLengthMultiplier = 1.0f;

	/**
	 * Maximum distance (cm) a ray may travel in one direction before changing course.
	 * Airborne segments that fly this far without hitting anything or gaining LoS turn
	 * mid-air (roughness scatter + listener bias; with both at 0, a deterministic
	 * perpendicular turn seeded from the turn point) instead of terminating; surface
	 * crawls are capped to this distance and bounce off the wall when they reach it
	 * without finding an edge. Every turn consumes a bounce from MaxBounces.
	 * 0 = disabled: airborne misses terminate the ray, crawl range is governed solely by
	 * MaxCrawlSteps x CrawlStepSize.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0"))
	float MaxStraightFlightDistance = 0.f;

	/**
	 * How diffusely rays scatter when they hit a surface.
	 * 0 = perfect mirror reflection. 1 = fully random hemisphere (diffuse).
	 * Diffuse scattering helps rays find the listener around corners and angled walls.
	 * Noise from stochastic bounces averages out across casts.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SurfaceRoughness = 0.7f;

	/**
	 * How strongly each ray's bounce direction is nudged toward the listener after roughness
	 * scatter is applied.
	 * 0 = no bias (pure reflection + roughness). 1 = fully toward the listener.
	 * 0.15–0.3 gives a gentle lean — keeps momentum from the reflected direction while
	 * steering rays toward the listener side, helping them find diffraction edges sooner
	 * without collapsing all rays to the same path.
	 * Applied after the lateral-band bias on the launch direction, so both shape the same ray.
	 * If the blended direction would go through the surface, it is re-projected onto the
	 * hemisphere above the hit normal.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BounceListenerBias = 0.f;

	/**
	 * Seconds of movement to lead ray STEERING by: the sweep's aiming axis and every
	 * listener-bias pull (bounce, crawl, mid-air turn) target where the listener and source
	 * will be after this long at their current smoothed velocity, so sweep results are less
	 * stale by the time they are consumed (they serve until the next sweep). Exception: for
	 * this long after direct LoS is lost (including pre-sweep-band casts and the LoS-break
	 * sweep), steering aims at the position this long in the PAST instead — the corner just
	 * crossed sits behind, and leading forward would aim into the shadow. Steering only —
	 * LoS probes, budget gates, occlusion sampling, and the edge cache always verify against
	 * the real positions, so a wrong prediction can never cache anything incorrect, it only
	 * aims some rays less well. Decays to no effect as movement stops. 0 = off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Ray Casting",
		meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float SteeringPredictionLeadTime = 0.f;


	// ── Performance ───────────────────────────────────────────────────────────
	// Adaptive behaviors that trade sweep frequency, ray count, and exploration
	// breadth for lower CPU cost.

	// ─ Sweep rate ─────────────────────────────────────────────────────────────
	// Controls how often full sweeps fire. All of these modulate FullSweepInterval
	// up or down at runtime: distance stretches it, velocity and geometry-change
	// compress it, and stationary idle stretches it dramatically.

	/**
	 * Seconds between full async sweeps at maximum distance (zero priority).
	 * Scales linearly from FullSweepInterval (close) to this value (far).
	 * Set equal to FullSweepInterval to disable interval scaling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.05"))
	float MaxFullSweepInterval = 2.0f;

	/**
	 * Combined speed (cm/s) at which velocity-based interval scaling reaches its maximum effect.
	 * Source speed and listener speed are added together, so both moving simultaneously
	 * contributes more than only one moving fast.
	 * All intervals (sweep, edge validation, offset LoS, movement-sweep cooldown) are
	 * multiplied by VelocityIntervalScale when the combined speed reaches this value.
	 * Typical single-mover values: walk ~200, jog ~350, sprint ~600.
	 * Set to ~400 so two moderate walkers (~200 each) also reach full scaling.
	 * 0 = disable velocity-based interval scaling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.0"))
	float VelocityScaleMaxSpeed = 400.f;

	/**
	 * Minimum interval multiplier applied when speed reaches VelocityScaleMaxSpeed.
	 * 0.5 = intervals shrink to half (sweeps fire twice as often) at full speed.
	 * 1.0 = disable velocity scaling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float VelocityIntervalScale = 0.5f;

	/** Factor applied to the full sweep interval once a full sweep has completed while both
	 *  source and listener were stationary and neither has moved significantly since.
	 *  Applied instead of GeometryChangeBurstMultiplier in this deeper idle state.
	 *  Burst mode still overrides this. 20 = sweeps fire 20× less often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float StationaryIdleMultiplier = 20.0f;

	/** Distance (cm) either source or listener must move from their positions when stationary
	 *  idle mode was entered to break out of idle and resume normal sweep pacing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "1.0", ClampMax = "200.0"))
	float StationaryIdleBreakDist = 25.0f;

	/** Distance in cm the listener must move since the last sweep to request an early new sweep,
	 *  bypassing the FullSweepInterval timer. Helps discover better edges quickly after the
	 *  player moves to a new area. 0 = disable movement-triggered sweeps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float MovementSweepTriggerDist = 200.f;

	/** Minimum seconds between movement-triggered sweeps. Prevents continuous sweep thrashing
	 *  while the player is running. Does not gate interval-triggered sweeps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float MovementSweepCooldown = 0.3f;

	/** After a movement-triggered sweep, keep sweeping at GeometryChangeBurstMultiplier speed for
	 *  up to this many completed full sweeps or until MovementCacheFillRequiredEdges NEW edges
	 *  (discovered since the trigger — carried-over entries don't count) are cached, whichever
	 *  comes first. Velocity scaling stops accelerating sweeps the moment movement stops —
	 *  without this, arriving occluded in a new spot can wait out the slow steady-state interval
	 *  without re-surveying. The sweep cap bounds the cost when nothing new exists to find. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0", ClampMax = "20"))
	int32 MovementCacheFillMaxSweeps = 0;

	/** Newly-discovered (since the movement trigger; non-relayed, non-evicting) cached edge
	 *  points that end the cache-fill fast sweeping early. Pre-existing edges re-confirmed at
	 *  the same spot don't count, and neither do relays — the point of the burst is discovering
	 *  edges for the NEW position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "1", ClampMax = "16"))
	int32 MovementCacheFillRequiredEdges = 1;

	/** Seconds to run in burst mode after detecting a geometry change (a previously-missed
	 *  direction now finds LoS, or a cached edge is evicted by chain-validation failure
	 *  while stationary). During burst the sweep interval is multiplied by
	 *  GeometryChangeBurstMultiplier to re-survey the scene quickly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float GeometryChangeBurstDuration = 3.0f;

	/** Multiplier applied to the full sweep interval during a geometry-change burst.
	 *  Values < 1 produce faster sweeps. 0.25 = sweeps fire 4× more often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float GeometryChangeBurstMultiplier = 0.25f;


	// ─ Ray budget ─────────────────────────────────────────────────────────────
	// Scales ray count and bounce depth with listener distance. Distant sources
	// get fewer rays and shallower bounces, saving async trace cost when detail
	// matters less.
	/**
	 * Shape of the distance priority falloff curve.
	 * 1.0 = linear. >1 = priority holds higher for longer then drops steeply near max distance
	 * (recommended — saves cost for truly distant sources without penalising medium-range ones).
	 * 2.0 = quadratic, 3.0 = cubic.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0.5", ClampMax = "8.0"))
	float DistancePriorityExponent = 2.0f;

	/** Minimum full-sweep ray count when distance scaling is active. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "4"))
	int32 MinFullSweepRayCount = 16;

	/**
	 * Minimum number of bounces at maximum distance (zero priority).
	 * Scales linearly from MaxBounces (close) down to this value (far).
	 * Fewer bounces = fewer async trace frames = cheaper for distant sources.
	 * Set equal to MaxBounces to disable bounce scaling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Performance",
		meta = (ClampMin = "0", ClampMax = "16"))
	int32 MinMaxBounces = 1;


	// ── Surface Crawl ─────────────────────────────────────────────────────────
	// When a ray hits a wall, instead of scattering in a random reflected direction it can
	// crawl along the surface until it finds the geometric edge, then continue from there.
	// This directly finds diffraction edges rather than relying on random bounces to stumble
	// onto them. Only applied in the per-frame sync update cast.
	/**
	 * Maximum number of crawl steps taken along a wall surface to find its edge.
	 * Combined with CrawlStepSize this sets the maximum crawl range.
	 * If no edge is found within this many steps the ray falls back to random reflection.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Surface Crawl",
		meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxCrawlSteps = 12;

	/**
	 * Distance (cm) between consecutive crawl sample points along the wall surface.
	 * Smaller = more precise edge location but more traces per wall hit.
	 * 12 steps × 10 cm = 1.2 m maximum crawl range per wall hit.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Surface Crawl",
		meta = (ClampMin = "1.0", ClampMax = "1000.0"))
	float CrawlStepSize = 10.f;

	/**
	 * How strongly the crawl direction is pulled toward the listener.
	 * 0 = crawl purely in the ray's slide direction (along wall, momentum-only).
	 * 1 = crawl directly toward the listener (projected onto the wall plane).
	 * 0.5 = equal blend of both — biased toward the listener but following momentum.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Surface Crawl",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CrawlListenerBias = 0.5f;
	// ── Occlusion ─────────────────────────────────────────────────────────────
	// Controls how the blocked/muffled effect is calculated and applied to audio.

	/** Sound Cue / MetaSound float parameter name that receives the occlusion value (0 = open, 1 = fully blocked). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion")
	FName OcclusionParamName = "Occlusion";


	/**
	 * Seconds for occlusion to reach fully blocked (1.0) when no rays reach the listener at all.
	 * Zero rays = definitively blocked, so this can be short.
	 * 0 = instant, 0.125 = eighth of a second, 1 = one second.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float OcclusionFullBlockTime = 0.125f;

	/**
	 * Seconds for occlusion to blend to its target when decreasing (below full block).
	 * 0 = instant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float OcclusionBlendTime = 0.25f;

	/**
	 * Time in seconds for occlusion to clear when direct line-of-sight is detected.
	 * Keep short so sound opens up quickly when rounding a corner. 0 = instant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0"))
	float OcclusionClearTime = 0.05f;

	/**
	 * Time in seconds for occlusion to rise when direct LoS is blocked.
	 * Applies whenever CurrentOcclusion is below TargetOcclusion and direct LoS is absent.
	 * Keep short so muffling kicks in quickly at LoS loss, letting the virtual source carry
	 * the sound naturally. 0 = instant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0"))
	float OcclusionAttackTime = 0.1f;

	/**
	 * Number of rays in the one-shot sync sweep fired when direct LoS is first blocked.
	 * Immediately seeds TargetVirtualSourceLocation so the virtual source starts at a
	 * plausible edge on the same frame as LoS loss. Fewer = cheaper; 0 = disabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0", ClampMax = "32"))
	int32 LoSBreakSweepRayCount = 8;

	/**
	 * Minimum seconds between successive LoS-break sweeps.
	 * Prevents repeated sync spikes when the listener oscillates at a shadow boundary.
	 * 0 = fire on every transition.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0"))
	float LoSBreakSweepCooldownTime = 0.3f;

	/**
	 * Seconds bHasDirectLoS must be continuously true before the virtual source snaps
	 * back toward the actor, edge caches are cleared, and the position drive switches its
	 * base anchor from the last known edge to the actor. Suppresses all those effects on
	 * single-frame LoS flickers at a shadow boundary, which would otherwise cause the
	 * virtual source to jerk briefly and the edge cache to be discarded unnecessarily.
	 * Occlusion itself still reacts immediately regardless of this value.
	 * 0 = react immediately (no debounce).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DirectLoSConfirmTime = 0.1f;

	/**
	 * Shape of the occlusion curve applied before driving audio effects.
	 * 1.0 = linear. 2.0 = quadratic (subtle at low occlusion, strong near full block).
	 * Higher values push the audible effect later in the 0–1 range.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float OcclusionCurveExponent = 1.5f;

	/**
	 * Radius (cm) of the ring sample points around the listener for the direct LoS check.
	 * Larger values widen the occlusion transition zone in listener-travel terms.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0"))
	float DirectLoSSampleRadius = 50.f;

	/**
	 * Scales the attenuation inner radius into the LoS target sphere around the SOURCE. Each
	 * listener sample pairs with a same-world-direction lateral offset around the source that is
	 * lifted toward the listener onto this sphere (the center pairs with the sphere point on the
	 * source↔listener line): head-on, the targets read as the usual sampling disc; from the
	 * side, they wrap the sphere's listener-facing cap. The source plays at full volume anywhere
	 * inside the inner radius, so seeing any of the sphere's surface counts as seeing the
	 * source, and a sample already inside it is clear without tracing. Models source extent at
	 * no extra trace cost (still 5 traces/check; cap points are computed, not resolve-traced).
	 * 1 = sphere at the inner radius, 0 = point source (traces reach the exact center).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0"))
	float SourceLoSSampleRadiusScale = 1.f;

	/** How often (in seconds) to run the direct LoS sample (center + ring, 5 sync traces plus 4
	 *  listener-point resolve traces). 0 = every frame. Scaled down with speed by
	 *  OffsetLoSVelocityScale, so stationary scenes check less often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OffsetLoSCheckInterval = 0.f;

	/**
	 * Smoothed occlusion level at which full sweeps start pre-warming the edge cache while the
	 * source is still partially visible, so the virtual voices start from a populated cache the
	 * moment occlusion reaches 100% instead of a cold cache. The 5-sample ring quantizes steady
	 * occlusion to 0% / 80% / 100%, so keep this at or below 0.8 to catch the pinhole-LoS state.
	 * Virtual audibility is unaffected — the crossfade gate still opens only at full occlusion.
	 * 1.0 = disabled (sweeps require full LoS loss, original behavior).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float PreSweepOcclusionThreshold = 0.75f;

	/**
	 * Minimum multiplier on OffsetLoSCheckInterval when the combined source+listener speed
	 * reaches VelocityScaleMaxSpeed. Separate from the shared VelocityIntervalScale so the
	 * cheap LoS sampling (and with it occlusion response) can accelerate aggressively while
	 * moving without also making the expensive sweeps and edge validation fire faster.
	 * 0.25 = checks fire 4x as often at full speed. 1.0 = no velocity scaling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float OffsetLoSVelocityScale = 0.25f;

	/** How many checks it takes the ring to complete one rotation pattern (step = 90°/N).
	 *  The occlusion fraction averages over exactly one full pattern, so a stationary scene
	 *  yields an exactly constant value (no sampling wobble) while covering 4×N distinct
	 *  directions. Higher = finer angular resolution but slower response; 1 = fixed ring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "1", ClampMax = "8"))
	int32 OffsetRingRotationSteps = 4;

	/** Exponent shaping the annulus radius ladder: check k of a rotation cycle samples its ring
	 *  at (k/RotationSteps)^this × full radius. 0.5 = equal-area annuli — the cycle average
	 *  estimates visible disc AREA, but the radii crowd toward the rim (4 steps sample at
	 *  50/71/87/100%). 1 = evenly spaced radii (25/50/75/100%), weighting the centre view more;
	 *  higher biases further inward; 0 = rim-only (pre-annulus behavior). Costs nothing — the
	 *  trace count and cycle length are unchanged, only the sample radii move. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float OffsetRingRadiusExponent = 0.5f;

	/** Time constant (seconds) for smoothing the pattern-averaged LoS fraction that drives
	 *  occlusion, softening the 1/(4×RotationSteps+…) quantization steps into a continuous
	 *  gradient. 0 = raw pattern average. Gating (sweep suppression, edge cache) always uses
	 *  the raw instant sample. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float LoSFractionSmoothingTime = 0.25f;


	// ── Path Attenuation ──────────────────────────────────────────────────────
	// Reduces VirtualGain based on total diffracted path distance — close sounds attenuate
	// very little regardless of corner geometry; far sounds attenuate more.

	/**
	 * How strongly total diffracted path distance attenuates VirtualGain.
	 * PathAttenuation = TotalPath / MaxRayDistance * this.
	 * 0 = no effect. 1 = fully attenuated when total path equals MaxRayDistance.
	 * Close sources (short total path) are barely affected; far sources scale linearly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Path Attenuation",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float PathAttenuationStrength = 0.3f;

	/**
	 * Blends the traveled (bent/crawled) path distance toward the straight-line source→virtual
	 * distance before computing PathAttenuation. 0 = pure traveled distance (default, matches
	 * old behavior). 1 = pure straight-line distance.
	 * A hedge, not a correctness improvement: in genuinely complex geometry the ray sweep can
	 * fail to find the true shortest path and instead cache a longer substitute, which this
	 * pulls back toward a more natural attenuation; the same blend also softens genuinely long,
	 * correctly-found detours, which is the deliberate trade-off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Path Attenuation",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PathAttenuationGeomBlend = 0.f;

	/**
	 * Seconds for the path attenuation value to follow its target.
	 * 0 = instant. Roughly how long it takes to reach a new value after the path changes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Path Attenuation",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float PathAttenuationBlendTime = 0.2f;

	// ── Virtual Source ────────────────────────────────────────────────────────
	// Controls where the diffraction point is placed and how the audio component moves toward it.
	/**
	 * Seconds for the AudioComponent to follow its blended target position.
	 * The target is the lerp between actor position and the virtual source,
	 * weighted by the blend weight. Smooths out position jumps.
	 * 0 = instant, 0.125 = eighth of a second, 1 = roughly one second.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AudioSourceMoveTime = 0.125f;

	/**
	 * How far behind each cached edge point the virtual emitter sits, in cm, walked back along
	 * that edge's string-pulled arrival path toward the source (verified segments only; clamps
	 * where the path runs out). An absolute distance keeps the emitter the same depth behind
	 * the opening regardless of how far away the source is, and the point always lies on the
	 * acoustic path in free space. 0 = emitter exactly at the edge point.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "10000.0"))
	float VirtualSourcePullbackDistance = 0.f;

	/**
	 * Seconds for the virtual source (cyan sphere) to follow its target.
	 * 0 = instant, 0.25 = quarter second, 1 = roughly one second.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float VirtualSourceMoveTime = 0.25f;

	/**
	 * Seconds for the virtual source to snap back to the actual source when direct
	 * line-of-sight is detected. 0 = instant snap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float VirtualSourceSnapTime = 0.05f;

	/**
	 * Seconds for VirtualGain's crossfade gate to ramp from silent to fully engaged once every
	 * offset point loses line-of-sight (fully occluded). 0 = instant. Smooths the on/off
	 * transition into a fast ramp instead of a single-frame volume jump.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualCrossfadeFadeInTime = 0.15f;

	/**
	 * Seconds for VirtualGain's crossfade gate to ramp back down to silent once any offset
	 * point regains line-of-sight. 0 = instant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualCrossfadeFadeOutTime = 0.15f;

	/**
	 * Smoothed-occlusion level at which the virtual voices begin fading in, reaching full level
	 * at total occlusion — instead of gating open only when every offset point has lost
	 * line-of-sight. Lets the diffracted sound bleed in through pinhole/corner-grazing states.
	 * Set at or above PreSweepOcclusionThreshold so the edge cache is already pre-warmed (and
	 * the voices positioned) before anything becomes audible. 1 = disabled (hard gate at total
	 * occlusion only).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VirtualCrossfadeStartOcclusion = 1.f;

	/**
	 * Seconds of exponential smoothing on the occlusion-keyed fade-in ramp. The ramp maps the
	 * [VirtualCrossfadeStartOcclusion, 1] band onto [0, 1], which amplifies any residual
	 * occlusion-sampling wobble by 1/(1-Start) — this low-pass keeps that out of the audible
	 * virtual gain without slowing the source's own occlusion response. 0 = off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualCrossfadeSmoothingTime = 0.35f;

	/**
	 * Path excess ratio (traveled/straight-line − 1) at which VirtualPathBend saturates at 1.
	 * 1 = full bend when the diffraction path is twice the straight-line distance; 0.5 = full
	 * bend already at 1.5x. The single saturation point for everything the MetaSound derives
	 * from VirtualPathBend (LPF, HPF, reverb) — keep the graph's bend input mapped 0→1.
	 * Large values (10+) effectively mute the detour-ratio term, leaving
	 * VirtualPathBendDistanceStrength as the sole bend driver (distance-only muffling).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.01", ClampMax = "100.0"))
	float VirtualPathBendFullExcess = 1.f;

	/**
	 * Adds a source→cluster distance term to VirtualPathBend so far-away diffraction points
	 * sound duller even on straight single-corner paths (air-absorption analog; uses the
	 * traveled source→edge path, same basis as PathAttenuation — listener-independent).
	 * Contribution = this × traveled distance / MaxRayDistance, added before the clamp:
	 * 1 = a path as long as MaxRayDistance alone reaches full bend. 0 = off (detour ratio only).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float VirtualPathBendDistanceStrength = 0.f;

	/**
	 * How strongly path distance reduces a diffraction candidate's contribution to the
	 * virtual source position. 0 = all candidates weighted equally. Higher values favour
	 * shorter (nearer) diffraction points more aggressively.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float CandidateDistanceFalloff = 1.f;

	/**
	 * How strongly listener→edge distance reduces a cached edge's priority when placing the
	 * virtual source and choosing which clusters win the voice slots. Position/selection only —
	 * deliberately kept out of every gain/muffling formula (PathDist averages, WeightShare),
	 * so walking around cannot change loudness; listener-proximity loudness comes from the
	 * engine's native attenuation on the moved emitters. 0 = source-side weighting only.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ListenerDistanceFalloff = 0.f;

	/**
	 * Maximum number of simultaneously audible virtual voices. Cached edge points cluster into
	 * up to this many groups, each played by its own spatialized audio component. The runtime
	 * component pool is 2x this so a voice fading out during a position handoff can coexist
	 * with its replacement.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxVirtualVoices = 3;

	/**
	 * Distance (cm) within which cached edge points group into a single cluster/voice. Clusters
	 * whose centroids end up closer than this are merged, so two voices never play near-co-located
	 * (which would just be duplicate DSP — summed loudness is already weight-normalized).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "10.0", ClampMax = "5000.0"))
	float VirtualVoiceClusterRadius = 250.f;

	/**
	 * Maximum distance (cm) a voice's target position may move per update while still gliding the
	 * same audio component. Larger jumps fade the old voice out in place and fade a new voice in
	 * at the new position instead of audibly sweeping through space.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "5000.0"))
	float VirtualVoiceGlideMaxDistance = 150.f;

	/** Seconds for a voice's fade-in/fade-out envelope on position handoffs and appear/disappear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VirtualVoiceHandoffFadeTime = 0.25f;

	/**
	 * Step size (cm) between line-of-sight sample points along each ray segment.
	 * At each step the ray checks if that point has LoS to the listener, catching diffraction
	 * edges that rays pass by without bouncing. Smaller = more accurate, more traces per cast.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "5.0", ClampMax = "1000.0"))
	float DiffractionEdgeSampleStep = 30.f;

	/**
	 * Maximum number of sample positions tested per ray segment during LoS sampling.
	 * Each sample costs 2 traces (nudge + LoS); sampling stops as soon as one succeeds.
	 * 0 = no limit (original behaviour). Lower values cap worst-case trace cost for long
	 * wall segments but may miss edges that would only be found deeper along the segment.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0", ClampMax = "64"))
	int32 MaxSamplesPerSegment = 0;

	/**
	 * When enabled, each additional bounce a ray takes reduces its contribution to the
	 * virtual source position by BounceCountFalloff. When disabled, only path distance matters.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source")
	bool bWeightCandidatesByBounceCount = false;

	/**
	 * Per-bounce weight multiplier when bWeightCandidatesByBounceCount is enabled.
	 * 0.5 = each extra bounce halves the candidate's influence. 1.0 = no falloff.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Virtual Source",
		meta = (ClampMin = "0.05", ClampMax = "10.0", EditCondition = "bWeightCandidatesByBounceCount"))
	float BounceCountFalloff = 0.5f;


	// ── Edge Cache ────────────────────────────────────────────────────────────
	// Confirmed diffraction edges persist across sweeps and are re-validated cheaply
	// each frame. Valid cached points inject as confirmed results, reducing the ray
	// budget and anchoring the virtual source between full sweeps.
	// Direction exclusion and sweep-rate effects driven by the cache live in Performance.

	/** Maximum number of edge points cached simultaneously.
	 *  Each additional point costs 2 extra synchronous traces per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "1", ClampMax = "32"))
	int32 CachedEdgeMaxCount = 4;

	/** Distance in cm within which two diffraction points count as the same geometric corner and
	 *  are consolidated into one cache entry. Applies both to a new candidate arriving from a
	 *  sweep and to entries that have drifted together in the cache (relay→edge conversion and
	 *  inner-anchor promotion both move an admitted entry's point). Whichever route travelled
	 *  least to reach the corner survives: at one point the listener leg is shared, so path
	 *  length is all that separates them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "5.0", ClampMax = "200.0"))
	float CachedEdgeMergeRadius = 25.f;

	/**
	 * How long (seconds) a cached edge fades out after Phase 0 detects it has lost line-of-sight
	 * to the listener, before it is fully removed. During the fade the point still contributes
	 * to the virtual source position with a linearly decaying weight, preventing abrupt jumps
	 * when a valid replacement has not yet been found. If Phase 0 clears again during the fade
	 * (e.g. a moving character unblocks the path), the fade is cancelled and the point is restored.
	 * Set to 0 for the original instant-removal behaviour.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float CachedEdgeEvictionFadeTime = 0.3f;

	/** Minimum distance (cm) the SOURCE must have moved since a cached edge's path data was
	 *  captured before the edge is evicted as stale (its PathDist/shortest path were measured
	 *  from the old source position and nothing else revalidates the source side). Listener
	 *  movement never evicts: listener validity is handled by Phase 0 (LoS/offset fan/relay),
	 *  and entries are otherwise only displaced by better-ranking sweep finds.
	 *  0 = disable source-movement eviction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.0", ClampMax = "500.0"))
	float CachedEdgeUpdateMoveThreshold = 15.f;

	/**
	 * Seconds between Phase 0 (listener→edge LoS) submission batches.
	 * Phase 0 submits one async trace per cached edge to detect when the listener's
	 * view of an edge becomes blocked. At the default 0 all edges are checked every frame;
	 * raising this reduces steady-state trace cost at the expense of eviction latency.
	 * Scales down automatically with listener speed so checks stay frequent while moving.
	 * 0 = check every frame (original behaviour).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Phase0CheckInterval = 0.1f;

	/**
	 * Seconds between source-side path re-verifications. Each check re-traces every segment of
	 * ONE cached edge's stored string-pulled source→edge polyline (round-robin; traces are
	 * submitted async and evaluated the following tick), including
	 * unverified segments, to detect geometry that has closed the path since discovery — Phase 0
	 * only guards the listener side, source movement only guards source position, and rank
	 * hysteresis rejects the longer re-finds a closed path produces, so nothing else catches e.g.
	 * a door closing between a static source and its edge. A single blocked segment starts the
	 * normal eviction fade and requests a sweep immediately. Because unverified segments (a raw
	 * crawl/bounce hop the string pull couldn't shortcut past) were already blocked at discovery,
	 * this also evicts ordinary multi-corner diffraction paths the moment they're rechecked, not
	 * just genuine geometry changes — a deliberate trade-off of enabling this. 0 = off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float ShortestPathRecheckInterval = 0.f;

	/**
	 * Seconds between attempts to shrink a cached edge back toward the source even while it
	 * already has direct listener LoS — unlike TickPhase0Readback's promotion (which only fires
	 * as a rescue when the edge itself just went blocked), this runs opportunistically so an
	 * edge keeps migrating toward the true minimal diffraction point as the listener moves,
	 * rather than sitting wherever it was first discovered. Each check (round-robin, one edge)
	 * tries only the single point immediately before the edge on its own polyline; if that point
	 * has direct listener LoS too, the edge moves there — one step per interval, not a jump all
	 * the way to the source. 0 = off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float ShortestPathPromotionInterval = 0.f;

	/**
	 * Binary-search steps the promotion's sub-segment refinement spends locating the corner on
	 * the final polyline segment (the stage that runs when the previous vertex is blocked).
	 * Each step costs 2 traces and halves the remaining bracket, so the corner lands within
	 * segmentLength / 2^steps of its true position.
	 *
	 * 0 = derive the count from the segment, stopping once the bracket is inside half
	 * CachedEdgeMergeRadius (clamped to 5..10 steps) — the corner is then located finely enough
	 * that two entries refining onto the same corner land close enough to be merged, which is
	 * what "same corner" means everywhere else. Set a fixed count only to pin the cost: a low
	 * value on a long segment leaves the edge visibly short of the real corner and can strand
	 * near-duplicate cache entries just outside the merge radius.
	 *
	 * Only the opportunistic promotion path reads this. Relay→edge conversion always derives its
	 * own count, because its bracket is the entire edge→relay leg and a fixed count there scales
	 * the error with leg length — the reason simultaneous relay conversions used to leave
	 * permanently unmergeable duplicates.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Edge Cache",
		meta = (ClampMin = "0", ClampMax = "12"))
	int32 ShortestPathPromotionBisectSteps = 0;


	// ── Debug ─────────────────────────────────────────────────────────────────

	/**
	 * Caps how many sources draw at once BEFORE the N key (CycleDebugSourceKey) has collapsed
	 * the set to a single selection — while more than this many registered sources have
	 * bDrawDebugRays enabled, only the closest ones to the listener actually draw; the rest are
	 * suppressed until back in range or the N key takes over entirely. 0 = off (unlimited,
	 * every enabled source draws).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	int32 MaxUncycledDebugSources = 0;

	/** How long debug lines and spheres persist. Matching FullCastInterval avoids visual clutter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (ClampMin = "0.01"))
	float DebugLineDuration = 0.5f;
};
