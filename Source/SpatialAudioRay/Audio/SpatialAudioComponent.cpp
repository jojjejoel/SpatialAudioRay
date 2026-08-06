// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/SpatialAudioComponent.h"
#include "Audio/Math.h"

#include "SpatialAudioRayModule.h"

#include "Components/AudioComponent.h"
#include "Sound/AudioBus.h"
#include "Sound/SoundAttenuation.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

#include "Audio/AsyncCastManager.h"
#include "Audio/EdgeCache.h"
#include "Audio/SpatialAudioSubsystem.h"
#include "Audio/Updater.h"

USpatialAudioComponent::USpatialAudioComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	CurrentOcclusion = 1.f;
	TargetOcclusion = 1.f;
}

bool USpatialAudioComponent::TraceLine(const UWorld* World, FHitResult& Hit,
                                       const FVector& Start, const FVector& End) const {
	CountTrace();
	return World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, TraceQueryParams);
}

FTraceHandle USpatialAudioComponent::SubmitAsyncTrace(UWorld* World, const FVector& Start, const FVector& End) const {
	CountTrace();
	return World->AsyncLineTraceByChannel(EAsyncTraceType::Single, Start, End, ECC_Visibility, TraceQueryParams);
}

void USpatialAudioComponent::CountTrace() const {
	++TraceDiag.FrameCount;
	++TraceDiag.BucketFrameCounts[static_cast<int32>(CurrentTraceBucket)];
}

void USpatialAudioComponent::BeginPlay() {
	Super::BeginPlay();

	CacheAudioComponents();
	ApplyAttenuationOverrides();
	CreateAndAssignAudioBus();
	CreateVirtualVoicePool();
	ApplyWaveParameterOverride();

	if (const AActor* Owner = GetOwner()) {
		TargetVirtualSourceLocation = Owner->GetActorLocation();
	}
	RefreshTraceIgnoreList();

	ReadAttenuationSettings();
	PerformStartupLoSCheck();

	FUpdater::UpdateAudioParameters(*this, 0.0f, GetSettings());
	FAsyncCastManager::StartAsyncFullCast(*this, GetSettings());
}

void USpatialAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
	if (USpatialAudioSubsystem* Subsystem = GetWorld()
		                                        ? GetWorld()->GetSubsystem<USpatialAudioSubsystem>()
		                                        : nullptr) {
		Subsystem->Unregister(this);
	}
	Super::EndPlay(EndPlayReason);
}

void USpatialAudioComponent::CacheAudioComponents() {
	CachedAudioComponentSources.Reset();
	if (AActor* Owner = GetOwner()) {
		for (UActorComponent* C : Owner->GetComponentsByTag(UAudioComponent::StaticClass(),
		                                                    TEXT("AudioComponentSource"))) {
			CachedAudioComponentSources.Add(CastChecked<UAudioComponent>(C));
		}
		CachedAudioComponentVirtual = Owner->FindComponentByTag<UAudioComponent>(TEXT("AudioComponentVirtual"));
	}
}

void USpatialAudioComponent::CreateAndAssignAudioBus() {
	DiffractionBus = NewObject<UAudioBus>(this);
	DiffractionBus->AudioBusChannels = EAudioBusChannels::Mono;

	for (const TWeakObjectPtr<UAudioComponent>& Src : CachedAudioComponentSources) {
		if (UAudioComponent* AC = Src.Get()) {
			AC->SetObjectParameter(AudioBusParameterName, DiffractionBus);
		}
	}
}

