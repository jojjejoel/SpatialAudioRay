#include "Audio/Updater.h"
#include "Audio/Math.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/SpatialAudioSettings.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

namespace {
	int32 AcquireVirtualVoiceSlot(const TArray<FVirtualSlot>& Slots) {
		int32 Best = INDEX_NONE;
		float BestAlpha = TNumericLimits<float>::Max();
		for (int32 i = 0; i < Slots.Num(); ++i) {
			if (Slots[i].State == FVirtualSlot::EState::Idle) {
				return i;
			}
			// A shortened fade beats ever creating a component at runtime.
			if (Slots[i].State == FVirtualSlot::EState::FadingOut && Slots[i].FadeAlpha < BestAlpha) {
				BestAlpha = Slots[i].FadeAlpha;
				Best = i;
			}
		}
		return Best;
	}
}

bool FUpdater::TryResolveCastContext(const USpatialAudioComponent& Component, FCastContext& OutContext) {
	OutContext.World = Component.GetWorld();
	const AActor* OwnerActor = Component.GetOwner();
	if (!OutContext.World || !OwnerActor) {
		return false;
	}

	const APlayerController* PC = OutContext.World->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn) {
		return false;
	}

	OutContext.SourcePos = OwnerActor->GetActorLocation();
	OutContext.ListenerPos = Pawn->GetActorLocation();
	return true;
}

// Clamped so the point hugs the wall as the player closes in, instead of being excluded.
FVector FUpdater::ResolveOffsetPoint(const USpatialAudioComponent& Component, const UWorld* World,
                                     const FVector& ListenerPos, const FVector& CandidatePoint) {
	FHitResult H;
	if (Component.TraceLine(World, H, ListenerPos, CandidatePoint)) {
		const FVector ToListener = (ListenerPos - H.Location).GetSafeNormal();
		return H.Location + ToListener * 5.f;
	}
	return CandidatePoint;
}

// The source plays at full volume inside its inner radius, so seeing any of that sphere counts
// as seeing it. The centre is one vote of five, so a pinhole reads as mostly occluded. The
// denominator stays 5 rather than the valid-point count: a blocked ring point cannot see the
// source either, which keeps the fraction about visibility, not about where the player stands.
float FUpdater::SyncOffsetLoSFraction(USpatialAudioComponent& Component, UWorld* World,
                                      const FVector& SourcePos, const FVector& ListenerPos,
                                      float OffsetR, float SourceR, float SourceRingR,
                                      float RingStepRad) {
	const FVector ToListenerDir = (ListenerPos - SourcePos).GetSafeNormal();

	// Inside the sphere is clear by definition. The floor doubles as the guard for SourceR 0.
	auto SampleClear = [&Component, World, &SourcePos, SourceR](const FVector& From, const FVector& End) {
		if (FVector::Dist(From, SourcePos) <= FMath::Max(SourceR, UE_KINDA_SMALL_NUMBER)) {
			return true;
		}
		FHitResult Hit;
		return !Component.TraceLine(World, Hit, From, End);
	};
	auto SphereCapPoint = [&SourcePos, &ToListenerDir, SourceR](const FVector& RingDir, float LateralR) {
		const float Lift = FMath::Sqrt(FMath::Max(SourceR * SourceR - LateralR * LateralR, 0.f));
		return SourcePos + RingDir * LateralR + ToListenerDir * Lift;
	};

	const FVector CenterEnd = SphereCapPoint(FVector::ZeroVector, 0.f);
	const bool bCenterClear = SampleClear(ListenerPos, CenterEnd);

	// Zero radius on both rings would collapse every sample onto the center trace.
	if (OffsetR <= 0.f && SourceRingR <= 0.f) {
		return bCenterClear ? 1.f : 0.f;
	}

	FVector RightDir = FVector::CrossProduct(ToListenerDir, FVector::UpVector).GetSafeNormal();
	if (RightDir.IsNearlyZero()) {
		RightDir = FVector::CrossProduct(ToListenerDir, FVector::RightVector).GetSafeNormal();
	}
	const FVector RingUpDir = FVector::CrossProduct(RightDir, ToListenerDir).GetSafeNormal();

	// The 4-point cross is symmetric under 90 degrees, so the pattern repeats exactly every
	// 90/RingStepRad checks and a stationary scene resamples identical directions each period.
	Component.OffsetRingAngle = FMath::Fmod(Component.OffsetRingAngle + RingStepRad, UE_HALF_PI);

	const float LateralR = FMath::Min(SourceRingR, SourceR);
	int32 Clear = bCenterClear ? 1 : 0;
	FVector Pts[4];
	FVector SrcPts[4];
	bool bClearArr[4] = {};
	for (int32 i = 0; i < 4; ++i) {
		const float Angle = Component.OffsetRingAngle + i * UE_HALF_PI;
		const FVector RingDir = FMath::Cos(Angle) * RightDir + FMath::Sin(Angle) * RingUpDir;
		Pts[i] = OffsetR > 0.f
			         ? ResolveOffsetPoint(Component, World, ListenerPos, ListenerPos + RingDir * OffsetR)
			         : ListenerPos;
		// Same world direction as its listener point, so head-on the rays stay parallel.
		SrcPts[i] = SphereCapPoint(RingDir, LateralR);
		bClearArr[i] = SampleClear(Pts[i], SrcPts[i]);
		if (bClearArr[i]) {
			++Clear;
		}
	}
	if (Clear > 0 && Component.bDrawDebugRays && Component.bShowOffsetLoSChecks) {
		DrawDebugLine(World, CenterEnd, ListenerPos, bCenterClear ? FColor::Green : FColor::Red,
		              false, Component.GetSettings().DebugLineDuration, 0, 0.75f);
		for (int32 i = 0; i < 4; ++i) {
			DrawDebugLine(World, SrcPts[i], Pts[i], bClearArr[i] ? FColor::Green : FColor::Red,
			              false, Component.GetSettings().DebugLineDuration, 0, 0.75f);
		}
	}
	return static_cast<float>(Clear) / 5.f;
}

