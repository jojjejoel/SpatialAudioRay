// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioTypes.h"
#include "Components/ActorComponent.h"
#include "Audio/SpatialAudioSettings.h"
#include "SpatialAudioComponent.generated.h"


class UAudioBus;
class UAudioComponent;
class USoundBase;
class USoundWave;
class FAsyncCastManager;
class FEdgeCache;

UCLASS(ClassGroup=(Audio), meta=(BlueprintSpawnableComponent))
class SPATIALAUDIORAY_API USpatialAudioComponent : public UActorComponent {
	GENERATED_BODY()

public:
	USpatialAudioComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	/**
	 * Shared tunable settings for all spatial audio behaviour (ray counts, occlusion,
	 * virtual source, etc.).
	 * Assign the same USpatialAudioSettings data asset to every component in the project
	 * so that all instances are tuned from one place — like a ScriptableObject in Unity.
	 * If left null, the class defaults (CDO) are used automatically.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio")
	TObjectPtr<USpatialAudioSettings> _Settings = nullptr;

	/** True once the debug-visible occlusion (CurrentOcclusion, the "cur=" value in the on-screen
	 *  HUD) has reached PreSweepOcclusionThreshold. Sweeps run to fill the edge cache before full
	 *  occlusion hits; cache clearing and sweep-result discards are suspended so the warmed edges
	 *  survive. The virtual crossfade gate is unaffected. Deliberately independent of
	 *  bHasDirectLoS/NoLoSSampleStreak: fast occlusion transitions can drop bHasDirectLoS on a
	 *  single blocked sample well before a full offset-ring rotation confirms no-LoS, and this
	 *  band exists precisely to start pre-warm sweeps without waiting on that rotation gate. */
	bool IsPreSweepActive() const {
		const USpatialAudioSettings& S = GetSettings();
		return S.PreSweepOcclusionThreshold < 1.f && CurrentOcclusion >= S.PreSweepOcclusionThreshold;
	}

	const USpatialAudioSettings& GetSettings() const {
		return _Settings ? *_Settings : *GetMutableDefault<USpatialAudioSettings>();
	}

	/**
	 * Optional sound wave to inject into the AudioComponent's Sound Cue at BeginPlay.
	 * Requires the Sound Cue to contain a Wave Parameter node with the name set in WaveParameterName.
	 * Leave empty to use whatever wave is already baked into the Sound Cue.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters")
	USoundWave* SoundWaveOverride = nullptr;

	/** Name of the Wave Parameter node in the Sound Cue that SoundWaveOverride feeds into. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (EditCondition = "SoundWaveOverride != nullptr"))
	FName WaveParameterName = TEXT("SoundWave");

	/** Name of the Audio Bus graph input on both MetaSounds (Source writer and Virtual reader). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters")
	FName AudioBusParameterName = TEXT("AudioBus");

	/** Plays a runtime one-shot through this source's spatial pipeline: the component is
	 *  attached to the owner, writes into the shared diffraction bus, and receives the live
	 *  Occlusion parameter every frame until it finishes (auto-destroys). The sound's MetaSound
	 *  must expose the same AudioBus and Occlusion inputs as the persistent source graph.
	 *  Attenuation range overrides are applied to it; it does NOT widen the ray range — the
	 *  persistent tagged sources define AttenuationInnerRadius/MaxRayDistance. */
	UFUNCTION(BlueprintCallable, Category = "Spatial Audio")
	UAudioComponent* PlaySoundThroughSpatialBus(USoundBase* Sound);