void USpatialAudioComponent::CreateVirtualVoicePool() {
	UAudioComponent* Template = CachedAudioComponentVirtual.Get();
	AActor* Owner = GetOwner();
	if (!Template || !Owner) {
		return;
	}

	const int32 VoiceCount = FMath::Max(1, GetSettings().MaxVirtualVoices);
	const int32 PoolSize = 2 * VoiceCount;
	VirtualVoices.SetNum(VoiceCount);
	VirtualSlots.SetNum(PoolSize);
	VirtualSlotComponents.Reserve(PoolSize);

	for (int32 i = 0; i < PoolSize; ++i) {
		UAudioComponent* Comp = NewObject<UAudioComponent>(Owner);
		Comp->bAutoActivate = false;
		Comp->SetSound(Template->Sound);
		Comp->AttenuationSettings = Template->AttenuationSettings;
		Comp->bOverrideAttenuation = Template->bOverrideAttenuation;
		Comp->AttenuationOverrides = Template->AttenuationOverrides;
		Comp->RegisterComponent();
		if (USceneComponent* Root = Owner->GetRootComponent()) {
			Comp->AttachToComponent(Root, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}
		if (DiffractionBus) {
			Comp->SetObjectParameter(AudioBusParameterName, DiffractionBus);
		}
		Comp->SetFloatParameter(FName("VirtualGain"), 0.f);
		Comp->Play();
		VirtualSlotComponents.Add(Comp);
	}
}

void USpatialAudioComponent::ApplyWaveParameterOverride() const {
	if (SoundWaveOverride) {
		for (const TWeakObjectPtr<UAudioComponent>& Src : CachedAudioComponentSources) {
			if (UAudioComponent* AC = Src.Get()) {
				AC->SetWaveParameter(WaveParameterName, SoundWaveOverride);
			}
		}
	}
}

void USpatialAudioComponent::ApplyAttenuationOverridesTo(UAudioComponent* AC) const {
	if (!AC || (OverrideAttenuationInnerRadius <= 0.f && OverrideAttenuationFalloffDistance <= 0.f)) {
		return;
	}
	FSoundAttenuationSettings Effective;
	if (AC->bOverrideAttenuation) {
		Effective = AC->AttenuationOverrides;
	}
	else if (AC->AttenuationSettings) {
		Effective = AC->AttenuationSettings->Attenuation;
	}
	if (OverrideAttenuationInnerRadius > 0.f) {
		Effective.AttenuationShapeExtents.X = OverrideAttenuationInnerRadius;
	}
	if (OverrideAttenuationFalloffDistance > 0.f) {
		Effective.FalloffDistance = OverrideAttenuationFalloffDistance;
	}
	AC->AttenuationOverrides = Effective;
	AC->bOverrideAttenuation = true;
}

void USpatialAudioComponent::ApplyFalloffScaleTo(UAudioComponent* AC, float Ratio) {
	if (!AC || FMath::IsNearlyEqual(Ratio, 1.f)) {
		return;
	}
	if (!AC->bOverrideAttenuation) {
		if (!AC->AttenuationSettings) {
			return;
		}
		AC->AttenuationOverrides = AC->AttenuationSettings->Attenuation;
		AC->bOverrideAttenuation = true;
	}
	AC->AttenuationOverrides.FalloffDistance *= Ratio;
}

void USpatialAudioComponent::SetAttenuationOuterRadius(const float TargetOuterCm) {
	SetAttenuationFalloffScale(Math::ComputeFalloffScaleForOuterRadius(
		TargetOuterCm, AttenuationInnerRadius, BaseAttenuationFalloffDistance));
}

void USpatialAudioComponent::SetAttenuationFalloffScale(float NewScale) {
	NewScale = FMath::Clamp(NewScale, Math::MinFalloffScale, 1.f);
	if (FMath::IsNearlyEqual(NewScale, AttenuationFalloffScale)) {
		return;
	}
	const float Ratio = NewScale / AttenuationFalloffScale;
	for (const TWeakObjectPtr<UAudioComponent>& Src : CachedAudioComponentSources) {
		ApplyFalloffScaleTo(Src.Get(), Ratio);
	}
	ApplyFalloffScaleTo(CachedAudioComponentVirtual.Get(), Ratio);
	for (UAudioComponent* Slot : VirtualSlotComponents) {
		ApplyFalloffScaleTo(Slot, Ratio);
	}
	AttenuationFalloffScale = NewScale;
}

void USpatialAudioComponent::ApplyAttenuationOverrides() {
	for (const TWeakObjectPtr<UAudioComponent>& Src : CachedAudioComponentSources) {
		ApplyAttenuationOverridesTo(Src.Get());
	}
	ApplyAttenuationOverridesTo(CachedAudioComponentVirtual.Get());
}

void USpatialAudioComponent::ReadAttenuationSettings() {
	const FSoundAttenuationSettings* Widest = nullptr;
	float WidestRange = 0.f;
	for (const TWeakObjectPtr<UAudioComponent>& Src : CachedAudioComponentSources) {
		const UAudioComponent* AC = Src.Get();
		if (!AC) {
			continue;
		}
		const FSoundAttenuationSettings* AttenuationSettings = nullptr;
		if (AC->bOverrideAttenuation) {
			AttenuationSettings = &AC->AttenuationOverrides;
		}
		else if (AC->AttenuationSettings) {
			AttenuationSettings = &AC->AttenuationSettings->Attenuation;
		}
		if (AttenuationSettings && AttenuationSettings->bAttenuate) {
			const float Range = AttenuationSettings->AttenuationShapeExtents.X
				+ AttenuationSettings->FalloffDistance;
			if (!Widest || Range > WidestRange) {
				Widest = AttenuationSettings;
				WidestRange = Range;
			}
		}
	}

	const FSoundAttenuationSettings* CurveSource = nullptr;
	if (const UAudioComponent* VirtualTemplate = CachedAudioComponentVirtual.Get()) {
		if (VirtualTemplate->bOverrideAttenuation) {
			CurveSource = &VirtualTemplate->AttenuationOverrides;
		}
		else if (VirtualTemplate->AttenuationSettings) {
			CurveSource = &VirtualTemplate->AttenuationSettings->Attenuation;
		}
	}
	if (!CurveSource || !CurveSource->bAttenuate) {
		CurveSource = Widest;
	}
	if (CurveSource) {
		VirtualAttenuationSettings = *CurveSource;
		bHasVirtualAttenuationSettings = true;
	}

	if (!Widest) {
		UE_LOG(LogSpatialAudio, Warning,
		       TEXT("SpatialAudioComponent: no attenuation found on any tagged source. Ray range falls back to %.0f cm."
		       ),
		       MaxRayDistance);
		return;
	}

	AttenuationInnerRadius = Widest->AttenuationShapeExtents.X;
	BaseAttenuationFalloffDistance = Widest->FalloffDistance;
	MaxRayDistance = WidestRange;
	UE_LOG(LogSpatialAudio, Log, TEXT("SpatialAudioComponent: ray range %.0f cm, from the widest source attenuation."),
	       MaxRayDistance);
}

float USpatialAudioComponent::EvaluateVirtualAttenuationVolumeAt(const float Distance) const {
	if (!bHasVirtualAttenuationSettings || !VirtualAttenuationSettings.bAttenuate) {
		return 1.f - FMath::Clamp(Distance / FMath::Max(MaxRayDistance, 1.f), 0.f, 1.f);
	}
	return VirtualAttenuationSettings.Evaluate(FTransform::Identity, FVector(Distance, 0.f, 0.f));
}

float USpatialAudioComponent::ComputePathAttenuationCurved(const float AvgPathDist,
                                                           const USpatialAudioSettings& S) const {
	return FMath::Clamp((1.f - EvaluateVirtualAttenuationVolumeAt(AvgPathDist)) * S.PathAttenuationStrength,
	                    0.f, 1.f);
}

float USpatialAudioComponent::GetEffectiveAcousticDistance(const FVector& ListenerPos,
                                                           const float DetourOcclusionFloor) const {
	const AActor* Owner = GetOwner();
	const FVector SourcePos = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
	const float DirectDist = static_cast<float>(FVector::Dist(SourcePos, ListenerPos));

	float MinPathDist = TNumericLimits<float>::Max();
	for (const FCachedEdgePoint& Edge : CachedEdgePoints) {
		if (Edge.bEvicting) {
			continue;
		}
		const float Total = Edge.EffectivePathDist()
			+ static_cast<float>(FVector::Dist(Edge.EffectivePoint(), ListenerPos));
		MinPathDist = FMath::Min(MinPathDist, Total);
	}
	if (MinPathDist == TNumericLimits<float>::Max()) {
		return DirectDist;
	}
	return Math::ComputeEffectiveAcousticDistance(DirectDist, MinPathDist, CurrentOcclusion,
	                                              DetourOcclusionFloor);
}

void USpatialAudioComponent::PerformStartupLoSCheck() {
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner) {
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		return;
	}

	const FVector SourcePos = Owner->GetActorLocation();
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();
	const FVector ToListener = (ListenerPos - SourcePos).GetSafeNormal();

	FHitResult Hit;
	if (TraceLine(World, Hit, SourcePos + ToListener * 5.f, ListenerPos)) {
		CurrentOcclusion = 1.f;
		TargetOcclusion = 1.f;
	}
}