void FUpdater::TrySampleOffsetLoS(USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings,
                                  float DeltaTime, const FVector& SourcePos, const FVector& ListenerPos,
                                  int32 RotationSteps) {
	Component.OffsetLoSCheckTimer += DeltaTime;
	const float CheckInterval = Settings.OffsetLoSCheckInterval * Component.VelocityScaling.OffsetLoSMultiplier;
	if (Component.OffsetLoSCheckTimer < CheckInterval) {
		return;
	}
	Component.OffsetLoSCheckTimer = 0.f;

	// Each check samples one annulus, innermost first, rather than always the rim: rim-only
	// pinned the fraction at 1/5 with a large radius and a small opening. OffsetRingRadiusExponent
	// shapes the ladder (0.5 gives equal-area annuli). The sphere radius itself never ladders.
	const float RadiusScale = FMath::Pow((Component.LoSSlotIndex + 1.f) / RotationSteps,
	                                     FMath::Max(Settings.OffsetRingRadiusExponent, 0.f));
	const float OffsetR = Settings.DirectLoSSampleRadius * RadiusScale;
	const float SourceR = Component.AttenuationInnerRadius * Settings.SourceLoSSampleRadiusScale;
	Component.LastOffsetLoSFraction = SyncOffsetLoSFraction(
		Component, World, SourcePos, ListenerPos, OffsetR, SourceR, SourceR * RadiusScale,
		UE_HALF_PI / RotationSteps);

	if (!Component.bLoSFractionSeeded) {
		// Fill every slot, or untraced slots drag the average toward occluded before one rotation.
		for (float& Slot : Component.LoSSlotFractions) {
			Slot = Component.LastOffsetLoSFraction;
		}
		Component.bLoSFractionSeeded = true;
		Component.LastDirectLoSFraction = Component.LastOffsetLoSFraction;
	} else {
		Component.LoSSlotFractions[Component.LoSSlotIndex] = Component.LastOffsetLoSFraction;
	}
	Component.LoSSlotIndex = (Component.LoSSlotIndex + 1) % RotationSteps;

	float SlotSum = 0.f;
	for (int32 i = 0; i < RotationSteps; ++i) {
		SlotSum += Component.LoSSlotFractions[i];
	}
	Component.WindowedLoSFraction = SlotSum / RotationSteps;

	Component.NoLoSSampleStreak = Component.LastOffsetLoSFraction > 0.f
		                              ? 0
		                              : Component.NoLoSSampleStreak + 1;
}

