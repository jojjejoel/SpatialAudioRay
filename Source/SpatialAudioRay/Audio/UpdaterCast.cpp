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
			// With all 2xN slots busy, steal the quietest fading-out slot — a shortened fade
			// beats ever creating a component at runtime.
			if (Slots[i].State == FVirtualSlot::EState::FadingOut && Slots[i].FadeAlpha < BestAlpha) {
				BestAlpha = Slots[i].FadeAlpha;
				Best = i;
			}
		}
		return Best;
	}
}

// Lets the sample point hug the wall as the player closes in on it, rather than always being
// excluded once the fixed-radius point ends up embedded.
FVector FUpdater::ResolveOffsetPoint(USpatialAudioComponent& Component, UWorld* World,
                                     const FVector& ListenerPos, const FVector& CandidatePoint) {
	FHitResult H;
	if (Component.TraceLine(World, H, ListenerPos, CandidatePoint)) {
		const FVector ToListener = (ListenerPos - H.Location).GetSafeNormal();
		return H.Location + ToListener * 5.f;
	}
	return CandidatePoint;
}

// Clear fraction over 5 samples: center plus 4 ring points perpendicular to the source↔listener
// axis. Listener samples pair with points on the SOURCE's inner-radius sphere (radius SourceR):
// each same-world-direction lateral offset r (SourceRingR, annulus-laddered by the caller) is
// lifted toward the listener onto the sphere (lift = sqrt(R²−r²); the center, r = 0, lifts the
// full R) — seen from the listener the source targets still form the familiar filled disc of
// radius R, seen from the side they wrap the sphere's listener-facing cap. The source plays at
// full volume anywhere inside the inner radius, so seeing any of that sphere's surface counts
// as seeing the source. The center is deliberately just one vote — LoS through a small hole
// should read as mostly occluded, not fully clear. The ring basis rotates exactly 90°/steps per
// call, and the caller steps OffsetR/SourceRingR through OffsetRingRadiusExponent-shaped annuli
// (equal-area at the 0.5 default) with the same period, so one cycle covers the whole disc and
// the average holds constant when stationary. Fixed denominator (5), not the count of
// geometrically valid points — a ring point whose own path from the listener is blocked can't
// have LoS to the source either, so it counts as "not clear" rather than being excluded,
// keeping the fraction a stable function of how many samples have LoS, not of player position.
float FUpdater::SyncOffsetLoSFraction(USpatialAudioComponent& Component, UWorld* World,
                                      const FVector& SourcePos, const FVector& ListenerPos,
                                      float OffsetR, float SourceR, float SourceRingR,
                                      float RingStepRad) {
	const FVector ToListenerDir = (ListenerPos - SourcePos).GetSafeNormal();

	// A sample inside the sphere is clear by definition (full-volume zone). The KINDA_SMALL
	// floor doubles as the degenerate-distance guard when SourceR is 0.
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

	// Wraps at 90° (the 4-point cross is symmetric under 90° rotation), so the pattern repeats
	// exactly every 90°/RingStepRad checks — a stationary scene resamples identical directions
	// each period, which is what lets the one-period average hold perfectly constant.
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
		// Same-side pairing: the cap point offsets in the SAME world direction as its listener
		// point, so head-on the rays still form parallel corridors sampling the joint aperture.
		SrcPts[i] = SphereCapPoint(RingDir, LateralR);
		bClearArr[i] = SampleClear(Pts[i], SrcPts[i]);
		if (bClearArr[i]) {
			++Clear;
		}
	}
	if (Clear > 0 && Component.bDrawDebugRays && Component.bShowOffsetLoSChecks) {
		DrawDebugLine(World, CenterEnd, ListenerPos, bCenterClear ? FColor::Green : FColor::Red,
		              false, Component.DebugLineDuration, 0, 0.75f);
		for (int32 i = 0; i < 4; ++i) {
			DrawDebugLine(World, SrcPts[i], Pts[i], bClearArr[i] ? FColor::Green : FColor::Red,
			              false, Component.DebugLineDuration, 0, 0.75f);
		}
	}
	return static_cast<float>(Clear) / 5.f;
}