float USpatialAudioComponent::TimeToBlendSpeed(const float Seconds) {
	return Seconds > 0.f ? 1.f / Seconds : 1000.f;
}

bool USpatialAudioComponent::HasConfirmedLoSLoss() const {
	return !bHasDirectLoS
		&& NoLoSSampleStreak >= ResolveRingRotationSteps();
}

void USpatialAudioComponent::TickAsyncPipeline(const USpatialAudioSettings& Settings) {
	CurrentTraceBucket = ETraceBucket::Sweep;
	if (Finalize.bPending) {
		FAsyncCastManager::ReadbackFinalizeBatch(*this, Settings);
	}

	const bool bWasAsyncActive = bAsyncCastActive;
	if (bAsyncCastActive) {
		FAsyncCastManager::TickAsyncCast(*this, Settings);
	}

	if (bWasAsyncActive) {
		++TraceDiag.SweepFrameAccum;
		if (!bAsyncCastActive) {
			TraceDiag.LastSweepFrames = TraceDiag.SweepFrameAccum;
			TraceDiag.LastSweepAsyncRays = TraceDiag.SweepAsyncRayAccum;
		}
	}
}

void USpatialAudioComponent::TickNormalSweepDispatch(const float DeltaTime, const bool bInRange,
                                                     const float SweepInterval) {
	CurrentTraceBucket = ETraceBucket::Occlusion;
	FUpdater::TickDirectLoSSampling(*this, DeltaTime, GetSettings());
	CurrentTraceBucket = ETraceBucket::Sweep;

	const bool bPreSweep = IsPreSweepActive();
	RequestSweepOnPreSweepBandEntry(bPreSweep);

	Math::FSweepDispatchState Dispatch;
	Dispatch.bAsyncCastActive = bAsyncCastActive;
	Dispatch.bFinalizePending = Finalize.bPending;
	Dispatch.bInRange = bInRange;
	Dispatch.bPreSweepBand = bPreSweep;
	Dispatch.bConfirmedLoSLoss = HasConfirmedLoSLoss();
	Dispatch.bEarlySweepRequested = SweepScheduling.bEarlySweepRequested;
	Dispatch.TimeSinceFullCast = TimeSinceFullCast;
	Dispatch.SweepInterval = SweepInterval;

	if (Math::ShouldDispatchSweep(Dispatch)) {
		FUpdater::PerformUpdateRayCast(*this, GetSettings());
		if (!bHasDirectLoS || bPreSweep) {
			const bool bWasEarlySweep = SweepScheduling.bEarlySweepRequested;
			TimeSinceFullCast = 0.f;
			SweepScheduling.bEarlySweepRequested = false;
			if (bWasEarlySweep) {
				SweepScheduling.CacheFillSweepsRemaining = GetSettings().CacheFillMaxSweeps;
				for (FCachedEdgePoint& EP : CachedEdgePoints) {
					EP.bNewSinceFillArm = false;
				}
			}
			TraceDiag.SweepStartTime = GetWorld()->GetTimeSeconds();
			TraceDiag.LastSweepInterval = SweepInterval;
			FAsyncCastManager::StartAsyncFullCast(*this, GetSettings());
		}
	}
	else if (!bAsyncCastActive && !Finalize.bPending) {
		FUpdater::PerformUpdateRayCast(*this, GetSettings());
	}
}