void FUpdater::UpdateSmoothedOcclusionFromSamples(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                                   float DeltaTime, int32 RotationSteps) {
	// Recomputed as soon as one slot refreshes, so occlusion moves mid-rotation instead of freezing.
	const float PatternLoSFraction = Component.WindowedLoSFraction;

	// Scaled by the same multiplier as the check interval, so smoothing stays proportional. Gating
	// stays on the raw sample below: a smoothed decay must not suppress sweeps after real loss.
	const float EffSmoothingTime = Settings.LoSFractionSmoothingTime * Component.VelocityScaling.OffsetLoSMultiplier;
	const float DirectLoSFraction = EffSmoothingTime > 0.f
		? FMath::FInterpTo(Component.LastDirectLoSFraction, PatternLoSFraction,
		                   DeltaTime, 1.f / EffSmoothingTime)
		: PatternLoSFraction;

	// At the boundary the ring alternates between patterns that do and do not contain the one
	// marginal clear direction, which flip-flopped playback. Gaining LoS and movement stay instant.
	const bool bHoldLoSThroughRotation = Component.bHasDirectLoS && Component.VelocityScaling.IsStationary()
		&& Component.NoLoSSampleStreak < RotationSteps;
	Component.bHasDirectLoS = Component.LastOffsetLoSFraction > 0.f || bHoldLoSThroughRotation;
	Component.LastDirectLoSFraction = DirectLoSFraction;

	// How muffled the diffracted path sounds is TargetPathAttenuation's job, not this one's.
	Component.TargetOcclusion = 1.f - DirectLoSFraction;
}

void FUpdater::TickDirectLoSSampling(USpatialAudioComponent& Component, const float DeltaTime,
                                     const USpatialAudioSettings& Settings) {
	FCastContext Context;
	if (!TryResolveCastContext(Component, Context) || IsOutOfRange(Context, Component.MaxRayDistance)) {
		return;
	}
	UWorld* World = Context.World;
	const FVector SourcePos = Context.SourcePos;
	const FVector ListenerPos = Context.ListenerPos;

	const int32 RotationSteps = FMath::Clamp(Settings.OffsetRingRotationSteps, 1, 8);

	TrySampleOffsetLoS(Component, World, Settings, DeltaTime, SourcePos, ListenerPos, RotationSteps);
	UpdateSmoothedOcclusionFromSamples(Component, Settings, DeltaTime, RotationSteps);

	if (Component.bDrawDebugRays && Component.bShowEdgePoints) {
		const bool bFullyClear = Component.LastOffsetLoSFraction >= 1.f;
		const FColor LoSColor = bFullyClear ? FColor::Green : FColor(255, 165, 0);
		DrawDebugSphere(World, SourcePos, 8.f, 6, LoSColor, false, Settings.DebugLineDuration, SDPG_Foreground);
		DrawDebugSphere(World, ListenerPos, 8.f, 6, LoSColor, false, Settings.DebugLineDuration, SDPG_Foreground);
		if (bFullyClear) {
			DrawDebugLine(World, SourcePos, ListenerPos, FColor::Green, false,
			              Settings.DebugLineDuration, 0, 1.f);
		}
	}
}

FUpdater::FEdgeWeightAccum FUpdater::AccumulateCachedEdgeWeights(USpatialAudioComponent& Component, const UWorld* World,
                                                                  const USpatialAudioSettings& Settings,
                                                                  const FVector& ListenerPos) {
	FEdgeWeightAccum Accum;
	for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
		const FCachedEdgePoint& Ep = Component.CachedEdgePoints[i];

		// The source-side weight must stay listener independent. The position weight adds listener
		// falloff, since proximity may steer where the emitter sits but never how loud it is.
		const float SrcW = Ep.EvictionAlpha / (1.f + Settings.CandidateDistanceFalloff
			* Ep.GeomDist / FMath::Max(Component.MaxRayDistance, 1.f));
		const float PosW = SrcW / (1.f + Settings.ListenerDistanceFalloff
			* FVector::Dist(ListenerPos, Ep.EffectivePoint()) / FMath::Max(Component.MaxRayDistance, 1.f));
		Accum.WeightedPos += Ep.EmitterPoint(Settings.VirtualSourcePullbackDistance) * PosW;
		Accum.PosWeightTotal += PosW;
		Accum.WeightedDistSum += Ep.EffectivePathDist() * SrcW;
		Accum.SrcWeightTotal += SrcW;

		if (Component.bDrawDebugRays && Component.bShowEdgePoints) {
			DrawDebugLine(World, Ep.EffectivePoint(), ListenerPos, FColor::Cyan, false, Settings.DebugLineDuration, 0,
			              1.f);
		}
	}
	return Accum;
}

void FUpdater::ClearCacheOnConfirmedDirectLoS(USpatialAudioComponent& Component,
                                              const USpatialAudioSettings& Settings) {
	if (Component.bHasDirectLoS) {
		Component.LoSDiffractionPaths.Reset();
		if (Component.DirectLoSConfirmedDuration >= Settings.DirectLoSConfirmTime
			&& !Component.IsPreSweepActive()) {
			Component.CachedEdgePoints.Reset();
		}
	}
}