void FUpdater::TickDirectLoSSampling(USpatialAudioComponent& Component, const float DeltaTime,
                                     const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();
	AActor* OwnerActor = Component.GetOwner();
	if (!World || !OwnerActor) {
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		return;
	}

	const FVector SourcePos = OwnerActor->GetActorLocation();
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();

	if (FVector::DistSquared(SourcePos, ListenerPos) > FMath::Square(Component.MaxRayDistance)) {
		return;
	}

	const int32 RotationSteps = FMath::Clamp(Settings.OffsetRingRotationSteps, 1, 8);

	Component.OffsetLoSCheckTimer += DeltaTime;
	if (const float CheckInterval = Settings.OffsetLoSCheckInterval * Component.VelocityScaling.OffsetLoSMultiplier;
		Component.OffsetLoSCheckTimer >= CheckInterval) {
		Component.OffsetLoSCheckTimer = 0.f;
		// Each check in the rotation cycle samples one annulus of the listener disc (innermost
		// first, outermost at full radius) instead of always the rim — with a large radius and a
		// small opening, rim-only sampling pinned the fraction at 1/5 no matter how clear the
		// centre view was. OffsetRingRadiusExponent shapes the ladder: 0.5 = equal-area annuli
		// (the one-cycle average estimates the disc's *visible area fraction*, but the radii
		// crowd toward the rim), higher pulls the annuli inward so the centre view weighs more.
		// The ladder's period equals the ring-rotation period, so a stationary scene still
		// resamples identical points. The sphere RADIUS never ladders (the source's extent is
		// fixed geometry) — only the lateral ring offset walking its listener-facing cap does.
		const float RadiusScale = FMath::Pow((Component.LoSCycleCount + 1.f) / RotationSteps,
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

		Component.LoSCycleSum += Component.LastOffsetLoSFraction;
		if (++Component.LoSCycleCount >= RotationSteps) {
			Component.WindowedLoSFraction = Component.LoSCycleSum / RotationSteps;
			Component.LoSCycleSum = 0.f;
			Component.LoSCycleCount = 0;
		}
		Component.NoLoSSampleStreak = Component.LastOffsetLoSFraction > 0.f
			                              ? 0
			                              : Component.NoLoSSampleStreak + 1;

		if (!Component.bLoSFractionSeeded) {
			Component.bLoSFractionSeeded = true;
			Component.LastDirectLoSFraction = Component.LastOffsetLoSFraction;
			Component.WindowedLoSFraction = Component.LastOffsetLoSFraction;
		}
	}

	// Occlusion's smoothing target updates only when a full ring rotation COMPLETES — not as a
	// sliding window over the last rotation's worth of checks. A sliding mean re-perturbs on
	// every check that resamples a marginal grazing direction (traces there flicker hit/miss),
	// pumping occlusion while standing still; batching per completed cycle means the value can
	// only step once per rotation, on the whole cycle's average.
	const float PatternLoSFraction = Component.WindowedLoSFraction;

	// Occlusion consumes the smoothed pattern average, softening its quantization steps into a
	// continuous gradient. Gating (bHasDirectLoS) stays on the raw instant sample — a smoothed
	// decay must not keep sweeps suppressed after LoS is actually gone.
	const float DirectLoSFraction = Settings.LoSFractionSmoothingTime > 0.f
		? FMath::FInterpTo(Component.LastDirectLoSFraction, PatternLoSFraction,
		                   DeltaTime, 1.f / Settings.LoSFractionSmoothingTime)
		: PatternLoSFraction;

	// At the LoS boundary the rotating ring alternates between patterns that do and don't
	// contain the one marginal clear direction, which flip-flopped playback between the
	// occluded source and the virtual path every few checks. Gaining LoS is still instant,
	// but a stationary scene only loses LoS once a full rotation pattern finds nothing —
	// movement is genuine change and drops immediately.
	const bool bHoldLoSThroughRotation = Component.bHasDirectLoS && Component.VelocityScaling.IsStationary()
		&& Component.NoLoSSampleStreak < RotationSteps;
	Component.bHasDirectLoS = Component.LastOffsetLoSFraction > 0.f || bHoldLoSThroughRotation;
	Component.LastDirectLoSFraction = DirectLoSFraction;

	if (Component.bDrawDebugRays && Component.bShowEdgePoints) {
		const bool bFullyClear = Component.LastOffsetLoSFraction >= 1.f;
		const FColor LoSColor = bFullyClear ? FColor::Green : FColor(255, 165, 0);
		DrawDebugSphere(World, SourcePos, 8.f, 6, LoSColor, false, Component.DebugLineDuration);
		DrawDebugSphere(World, ListenerPos, 8.f, 6, LoSColor, false, Component.DebugLineDuration);
		if (bFullyClear) {
			DrawDebugLine(World, SourcePos, ListenerPos, FColor::Green, false,
			              Component.DebugLineDuration, 0, 1.f);
		}
	}

	// Occlusion is purely a function of how much offset-LoS survives: full offset LoS (1.0)
	// clears it entirely, zero offset LoS fully occludes the direct source. The diffracted
	// path's actual "realism" (how muffled it sounds) is handled independently downstream by
	// TargetPathAttenuation and the virtual cue's path-driven HPF/reverb/pre-delay, not here.
	Component.TargetOcclusion = 1.f - DirectLoSFraction;
}

void FUpdater::PerformUpdateRayCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();
	AActor* OwnerActor = Component.GetOwner();
	if (!World || !OwnerActor) {
		UE_LOG(LogTemp, Error, TEXT("Invalid owner or settings"));
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		UE_LOG(LogTemp, Error, TEXT("Player controller not found"));
		return;
	}

	const FVector SourcePos = OwnerActor->GetActorLocation();
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();

	if (FVector::DistSquared(SourcePos, ListenerPos) > FMath::Square(Component.MaxRayDistance)) {
		return;
	}

	const float DirectDist = FVector::Dist(SourcePos, ListenerPos);

	if (Component.bHasDirectLoS) {
		Component.LoSDiffractionPaths.Reset();

		if (Component.DirectLoSConfirmedDuration >= Settings.DirectLoSConfirmTime
			&& !Component.IsPreSweepActive()) {
			Component.CachedEdgePoints.Reset();
			Component.CachedEdgeDirs.Reset();
		}
	}

	int32 RaysReached = 0;
	FVector WeightedPos = FVector::ZeroVector;
	float PosWeightTotal = 0.f;
	float SrcWeightTotal = 0.f;
	float WeightedDistSum = 0.f;

	if (!Settings.bCacheEdgePoints) {
		Component.CachedEdgePoints.Reset();
	}

	// Active while occluded AND through the pre-sweep band: the crossfade gate starts opening
	// before full occlusion, so the voices must already be weighted, positioned and clustered
	// then — otherwise the gate opens onto an empty voice list and the virtual stays silent
	// until LoS fully drops.
	const bool bVirtualPathActive = !Component.bHasDirectLoS || Component.IsPreSweepActive();

	if (Settings.bCacheEdgePoints && bVirtualPathActive) {
		for (int32 i = 0; i < Component.CachedEdgePoints.Num(); ++i) {
			const FCachedEdgePoint& Ep = Component.CachedEdgePoints[i];

			// Source-side weight (eviction confidence + geometric falloff from the source)
			// drives the path-distance average and must stay listener-independent. The position
			// weight adds listener→edge falloff on top: listener proximity may steer WHERE the
			// virtual source sits, never how loud/muffled it is.
			const float SrcW = Ep.EvictionAlpha / (1.f + Settings.CandidateDistanceFalloff
				* Ep.GeomDist / FMath::Max(Component.MaxRayDistance, 1.f));
			const float PosW = SrcW / (1.f + Settings.ListenerDistanceFalloff
				* FVector::Dist(ListenerPos, Ep.EffectivePoint()) / FMath::Max(Component.MaxRayDistance, 1.f));
			WeightedPos += Ep.EmitterPoint(Settings.VirtualSourcePullbackDistance) * PosW;
			PosWeightTotal += PosW;
			WeightedDistSum += Ep.EffectivePathDist() * SrcW;
			SrcWeightTotal += SrcW;
			++RaysReached;

			if (Component.bDrawDebugRays && Component.bShowEdgePoints) {
				DrawDebugLine(World, Ep.EffectivePoint(), ListenerPos, FColor::Cyan, false, Component.DebugLineDuration, 0,
				              1.f);
			}
		}
	}

	Component.AudioDiag.UpdateCachedEdges = RaysReached;
	Component.AudioDiag.UpdateDirectDist = DirectDist;

	if (PosWeightTotal > 0.f) {
		Component.TargetVirtualSourceLocation = WeightedPos / PosWeightTotal;
		Component.LastKnownEdgePoint = Component.CurrentVirtualSourceLocation;
		Component.bHasKnownEdge = true;
	}
	else {
		Component.TargetVirtualSourceLocation = Component.bHasKnownEdge ? Component.LastKnownEdgePoint : SourcePos;
	}

	{
		if (bVirtualPathActive) {
			// Updated every frame from the stable CachedEdgePoints cache (using the purely
			// source-side weight: eviction confidence + geometric falloff, no listener term),
			// instead of only refreshing once per completed full sweep. EvictionAlpha decays every
			// frame via FEdgeCache::TickCachedEdgeEviction, so this tracks smoothly between sweeps
			// rather than sitting frozen on a stale target until the next sweep happens to finish.
			// Falls back to the last sweep-derived distance if the cache is momentarily empty
			// (e.g. between an eviction and the next sweep's re-discovery).
			if (SrcWeightTotal > 0.f) {
				Component.CurrentSourceToVirtualDistance = WeightedDistSum / SrcWeightTotal;
			}
			Component.TargetPathAttenuation = Math::ComputePathAttenuation(
				Component.CurrentSourceToVirtualDistance, Component.MaxRayDistance, Settings);
		}
		else if (Component.bHasDirectLoS && Component.DirectLoSConfirmedDuration >= Settings.DirectLoSConfirmTime) {
			Component.TargetPathAttenuation = 0.f;
		}
	}

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

void FUpdater::SyncVirtualVoicesToClusters(USpatialAudioComponent& Component,
                                           const TArray<FEdgeCluster>& Clusters,
                                           const USpatialAudioSettings& Settings) {
	TArray<FVirtualVoice>& Voices = Component.VirtualVoices;
	if (Voices.IsEmpty() || Component.VirtualSlots.IsEmpty()) {
		return;
	}

	struct FDesired {
		FVector Position;
		float PathDist;
		float PathAttenuation;
		float WeightShare;
		int32 MatchedVoice = INDEX_NONE;
	};
	TArray<FDesired> Desired;

	if (!Clusters.IsEmpty()) {
		float TotalWeight = 0.f;
		for (const FEdgeCluster& Cluster : Clusters) {
			TotalWeight += Cluster.TotalWeight;
		}
		for (const FEdgeCluster& Cluster : Clusters) {
			Desired.Add({Cluster.Centroid, Cluster.PathDist,
			             Math::ComputePathAttenuation(Cluster.PathDist, Component.MaxRayDistance, Settings),
			             Cluster.TotalWeight / FMath::Max(TotalWeight, KINDA_SMALL_NUMBER)});
		}
	}
	else if (!Settings.bCacheEdgePoints) {
		// Legacy single-voice fallback fed from the global fields (sweep-published target,
		// LastKnownEdgePoint chain, TargetPathAttenuation incl. its confirmed-LoS zeroing) —
		// only for the cache-off configuration, where clusters can never form. With caching
		// enabled an empty cache means NO voice: every audible virtual path must be backed by
		// a LoS-verified cached edge, otherwise the fallback kept playing from a stale
		// last-known position with nothing validating it.
		Desired.Add({Component.TargetVirtualSourceLocation,
		             Component.CurrentSourceToVirtualDistance,
		             Component.TargetPathAttenuation, 1.f});
	}

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
			// Compare against where the voice is heading, not where it audibly is — a cluster
			// drifting faster than the position smoothing must not read as a jump.
			const float DistSq = FVector::DistSquared(Voices[V].TargetPosition, Desired[D].Position);
			if (DistSq <= GlideMaxSq) {
				Pairs.Add({DistSq, D, V});
			}
		}
	}
	Pairs.Sort([](const FMatchPair& A, const FMatchPair& B) { return A.DistSq < B.DistSq; });

	TArray<bool> VoiceClaimed;
	VoiceClaimed.Init(false, Voices.Num());
	for (const FMatchPair& P : Pairs) {
		if (Desired[P.DesiredIdx].MatchedVoice != INDEX_NONE || VoiceClaimed[P.VoiceIdx]) {
			continue;
		}
		Desired[P.DesiredIdx].MatchedVoice = P.VoiceIdx;
		VoiceClaimed[P.VoiceIdx] = true;
	}

	for (int32 V = 0; V < Voices.Num(); ++V) {
		if (Voices[V].bActive && !VoiceClaimed[V]) {
			// Cluster vanished or moved beyond glide range: fade the slot out in place
			// (FrozenGainScale holds its last audible gain) instead of sweeping the
			// component through space.
			if (Component.VirtualSlots.IsValidIndex(Voices[V].SlotIndex)) {
				FVirtualSlot& Slot = Component.VirtualSlots[Voices[V].SlotIndex];
				Slot.State = FVirtualSlot::EState::FadingOut;
				Slot.VoiceIndex = INDEX_NONE;
			}
			Voices[V] = FVirtualVoice{};
		}
	}

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
			// A stolen fading-out slot carries its alpha into the fade-in so gain stays
			// continuous through the hard switch.
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
			// New voices start their smoothed params at the targets — the slot's fade-in
			// envelope is the ramp; ramping PathAttenuation up from 0 would fade in too loud.
			Voice.CurrentPathAttenuation = Voice.TargetPathAttenuation;
			Voice.CurrentWeightShare = Voice.TargetWeightShare;
		}
	}
}