void USpatialAudioComponent::TickDirectLoSState(const float DeltaTime) {
	TimeSinceHadDirectLoS = bHasDirectLoS ? 0.f : TimeSinceHadDirectLoS + DeltaTime;

	if (bHasDirectLoS && !IsPreSweepActive()) {
		CachedEdgePoints.Empty();
		SweepScheduling.CacheFillSweepsRemaining = 0;
	}
}

void USpatialAudioComponent::SmoothTowardTargets(const float DeltaTime) {
	CurrentOcclusion = FMath::FInterpConstantTo(CurrentOcclusion, TargetOcclusion, DeltaTime,
	                                            TimeToBlendSpeed(GetSettings().OcclusionBlendTime));
	CurrentPathAttenuation = FMath::FInterpTo(CurrentPathAttenuation, TargetPathAttenuation, DeltaTime,
	                                          TimeToBlendSpeed(GetSettings().PathAttenuationBlendTime));
}

void USpatialAudioComponent::UpdateTraceDiagnostics(const float DeltaTime) {
	if (DeltaTime <= 0.f) {
		return;
	}

	TraceDiag.SmoothedFrameTraces = FMath::FInterpTo(TraceDiag.SmoothedFrameTraces,
	                                                 static_cast<float>(TraceDiag.FrameCount),
	                                                 DeltaTime, 4.f);
	for (int32 b = 0; b < FTraceDiagnostics::BucketCount; ++b) {
		TraceDiag.SmoothedBucketTraces[b] = FMath::FInterpTo(TraceDiag.SmoothedBucketTraces[b],
		                                                     static_cast<float>(TraceDiag.BucketFrameCounts[b]),
		                                                     DeltaTime, 4.f);
	}

	if (VelocityScaling.IsStationary()) {
		TraceDiag.RestTraceAccum += TraceDiag.FrameCount;
		TraceDiag.RestSeconds += DeltaTime;
	}
	else {
		TraceDiag.MovingTraceAccum += TraceDiag.FrameCount;
		TraceDiag.MovingSeconds += DeltaTime;
	}

	TraceDiag.AccumBucket += TraceDiag.FrameCount;
	TraceDiag.SnapshotTimer += DeltaTime;
	if (TraceDiag.SnapshotTimer < 1.f) {
		return;
	}

	TraceDiag.SnapshotTracesPerSec = TraceDiag.AccumBucket / TraceDiag.SnapshotTimer;
	TraceDiag.PeakTracesPerSec = FMath::Max(TraceDiag.PeakTracesPerSec, TraceDiag.SnapshotTracesPerSec);
	TraceDiag.SnapshotTimer = 0.f;
	TraceDiag.AccumBucket = 0;
	TraceDiag.SnapshotFrameTraces = TraceDiag.SmoothedFrameTraces;
	for (int32 b = 0; b < FTraceDiagnostics::BucketCount; ++b) {
		TraceDiag.SnapshotBucketTraces[b] = TraceDiag.SmoothedBucketTraces[b];
	}

	TraceDiag.History[TraceDiag.HistoryHead] = TraceDiag.SnapshotTracesPerSec;
	TraceDiag.HistoryHead = (TraceDiag.HistoryHead + 1) % TraceDiag.HistoryLen;
	if (TraceDiag.HistoryCount < TraceDiag.HistoryLen) {
		++TraceDiag.HistoryCount;
	}
	float Sum10 = 0.f;
	float Sum60 = 0.f;
	for (int32 k = 0; k < TraceDiag.HistoryCount; ++k) {
		const int32 Idx = (TraceDiag.HistoryHead - 1 - k + TraceDiag.HistoryLen) % TraceDiag.HistoryLen;
		Sum60 += TraceDiag.History[Idx];
		if (k < TraceDiag.Avg10Len) {
			Sum10 += TraceDiag.History[Idx];
		}
	}
	const int32 Count10 = FMath::Min(TraceDiag.HistoryCount, TraceDiag.Avg10Len);
	TraceDiag.Avg10Sec = Count10 > 0 ? Sum10 / Count10 : 0.f;
	TraceDiag.Avg60Sec = TraceDiag.HistoryCount > 0 ? Sum60 / TraceDiag.HistoryCount : 0.f;
}

