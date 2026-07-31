#include "Audio/Updater.h"
#include "Audio/Math.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/SpatialAudioSettings.h"
#include "Audio/AsyncCastManager.h"

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

// Lets the sample point hug the wall as the player closes in on it, rather than always being
// excluded once the fixed-radius point ends up embedded.
FVector FUpdater::ResolveOffsetPoint(const USpatialAudioComponent& Component, const UWorld* World,
                                     const FVector& ListenerPos, const FVector& CandidatePoint) {
	FHitResult H;
	if (Component.TraceLine(World, H, ListenerPos, CandidatePoint)) {
		const FVector ToListener = (ListenerPos - H.Location).GetSafeNormal();
		return H.Location + ToListener * 5.f;
	}
	return CandidatePoint;
}

// The source plays at full volume anywhere inside its inner radius, so seeing any of that
// sphere's surface counts as seeing the source. The centre is deliberately one vote of five, so
// LoS through a small hole reads as mostly occluded rather than clear.
// The denominator is fixed at 5 rather than the count of valid points: a ring point whose own
// path from the listener is blocked cannot see the source either, so it counts as not clear
// instead of being excluded. That keeps the fraction a function of visibility, not of where the
// player happens to stand.
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
		// Offsets in the same world direction as its listener point, so head-on the rays stay
		// parallel and sample the joint aperture.
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

	// Each check samples one annulus of the listener disc, innermost first, rather than always the
	// rim. Rim-only sampling pinned the fraction at 1/5 with a large radius and a small opening.
	// OffsetRingRadiusExponent shapes the ladder: 0.5 gives equal-area annuli, higher pulls them
	// inward so the centre view weighs more. The sphere radius itself never ladders.
	const float RadiusScale = FMath::Pow((Component.LoSSlotIndex + 1.f) / RotationSteps,
	                                     FMath::Max(Settings.OffsetRingRadiusExponent, 0.f));
	const float OffsetR = Settings.bEnableOffsetLoSChecks
		                      ? Settings.DirectLoSSampleRadius * RadiusScale
		                      : 0.f;
	const float SourceR = Settings.bEnableOffsetLoSChecks
		                      ? Component.AttenuationInnerRadius * Settings.SourceLoSSampleRadiusScale
		                      : 0.f;
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
	// Recomputed as soon as any one slot refreshes rather than on a completed rotation, so
	// occlusion can move mid-rotation instead of freezing for a full cycle.
	const float PatternLoSFraction = Component.WindowedLoSFraction;

	// Scaled by the same multiplier as the check interval, so smoothing stays proportional to how
	// often the fraction actually steps. Gating stays on the raw sample further down: a smoothed
	// decay must not keep sweeps suppressed after LoS is really gone.
	const float EffSmoothingTime = Settings.LoSFractionSmoothingTime * Component.VelocityScaling.OffsetLoSMultiplier;
	const float DirectLoSFraction = EffSmoothingTime > 0.f
		? FMath::FInterpTo(Component.LastDirectLoSFraction, PatternLoSFraction,
		                   DeltaTime, 1.f / EffSmoothingTime)
		: PatternLoSFraction;

	// At the boundary the rotating ring alternates between patterns that do and do not contain the
	// one marginal clear direction, which flip-flopped playback. Gaining LoS stays instant, and
	// movement still drops it immediately.
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

		// The source-side weight drives the path-distance average and must stay listener
		// independent. The position weight adds listener falloff on top, since listener proximity
		// may steer where the virtual source sits but never how loud or muffled it is.
		const float SrcW = Ep.EvictionAlpha / (1.f + Settings.CandidateDistanceFalloff
			* Ep.GeomDist / FMath::Max(Component.MaxRayDistance, 1.f));
		const float PosW = SrcW / (1.f + Settings.ListenerDistanceFalloff
			* FVector::Dist(ListenerPos, Ep.EffectivePoint()) / FMath::Max(Component.MaxRayDistance, 1.f));
		Accum.WeightedPos += Ep.EmitterPoint(Settings.VirtualSourcePullbackDistance) * PosW;
		Accum.PosWeightTotal += PosW;
		Accum.WeightedDistSum += Ep.EffectivePathDist() * SrcW;
		Accum.SrcWeightTotal += SrcW;
		++Accum.RaysReached;

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
			Component.CachedEdgeDirIndices.Reset();
		}
	}
	if (!Settings.bCacheEdgePoints) {
		Component.CachedEdgePoints.Reset();
	}
}