	/** Per-source override of the attenuation shape's inner radius (cm). 0 = keep the assigned
	 *  attenuation's value. Applied at BeginPlay to the Source component AND the virtual voice
	 *  template (so pooled voices inherit it); every other attenuation setting stays exactly as
	 *  assigned. AttenuationInnerRadius and bAutoMaxDistance read the overridden result. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (ClampMin = "0.0"))
	float OverrideAttenuationInnerRadius = 0.f;

	/** Per-source override of the attenuation falloff distance (cm). 0 = keep the assigned
	 *  attenuation's value. Same application rules as OverrideAttenuationInnerRadius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (ClampMin = "0.0"))
	float OverrideAttenuationFalloffDistance = 0.f;


	/** Master switch — no debug rendering when false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDrawDebugRays = false;

	/** Show the virtual emitter spheres: magenta sphere at the source actor plus one colored
	 *  sphere + line per audible virtual voice (pool slot). Emitters only — cached edges live
	 *  under bShowEdgePoints and the steering-prediction spheres under bShowSteeringPrediction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowVirtualSourceRays = true;

	/** Show bounce ray segments (white → orange → red) and hit point spheres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowBounceRays = true;

	/** Show on-screen debug text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowDebugText = true;

	/** Key that toggles bShowVirtualSourceRays at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleVirtualSourceKey = EKeys::One;

	/** Key that toggles bShowBounceRays at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleBounceRaysKey = EKeys::Two;

	/** Key that toggles bShowDebugText at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleDebugTextKey = EKeys::Three;

	/** Key that toggles the world-global debug line (all-source trace totals, drawn by
	 *  USpatialAudioDebugSubsystem). Polled by the subsystem, not per component — independent
	 *  of bShowDebugText, so global-only and per-source-only displays are both possible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleGlobalDebugTextKey = EKeys::G;

	/** Key that cycles bDrawDebugRays through the registered sources one at a time
	 *  (OFF → source 1 → … → source N → OFF), so the debug view inspects one source without
	 *  the others drawing over it. Polled by USpatialAudioDebugSubsystem (once, from the first
	 *  registered source) and deliberately NOT gated on bDrawDebugRays — it must keep working
	 *  while every source is off, since it is the way back into the cycle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	FKey CycleDebugSourceKey = EKeys::N;

	/** Key that toggles world-space name labels at every registered source's location (drawn
	 *  by USpatialAudioDebugSubsystem via DrawDebugString — camera-facing text, not depth-tested
	 *  against geometry, so it stays readable through walls). Polled independent of
	 *  bDrawDebugRays / bAnyDebugRays, so labels work even with ray debugging fully off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	FKey ToggleActorLabelsKey = EKeys::L;

	/** Show the full diffraction ray paths (cyan lines from LoSDiffractionPaths) and
	 *  the direct-path LoS sampling lines from the full cast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowDiffractionPaths = true;

	/** Show edge detection results: green backtracked-edge spheres per ray,
	 *  yellow stored-path recheck lines, LoS state spheres at source/listener, and the
	 *  yellow cached-edge spheres (+ relay legs) from the live cache. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowEdgePoints = true;

	/** Key that toggles bShowDiffractionPaths at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleDiffractionPathsKey = EKeys::Five;

	/** Key that toggles bShowEdgePoints at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleEdgePointsKey = EKeys::Six;

	/** Show surface-crawl debug: step spheres (white=continues, yellow=edge, orange=perp wall,
	 *  red=failed) and bright-red spheres wherever a back-face hit is detected (ray inside wall). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowSurfaceCrawl = false;

	/** Key that toggles bShowSurfaceCrawl at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleSurfaceCrawlKey = EKeys::Seven;

	/** Show purple lines from each point that submits a LoS probe toward the listener. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowLoSChecks = false;

	/** Key that toggles bShowLoSChecks at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleLoSChecksKey = EKeys::Eight;

	/** Show the offset-point LoS check rays (green = clear, red = blocked) used to find a
	 *  partial line of sight around the direct path's occluder. Only drawn for a given check
	 *  when at least one of that check's offset rays actually found a clear path — batches
	 *  where every ray is blocked are not drawn, to avoid cluttering the view with misses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowOffsetLoSChecks = false;

	/** Key that toggles bShowOffsetLoSChecks at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleOffsetLoSChecksKey = EKeys::Nine;

	/** Show each cached edge's stored string-pulled shortest path (the polyline its PathDist —
	 *  which feeds the voice clusters — was measured along): magenta chain of straight segments
	 *  source → anchors → edge, with a sphere at each intermediate anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowShortestPaths = true;

	/** Key that toggles bShowShortestPaths at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleShortestPathsKey = EKeys::Zero;

	/** Show the live steering-prediction aim spheres (blue = forward lead, orange = retro
	 *  window after LoS loss), drawn while SteeringPredictionLeadTime > 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowSteeringPrediction = true;

	/** Key that toggles bShowSteeringPrediction at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleSteeringPredictionKey = EKeys::P;

	/** Force the source audio component to silence — use to confirm whether pops come from source vs virtual. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDebugSilenceSource = false;

	/** Force the virtual audio component to silence — use to confirm whether pops come from source vs virtual. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDebugSilenceVirtual = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentOcclusion = 0.f;

	/** 1.0 = at listener, 0.0 = at MaxRayDistance. Drives ray count scaling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentPriority = 1.f;

	/** Smoothed combined speed (cm/s) of source and listener used for interval scaling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentCombinedSpeed = 0.f;

	/** Current interval multiplier from velocity scaling [VelocityIntervalScale, 1.0].
	 *  1.0 = stationary (no scaling). Lower = moving fast (shorter intervals). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentVelocityIntervalMultiplier = 1.f;

	/** True while both source and listener are still after completing a full sweep at their
	 *  current positions. Sweep interval is multiplied by StationaryIdleMultiplier. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	bool bIsStationaryIdle = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	int32 LastRaysReached = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float LastAvgLoSBounces = 0.f;

	/** Current smoothed virtual source position in world space. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Virtual Source")
	FVector CurrentVirtualSourceLocation = FVector::ZeroVector;

	/** Smoothed offset from the actor position currently applied to the AudioComponent. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Virtual Source")
	FVector CurrentAudioComponentOffset = FVector::ZeroVector;

	/** Slewed virtual crossfade gate [0-1] — chases ComputeVirtualCrossfadeTarget (1 on total
	 *  LoS loss; with VirtualCrossfadeStartOcclusion < 1, also a partial occlusion-keyed ramp
	 *  through the pre-sweep band) at VirtualCrossfadeFadeInTime/FadeOutTime. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Virtual Source")
	float CurrentVirtualCrossfade = 0.f;

private:
	friend class FAsyncCastManager;
	friend class FEdgeCache;
	friend class FUpdater;
	friend class USpatialAudioDebugSubsystem;

	/** Compute the effective full-sweep interval for this tick, accounting for velocity,
	 *  geometry burst, post-movement cache fill, stationary coverage, and priority.
	 *  Does NOT write StoredEffFullSweepInterval. */
	float ComputeEffectiveSweepInterval() const;
	/** Cached edges that count toward MovementCacheFillRequiredEdges: discovered since the
	 *  latest movement trigger armed the burst (bNewSinceFillArm), not relayed, not evicting. */
	int32 CountCacheFillEdges() const;
	/** Signed steering-prediction offset for one mover: +velocity×lead normally, −velocity×lead
	 *  within SteeringPredictionLeadTime of losing direct LoS (the aperture just crossed sits on
	 *  the past side — leading forward would aim deeper into the shadow). Zero at lead 0. */
	FVector ComputeSteeringLead(const FVector& SmoothedVelocity, const USpatialAudioSettings& Settings) const;
	/** Advance the movement-triggered early-sweep cooldown and request a sweep when
	 *  the listener has moved far enough since the last triggered position. */
	void TickMovementSweepTrigger(float DeltaTime, bool bInRange, const APawn* Pawn);