void USpatialAudioComponent::IgnoreActorAndAttachments(AActor* Actor) {
	if (!Actor) {
		return;
	}
	TraceQueryParams.AddIgnoredActor(Actor);
	Actor->GetAttachedActors(AttachedActorScratch, true, true);
	TraceQueryParams.AddIgnoredActors(AttachedActorScratch);
}

/** Rebuilt every tick rather than filled once at BeginPlay: equipment is a separate actor that
 *  attaches and detaches at runtime, and a weapon held by the source blocks the rays leaving it.
 *  The listener is ignored for the same reason, since nobody is occluded by their own body. */
void USpatialAudioComponent::RefreshTraceIgnoreList() {
	TraceQueryParams = FCollisionQueryParams();
	TraceQueryParams.bTraceComplex = false;

	IgnoreActorAndAttachments(GetOwner());

	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	IgnoreActorAndAttachments(PC ? PC->GetPawn() : nullptr);
}

bool USpatialAudioComponent::IsOwnedByListener() const {
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	return Pawn && Pawn == GetOwner();
}

/** Both subsystem membership and source playback follow possession rather than component lifetime.
 *  The pawn the listener hears from must not appear in the source list, take a share of the global
 *  trace budget, or emit through the spatial path at all, and which pawn that is changes as the
 *  player respawns. Applied only on transitions so a finished one-shot is never retriggered. */