void FUpdater::PerformLoSBreakSweep(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();
	AActor* OwnerActor = Component.GetOwner();
	if (!World || !OwnerActor) {
		UE_LOG(LogTemp, Error, TEXT("Invalid owner or settings"));
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		UE_LOG(LogTemp, Error, TEXT("Player controller not found"));
		return;
	}

	if (Settings.LoSBreakSweepRayCount <= 0) {
		return;
	}

	const FVector SourcePos = OwnerActor->GetActorLocation();
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();
	if (FVector::DistSquared(SourcePos, ListenerPos) > FMath::Square(Settings.MaxRayDistance)) {
		return;
	}

	float Priority;
	{
		int32 Unused;
		Component.GetEffectiveRayCounts(Unused, Priority);
	}

	const float DirectDist = FVector::Dist(SourcePos, ListenerPos);
	// Steering-only lead (see ComputeSteeringLead — retro right after LoS loss, which is
	// exactly when this sweep fires): the launch bias aims at the led positions; DirectDist
	// and every trace in the loop stay on the actual ones.
	const FVector SteerSrc = SourcePos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedSourceVelocity, Settings);
	const FVector SteerLis = ListenerPos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedListenerVelocity, Settings);
	const float SteerDist = FVector::Dist(SteerSrc, SteerLis);
	const FVector ToListenerDir = SteerDist > 0.f ? (SteerLis - SteerSrc) / SteerDist : FVector::ForwardVector;
	const bool bBias = Settings.bBiasRayDirections && DirectDist > 0.f;
	const int32 EffMaxBounces = FMath::Max(Settings.MinMaxBounces, FMath::RoundToInt(Settings.MaxBounces * Priority));
	const int32 NumRays = Settings.LoSBreakSweepRayCount;
	const float MaxPathDist = Component.MaxRayDistance * Settings.TotalPathBudgetMultiplier;

	int32 RaysReached = 0;
	float TotalDist = TNumericLimits<float>::Max();
	FVector WeightedPos = FVector::ZeroVector;
	float TotalW = 0.f;

	for (int32 i = 0; i < NumRays; ++i) {
		bool bFoundLoS = false;
		FVector LoSPoint = FVector::ZeroVector;
		float LoSCumDist = 0.f;

		FVector Origin = SourcePos;
		float CumDist = 0.f;
		int32 BounceIdx = 0;
		bool bNextHitCrawls = (i % 2 == 0);

		FVector Dir = FMath::VRand();
		if (bBias) {
			for (int32 Attempt = 0; Attempt < 30; ++Attempt) {
				const FVector Candidate = FMath::VRand();
				if (FMath::FRand() < Math::ComputeRayDirectionWeight(Candidate, ToListenerDir,
				                                                     Component.LastDirectLoSFraction,
				                                                     Settings.DirectLoSSampleRadius, SteerDist)) {
					Dir = Candidate;
					break;
				}
			}
		}

		for (int32 Bounce = 0; Bounce <= EffMaxBounces; ++Bounce) {
			const float LoSSegDist = FVector::Dist(Origin, ListenerPos);
			if (!bFoundLoS && CumDist + LoSSegDist <= MaxPathDist) {
				FHitResult LoSHit;
				if (!Component.TraceLine(World, LoSHit, Origin, ListenerPos)) {
					bFoundLoS = true;
					LoSPoint = Origin;
					LoSCumDist = CumDist;
				}
			}
			if (bFoundLoS) {
				break;
			}
			// Best-case prune: traveled distance grows at least as fast as listener distance can
			// shrink (triangle inequality), so past this bound no future sample of this ray can
			// pass the budget gate — see the same check in TickAsyncCast.
			if (CumDist + LoSSegDist > MaxPathDist) {
				break;
			}

			float RemainingBudget = FMath::Min(Component.MaxRayDistance, MaxPathDist - CumDist);
			if (Settings.MaxStraightFlightDistance > 0.f) {
				RemainingBudget = FMath::Min(RemainingBudget, Settings.MaxStraightFlightDistance);
			}
			if (RemainingBudget < 1.f) {
				break;
			}

			const FVector OriginAtBounceStart = Origin;
			FHitResult RayHit;
			const FVector RayEnd = Origin + Dir * RemainingBudget;
			if (!Component.TraceLine(World, RayHit, Origin, RayEnd)) {
				const float TermDist = FVector::Dist(Origin, RayEnd);
				if (!bFoundLoS && TermDist > 1.f) {
					FVector LoSPt;
					float LoSDist;
					if (Component.StepSampleSegmentForLoS(Origin, Dir, TermDist, CumDist, ListenerPos,
					                                      MaxPathDist, Settings, World, LoSPt, LoSDist)) {
						bFoundLoS = true;
						LoSPoint = LoSPt;
						LoSCumDist = LoSDist;
					}
				}
				CumDist += TermDist;
				if (bFoundLoS) {
					break;
				}
				if (Settings.MaxStraightFlightDistance > 0.f && BounceIdx < EffMaxBounces) {
					Dir = FAsyncCastManager::ComputeMidAirTurnDirection(Dir, RayEnd, ListenerPos, bBias,
					                                                    Settings.SurfaceRoughness,
					                                                    Settings.BounceListenerBias);
					Origin = RayEnd;
					++BounceIdx;
					continue;
				}
				break;
			}

			const float SegLen = FVector::Dist(Origin, RayHit.Location);
			const float SegStartCumDist = CumDist;
			CumDist += SegLen;

			if (!bFoundLoS && SegLen > 1.f) {
				FVector LoSPt;
				float LoSDist;
				const FVector SegDir = (RayHit.Location - OriginAtBounceStart) / SegLen;
				if (Component.StepSampleSegmentForLoS(OriginAtBounceStart, SegDir, SegLen, SegStartCumDist,
				                                      ListenerPos, MaxPathDist, Settings, World, LoSPt, LoSDist)) {
					bFoundLoS = true;
					LoSPoint = LoSPt;
					LoSCumDist = LoSDist;
				}
			}
			if (bFoundLoS) {
				break;
			}

			FRayHitOutput HitOut;
			Component.ProcessRayHit(RayHit, Origin, Dir, CumDist, BounceIdx, bNextHitCrawls,
			                        bFoundLoS, bBias, ListenerPos, MaxPathDist, Settings, World, HitOut);

			if (!bFoundLoS && HitOut.bCrawlSucceeded && HitOut.bLoSFound) {
				bFoundLoS = true;
				LoSPoint = HitOut.LoSPoint;
				LoSCumDist = HitOut.LoSCumDist;
				break;
			}
		}

		if (!bFoundLoS) {
			continue;
		}

		++RaysReached;
		TotalDist = FMath::Min(TotalDist, LoSCumDist + FVector::Dist(LoSPoint, ListenerPos));

		const float GeomDist = FVector::Dist(SourcePos, LoSPoint);
		const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff * GeomDist
			/ FMath::Max(Component.MaxRayDistance, 1.f));
		WeightedPos += LoSPoint * DistW;
		TotalW += DistW;
	}

	FAsyncCastManager::FRayAccumulatorInput AccumIn;
	AccumIn.RaysReached = RaysReached;
	AccumIn.MinLoSDist = TotalDist;
	AccumIn.WeightedPos = WeightedPos;
	AccumIn.TotalWeight = TotalW;
	AccumIn.DirectDist = DirectDist;
	AccumIn.MaxRayDistance = Component.MaxRayDistance;
	AccumIn.bDirectLoSFound = false;
	const FAsyncCastManager::FRayAccumulatorOutput AccumOut = FAsyncCastManager::ComputeAudioFromRayAccumulator(
		AccumIn, Settings);

	// Neither TargetOcclusion nor TargetPathAttenuation is written here. Occlusion is owned by
	// the per-frame offset-LoS sampler (TickDirectLoSSampling) — this sweep's path-ratio value
	// would drag the target below the fraction-derived one. AccumOut.PathAttenuation is derived
	// from a MinLoSDist that includes the edge->listener leg (TotalDist above), which would
	// reintroduce listener-distance into the live audio path attenuation; PerformUpdateRayCast
	// is its sole, listener-independent source, using Component.CurrentSourceToVirtualDistance.

	if (AccumOut.bHasVirtualSource) {
		Component.TargetVirtualSourceLocation = AccumOut.VirtualSourcePos;
		Component.LastKnownEdgePoint = AccumOut.VirtualSourcePos;
		Component.bHasKnownEdge = true;
	}
}