	// ── TickComponent phases ─────────────────────────────────────────────────
	static float TimeToBlendSpeed(float Seconds);
	void TickAsyncPipeline(const USpatialAudioSettings& Settings);
	void TickNormalSweepDispatch(float DeltaTime, bool bInRange, float SubInterval);
	/** Advances DirectLoSConfirmedDuration/TimeSinceHadDirectLoS, clears the edge cache on a
	 *  confirmed clear, and returns the occlusion blend speed for this tick. */
	float UpdateDirectLoSConfirmationAndBlendSpeed(float DeltaTime);
	void SmoothTowardTargets(float DeltaTime, float OccBlendSpeed, bool bConfirmedDirectLoS);
	void UpdateTraceDiagnostics(float DeltaTime);

	bool StepSampleSegmentForLoS(const FVector& SegStart, const FVector& SegDir, float SegLen,
	                             float CumDistBase, const FVector& ListenerPos, float MaxBudget,
	                             const USpatialAudioSettings& S, const UWorld* World,
	                             FVector& OutLoSPoint, float& OutLoSCumDist) const;

	void ProcessRayHit(const FHitResult& Hit,
	                   FVector& InOutOrigin, FVector& InOutDir,
	                   float& InOutCumDist, int32& InOutBounce, bool& InOutNextHitCrawls,
	                   bool bLoSAlreadyFound, bool bBias,
	                   const FVector& ListenerPos,
	                   const USpatialAudioSettings& Settings, UWorld* World,
	                   FRayHitOutput& Out) const;

