// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/SpatialAudioTypes.h"
#include "Components/ActorComponent.h"
#include "Audio/SpatialAudioSettings.h"
#include "Sound/SoundAttenuation.h"
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
	 * so that all instances are tuned from one place, like a ScriptableObject in Unity.
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

	/** Distance (cm) the sound effectively travels to reach ListenerPos: the straight line
	 *  while clear, the shortest cached diffraction route (min over cached edges of the
	 *  string-pulled source→edge path + edge→ListenerPos) while occluded, blended by
	 *  smoothed occlusion. Falls back to the straight line until a diffraction path has
	 *  ever been found. Selection and content input (e.g. NPC vocal effort): must never feed
	 *  VirtualGain/PathAttenuation math.
	 *
	 *  DetourOcclusionFloor holds the result at the straight line until occlusion passes it,
	 *  then phases the route in across the remaining range. Cached routes exist well before the
	 *  source is hidden (the pre-sweep band pre-warms the cache during partial LoS), and
	 *  counting them while most of the sound still arrives straight overstates the distance.
	 *  Pass the same threshold you use to decide the listener is hidden. */
	UFUNCTION(BlueprintPure, Category = "Spatial Audio")
	float GetEffectiveAcousticDistance(const FVector& ListenerPos,
	                                   float DetourOcclusionFloor = 0.f) const;

	/** Scales the attenuation FalloffDistance of every cached source component AND the live
	 *  virtual pool relative to their base (scale 1 = as authored/overridden). Lets a caller
	 *  vary audible reach per content, e.g. NPC vocal effort: a whisper carries meters, a
	 *  shout carries the map. Clamped to [Math::MinFalloffScale, 1]: author the base
	 *  attenuation for the LONGEST reach and scale DOWN, because the ray/LoS ranges captured
	 *  by ReadAttenuationSettings at BeginPlay stay at base and deliberately do not follow
	 *  this scale, since a sound audible past them would have no diffraction paths to play through. */
	UFUNCTION(BlueprintCallable, Category = "Spatial Audio")
	void SetAttenuationFalloffScale(float NewScale);

	/** Puts the audible edge at TargetOuterCm from the source (0 = restore the asset's own
	 *  range). Same mechanism as SetAttenuationFalloffScale, addressed in absolute distance so
	 *  callers can tie reach to a design distance they already own, e.g. the NPC voice bands.
	 *  The result is clamped, so ask GetAttenuationOuterRadius what you actually got. */
	UFUNCTION(BlueprintCallable, Category = "Spatial Audio")
	void SetAttenuationOuterRadius(float TargetOuterCm);

	/** Distance (cm) at which the sources currently fall silent, after clamping. */
	UFUNCTION(BlueprintPure, Category = "Spatial Audio")
	float GetAttenuationOuterRadius() const {
		return AttenuationInnerRadius + BaseAttenuationFalloffDistance * AttenuationFalloffScale;
	}

	/** Per-source override of the attenuation shape's inner radius (cm). 0 = keep the assigned
	 *  attenuation's value. Applied at BeginPlay to the Source component AND the virtual voice
	 *  template (so pooled voices inherit it); every other attenuation setting stays exactly as
	 *  assigned. AttenuationInnerRadius and the derived ray range read the overridden result. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (ClampMin = "0.0"))
	float OverrideAttenuationInnerRadius = 0.f;

	/** Per-source override of the attenuation falloff distance (cm). 0 = keep the assigned
	 *  attenuation's value. Same application rules as OverrideAttenuationInnerRadius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (ClampMin = "0.0"))
	float OverrideAttenuationFalloffDistance = 0.f;


	/** Master switch: no debug rendering when false. Every sub-view below also defaults
	 *  to false, so enabling this alone draws nothing until you turn on the specific
	 *  views you want, either here or with their runtime keys. The views overlap heavily
	 *  and several are only readable in isolation, so opting in beats opting out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDrawDebugRays = false;

	/** Show the virtual emitter spheres: magenta sphere at the source actor plus one colored
	 *  sphere + line per audible virtual voice (pool slot). Emitters only: cached edges live
	 *  under bShowEdgePoints and the steering-prediction spheres under bShowSteeringPrediction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowVirtualSourceRays = false;

	/** Show bounce ray segments (white → orange → red) and hit point spheres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowBounceRays = false;

	/** Show on-screen debug text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowDebugText = false;

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
	 *  USpatialAudioDebugSubsystem). Polled by the subsystem, not per component, and independent
	 *  of bShowDebugText, so global-only and per-source-only displays are both possible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleGlobalDebugTextKey = EKeys::G;

	/** Key that cycles bDrawDebugRays through the registered sources one at a time
	 *  (OFF → source 1 → … → source N → OFF), so the debug view inspects one source without
	 *  the others drawing over it. Polled by USpatialAudioDebugSubsystem (once, from the first
	 *  registered source) and deliberately NOT gated on bDrawDebugRays, since it must keep working
	 *  while every source is off, since it is the way back into the cycle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	FKey CycleDebugSourceKey = EKeys::N;

	/** Key that toggles world-space name labels at every registered source's location (drawn
	 *  by USpatialAudioDebugSubsystem via DrawDebugString: camera-facing text, not depth-tested
	 *  against geometry, so it stays readable through walls). Polled independent of
	 *  bDrawDebugRays / bAnyDebugRays, so labels work even with ray debugging fully off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	FKey ToggleActorLabelsKey = EKeys::L;

	/** Show the full diffraction ray paths (cyan lines from LoSDiffractionPaths) and
	 *  the direct-path LoS sampling lines from the full cast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowDiffractionPaths = false;

	/** Show edge detection results: green backtracked-edge spheres per ray,
	 *  yellow stored-path recheck lines, LoS state spheres at source/listener, and the
	 *  yellow cached-edge spheres (+ relay legs) from the live cache. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowEdgePoints = false;

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
	 *  when at least one of that check's offset rays actually found a clear path. Batches
	 *  where every ray is blocked are not drawn, to avoid cluttering the view with misses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowOffsetLoSChecks = false;

	/** Key that toggles bShowOffsetLoSChecks at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleOffsetLoSChecksKey = EKeys::Nine;

	/** Show each cached edge's stored string-pulled shortest path (the polyline its PathDist,
	 *  which feeds the voice clusters, was measured along): magenta chain of straight segments
	 *  source → anchors → edge, with a sphere at each intermediate anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowShortestPaths = false;

	/** Key that toggles bShowShortestPaths at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleShortestPathsKey = EKeys::Zero;

	/** Show the live steering-prediction aim spheres (blue = forward lead, orange = retro
	 *  window after LoS loss), drawn while SteeringPredictionLeadTime > 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowSteeringPrediction = false;

	/** Key that toggles bShowSteeringPrediction at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleSteeringPredictionKey = EKeys::P;

	/** Force the source audio component to silence, to confirm whether pops come from source or virtual. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDebugSilenceSource = false;

	/** Force the virtual audio component to silence, to confirm whether pops come from source or virtual. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDebugSilenceVirtual = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentOcclusion = 0.f;

	/** 1.0 = at listener, 0.0 = at MaxRayDistance. Drives ray count scaling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentPriority = 1.f;

	/** Current interval multiplier from velocity scaling [VelocityIntervalScale, 1.0].
	 *  1.0 = stationary (no scaling). Lower = moving fast (shorter intervals). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Debug")
	float CurrentVelocityIntervalMultiplier = 1.f;

	/** Current smoothed virtual source position in world space. Diagnostic: emitters are placed
	 *  per voice from their own cluster centroid, so nothing audible reads this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Virtual Source")
	FVector CurrentVirtualSourceLocation = FVector::ZeroVector;

	/** Slewed virtual crossfade gate [0-1], chasing ComputeVirtualCrossfadeTarget (1 on total
	 *  LoS loss; with VirtualCrossfadeStartOcclusion < 1, also a partial occlusion-keyed ramp
	 *  through the pre-sweep band) at VirtualCrossfadeFadeInTime/FadeOutTime. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Virtual Source")
	float CurrentVirtualCrossfade = 0.f;

private:
	friend class FAsyncCastManager;
	friend class FEdgeCache;
	friend class FUpdater;
	friend class USpatialAudioDebugSubsystem;

	/** Priority and velocity scaling, then either the post-movement cache-fill burst or stationary
	 *  idle. Does NOT write StoredEffFullSweepInterval. */
	float ComputeEffectiveSweepInterval() const;
	/** Edges discovered since the latest movement trigger armed the burst, not relayed, not evicting. */
	int32 CountCacheFillEdges() const;
	/** Signed lead for one mover: forward normally, backward within SteeringPredictionLeadTime of
	 *  losing LoS, since the aperture just crossed sits on the past side. Zero at lead 0. */
	FVector ComputeSteeringLead(const FVector& SmoothedVelocity, const USpatialAudioSettings& Settings) const;
	/** Advances the early-sweep cooldown and requests a sweep once the listener has moved far enough. */
	void TickMovementSweepTrigger(float DeltaTime, bool bInRange, const APawn* Pawn);

	// ── TickComponent phases ─────────────────────────────────────────────────
	static float TimeToBlendSpeed(float Seconds);
	bool HasConfirmedLoSLoss() const;
	void TickAsyncPipeline(const USpatialAudioSettings& Settings);
	void TickNormalSweepDispatch(float DeltaTime, bool bInRange, float SubInterval);
	/** Advances the LoS duration timers, clears the cache on a confirmed clear, and returns the
	 *  occlusion blend speed for this tick. */
	float UpdateDirectLoSConfirmationAndBlendSpeed(float DeltaTime);
	void SmoothTowardTargets(float DeltaTime, float OccBlendSpeed, bool bConfirmedDirectLoS);
	void UpdateTraceDiagnostics(float DeltaTime);

	bool TraceLine(const UWorld* World, FHitResult& Hit, const FVector& Start, const FVector& End) const;

	void CountTrace() const;

	FTraceHandle SubmitAsyncTrace(UWorld* World, const FVector& Start, const FVector& End) const;

	void GetEffectiveRayCounts(int32& OutFull, float& OutPriority) const;


	void DrawDebugVisualization(const USpatialAudioSettings& Settings);

	// ── DrawDebugVisualization phases ────────────────────────────────────────
	void DrawSteeringPredictionDebug(const USpatialAudioSettings& Settings) const;
	void DrawVirtualSourceDebug();
	void DrawEdgePointsDebug();
	void DrawShortestPathsDebug();
	void DrawDiffractionPathsDebug();
	void DrawDebugTextHUD(const USpatialAudioSettings& Settings) const;
	void DrawDebugLegends() const;

	// ── DrawDebugTextHUD slots ───────────────────────────────────────────────
	void DrawSourceAudioDebugText(uint64 Base) const;
	void DrawVirtualAudioDebugText(uint64 Base) const;
	void DrawOcclusionDebugText(uint64 Base) const;
	void DrawEdgeCacheDebugText(uint64 Base, const USpatialAudioSettings& Settings) const;
	void DrawSweepPacingDebugText(uint64 Base, const USpatialAudioSettings& Settings, int32 ScaledRayCount) const;
	void DrawTraceStatsDebugText(uint64 Base) const;
	void DrawEdgeTimerDebugText(uint64 Base, const USpatialAudioSettings& Settings) const;

	/** Volume multiplier [0-1] of the VIRTUAL template's curve at Distance, the same curve the
	 *  engine applies to the emitter-listener leg, so both legs attenuate coherently even when the
	 *  virtual and source assets differ. Falls back to the widest source, then a linear ramp. */
	float EvaluateVirtualAttenuationVolumeAt(float Distance) const;
	/** Charging Leg1 against the same curve the engine applies to Leg2 is what stops a close emitter
	 *  with a long path out-shouting a far one with a short path. */
	float ComputePathAttenuationCurved(float AvgPathDist, float Leg1Geom, const USpatialAudioSettings& S) const;

	void CacheAudioComponents();
	void ApplyAttenuationOverrides();
	void ApplyAttenuationOverridesTo(UAudioComponent* AC) const;
	static void ApplyFalloffScaleTo(UAudioComponent* AC, float Ratio);

	/** New scales are applied as a ratio against this, so every cached component must always sit at
	 *  exactly this scale. */
	float AttenuationFalloffScale = 1.f;
	void CreateAndAssignAudioBus();
	void CreateVirtualVoicePool();
	void ApplyWaveParameterOverride() const;
	void ReadAttenuationSettings();
	void PerformStartupLoSCheck();

	void UpdateVelocityScaling(float DeltaTime, bool bInRange, const APawn* Pawn);
	void UpdateStationaryIdleState(bool bInRange, const APawn* Pawn);

	/** Every component tagged AudioComponentSource on the owner. Co-located sounds share ONE
	 *  pipeline (bus, occlusion, edge cache, virtual pool). Entries whose component has been
	 *  destroyed are pruned in the per-frame occlusion write. */
	TArray<TWeakObjectPtr<UAudioComponent>> CachedAudioComponentSources;
	TWeakObjectPtr<UAudioComponent> CachedAudioComponentVirtual;

	// UPROPERTY keeps the runtime-created bus alive: GC of a bus in use by the render thread crashes.
	UPROPERTY(Transient)
	TObjectPtr<UAudioBus> DiffractionBus;

	// Indexed in parallel with VirtualSlots; the components need UPROPERTY for GC, the state does not.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> VirtualSlotComponents;
	TArray<FVirtualSlot> VirtualSlots;
	TArray<FVirtualVoice> VirtualVoices;

	FCollisionQueryParams TraceQueryParams;


	/** Rays in flight during a sweep. Empty between sweeps. */
	TArray<FSpatialRayState> AsyncRays;
	bool bAsyncCastActive = false;
	int32 AsyncMaxBounces = 4; 
	FVector AsyncSourcePos = FVector::ZeroVector;
	FVector AsyncListenerPos = FVector::ZeroVector;
	/** Velocity-led positions for ray STEERING only. Never used by LoS probes, budget gates, or
	 *  anything that validates or caches, which read the actual Async*Pos above. */
	FVector AsyncSteeringSourcePos = FVector::ZeroVector;
	FVector AsyncSteeringListenerPos = FVector::ZeroVector;
	int32 AsyncTotalRays = 0;

	struct FCycleAccumulator {
		int32 Index = 0;
		int32 RaysReached = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPos = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float WeightedDist = 0.f;
		bool bDirectLoSFound = false;
	};

	/** Accumulates across the sub-cycles of one sweep sequence. Only the last cycle publishes. */
	FCycleAccumulator CycleAccum;
	int32 StaggeredCycleIndex = 0;

	float TimeSinceFullCast = 0.f;
	float StoredEffFullSweepInterval = 0.5f;

	/** A pre-warm cast fired while partial LoS remains. Readback then ignores direct-hit rays
	 *  instead of treating them as all-clear and wiping the cache the sweep just filled. */
	bool bPreSweepCast = false;

	float TargetOcclusion = 0.f;
	float TargetPathAttenuation = 0.f;
	float CurrentPathAttenuation = 0.f;
	FVector TargetVirtualSourceLocation = FVector::ZeroVector;

	/** Drives OcclusionClearBlendSpeed, so occlusion clears almost instantly. */
	bool bHasDirectLoS = false;

	/** Debounces snap and edge-clear effects so a single-frame flicker cannot glitch the source. */
	float DirectLoSConfirmedDuration = 0.f;

	/** Mirror of the above. Drives the steering-prediction sign. Starts large, since "never had
	 *  LoS" must not read as "just lost it". */
	float TimeSinceHadDirectLoS = 1e9f;

	/** Derived at BeginPlay from the widest source attenuation times MaxRayDistanceScale. Every
	 *  range check reads this, never the settings asset, which has no range of its own. */
	float MaxRayDistance = 5000.f;


	/** Read from the AudioComponent at BeginPlay. Ray priority holds at 1.0 within 2x this, then
	 *  scales to 0 at MaxRayDistance. 0 means no attenuation asset was found. */
	float AttenuationInnerRadius = 0.f;

	/** Captured before any scaling: the base every AttenuationFalloffScale is relative to. */
	float BaseAttenuationFalloffDistance = 0.f;

	/** The virtual template's effective attenuation, captured after ApplyAttenuationOverrides so
	 *  overrides are included. Must be the same curve the engine applies to the pooled emitters'
	 *  own listener leg. Falls back to the widest source when the template has none. */
	FSoundAttenuationSettings VirtualAttenuationSettings;
	bool bHasVirtualAttenuationSettings = false;

	float CurrentSourceToVirtualDistance = 0.f;

	float LastDirectLoSFraction = 0.f;

	struct FAudioDiagnostics {
		float CurvedOcclusion = 0.f;
		float VirtualGain = 0.f;
		float VirtualPathBend = 0.f;
	} AudioDiag;

	/** Set at the top of each tick phase. Other is the default so an untagged phase shows on the
	 *  HUD as an unexplained bucket rather than inflating whichever one ran last. */
	enum class ETraceBucket : uint8 { Sweep, Occlusion, Phase0, Relay, Bisect, PathCheck, Other, Count };
	mutable ETraceBucket CurrentTraceBucket = ETraceBucket::Other;

	/** Restores the previous bucket on exit, so a nested phase charges itself rather than its
	 *  caller. */
	struct FTraceBucketScope {
		const USpatialAudioComponent& Comp;
		const ETraceBucket Previous;
		FTraceBucketScope(const USpatialAudioComponent& InComp, const ETraceBucket Bucket)
			: Comp(InComp), Previous(InComp.CurrentTraceBucket) {
			InComp.CurrentTraceBucket = Bucket;
		}
		~FTraceBucketScope() { Comp.CurrentTraceBucket = Previous; }
	};

	struct FTraceDiagnostics {
		mutable int32 FrameCount = 0;
		float SmoothedFrameTraces = 0.f;

		/** Smoothed and snapshotted exactly like the total, so the parts stay comparable with it
		 *  and answer "why was this frame expensive" directly. */
		static constexpr int32 BucketCount = static_cast<int32>(ETraceBucket::Count);
		mutable int32 BucketFrameCounts[BucketCount] = {};
		float SmoothedBucketTraces[BucketCount] = {};
		float SnapshotBucketTraces[BucketCount] = {};

		float SnapshotTimer = 0.f;
		int32 AccumBucket = 0;
		float SnapshotFrameTraces = 0.f;
		float SnapshotTracesPerSec = 0.f;
		/** Never sum these across sources for a global peak; the subsystem peaks the summed figure. */
		float PeakTracesPerSec = 0.f;

		/** Split on the same IsStationary() the sweep pacing uses, so the pair measures what velocity
		 *  scaling and stationary idle actually buy. Cumulative, so the level-load sweep washes out.
		 *  Time out of range counts as elapsed with no traces, so read these from a controlled run. */
		int32 MovingTraceAccum = 0;
		float MovingSeconds = 0.f;
		int32 RestTraceAccum = 0;
		float RestSeconds = 0.f;

		float MovingTracesPerSec() const { return MovingSeconds > 0.f ? MovingTraceAccum / MovingSeconds : 0.f; }
		float RestTracesPerSec() const { return RestSeconds > 0.f ? RestTraceAccum / RestSeconds : 0.f; }

		static constexpr int32 HistoryLen = 60;
		static constexpr int32 Avg10Len = 10;
		float History[HistoryLen] = {};
		int32 HistoryHead = 0;
		int32 HistoryCount = 0;
		float Avg10Sec = 0.f;
		float Avg60Sec = 0.f;
		int32 LastSweepFrames = 0;
		int32 LastSweepAsyncRays = 0;
		float LastSweepDuration = 0.f;
		float LastSweepInterval = 0.f;
		float SweepStartTime = 0.f;
		int32 SweepFrameAccum = 0;
		int32 SweepAsyncRayAccum = 0;
	} TraceDiag;

	/** Filled by AccumulateRefineProbesIntoCycle and consumed by MergeStoredPathsIntoCache within
	 *  the same readback, so it is empty by the time anything else in the tick runs. */
	TArray<FStoredLoSPath> StoredLoSPaths;

	/** Revalidated one entry per Phase 0 slice. A full cache scales the sweep's ray budget down
	 *  through FullCacheRayScale and stabilises the virtual source. */
	TArray<FCachedEdgePoint> CachedEdgePoints;

	/** Taken when a sweep starts, so it finalises against the cache as it stood at capture rather
	 *  than as intervening ticks have changed it. */
	TArray<FCachedEdgePoint> PendingValidCachedPoints;

	/** One [source, virtual source, listener] polyline per sweep that published a virtual source.
	 *  Cleared when a sweep starts or direct LoS returns; drawn when bShowDiffractionPaths. */
	TArray<TArray<FVector>> LoSDiffractionPaths;

	/** Drives gating (bHasDirectLoS); occlusion uses the smoothed LastDirectLoSFraction instead. */
	float LastOffsetLoSFraction = 0.f;
	float OffsetLoSCheckTimer = 0.f;
	/** Wraps at 90 degrees, advanced 90/OffsetRingRotationSteps per sample, so one pattern covers
	 *  4xSteps evenly spaced directions and then repeats exactly. */
	float OffsetRingAngle = 0.f;
	/** Index = the check's position within the rotation. A stationary scene revisits the same
	 *  angle and radius per slot every rotation, so a slot really is the value from the last time
	 *  this exact ray was traced. Sized to the clamp; only the first RotationSteps are read. */
	float LoSSlotFractions[8] = {};
	/** Circular index into LoSSlotFractions for the next check. */
	int32 LoSSlotIndex = 0;
	/** Average of all RotationSteps slots, recomputed as soon as one refreshes. A stationary ray
	 *  retraces the identical point each cycle, so this only moves on genuine change. */
	float WindowedLoSFraction = 0.f;
	/** While stationary, LoS is only declared lost once this spans a full rotation pattern. */
	int32 NoLoSSampleStreak = 0;
	bool bLoSFractionSeeded = false;
	/** The ramp amplifies occlusion wobble by 1/(1-Start), so the raw value is too jittery to
	 *  drive the gate directly. */
	float SmoothedCrossfadeRamp = 0.f;
	float Phase0Timer = 0.f;
	/** Phase 0 fires one edge per slice rather than the whole cache, spreading its cost. */
	int32 Phase0Cursor = 0;

	struct FVelocityScalingState {
		FVector LastSourcePos = FVector::ZeroVector;
		FVector LastListenerPos = FVector::ZeroVector;
		bool bPosSet = false;
		float SmoothedCombinedSpeed = 0.f;
		float SmoothedSourceSpeed = 0.f;
		float SmoothedListenerSpeed = 0.f;
		/** Feed the steering-prediction lead. Smoothing matters twice: raw velocity jitters, and the
		 *  decay to zero after stopping returns steering to the actual positions without a jump. */
		FVector SmoothedSourceVelocity = FVector::ZeroVector;
		FVector SmoothedListenerVelocity = FVector::ZeroVector;
		float SweepMultiplier = 1.f;
		float EdgeMultiplier = 1.f;
		float OffsetLoSMultiplier = 1.f;

		bool IsStationary() const { return SweepMultiplier > 0.95f && EdgeMultiplier > 0.95f; }
	} VelocityScaling;

	struct FSweepSchedulingState {
		bool bStationaryIdleMode = false;
		FVector StationaryIdleSourcePos = FVector::ZeroVector;
		FVector StationaryIdleListenerPos = FVector::ZeroVector;
		FVector LastTriggerListenerPos = FVector::ZeroVector;
		bool bTriggerPosSet = false;
		float MovementCooldownTimer = 0.f;
		bool bMovementRequested = false;

		/** While > 0 and fewer than MovementCacheFillRequiredEdges new edges are cached, sweeps run at
		 *  burst speed. Re-armed by every movement-triggered sweep, which also rebases "new". */
		int32 CacheFillSweepsRemaining = 0;
	} SweepScheduling;

	bool bPhase0HandlesStale = false;

	/** Round-robin state for the source-side ShortestPath re-verification, one edge per interval. */
	float ShortestPathCheckTimer = 0.f;
	int32 ShortestPathCheckCursor = 0;

	/** All segment traces are submitted up front and read back the following tick(s). The entry is
	 *  re-found by exact EdgePoint match, so a rewrite, promotion or eviction in between drops the
	 *  stale result instead of evicting the wrong entry. */
	struct FPathRecheckState {
		bool bPending = false;
		FVector EdgePoint = FVector::ZeroVector;
		TArray<FTraceHandle> Handles;
		/** Kept for the blocked-segment debug draw at readback. */
		TArray<FVector> SegStarts;
		TArray<FVector> SegEnds;
		/** The live source position the first leg was traced from. A clear result re-anchors the entry
		 *  to it, catching source movement by measurement rather than by a distance threshold. */
		bool bReanchored = false;
		FVector ReanchorSource = FVector::ZeroVector;
	} PathRecheck;

	/** Separate from ShortestPathCheckCursor so the two intervals can be tuned independently. */
	float ShortestPathPromotionTimer = 0.f;
	int32 ShortestPathPromotionCursor = 0;

	struct FFinalizeBatch {
		bool bPending = false;
		TArray<FFinalizeRefineProbe> RefineProbes;
		
		int32 RaysReached = 0;
		float MinLoSDist = TNumericLimits<float>::Max();
		FVector WeightedPosSum = FVector::ZeroVector;
		float TotalWeight = 0.f;
		float WeightedDistSum = 0.f;
		bool bDirectLoSFound = false;
	};

	/** Handed from SubmitFinalizeBatch to ReadbackFinalizeBatch, pending exactly one frame. */
	FFinalizeBatch Finalize;
};