void USpatialAudioComponent::SyncListenerOwnership(const bool bOwnedByListener) {
	if (bListenerOwnershipApplied && bOwnedByListener == bSuppressedAsListenerSource) {
		return;
	}
	bListenerOwnershipApplied = true;
	bSuppressedAsListenerSource = bOwnedByListener;

	if (USpatialAudioSubsystem* Subsystem = GetWorld()
		                                        ? GetWorld()->GetSubsystem<USpatialAudioSubsystem>()
		                                        : nullptr) {
		if (bOwnedByListener) {
			Subsystem->Unregister(this);
		}
		else {
			Subsystem->Register(this);
		}
	}

	for (const TWeakObjectPtr<UAudioComponent>& Src : CachedAudioComponentSources) {
		if (UAudioComponent* AC = Src.Get()) {
			if (bOwnedByListener) {
				AC->Stop();
			}
			else {
				AC->Play();
			}
		}
	}
}

/** Occlusion is pinned rather than simply left unwritten: the MetaSound input defaults to fully
 *  occluded, so a source nobody drives would muffle itself. Clearing the cache covers possession
 *  changing which pawn the listener hears from. */
void USpatialAudioComponent::TickOwnListenerSource(const float DeltaTime,
                                                   const USpatialAudioSettings& Settings) {
	TargetOcclusion = 0.f;
	CurrentOcclusion = 0.f;
	CachedEdgePoints.Reset();
	FUpdater::UpdateAudioParameters(*this, DeltaTime, Settings);
}

void USpatialAudioComponent::TickComponent(const float DeltaTime, const ELevelTick TickType,
                                           FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	TraceDiag.FrameCount = 0;
	FMemory::Memzero(TraceDiag.BucketFrameCounts);

	const bool bOwnedByListener = IsOwnedByListener();
	SyncListenerOwnership(bOwnedByListener);
	if (bOwnedByListener) {
		TickOwnListenerSource(DeltaTime, GetSettings());
		return;
	}
	RefreshTraceIgnoreList();

	TickAsyncPipeline(GetSettings());

	UWorld* TickWorld = GetWorld();
	APlayerController* TickPC = TickWorld ? TickWorld->GetFirstPlayerController() : nullptr;
	APawn* TickPawn = TickPC ? TickPC->GetPawn() : nullptr;
	const bool bInRange = TickPawn && GetOwner() &&
		FVector::DistSquared(GetOwner()->GetActorLocation(), TickPawn->GetActorLocation())
		<= FMath::Square(MaxRayDistance);

	bInAudibleRange = bInRange;

	int32 ScaledRayCount;
	GetEffectiveRayCounts(ScaledRayCount, CurrentPriority);

	UpdateVelocityScaling(DeltaTime, bInRange, TickPawn);
	UpdateStationaryIdleState(bInRange, TickPawn);

	FEdgeCache::TickCachedEdgeEviction(*this, DeltaTime, GetSettings());

	const float EffFullSweepInterval = ComputeEffectiveSweepInterval();
	StoredEffFullSweepInterval = EffFullSweepInterval;

	TimeSinceFullCast = bHasDirectLoS && !IsPreSweepActive() ? 0.f : TimeSinceFullCast + DeltaTime;

	const bool bPrevHadDirectLoS = bHasDirectLoS;
	TickNormalSweepDispatch(DeltaTime, bInRange, EffFullSweepInterval);

	if (bPrevHadDirectLoS && !bHasDirectLoS) {
		SweepScheduling.bEarlySweepRequested = true;
	}

	TickDirectLoSState(DeltaTime);
	SmoothTowardTargets(DeltaTime);

	UpdateTraceDiagnostics(DeltaTime);

	if (bDrawDebugRays) {
		DrawDebugVisualization(GetSettings());
	}

	FUpdater::UpdateAudioParameters(*this, DeltaTime, GetSettings());
}