	/**
	 * Crawls along a wall surface from HitPoint in a direction blending the ray's slide vector
	 * and the wall-projected listener direction. At each step casts a short back-probe in -Normal
	 * to detect whether the wall still exists there. When the probe misses, the wall edge has been
	 * found and OutEdgePoint/OutCrawlDir/OutCrawlDist are populated.
	 * Returns false if no edge is found within MaxCrawlSteps.
	 */
	struct FCrawlOutput {
		TArray<FVector> Steps;
		TArray<FVector> ProbeEnds;
		FHitResult PerpHit;
		bool bPerpWallHit = false;
		FVector LoSPoint = FVector::ZeroVector;
		float LoSCumDist = 0.f;
		bool bLoSFound = false;
	};

	bool CrawlSurfaceToEdge(const FVector& HitPoint, const FVector& IncomingDir,
	                        const FVector& SurfaceNormal, const FVector& ListenerPos,
	                        UWorld* World,
	                        FVector& OutEdgePoint, FVector& OutCrawlDir, float& OutCrawlDist,
	                        const USpatialAudioSettings& S,
	                        float InCumDist = 0.f,
	                        FCrawlOutput* Out = nullptr) const;

	// ── CrawlSurfaceToEdge phases ────────────────────────────────────────────
	/** Forward-probes from NudgedStart along CrawlDir to see whether the wall ends before the
	 *  full step cap; shrinks OutEffMaxSteps/OutMaxCrawlRange to the hit distance if so. */
	void ComputeCrawlStepBudget(const FVector& NudgedStart, const FVector& CrawlDir, UWorld* World,
	                            const USpatialAudioSettings& S, int32 CrawlStepCap,
	                            int32& OutEffMaxSteps, float& OutMaxCrawlRange) const;
	/** Continues the LoS-only search past the stepped range (debug-only, Out required) up to
	 *  MaxCrawlRange; the main loop already tried perp-wall/back-probe edges within EffMaxSteps. */
	bool TryFindLoSBeyondCrawlSteps(const FVector& NudgedStart, const FVector& CrawlDir,
	                                const FVector& SurfaceNormal, const FVector& ListenerPos,
	                                UWorld* World, const USpatialAudioSettings& S,
	                                int32 FromStep, float MaxCrawlRange, float InCumDist,
	                                FCrawlOutput& Out) const;

	bool TraceLine(const UWorld* World, FHitResult& Hit, const FVector& Start, const FVector& End) const;

	FTraceHandle SubmitAsyncTrace(UWorld* World, const FVector& Start, const FVector& End) const;

	void GetEffectiveRayCounts(int32& OutFull, float& OutPriority) const;


	void DrawDebugVisualization(const USpatialAudioSettings& Settings);

	// ── DrawDebugVisualization phases ────────────────────────────────────────
	void DrawSteeringPredictionDebug(const USpatialAudioSettings& Settings);
	void DrawVirtualSourceDebug();
	void DrawEdgePointsDebug();
	void DrawShortestPathsDebug();
	void DrawDiffractionPathsDebug();
	void DrawDebugTextHUD(const USpatialAudioSettings& Settings);
	void DrawDebugLegends();