// No weighted edge means no clusters and so no voices, so nothing audible is positioned from this.
// Following the source beats holding at a corner that no longer has a cached path through it.
void FUpdater::UpdateVirtualSourceTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
                                         const FVector& SourcePos) {
	Component.TargetVirtualSourceLocation = Accum.PosWeightTotal > 0.f
		                                        ? Accum.WeightedPos / Accum.PosWeightTotal
		                                        : SourcePos;
}

void FUpdater::UpdatePathAttenuationTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
                                           const USpatialAudioSettings& Settings, const FVector& SourcePos,
                                           const bool bVirtualPathActive) {
	if (!bVirtualPathActive) {
		if (Component.bHasDirectLoS && Component.DirectLoSConfirmedDuration >= Settings.DirectLoSConfirmTime) {
			Component.TargetPathAttenuation = 0.f;
		}
		return;
	}

	// Every frame, since EvictionAlpha decays every frame; an empty cache keeps the last value.
	if (Accum.SrcWeightTotal > 0.f) {
		Component.CurrentSourceToVirtualDistance = Accum.WeightedDistSum / Accum.SrcWeightTotal;
	}
	const float Leg1Geom = FVector::Dist(SourcePos, Component.TargetVirtualSourceLocation);
	Component.TargetPathAttenuation = Component.ComputePathAttenuationCurved(
		Component.CurrentSourceToVirtualDistance, Leg1Geom, Settings);
}

void FUpdater::PerformUpdateRayCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	FCastContext Context;
	if (!TryResolveCastContext(Component, Context) || IsOutOfRange(Context, Component.MaxRayDistance)) {
		return;
	}
	UWorld* World = Context.World;
	const FVector SourcePos = Context.SourcePos;
	const FVector ListenerPos = Context.ListenerPos;

	ClearCacheOnConfirmedDirectLoS(Component, Settings);

	// Active through the pre-sweep band too: the crossfade gate starts opening before LoS fully
	// drops, so voices must already be clustered or the gate opens onto an empty list.
	const bool bVirtualPathActive = !Component.bHasDirectLoS || Component.IsPreSweepActive();

	FEdgeWeightAccum Accum;
	if (bVirtualPathActive) {
		Accum = AccumulateCachedEdgeWeights(Component, World, Settings, ListenerPos);
	}

	UpdateVirtualSourceTarget(Component, Accum, SourcePos);
	UpdatePathAttenuationTarget(Component, Accum, Settings, SourcePos, bVirtualPathActive);

	TArray<FEdgeCluster> VoiceClusters;
	if (bVirtualPathActive) {
		Math::ClusterEdgePoints(Component.CachedEdgePoints, Settings.VirtualVoiceClusterRadius,
		                        Settings.CandidateDistanceFalloff, ListenerPos,
		                        Settings.ListenerDistanceFalloff, Component.MaxRayDistance,
		                        Settings.VirtualSourcePullbackDistance,
		                        Settings.MaxVirtualVoices, VoiceClusters);
	}
	SyncVirtualVoicesToClusters(Component, VoiceClusters, Settings);
}

TArray<FUpdater::FDesired> FUpdater::BuildDesiredVoices(const USpatialAudioComponent& Component,
                                                         const TArray<FEdgeCluster>& Clusters,
                                                         const USpatialAudioSettings& Settings) {
	TArray<FDesired> Desired;

	// No fallback voice: an empty cache means silence rather than a guessed position.
	const FVector ActorPos = Component.GetOwner()->GetActorLocation();
	float TotalWeight = 0.f;
	for (const FEdgeCluster& Cluster : Clusters) {
		TotalWeight += Cluster.TotalWeight;
	}
	for (const FEdgeCluster& Cluster : Clusters) {
		const float Leg1Geom = FVector::Dist(ActorPos, Cluster.Centroid);
		Desired.Add({Cluster.Centroid, Cluster.PathDist,
		             Component.ComputePathAttenuationCurved(Cluster.PathDist, Leg1Geom, Settings),
		             Cluster.TotalWeight / FMath::Max(TotalWeight, KINDA_SMALL_NUMBER)});
	}

	return Desired;
}

