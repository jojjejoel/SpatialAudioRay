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

	// Each check in the rotation cycle samples one annulus of the listener disc (innermost
	// first, outermost at full radius) instead of always the rim — with a large radius and a
	// small opening, rim-only sampling pinned the fraction at 1/5 no matter how clear the
	// centre view was. OffsetRingRadiusExponent shapes the ladder: 0.5 = equal-area annuli
	// (the one-cycle average estimates the disc's *visible area fraction*, but the radii
	// crowd toward the rim), higher pulls the annuli inward so the centre view weighs more.
	// The ladder's period equals the ring-rotation period, so a stationary scene still
	// resamples identical points. The sphere RADIUS never ladders (the source's extent is
	// fixed geometry) — only the lateral ring offset walking its listener-facing cap does.
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
		// First sample ever: fill every slot so slots not yet traced this rotation don't
		// drag the average toward "occluded" before a full rotation has had a chance to run.
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
	// WindowedLoSFraction is the average of the per-slot cache, recomputed as soon as any ONE
	// slot refreshes rather than only when a full rotation completes. A stationary scene
	// retraces the exact same ray per slot every rotation (see OffsetRingAngle/RadiusScale
	// periodicity above), so a slot's cached value really is "what this exact ray saw last
	// time" — occlusion can move mid-rotation instead of freezing for a full cycle. A slot can
	// still occasionally flip from floating-point noise on a grazing trace; accepted (2026-07-21)
	// rather than gated back to a once-per-cycle batch.
	const float PatternLoSFraction = Component.WindowedLoSFraction;

	// Occlusion consumes the smoothed pattern average, softening its quantization steps into a
	// continuous gradient. Gating (bHasDirectLoS) stays on the raw instant sample — a smoothed
	// decay must not keep sweeps suppressed after LoS is actually gone.
	// Scaled by the same OffsetLoSMultiplier as the check interval itself, so the smoothing
	// time stays proportional to how often WindowedLoSFraction actually steps to a new value —
	// movement (shorter interval, faster-arriving samples) shortens it to track genuinely fast
	// change more closely; standing still relaxes it back to the full baseline softening.
	const float EffSmoothingTime = Settings.LoSFractionSmoothingTime * Component.VelocityScaling.OffsetLoSMultiplier;
	const float DirectLoSFraction = EffSmoothingTime > 0.f
		? FMath::FInterpTo(Component.LastDirectLoSFraction, PatternLoSFraction,
		                   DeltaTime, 1.f / EffSmoothingTime)
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

	// Occlusion is purely a function of how much offset-LoS survives: full offset LoS (1.0)
	// clears it entirely, zero offset LoS fully occludes the direct source. The diffracted
	// path's actual "realism" (how muffled it sounds) is handled independently downstream by
	// TargetPathAttenuation and the virtual cue's path-driven HPF/reverb/pre-delay, not here.
	Component.TargetOcclusion = 1.f - DirectLoSFraction;
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