	// ── DrawDebugTextHUD slots ───────────────────────────────────────────────
	void DrawSourceAudioDebugText(uint64 Base) const;
	void DrawVirtualAudioDebugText(uint64 Base) const;
	void DrawOcclusionDebugText(uint64 Base) const;
	void DrawEdgeCacheDebugText(uint64 Base, const USpatialAudioSettings& Settings) const;
	void DrawSweepPacingDebugText(uint64 Base, const USpatialAudioSettings& Settings, int32 ScaledRayCount) const;
	void DrawAudioSpikeDebugText(uint64 Base) const;
	void DrawTraceStatsDebugText(uint64 Base) const;
	void DrawEdgeTimerDebugText(uint64 Base, const USpatialAudioSettings& Settings) const;

	void CacheAudioComponents();
	void ApplyAttenuationOverrides();
	void ApplyAttenuationOverridesTo(UAudioComponent* AC) const;
	void CreateAndAssignAudioBus();
	void CreateVirtualVoicePool();
	void ApplyWaveParameterOverride() const;
	void ReadAttenuationSettings();
	void PerformStartupLoSCheck();

	void UpdateVelocityScaling(float DeltaTime, bool bInRange, const APawn* Pawn);
	void UpdateGeometryBurstAndIdleState(float DeltaTime, bool bInRange, const APawn* Pawn);

	/** Every component tagged AudioComponentSource on the owner, plus any live bus one-shots
	 *  from PlaySoundThroughSpatialBus. Co-located sounds share ONE spatial pipeline (bus,
	 *  occlusion, edge cache, virtual pool) — all of them write the bus and receive the same
	 *  occlusion parameter. Finished one-shots auto-destroy; their stale entries are pruned in
	 *  the per-frame occlusion write. */
	TArray<TWeakObjectPtr<UAudioComponent>> CachedAudioComponentSources;
	TWeakObjectPtr<UAudioComponent> CachedAudioComponentVirtual;

	// UPROPERTY keeps the runtime-created bus alive: nothing else holds a UObject
	// reference to it, and GC of a bus in use by the audio render thread is a crash.
	UPROPERTY(Transient)
	TObjectPtr<UAudioBus> DiffractionBus;

	// Runtime pool of virtual audio components, all reading DiffractionBus. Indexed in
	// parallel with VirtualSlots; the components need UPROPERTY for GC, the plain state doesn't.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> VirtualSlotComponents;
	TArray<FVirtualSlot> VirtualSlots;
	TArray<FVirtualVoice> VirtualVoices;

	FCollisionQueryParams TraceQueryParams;


	TArray<FSpatialRayState> AsyncRays;
	bool bAsyncCastActive = false;
	int32 AsyncMaxBounces = 4; 
	FVector AsyncSourcePos = FVector::ZeroVector;
	FVector AsyncListenerPos = FVector::ZeroVector;
	/** Velocity-led positions for ray STEERING only (aiming axis, bounce/crawl/turn listener
	 *  pulls). Never used by LoS probes, budget gates, or anything that validates/caches —
	 *  those read the actual Async*Pos above. Equal to them at SteeringPredictionLeadTime 0
	 *  or when stationary. */
	FVector AsyncSteeringSourcePos = FVector::ZeroVector;
	FVector AsyncSteeringListenerPos = FVector::ZeroVector;
	int32 AsyncTotalRays = 0; 
	int32 AsyncActualRays = 0; 

	struct FCycleAccumulator {
		int32 Index = 0;
		int32 RaysReached = 0;
		int32 LoSBounces = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPos = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float WeightedDist = 0.f;
		bool bDirectLoSFound = false;
	};

	FCycleAccumulator CycleAccum;
	int32 StaggeredCycleIndex = 0;

	float TimeSinceFullCast = 0.f;
	float StoredEffFullSweepInterval = 0.5f;

	/** Captured at sweep start: this sweep is a pre-warm cast fired while partial LoS remains.
	 *  Finalize/readback then ignore direct-hit rays (expected when partially visible) instead
	 *  of treating them as "all clear" and wiping the cache the sweep just filled. */
	bool bPreSweepCast = false;

	float TargetOcclusion = 0.f;
	float TargetPathAttenuation = 0.f;
	float CurrentPathAttenuation = 0.f;
	FVector TargetVirtualSourceLocation = FVector::ZeroVector;