void FUpdater::UpdateVirtualSourceTarget(USpatialAudioComponent& Component, const FEdgeWeightAccum& Accum,
                                         const FVector& SourcePos) {
	if (Accum.PosWeightTotal > 0.f) {
		Component.TargetVirtualSourceLocation = Accum.WeightedPos / Accum.PosWeightTotal;
		Component.LastKnownEdgePoint = Component.CurrentVirtualSourceLocation;
		Component.bHasKnownEdge = true;
		return;
	}
	Component.TargetVirtualSourceLocation = Component.bHasKnownEdge ? Component.LastKnownEdgePoint : SourcePos;
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

	// Tracks the cache every frame rather than only on sweep completion, since EvictionAlpha decays
	// every frame. An empty cache keeps the last sweep-derived distance.
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

	// Active through the pre-sweep band as well as full occlusion: the crossfade gate starts
	// opening before LoS fully drops, so the voices must already be weighted and clustered by then
	// or the gate opens onto an empty voice list.
	const bool bVirtualPathActive = !Component.bHasDirectLoS || Component.IsPreSweepActive();

	FEdgeWeightAccum Accum;
	if (Settings.bCacheEdgePoints && bVirtualPathActive) {
		Accum = AccumulateCachedEdgeWeights(Component, World, Settings, ListenerPos);
	}

	Component.AudioDiag.UpdateCachedEdges = Accum.RaysReached;
	Component.AudioDiag.UpdateDirectDist = FVector::Dist(SourcePos, ListenerPos);

	UpdateVirtualSourceTarget(Component, Accum, SourcePos);
	UpdatePathAttenuationTarget(Component, Accum, Settings, SourcePos, bVirtualPathActive);

	TArray<FEdgeCluster> VoiceClusters;
	if (Settings.bCacheEdgePoints && bVirtualPathActive) {
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

	if (!Clusters.IsEmpty()) {
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
	}
	else if (!Settings.bCacheEdgePoints) {
		// Only for the cache-off configuration, where clusters can never form. With caching on, an
		// empty cache means no voice: every audible virtual path must be backed by a verified edge.
		Desired.Add({Component.TargetVirtualSourceLocation,
		             Component.CurrentSourceToVirtualDistance,
		             Component.TargetPathAttenuation, 1.f});
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
			// Compare against where the voice is heading, or a cluster drifting faster than the
			// position smoothing reads as a jump.
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

FVector FUpdater::PickBiasedLaunchDirection(const USpatialAudioComponent& Component,
                                            const USpatialAudioSettings& Settings,
                                            const FVector& ToListenerDir, const float SteerDist,
                                            const bool bBias) {
	const FVector Fallback = FMath::VRand();
	if (!bBias) {
		return Fallback;
	}
	for (int32 Attempt = 0; Attempt < 30; ++Attempt) {
		const FVector Candidate = FMath::VRand();
		if (FMath::FRand() < Math::ComputeRayDirectionWeight(Candidate, ToListenerDir,
		                                                     Component.LastDirectLoSFraction,
		                                                     Settings.DirectLoSSampleRadius, SteerDist)) {
			return Candidate;
		}
	}
	return Fallback;
}

bool FUpdater::TryConfirmLoSAtPoint(const USpatialAudioComponent& Component, const UWorld* World, const FVector& Point,
                                    const FVector& ListenerPos, const float CumDist, const float MaxPathDist,
                                    FLoSBreakRayResult& OutResult) {
	if (!Math::IsWithinPathBudget(CumDist, Point, ListenerPos, MaxPathDist)) {
		return false;
	}
	FHitResult LoSHit;
	if (Component.TraceLine(World, LoSHit, Point, ListenerPos)) {
		return false;
	}
	OutResult.bFound = true;
	OutResult.Point = Point;
	OutResult.CumDist = CumDist;
	return true;
}

bool FUpdater::TrySampleSegmentForLoS(const USpatialAudioComponent& Component, const UWorld* World,
                                      const USpatialAudioSettings& Settings, const FVector& SegOrigin,
                                      const FVector& SegDir, const float SegLen, const float SegStartCumDist,
                                      const FVector& ListenerPos, const float MaxPathDist,
                                      FLoSBreakRayResult& OutResult) {
	if (SegLen <= 1.f) {
		return false;
	}
	FVector LoSPoint;
	float LoSDist;
	if (!Component.StepSampleSegmentForLoS(SegOrigin, SegDir, SegLen, SegStartCumDist, ListenerPos,
	                                       MaxPathDist, Settings, World, LoSPoint, LoSDist)) {
		return false;
	}
	OutResult.bFound = true;
	OutResult.Point = LoSPoint;
	OutResult.CumDist = LoSDist;
	return true;
}

FUpdater::FLoSBreakRayResult FUpdater::TraceSingleLoSBreakRay(
	const USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings,
	const FVector& SourcePos, const FVector& ListenerPos, const FVector& ToListenerDir, bool bBias,
	int32 EffMaxBounces, float MaxPathDist, float SteerDist, int32 RayIndex) {
	FLoSBreakRayResult Result;

	FVector Origin = SourcePos;
	FVector Dir = PickBiasedLaunchDirection(Component, Settings, ToListenerDir, SteerDist, bBias);
	float CumDist = 0.f;
	int32 BounceIdx = 0;
	bool bNextHitCrawls = (RayIndex % 2 == 0);

	for (int32 Bounce = 0; Bounce <= EffMaxBounces; ++Bounce) {
		if (TryConfirmLoSAtPoint(Component, World, Origin, ListenerPos, CumDist, MaxPathDist, Result)) {
			break;
		}
		if (!Math::IsWithinPathBudget(CumDist, Origin, ListenerPos, MaxPathDist)) {
			break;
		}

		const float RemainingBudget = Math::ComputeNextSegmentLength(
			Component.MaxRayDistance, MaxPathDist - CumDist, Settings.MaxStraightFlightDistance);
		if (RemainingBudget < 1.f) {
			break;
		}

		const FVector SegStart = Origin;
		const float SegStartCumDist = CumDist;
		FHitResult RayHit;
		const FVector RayEnd = Origin + Dir * RemainingBudget;

		if (!Component.TraceLine(World, RayHit, Origin, RayEnd)) {
			const float TermDist = FVector::Dist(Origin, RayEnd);
			TrySampleSegmentForLoS(Component, World, Settings, Origin, Dir, TermDist, CumDist,
			                       ListenerPos, MaxPathDist, Result);
			CumDist += TermDist;
			if (Result.bFound) {
				break;
			}
			if (Settings.MaxStraightFlightDistance <= 0.f || BounceIdx >= EffMaxBounces) {
				break;
			}
			Dir = FAsyncCastManager::ComputeMidAirTurnDirection(Dir, RayEnd, ListenerPos, bBias,
			                                                    Settings.SurfaceRoughness,
			                                                    Settings.BounceListenerBias);
			Origin = RayEnd;
			++BounceIdx;
			continue;
		}

		const float SegLen = FVector::Dist(Origin, RayHit.Location);
		CumDist += SegLen;
		if (SegLen > 1.f) {
			const FVector SegDir = (RayHit.Location - SegStart) / SegLen;
			TrySampleSegmentForLoS(Component, World, Settings, SegStart, SegDir, SegLen, SegStartCumDist,
			                       ListenerPos, MaxPathDist, Result);
		}
		if (Result.bFound) {
			break;
		}

		FRayHitOutput HitOut;
		Component.ProcessRayHit(RayHit, Origin, Dir, CumDist, BounceIdx, bNextHitCrawls,
		                        Result.bFound, bBias, ListenerPos, Settings, World, HitOut);

		if (HitOut.bCrawlSucceeded && HitOut.bLoSFound) {
			Result.bFound = true;
			Result.Point = HitOut.LoSPoint;
			Result.CumDist = HitOut.LoSCumDist;
			break;
		}
	}

	return Result;
}

FAsyncCastManager::FRayAccumulatorInput FUpdater::AccumulateLoSBreakHits(
	const USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings,
	const FVector& SourcePos, const FVector& ListenerPos, const FSweepRayParams& RayParams) {
	FAsyncCastManager::FRayAccumulatorInput AccumIn;
	AccumIn.MinLoSDist = TNumericLimits<float>::Max();
	AccumIn.DirectDist = FVector::Dist(SourcePos, ListenerPos);
	AccumIn.MaxRayDistance = Component.MaxRayDistance;
	AccumIn.bDirectLoSFound = false;
	AccumIn.SourcePos = SourcePos;

	for (int32 i = 0; i < Settings.LoSBreakSweepRayCount; ++i) {
		const FLoSBreakRayResult Result = TraceSingleLoSBreakRay(
			Component, World, Settings, SourcePos, ListenerPos, RayParams.ToListenerDir, RayParams.bBias,
			RayParams.EffMaxBounces, RayParams.MaxPathDist, RayParams.SteerDist, i);
		if (!Result.bFound) {
			continue;
		}

		++AccumIn.RaysReached;
		AccumIn.MinLoSDist = FMath::Min(AccumIn.MinLoSDist,
		                                Result.CumDist + FVector::Dist(Result.Point, ListenerPos));

		const float GeomDist = FVector::Dist(SourcePos, Result.Point);
		const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff * GeomDist
			/ FMath::Max(Component.MaxRayDistance, 1.f));
		AccumIn.WeightedPos += Result.Point * DistW;
		AccumIn.TotalWeight += DistW;
	}
	return AccumIn;
}

void FUpdater::PerformLoSBreakSweep(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	if (Settings.LoSBreakSweepRayCount <= 0) {
		return;
	}

	FCastContext Context;
	if (!TryResolveCastContext(Component, Context) || IsOutOfRange(Context, Settings.MaxRayDistance)) {
		return;
	}
	UWorld* World = Context.World;
	const FVector SourcePos = Context.SourcePos;
	const FVector ListenerPos = Context.ListenerPos;

	float Priority;
	{
		int32 Unused;
		Component.GetEffectiveRayCounts(Unused, Priority);
	}

	// Steering leads only the launch bias. Every trace in the loop stays on the actual positions.
	const FVector SteerSrc = SourcePos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedSourceVelocity, Settings);
	const FVector SteerLis = ListenerPos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedListenerVelocity, Settings);
	const float SteerDist = FVector::Dist(SteerSrc, SteerLis);

	FSweepRayParams RayParams;
	RayParams.ToListenerDir = SteerDist > 0.f ? (SteerLis - SteerSrc) / SteerDist : FVector::ForwardVector;
	RayParams.SteerDist = SteerDist;
	RayParams.bBias = Settings.bBiasRayDirections && FVector::Dist(SourcePos, ListenerPos) > 0.f;
	RayParams.EffMaxBounces = FMath::Max(Settings.MinMaxBounces,
	                                     FMath::RoundToInt(Settings.MaxBounces * Priority));
	RayParams.MaxPathDist = Component.MaxRayDistance * Settings.TotalPathBudgetMultiplier;

	const FAsyncCastManager::FRayAccumulatorInput AccumIn = AccumulateLoSBreakHits(
		Component, World, Settings, SourcePos, ListenerPos, RayParams);
	const FAsyncCastManager::FRayAccumulatorOutput AccumOut = FAsyncCastManager::ComputeAudioFromRayAccumulator(
		AccumIn, Settings);

	// Deliberately writes neither TargetOcclusion nor TargetPathAttenuation. TickDirectLoSSampling
	// owns occlusion, and AccumOut.PathAttenuation comes from a distance that includes the edge to
	// listener leg, which would put listener distance back into the audible path attenuation.

	if (AccumOut.bHasVirtualSource) {
		Component.TargetVirtualSourceLocation = AccumOut.VirtualSourcePos;
		Component.LastKnownEdgePoint = AccumOut.VirtualSourcePos;
		Component.bHasKnownEdge = true;
	}
}
