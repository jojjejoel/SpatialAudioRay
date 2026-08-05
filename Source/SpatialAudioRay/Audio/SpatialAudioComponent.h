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

	/** Shared tunables for every spatial audio behaviour. Assign the same asset to every component
	 *  so the whole project is tuned from one place. Null falls back to the class defaults (CDO). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio")
	TObjectPtr<USpatialAudioSettings> _Settings = nullptr;

	bool IsPreSweepActive() const {
		const USpatialAudioSettings& S = GetSettings();
		return S.PreSweepOcclusionThreshold < 1.f && CurrentOcclusion >= S.PreSweepOcclusionThreshold;
	}

	const USpatialAudioSettings& GetSettings() const {
		return _Settings ? *_Settings : *GetMutableDefault<USpatialAudioSettings>();
	}

	/** Injected into the Sound Cue at BeginPlay. Requires a Wave Parameter node named
	 *  WaveParameterName. Leave empty to keep whatever the cue already has. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters")
	USoundWave* SoundWaveOverride = nullptr;

	/** Name of the Wave Parameter node in the Sound Cue that SoundWaveOverride feeds into. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (EditCondition = "SoundWaveOverride != nullptr"))
	FName WaveParameterName = TEXT("SoundWave");

	/** Name of the Audio Bus graph input on both MetaSounds (Source writer and Virtual reader). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters")
	FName AudioBusParameterName = TEXT("AudioBus");

	/** Distance (cm) the sound travels to reach ListenerPos: the straight line while clear, the
	 *  shortest cached diffraction route while occluded, blended by smoothed occlusion. Selection
	 *  and content input only, never gain. DetourOcclusionFloor holds the result at the straight
	 *  line until occlusion passes it. */
	UFUNCTION(BlueprintPure, Category = "Spatial Audio")
	float GetEffectiveAcousticDistance(const FVector& ListenerPos,
	                                   float DetourOcclusionFloor = 0.f) const;

	/** Scales the FalloffDistance of every source and the virtual pool against their base (1 = as
	 *  authored). Clamped to [MinFalloffScale, 1]: author for the longest reach and scale down. */
	UFUNCTION(BlueprintCallable, Category = "Spatial Audio")
	void SetAttenuationFalloffScale(float NewScale);

	/** Puts the audible edge at TargetOuterCm from the source (0 = restore the asset's own range).
	 *  The result is clamped, so ask GetAttenuationOuterRadius what you actually got. */
	UFUNCTION(BlueprintCallable, Category = "Spatial Audio")
	void SetAttenuationOuterRadius(float TargetOuterCm);

	/** Distance (cm) at which the sources currently fall silent, after clamping. */
	UFUNCTION(BlueprintPure, Category = "Spatial Audio")
	float GetAttenuationOuterRadius() const {
		return AttenuationInnerRadius + BaseAttenuationFalloffDistance * AttenuationFalloffScale;
	}

	/** Per-source override of the attenuation shape's inner radius (cm). 0 = keep the assigned
	 *  attenuation's value. Applied at BeginPlay to the sources and the virtual voice template. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (ClampMin = "0.0"))
	float OverrideAttenuationInnerRadius = 0.f;

	/** Per-source override of the attenuation falloff distance (cm). 0 = keep the assigned
	 *  attenuation's value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Parameters",
		meta = (ClampMin = "0.0"))
	float OverrideAttenuationFalloffDistance = 0.f;


	/** Master switch: no debug rendering when false. Every sub-view below also defaults to false,
	 *  so enabling this alone draws nothing until you turn on the views you want. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	bool bDrawDebugRays = false;

	/** Virtual emitter spheres: magenta at the source actor, one colored sphere and line per
	 *  audible virtual voice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowVirtualSourceRays = false;

	/** Bounce ray segments (white to orange to red) and hit point spheres. */
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

	/** Key that toggles the world-global debug line of all-source trace totals. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleGlobalDebugTextKey = EKeys::G;

	/** Cycles bDrawDebugRays through the registered sources one at a time (off, source 1 to N,
	 *  off), so one source can be inspected without the others drawing over it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	FKey CycleDebugSourceKey = EKeys::N;

	/** World-space name labels at every registered source, drawn camera-facing and not
	 *  depth-tested, so they stay readable through walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug")
	FKey ToggleActorLabelsKey = EKeys::L;

	/** Full diffraction ray paths (cyan) plus the direct-path LoS sampling lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowDiffractionPaths = false;

	/** Edge detection results: green per-ray edge spheres, yellow recheck lines, LoS state
	 *  spheres, the yellow cached-edge spheres with their relay legs, and a line from each edge
	 *  to the virtual emitter it feeds, in that emitter's colour. */
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

	/** Surface-crawl steps (white continues, yellow edge, orange perp wall, red failed) and
	 *  bright-red spheres wherever a back-face hit puts the ray inside a wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowSurfaceCrawl = false;

	/** Key that toggles bShowSurfaceCrawl at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleSurfaceCrawlKey = EKeys::Seven;

	/** Purple lines from each point that submits a LoS probe toward the listener. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowLoSChecks = false;

	/** Key that toggles bShowLoSChecks at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleLoSChecksKey = EKeys::Eight;

	/** Offset-point LoS check rays (green clear, red blocked). Only drawn for checks where at
	 *  least one offset ray found a clear path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowOffsetLoSChecks = false;

	/** Key that toggles bShowOffsetLoSChecks at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleOffsetLoSChecksKey = EKeys::Nine;

	/** Each cached edge's stored string-pulled shortest path, the polyline its PathDist was
	 *  measured along: magenta segments source to anchors to edge, sphere at each anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	bool bShowShortestPaths = false;

	/** Key that toggles bShowShortestPaths at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spatial Audio|Debug",
		meta = (EditCondition = "bDrawDebugRays"))
	FKey ToggleShortestPathsKey = EKeys::Zero;

	/** Live steering-prediction aim spheres (blue forward lead, orange retro window after LoS
	 *  loss), drawn while SteeringPredictionLeadTime > 0. */
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

	/** Slewed virtual crossfade gate [0-1], chasing ComputeVirtualCrossfadeTarget. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spatial Audio|Virtual Source")
	float CurrentVirtualCrossfade = 0.f;

private:
	friend class FAsyncCastManager;
	friend class FEdgeCache;
	friend class FUpdater;
	friend class USpatialAudioSubsystem;

	float ComputeEffectiveSweepInterval() const;
	/** Openings closer together than the virtual emitter's full-volume radius are one sound, so an
	 *  emitter's inner sphere covers exactly the edges its voice represents. 0 = one voice per edge. */
	float GetVoiceClusterRadius() const;
	bool HasNewEdgeSinceFillArm() const;
	bool IsCacheFillPending() const;
	int32 GetEffectiveMaxVirtualVoices() const;
	int32 GetEffectiveCachedEdgeMaxCount() const;
	/** Shared with the HUD so the readout cannot disagree with the check. Squared, so only the debug
	 *  draw pays a square root. */
	float ComputeIdleAnchorDriftSq() const;
	FVector ComputeSteeringLead(const FVector& SmoothedVelocity, const USpatialAudioSettings& Settings) const;
	void RequestSweepOnPreSweepBandEntry(bool bPreSweepActive);

	static float TimeToBlendSpeed(float Seconds);
	bool HasConfirmedLoSLoss() const;

	static constexpr int32 MaxRingRotationSteps = 8;

	int32 ResolveRingRotationSteps() const {
		return FMath::Clamp(GetSettings().OffsetRingRotationSteps, 1, MaxRingRotationSteps);
	}

	void TickAsyncPipeline(const USpatialAudioSettings& Settings);
	void TickNormalSweepDispatch(float DeltaTime, bool bInRange, float SweepInterval);
	void TickDirectLoSState(float DeltaTime);
	void SmoothTowardTargets(float DeltaTime);
	void UpdateTraceDiagnostics(float DeltaTime);

	bool TraceLine(const UWorld* World, FHitResult& Hit, const FVector& Start, const FVector& End) const;

	void CountTrace() const;

	FTraceHandle SubmitAsyncTrace(UWorld* World, const FVector& Start, const FVector& End) const;

	void GetEffectiveRayCounts(int32& OutFull, float& OutPriority) const;


	void DrawDebugVisualization(const USpatialAudioSettings& Settings);

	void DrawSteeringPredictionDebug(const USpatialAudioSettings& Settings) const;
	void DrawVirtualSourceDebug();
	int32 FindSlotDrawingEdge(int32 EdgeIndex) const;
	void DrawEdgePointsDebug();
	void DrawShortestPathsDebug();
	void DrawDiffractionPathsDebug();
	void DrawDebugTextHUD(const USpatialAudioSettings& Settings) const;
	void DrawDebugLegends() const;

	void DrawVirtualAudioDebugText(uint64 Base) const;
	void DrawOcclusionDebugText(uint64 Base) const;
	void DrawEdgeCacheDebugText(uint64 Base, const USpatialAudioSettings& Settings) const;
	void DrawSweepPacingDebugText(uint64 Base, const USpatialAudioSettings& Settings, int32 ScaledRayCount) const;
	void DrawTraceStatsDebugText(uint64 Base) const;
	void DrawEdgeTimerDebugText(uint64 Base, const USpatialAudioSettings& Settings) const;

	float EvaluateVirtualAttenuationVolumeAt(float Distance) const;
	float ComputePathAttenuationCurved(float AvgPathDist, const USpatialAudioSettings& S) const;

	void CacheAudioComponents();
	void ApplyAttenuationOverrides();
	void ApplyAttenuationOverridesTo(UAudioComponent* AC) const;
	static void ApplyFalloffScaleTo(UAudioComponent* AC, float Ratio);

	float AttenuationFalloffScale = 1.f;
	void CreateAndAssignAudioBus();
	void CreateVirtualVoicePool();
	void ApplyWaveParameterOverride() const;
	void ReadAttenuationSettings();
	void PerformStartupLoSCheck();

	void UpdateVelocityScaling(float DeltaTime, bool bInRange, const APawn* Pawn);
	void UpdateStationaryIdleState(bool bInRange, const APawn* Pawn);

	TArray<TWeakObjectPtr<UAudioComponent>> CachedAudioComponentSources;
	TWeakObjectPtr<UAudioComponent> CachedAudioComponentVirtual;

	UPROPERTY(Transient)
	TObjectPtr<UAudioBus> DiffractionBus;

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
	FVector AsyncSteeringSourcePos = FVector::ZeroVector;
	FVector AsyncSteeringListenerPos = FVector::ZeroVector;
	int32 AsyncTotalRays = 0;

	float TimeSinceFullCast = 0.f;
	float StoredEffFullSweepInterval = 0.5f;

	bool bPreSweepCast = false;

	float TargetOcclusion = 0.f;
	float TargetPathAttenuation = 0.f;
	float CurrentPathAttenuation = 0.f;
	FVector TargetVirtualSourceLocation = FVector::ZeroVector;

	bool bHasDirectLoS = false;

	float TimeSinceHadDirectLoS = 1e9f;

	/** Pushed by the subsystem. 1 = under budget. */
	float TraceBudgetStretch = 1.f;

	bool bInAudibleRange = false;

	float MaxRayDistance = 5000.f;


	float AttenuationInnerRadius = 0.f;

	float BaseAttenuationFalloffDistance = 0.f;

	FSoundAttenuationSettings VirtualAttenuationSettings;
	bool bHasVirtualAttenuationSettings = false;

	float CurrentSourceToVirtualDistance = 0.f;


	struct FAudioDiagnostics {
		float VirtualGain = 0.f;
	} AudioDiag;

	enum class ETraceBucket : uint8 { Sweep, Occlusion, Phase0, Relay, Bisect, PathCheck, Other, Count };

	mutable ETraceBucket CurrentTraceBucket = ETraceBucket::Other;

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

		static constexpr int32 BucketCount = static_cast<int32>(ETraceBucket::Count);
		mutable int32 BucketFrameCounts[BucketCount] = {};
		float SmoothedBucketTraces[BucketCount] = {};
		float SnapshotBucketTraces[BucketCount] = {};

		float SnapshotTimer = 0.f;
		int32 AccumBucket = 0;
		float SnapshotFrameTraces = 0.f;
		float SnapshotTracesPerSec = 0.f;
		float PeakTracesPerSec = 0.f;

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

	TArray<FStoredLoSPath> StoredLoSPaths;

	TArray<FCachedEdgePoint> CachedEdgePoints;

	/** Parallel to CachedEdgePoints: which voice cluster each edge feeds, INDEX_NONE if none.
	 *  Debug only, and refreshed by clustering, so it can lag the cache by a frame. */
	TArray<int32> EdgeClusterIndices;

	TArray<FCachedEdgePoint> PendingValidCachedPoints;

	TArray<TArray<FVector>> LoSDiffractionPaths;

	float LastOffsetLoSFraction = 0.f;
	float OffsetLoSCheckTimer = 0.f;
	float OffsetRingAngle = 0.f;
	float LoSSlotFractions[MaxRingRotationSteps] = {};
	int32 LoSSlotIndex = 0;
	float WindowedLoSFraction = 0.f;
	int32 NoLoSSampleStreak = 0;
	bool bLoSFractionSeeded = false;
	float SmoothedCrossfadeRamp = 0.f;
	float Phase0Timer = 0.f;
	int32 Phase0Cursor = 0;

	struct FVelocityScalingState {
		FVector LastSourcePos = FVector::ZeroVector;
		FVector LastListenerPos = FVector::ZeroVector;
		bool bPosSet = false;
		float SmoothedCombinedSpeed = 0.f;
		float SmoothedSourceSpeed = 0.f;
		float SmoothedListenerSpeed = 0.f;
		FVector SmoothedSourceVelocity = FVector::ZeroVector;
		FVector SmoothedListenerVelocity = FVector::ZeroVector;
		float SweepMultiplier = 1.f;
		float EdgeMultiplier = 1.f;
		float OffsetLoSMultiplier = 1.f;

		bool IsStationary() const { return SweepMultiplier > 0.95f && EdgeMultiplier > 0.95f; }
	} VelocityScaling;

	struct FSweepSchedulingState {
		/** Ends only on leaving StationaryIdleBreakDist of the anchor, never on a sweep dispatching. */
		bool bStationaryIdleMode = false;
		FVector StationaryIdleSourcePos = FVector::ZeroVector;
		FVector StationaryIdleListenerPos = FVector::ZeroVector;
		bool bEarlySweepRequested = false;
		bool bWasPreSweepActive = false;

		int32 CacheFillSweepsRemaining = 0;
	} SweepScheduling;

	bool bPhase0HandlesStale = false;

	float ShortestPathCheckTimer = 0.f;
	int32 ShortestPathCheckCursor = 0;

	struct FPathRecheckState {
		bool bPending = false;
		FVector EdgePoint = FVector::ZeroVector;
		TArray<FTraceHandle> Handles;
		TArray<FVector> SegStarts;
		TArray<FVector> SegEnds;
		bool bReanchored = false;
		FVector ReanchorSource = FVector::ZeroVector;
	} PathRecheck;

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

	FFinalizeBatch Finalize;
};