	/** True when either cast detected an unobstructed direct path to the listener.
	 *  Drives OcclusionClearBlendSpeed so occlusion clears almost instantly. */
	bool bHasDirectLoS = false;

	/** Accumulates how long bHasDirectLoS has been continuously true; resets to 0 on any LoS loss.
	 *  Used to debounce snap/edge-clear effects so single-frame LoS flickers don't glitch the virtual source. */
	float DirectLoSConfirmedDuration = 0.f;

	/** Mirror of the above: seconds since bHasDirectLoS was last true (0 while it is). Drives the
	 *  steering-prediction sign — sweeps within SteeringPredictionLeadTime of losing LoS aim at
	 *  the PAST position instead of the predicted one. Starts large: "never had LoS" must not
	 *  read as "just lost it". */
	float TimeSinceHadDirectLoS = 1e9f;

	/** Runtime max distance in cm. Initialised from Settings->MaxRayDistance at BeginPlay;
	 *  may be overwritten by bAutoMaxDistance logic reading from the attenuation asset. */
	float MaxRayDistance = 5000.f;


	/** Inner radius of the attenuation shape in cm, read from the AudioComponent at BeginPlay.
	 *  Ray count priority is clamped to 1.0 within 2x this distance, then scales to 0 at MaxRayDistance.
	 *  0 means no attenuation asset found — falls back to scaling from distance 0. */
	float AttenuationInnerRadius = 0.f;

	float CurrentSourceToVirtualDistance;

	float LastDirectLoSFraction = 0.f;
	float LoSBreakSweepCooldown = 0.f;

	struct FAudioDiagnostics {
		float CurvedOcclusion = 0.f;
		float SourceCrossfade = 1.f;
		float VirtualGain = 0.f;
		float VirtualPathBend = 0.f;
		int32 UpdateCachedEdges = 0;
		float UpdateDirectDist = 0.f;
		float FullExcessRatio = 0.f;

		float DeltaSrcVol = 0.f;
		float DeltaOcc = 0.f;
		float DeltaVrtGain = 0.f;

		float SpikeTimer = 0.f;
		float SpikeSrcDelta = 0.f;
		float SpikeVrtGainDelta = 0.f;
		float SpikeOccDelta = 0.f;
		bool bSpikeLoSBreak = false;
	} AudioDiag;

	struct FTraceDiagnostics {
		mutable int32 FrameCount = 0;
		int32 AsyncFrameTraces = 0;
		int32 UpdateFrameTraces = 0;
		float SmoothedFrameTraces = 0.f;
		float SmoothedAsyncTraces = 0.f;
		float SmoothedUpdateTraces = 0.f;

		float SnapshotTimer = 0.f;
		int32 AccumBucket = 0;
		float SnapshotFrameTraces = 0.f;
		float SnapshotAsyncTraces = 0.f;
		float SnapshotUpdateTraces = 0.f;
		float SnapshotTracesPerSec = 0.f;

		static constexpr int32 HistoryLen = 60;
		static constexpr int32 Avg10Len = 10;
		float History[HistoryLen] = {};
		int32 HistoryHead = 0;
		int32 HistoryCount = 0;
		float Avg10Sec = 0.f;
		float Avg60Sec = 0.f;
		int32 LastSweepTraces = 0;
		int32 LastSweepFrames = 0;
		int32 LastSweepAsyncRays = 0;
		int32 LastSweepCachedReplaced = 0;
		float LastSweepDuration = 0.f;
		float LastSweepInterval = 0.f;
		int32 FinalizeRetries = 0;
		int32 LastFinalizeRetries = 0;
		float SweepStartTime = 0.f;
		int32 SweepTraceAccum = 0;
		int32 SweepFrameAccum = 0;
		int32 SweepAsyncRayAccum = 0;
	} TraceDiag;

	/** LoS origins saved from the last full cast. Re-checked each update cast to keep
	 *  the virtual source position responsive between full sweeps. */
	TArray<FStoredLoSPath> StoredLoSPaths;