FUpdater::FEdgeWeightAccum FUpdater::AccumulateCachedEdgeWeights(USpatialAudioComponent& Component, UWorld* World,
                                                                  const USpatialAudioSettings& Settings,
                                                                  const FVector& ListenerPos) {
	FEdgeWeightAccum Accum;
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

	if (!Settings.bCacheEdgePoints) {
		Component.CachedEdgePoints.Reset();
	}

	// Active while occluded AND through the pre-sweep band: the crossfade gate starts opening
	// before full occlusion, so the voices must already be weighted, positioned and clustered
	// then — otherwise the gate opens onto an empty voice list and the virtual stays silent
	// until LoS fully drops.
	const bool bVirtualPathActive = !Component.bHasDirectLoS || Component.IsPreSweepActive();

	FEdgeWeightAccum Accum;
	if (Settings.bCacheEdgePoints && bVirtualPathActive) {
		Accum = AccumulateCachedEdgeWeights(Component, World, Settings, ListenerPos);
	}

	Component.AudioDiag.UpdateCachedEdges = Accum.RaysReached;
	Component.AudioDiag.UpdateDirectDist = DirectDist;

	if (Accum.PosWeightTotal > 0.f) {
		Component.TargetVirtualSourceLocation = Accum.WeightedPos / Accum.PosWeightTotal;
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
			if (Accum.SrcWeightTotal > 0.f) {
				Component.CurrentSourceToVirtualDistance = Accum.WeightedDistSum / Accum.SrcWeightTotal;
			}
			const float Leg1Geom = FVector::Dist(SourcePos, Component.TargetVirtualSourceLocation);
			Component.TargetPathAttenuation = Math::ComputePathAttenuation(
				Component.CurrentSourceToVirtualDistance, Leg1Geom, Component.MaxRayDistance, Settings);
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

TArray<FUpdater::FDesired> FUpdater::BuildDesiredVoices(USpatialAudioComponent& Component,
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
			             Math::ComputePathAttenuation(Cluster.PathDist, Leg1Geom, Component.MaxRayDistance, Settings),
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
			// Compare against where the voice is heading, not where it audibly is — a cluster
			// drifting faster than the position smoothing must not read as a jump.
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

FUpdater::FLoSBreakRayResult FUpdater::TraceSingleLoSBreakRay(
	USpatialAudioComponent& Component, UWorld* World, const USpatialAudioSettings& Settings,
	const FVector& SourcePos, const FVector& ListenerPos, const FVector& ToListenerDir, bool bBias,
	int32 EffMaxBounces, float MaxPathDist, float SteerDist, int32 RayIndex) {
	FLoSBreakRayResult Result;

	FVector Origin = SourcePos;
	float CumDist = 0.f;
	int32 BounceIdx = 0;
	bool bNextHitCrawls = (RayIndex % 2 == 0);

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
		if (!Result.bFound && CumDist + LoSSegDist <= MaxPathDist) {
			FHitResult LoSHit;
			if (!Component.TraceLine(World, LoSHit, Origin, ListenerPos)) {
				Result.bFound = true;
				Result.Point = Origin;
				Result.CumDist = CumDist;
			}
		}
		if (Result.bFound) {
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
			if (!Result.bFound && TermDist > 1.f) {
				FVector LoSPt;
				float LoSDist;
				if (Component.StepSampleSegmentForLoS(Origin, Dir, TermDist, CumDist, ListenerPos,
				                                      MaxPathDist, Settings, World, LoSPt, LoSDist)) {
					Result.bFound = true;
					Result.Point = LoSPt;
					Result.CumDist = LoSDist;
				}
			}
			CumDist += TermDist;
			if (Result.bFound) {
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

		if (!Result.bFound && SegLen > 1.f) {
			FVector LoSPt;
			float LoSDist;
			const FVector SegDir = (RayHit.Location - OriginAtBounceStart) / SegLen;
			if (Component.StepSampleSegmentForLoS(OriginAtBounceStart, SegDir, SegLen, SegStartCumDist,
			                                      ListenerPos, MaxPathDist, Settings, World, LoSPt, LoSDist)) {
				Result.bFound = true;
				Result.Point = LoSPt;
				Result.CumDist = LoSDist;
			}
		}
		if (Result.bFound) {
			break;
		}

		FRayHitOutput HitOut;
		Component.ProcessRayHit(RayHit, Origin, Dir, CumDist, BounceIdx, bNextHitCrawls,
		                        Result.bFound, bBias, ListenerPos, MaxPathDist, Settings, World, HitOut);

		if (!Result.bFound && HitOut.bCrawlSucceeded && HitOut.bLoSFound) {
			Result.bFound = true;
			Result.Point = HitOut.LoSPoint;
			Result.CumDist = HitOut.LoSCumDist;
			break;
		}
	}

	return Result;
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
		const FLoSBreakRayResult Result = TraceSingleLoSBreakRay(Component, World, Settings, SourcePos, ListenerPos,
		                                                         ToListenerDir, bBias, EffMaxBounces, MaxPathDist,
		                                                         SteerDist, i);
		if (!Result.bFound) {
			continue;
		}

		++RaysReached;
		TotalDist = FMath::Min(TotalDist, Result.CumDist + FVector::Dist(Result.Point, ListenerPos));

		const float GeomDist = FVector::Dist(SourcePos, Result.Point);
		const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff * GeomDist
			/ FMath::Max(Component.MaxRayDistance, 1.f));
		WeightedPos += Result.Point * DistW;
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
	AccumIn.SourcePos = SourcePos;
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