void FUpdater::MatchVoicesToDesired(const TArray<FVirtualVoice>& Voices, TArray<FDesired>& Desired,
                                    const USpatialAudioSettings& Settings, TArray<bool>& OutVoiceClaimed) {
	struct FMatchPair {
		float DistSq;
		int32 DesiredIdx;
		int32 VoiceIdx;
	};
	TArray<FMatchPair> Pairs;
	const float GlideMaxSq = FMath::Square(Settings.VirtualVoiceGlideMaxDistance);
	for (int32 D = 0; D < Desired.Num(); ++D) {
		for (int32 V = 0; V < Voices.Num(); ++V) {
			if (!Voices[V].bActive) {
				continue;
			}
			// Compare against where the voice is heading, or fast cluster drift reads as a jump.
			const float DistSq = FVector::DistSquared(Voices[V].TargetPosition, Desired[D].Position);
			if (DistSq <= GlideMaxSq) {
				Pairs.Add({DistSq, D, V});
			}
		}
	}
	Pairs.Sort([](const FMatchPair& A, const FMatchPair& B) { return A.DistSq < B.DistSq; });

	OutVoiceClaimed.Init(false, Voices.Num());
	for (const FMatchPair& P : Pairs) {
		if (Desired[P.DesiredIdx].MatchedVoice != INDEX_NONE || OutVoiceClaimed[P.VoiceIdx]) {
			continue;
		}
		Desired[P.DesiredIdx].MatchedVoice = P.VoiceIdx;
		OutVoiceClaimed[P.VoiceIdx] = true;
	}
}

void FUpdater::FadeOutUnmatchedVoices(USpatialAudioComponent& Component, TArray<FVirtualVoice>& Voices,
                                      const TArray<bool>& VoiceClaimed) {
	for (int32 V = 0; V < Voices.Num(); ++V) {
		if (Voices[V].bActive && !VoiceClaimed[V]) {
			// Fade out in place rather than sweeping the component through space.
			if (Component.VirtualSlots.IsValidIndex(Voices[V].SlotIndex)) {
				FVirtualSlot& Slot = Component.VirtualSlots[Voices[V].SlotIndex];
				Slot.State = FVirtualSlot::EState::FadingOut;
				Slot.VoiceIndex = INDEX_NONE;
			}
			Voices[V] = FVirtualVoice{};
		}
	}
}

void FUpdater::AssignDesiredToVoices(USpatialAudioComponent& Component, TArray<FVirtualVoice>& Voices,
                                     TArray<FDesired>& Desired) {
	for (FDesired& D : Desired) {
		int32 V = D.MatchedVoice;
		if (V == INDEX_NONE) {
			for (int32 i = 0; i < Voices.Num(); ++i) {
				if (!Voices[i].bActive) {
					V = i;
					break;
				}
			}
			if (V == INDEX_NONE) {
				continue;
			}
			const int32 SlotIdx = AcquireVirtualVoiceSlot(Component.VirtualSlots);
			if (SlotIdx == INDEX_NONE) {
				continue;
			}

			FVirtualVoice& NewVoice = Voices[V];
			NewVoice = FVirtualVoice{};
			NewVoice.bActive = true;
			NewVoice.SmoothedPosition = D.Position;
			NewVoice.SlotIndex = SlotIdx;

			FVirtualSlot& Slot = Component.VirtualSlots[SlotIdx];
			// A stolen slot carries its alpha into the fade-in so gain stays continuous.
			const float CarriedAlpha = Slot.State == FVirtualSlot::EState::FadingOut ? Slot.FadeAlpha : 0.f;
			Slot = FVirtualSlot{};
			Slot.State = FVirtualSlot::EState::FadingIn;
			Slot.FadeAlpha = CarriedAlpha;
			Slot.VoiceIndex = V;
		}

		FVirtualVoice& Voice = Voices[V];
		Voice.TargetPosition = D.Position;
		Voice.PathDist = D.PathDist;
		Voice.TargetWeightShare = D.WeightShare;
		Voice.TargetPathAttenuation = D.PathAttenuation;
		if (D.MatchedVoice == INDEX_NONE) {
			// The slot's fade-in envelope is the ramp. Starting PathAttenuation at 0 fades in loud.
			Voice.CurrentPathAttenuation = Voice.TargetPathAttenuation;
			Voice.CurrentWeightShare = Voice.TargetWeightShare;
		}
	}
}

void FUpdater::SyncVirtualVoicesToClusters(USpatialAudioComponent& Component,
                                           const TArray<FEdgeCluster>& Clusters,
                                           const USpatialAudioSettings& Settings) {
	TArray<FVirtualVoice>& Voices = Component.VirtualVoices;
	if (Voices.IsEmpty() || Component.VirtualSlots.IsEmpty()) {
		return;
	}

	TArray<FDesired> Desired = BuildDesiredVoices(Component, Clusters, Settings);

	TArray<bool> VoiceClaimed;
	MatchVoicesToDesired(Voices, Desired, Settings, VoiceClaimed);

	FadeOutUnmatchedVoices(Component, Voices, VoiceClaimed);

	AssignDesiredToVoices(Component, Voices, Desired);
}