	/** Confirmed diffraction edges that survive across multiple sweeps.
	 *  Validated with 2 traces per frame; reduce the ray budget and stabilise the virtual source. */
	TArray<FCachedEdgePoint> CachedEdgePoints;

	/** Snapshot of cached points validated synchronously at the start of StartAsyncFullCast.
	 *  Injected as confirmed results in FinalizeAsyncCast after the async rays complete. */
	TArray<FCachedEdgePoint> PendingValidCachedPoints;

	/** Normalised source→edge directions for cached edges that haven't moved beyond the
	 *  update threshold. Built once at cycle 0; used each cycle to exclude ray directions
	 *  already covered by a stationary cached edge. */
	TArray<FVector> CachedEdgeDirs;

	/** Persistent confirmed-miss directions. Evicted when source moves significantly.
	 *  Future rays in these directions are cast only with MissDirectionCastProbability. */
	TArray<FCachedMissDir> CachedMissDirs;

	/** Stationary-only subset of CachedMissDirs, built at cycle 0 of each sweep.
	 *  Only directions where source and listener haven't moved are included. */
	TArray<FVector> ActiveMissDirs;

	/** Normalised source→edge directions for every LoS path found in the last completed sweep.
	 *  Populated at the end of ReadbackFinalizeBatch; consumed in StartAsyncFullCast to redirect
	 *  skipped miss-direction rays toward areas that previously found a valid edge. */
	TArray<FVector> SuccessfulEdgeDirHints;

	/** Last diffraction edge position computed while indirect LoS was active. Used as the
	 *  virtual position target when the edge cache is momentarily empty, so the AudioComponent
	 *  holds at the known edge rather than snapping back to the source.
	 *  Reset to valid only once a real edge position has been established. */
	FVector LastKnownEdgePoint = FVector::ZeroVector;
	bool bHasKnownEdge = false;

	/**
	 * Full paths of rays that contributed to the virtual source position in the last full cast.
	 * Each entry is [source, wallHit0, wallHit1, ..., listener].
	 * Rebuilt by FinalizeAsyncCast; drawn every tick when bShowDiffractionPaths.
	 */
	TArray<TArray<FVector>> LoSDiffractionPaths;

	/** Raw clear fraction from the last 5-point LoS sample (center + rotating ring). Drives
	 *  gating (bHasDirectLoS); occlusion uses the smoothed LastDirectLoSFraction instead. */
	float LastOffsetLoSFraction = 0.f;
	float OffsetLoSCheckTimer = 0.f;
	/** Ring basis rotation (radians, wraps at 90°), advanced 90°/OffsetRingRotationSteps per
	 *  sample so one pattern covers 4×Steps evenly spaced directions, then repeats exactly. */
	float OffsetRingAngle = 0.f;
	/** Per-slot cache of the last sampled fraction at each position in the rotation pattern
	 *  (index = check's position within the OffsetRingRotationSteps cycle). A stationary scene
	 *  revisits the exact same angle/radius per slot every rotation (see OffsetRingAngle /
	 *  RadiusScale periodicity in TickDirectLoSSampling), so a slot really is "the value from
	 *  the last time this exact ray was traced". Sized to the RotationSteps clamp (max 8); only
	 *  the first RotationSteps entries are read. */
	float LoSSlotFractions[8] = {};
	/** Circular index into LoSSlotFractions for the next check. */
	int32 LoSSlotIndex = 0;
	/** Average of all RotationSteps slots' most recent values — the smoothing target that
	 *  occlusion chases (debug HUD). Recomputed every check as soon as one slot refreshes
	 *  (2026-07-21: was batched once per completed rotation; a stationary ray retraces the
	 *  identical point each cycle so this only moves on genuine change, aside from rare
	 *  floating-point flicker on grazing traces, which is accepted). */
	float WindowedLoSFraction = 0.f;
	/** Consecutive LoS samples with zero clear points. While stationary, LoS is only declared
	 *  lost once this spans a full rotation pattern — see the hold logic in the update cast. */
	int32 NoLoSSampleStreak = 0;
	bool bLoSFractionSeeded = false;
	/** Low-passed ComputeVirtualCrossfadeRamp output (VirtualCrossfadeSmoothingTime) — the
	 *  ramp amplifies occlusion-sampling wobble by 1/(1-Start), so the raw value is too
	 *  jittery to drive the gate directly. */
	float SmoothedCrossfadeRamp = 0.f;
	float Phase0Timer = 0.f;