float USpatialAudioComponent::ComputeEffectiveSweepInterval() const {
	Math::FSweepPacingState State;
	State.CurrentPriority = CurrentPriority;
	State.SweepMultiplier = VelocityScaling.SweepMultiplier;
	State.EdgeMultiplier = VelocityScaling.EdgeMultiplier;
	State.bStationary = VelocityScaling.IsStationary();
	State.bCacheFillPending = IsCacheFillPending();
	State.bStationaryIdleMode = SweepScheduling.bStationaryIdleMode;
	State.TraceBudgetStretch = TraceBudgetStretch;
	return Math::ComputeSweepInterval(State, GetSettings());
}

FVector USpatialAudioComponent::ComputeSteeringLead(const FVector& SmoothedVelocity,
                                                    const USpatialAudioSettings& Settings) const {
	const float Lead = Settings.SteeringPredictionLeadTime;
	const bool bRetro = TimeSinceHadDirectLoS <= Lead;
	return SmoothedVelocity * (bRetro ? -Lead : Lead);
}

float USpatialAudioComponent::GetVoiceClusterRadius() const {
	return bHasVirtualAttenuationSettings ? VirtualAttenuationSettings.AttenuationShapeExtents.X : 0.f;
}

bool USpatialAudioComponent::HasNewEdgeSinceFillArm() const {
	for (const FCachedEdgePoint& Edge : CachedEdgePoints) {
		if (Edge.bNewSinceFillArm && !Edge.bRelayed && !Edge.bEvicting) {
			return true;
		}
	}
	return false;
}

bool USpatialAudioComponent::IsCacheFillPending() const {
	return SweepScheduling.CacheFillSweepsRemaining > 0 && !HasNewEdgeSinceFillArm();
}

int32 USpatialAudioComponent::GetEffectiveMaxVirtualVoices() const {
	return FMath::Max(1, Math::ScaleCountByDistancePriority(
		                  GetSettings().MaxVirtualVoices, CurrentPriority,
		                  GetSettings().MaxDistanceEffortScale));
}

int32 USpatialAudioComponent::GetEffectiveCachedEdgeMaxCount() const {
	return FMath::Max(1, Math::ScaleCountByDistancePriority(
		                  GetSettings().CachedEdgeMaxCount, CurrentPriority,
		                  GetSettings().MaxDistanceEffortScale));
}

float USpatialAudioComponent::ComputeIdleAnchorDriftSq() const {
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn || !GetOwner()) {
		return 0.f;
	}
	return FMath::Max(
		static_cast<float>(FVector::DistSquared(GetOwner()->GetActorLocation(),
		                                        SweepScheduling.StationaryIdleSourcePos)),
		static_cast<float>(FVector::DistSquared(Pawn->GetActorLocation(),
		                                       SweepScheduling.StationaryIdleListenerPos)));
}

/** TimeSinceFullCast un-pins from zero on entry, so without this the band spends its whole head
 *  start not sweeping. */
void USpatialAudioComponent::RequestSweepOnPreSweepBandEntry(const bool bPreSweepActive) {
	if (bPreSweepActive && !SweepScheduling.bWasPreSweepActive) {
		SweepScheduling.bEarlySweepRequested = true;
	}
	SweepScheduling.bWasPreSweepActive = bPreSweepActive;
}

void USpatialAudioComponent::GetEffectiveRayCounts(int32& OutFull, float& OutPriority) const {
	OutPriority = 1.f;

	if (const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		PC && PC->GetPawn() && GetOwner()) {
		const float Dist = FVector::Dist(GetOwner()->GetActorLocation(), PC->GetPawn()->GetActorLocation());
		const float ScaleStart = AttenuationInnerRadius * 2.f;
		const float ScaleRange = MaxRayDistance - ScaleStart;
		const float TLinear = (ScaleRange > 0.f && Dist > ScaleStart)
			                      ? FMath::Clamp((Dist - ScaleStart) / ScaleRange, 0.f, 1.f)
			                      : 0.f;
		OutPriority = 1.f - FMath::Pow(TLinear, GetSettings().DistancePriorityExponent);
	}

	OutFull = Math::ScaleCountByDistancePriority(GetSettings().FullSweepRayCount, OutPriority,
	                                             GetSettings().MaxDistanceEffortScale);
}