	struct FVelocityScalingState {
		FVector LastSourcePos = FVector::ZeroVector;
		FVector LastListenerPos = FVector::ZeroVector;
		bool bPosSet = false;
		float SmoothedCombinedSpeed = 0.f;
		float SmoothedSourceSpeed = 0.f;
		float SmoothedListenerSpeed = 0.f;
		/** Smoothed velocity VECTORS (speeds above are scalars) — feed the steering-prediction
		 *  lead. Smoothing matters twice: raw per-frame velocity jitters, and the decay to zero
		 *  after stopping returns steering to the actual positions (and to seeded-bias
		 *  determinism) without a discontinuity. */
		FVector SmoothedSourceVelocity = FVector::ZeroVector;
		FVector SmoothedListenerVelocity = FVector::ZeroVector;
		float SweepMultiplier = 1.f;
		float EdgeMultiplier = 1.f;
		float OffsetLoSMultiplier = 1.f;

		bool IsStationary() const { return SweepMultiplier > 0.95f && EdgeMultiplier > 0.95f; }
	} VelocityScaling;

	struct FSweepSchedulingState {
		float GeometryBurstTimer = 0.f;
		bool bGeometryChangeDetected = false;
		
		bool bStationaryIdleMode = false;
		FVector StationaryIdleSourcePos = FVector::ZeroVector;
		FVector StationaryIdleListenerPos = FVector::ZeroVector;
		FVector LastTriggerListenerPos = FVector::ZeroVector;
		bool bTriggerPosSet = false;
		float MovementCooldownTimer = 0.f;
		bool bMovementRequested = false;

		/** Post-movement cache-fill budget: while > 0 and fewer than
		 *  MovementCacheFillRequiredEdges NEW edges (bNewSinceFillArm) are cached, sweeps run
		 *  at burst speed. Re-armed to MovementCacheFillMaxSweeps by every movement-triggered
		 *  sweep, which also clears all bNewSinceFillArm flags (rebasing "new"). */
		int32 CacheFillSweepsRemaining = 0;
	} SweepScheduling;

	bool bPhase0HandlesStale = false;

	/** Round-robin state for the source-side ShortestPath re-verification (one edge per
	 *  ShortestPathRecheckInterval). */
	float ShortestPathCheckTimer = 0.f;
	int32 ShortestPathCheckCursor = 0;

	/** In-flight async re-trace of one cached edge's stored ShortestPath polyline (the
	 *  round-robin source-side validation). All segment traces are submitted up front on the
	 *  interval and read back the following tick(s). The entry is re-found by exact EdgePoint
	 *  match at readback, so a sweep rewrite, promotion, or eviction in between simply drops
	 *  the stale result instead of evicting the wrong entry. */
	struct FPathRecheckState {
		bool bPending = false;
		FVector EdgePoint = FVector::ZeroVector;
		TArray<FTraceHandle> Handles;
		/** Pulled-in endpoints per traced segment (Handles holds forward + reverse per entry),
		 *  kept for the blocked-segment debug draw at readback. */
		TArray<FVector> SegStarts;
		TArray<FVector> SegEnds;
	} PathRecheck;

	/** Round-robin state for opportunistic inner-anchor promotion (one edge per
	 *  ShortestPathPromotionInterval). Separate from ShortestPathCheckCursor above so the two
	 *  intervals can be tuned independently. */
	float ShortestPathPromotionTimer = 0.f;
	int32 ShortestPathPromotionCursor = 0;

	struct FFinalizeBatch {
		bool bPending = false;
		TArray<FFinalizeRefineProbe> RefineProbes;
		
		int32 RaysReached = 0;
		int32 TotalLoSBounces = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPosSum = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float WeightedDistSum = 0.f;
		bool bDirectLoSFound = false;
	};

	FFinalizeBatch Finalize;
};