void USpatialAudioComponent::UpdateVelocityScaling(const float DeltaTime, const bool bInRange, const APawn* Pawn) {
	if (bInRange && Pawn && GetOwner() && DeltaTime > 0.f) {
		const FVector SrcPos = GetOwner()->GetActorLocation();
		const FVector LisPos = Pawn->GetActorLocation();
		if (VelocityScaling.bPosSet) {
			const float SrcSpeed = FVector::Dist(SrcPos, VelocityScaling.LastSourcePos) / DeltaTime;
			const float LisSpeed = FVector::Dist(LisPos, VelocityScaling.LastListenerPos) / DeltaTime;
			VelocityScaling.SmoothedSourceSpeed = FMath::FInterpTo(VelocityScaling.SmoothedSourceSpeed, SrcSpeed,
			                                                       DeltaTime, 5.f);
			VelocityScaling.SmoothedListenerSpeed = FMath::FInterpTo(VelocityScaling.SmoothedListenerSpeed, LisSpeed,
			                                                         DeltaTime, 5.f);
			VelocityScaling.SmoothedCombinedSpeed = VelocityScaling.SmoothedSourceSpeed + VelocityScaling.
				SmoothedListenerSpeed;
			VelocityScaling.SmoothedSourceVelocity = FMath::VInterpTo(
				VelocityScaling.SmoothedSourceVelocity, (SrcPos - VelocityScaling.LastSourcePos) / DeltaTime, DeltaTime,
				5.f);
			VelocityScaling.SmoothedListenerVelocity = FMath::VInterpTo(
				VelocityScaling.SmoothedListenerVelocity, (LisPos - VelocityScaling.LastListenerPos) / DeltaTime,
				DeltaTime, 5.f);
		}
		VelocityScaling.bPosSet = true;
		VelocityScaling.LastSourcePos = SrcPos;
		VelocityScaling.LastListenerPos = LisPos;
	}
	else if (!bInRange) {
		VelocityScaling.SmoothedSourceSpeed = VelocityScaling.SmoothedListenerSpeed = VelocityScaling.
			SmoothedCombinedSpeed = 0.f;
		VelocityScaling.SmoothedSourceVelocity = VelocityScaling.SmoothedListenerVelocity = FVector::ZeroVector;
	}

	const float MaxSpeed = GetSettings().VelocityScaleMaxSpeed;
	const float MinScale = FMath::Max(0.05f, GetSettings().MinSweepIntervalScale);
	const float VelocityFraction = MaxSpeed > 0.f
		                               ? FMath::Clamp(VelocityScaling.SmoothedCombinedSpeed / MaxSpeed, 0.f, 1.f)
		                               : 0.f;
	const float SourceVelocityFraction = MaxSpeed > 0.f
		                                     ? FMath::Clamp(VelocityScaling.SmoothedSourceSpeed / MaxSpeed, 0.f, 1.f)
		                                     : 0.f;
	const float ListenerVelocityFraction = MaxSpeed > 0.f
		                                       ? FMath::Clamp(VelocityScaling.SmoothedListenerSpeed / MaxSpeed, 0.f,
		                                                      1.f)
		                                       : 0.f;
	VelocityScaling.SweepMultiplier = FMath::Lerp(1.f, MinScale, SourceVelocityFraction);
	VelocityScaling.EdgeMultiplier = FMath::Lerp(1.f, MinScale, ListenerVelocityFraction);
	VelocityScaling.OffsetLoSMultiplier = FMath::Lerp(
		1.f, FMath::Max(0.05f, GetSettings().OffsetLoSVelocityScale), VelocityFraction);
}

void USpatialAudioComponent::UpdateStationaryIdleState(const bool bInRange, const APawn* Pawn) {
	if (!SweepScheduling.bStationaryIdleMode || !bInRange || !Pawn || !GetOwner()) {
		return;
	}
	if (ComputeIdleAnchorDriftSq() <= FMath::Square(GetSettings().StationaryIdleBreakDist)) {
		return;
	}

	SweepScheduling.bStationaryIdleMode = false;
	/** Only an early sweep arms the cache fill, which is the point of having moved somewhere new. */
	SweepScheduling.bEarlySweepRequested = true;
}
