// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/SpatialAudioComponent.h"
#include "Audio/AsyncCastManager.h"
#include "Audio/Math.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Audio/SpatialAudioSettings.h"

using namespace Math;

namespace {
	// Shared between the per-slot debug spheres and the VRT text line so the color words in
	// the text always name the sphere you're looking at.
	constexpr int32 NumVirtualVoiceDebugColors = 4;
	const FColor VirtualVoiceDebugColors[NumVirtualVoiceDebugColors] = {
		FColor::Cyan, FColor::Purple, FColor::Emerald, FColor::Silver
	};
	const TCHAR* VirtualVoiceDebugColorNames[NumVirtualVoiceDebugColors] = {
		TEXT("cyan"), TEXT("purple"), TEXT("emerald"), TEXT("silver")
	};

	FColor SelectThresholdColor(float Value, float GreenMin, float YellowMin) {
		return Value >= GreenMin ? FColor::Green : Value >= YellowMin ? FColor::Yellow : FColor::Red;
	}

	FString BuildProgressBar(float Fraction, int32 Width = 20) {
		const int32 Filled = FMath::Clamp(FMath::RoundToInt(Fraction * Width), 0, Width);
		FString Bar;
		Bar.Reserve(Width + 2);
		Bar.AppendChar(TEXT('['));
		for (int32 i = 0; i < Width; ++i)
			Bar.AppendChar(i < Filled ? TEXT('#') : TEXT('-'));
		Bar.AppendChar(TEXT(']'));
		return Bar;
	}

	struct FPacingDebugInfo {
		FString Label;
		FColor Color;
	};

	FPacingDebugInfo ComputePacingDebugInfo(
		float GeometryBurstTimer, bool bBothStationary, bool bStationaryIdleMode,
		float GeometryChangeBurstMultiplier, float StoredEffFullSweepInterval, float GeometryBurstTimerVal,
		float StationaryIdleMultiplier, float CoverageFraction,
		float SmoothedSourceSpeed, float SmoothedListenerSpeed,
		int32 CacheFillSweepsRemaining, int32 UsableEdges, int32 RequiredEdges) {
		FPacingDebugInfo Info;
		if (GeometryBurstTimer > 0.f && bBothStationary) {
			Info.Label = FString::Printf(TEXT("BURST (%.2f×  %.2fs  %.1fs left)"),
			                             GeometryChangeBurstMultiplier, StoredEffFullSweepInterval,
			                             GeometryBurstTimerVal);
			Info.Color = FColor::Cyan;
		}
		else if (bBothStationary && CacheFillSweepsRemaining > 0 && UsableEdges < RequiredEdges) {
			Info.Label = FString::Printf(TEXT("CACHE-FILL (%.2f×  %.2fs  new edges %d/%d  %d sweeps left)"),
			                             GeometryChangeBurstMultiplier, StoredEffFullSweepInterval,
			                             UsableEdges, RequiredEdges, CacheFillSweepsRemaining);
			Info.Color = FColor::Cyan;
		}
		else if (bBothStationary && bStationaryIdleMode) {
			Info.Label = FString::Printf(TEXT("IDLE (%.0f×  %.2fs  cov=%.0f%%)"),
			                             StationaryIdleMultiplier, StoredEffFullSweepInterval, CoverageFraction * 100.f);
			Info.Color = FColor::Orange;
		}
		else {
			Info.Label = FString::Printf(TEXT("NORMAL (%.2fs  cov=%.0f%%  src=%.0fcm/s  lis=%.0fcm/s)"),
			                             StoredEffFullSweepInterval, CoverageFraction * 100.f, SmoothedSourceSpeed,
			                             SmoothedListenerSpeed);
			Info.Color = FColor::White;
		}
		return Info;
	}

	FColor ComputeBounceGradientColor(int32 BounceIndex, int32 MaxBounces) {
		const float f = FMath::Clamp(static_cast<float>(BounceIndex) / FMath::Max(1, MaxBounces), 0.f, 1.f);
		return FColor(255, static_cast<uint8>(FMath::Lerp(255.f, 80.f, f)), 0);
	}
} // namespace

void USpatialAudioComponent::DrawDebugVisualization(float DeltaTime, const USpatialAudioSettings& Settings) {
	TickReplayDebug(DeltaTime, Settings);

	if (bShowSteeringPrediction && Settings.SteeringPredictionLeadTime > 0.f) {
		// Live steering-prediction targets (sweeps capture their own snapshot of these at start).
		// Only drawn while the lead is meaningful, so stationary scenes stay clean. One sphere
		// per mover, at the signed aim point sweeps actually use — blue when leading forward,
		// orange in the retro window after LoS loss (same condition as ComputeSteeringLead).
		const bool bRetroSteer = TimeSinceHadDirectLoS <= Settings.SteeringPredictionLeadTime;
		const FColor SteerColor = bRetroSteer ? FColor(255, 140, 0) : FColor(0, 128, 255);
		const APlayerController* PredPC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (const APawn* PredPawn = PredPC ? PredPC->GetPawn() : nullptr) {
			const FVector LisLead = ComputeSteeringLead(VelocityScaling.SmoothedListenerVelocity, Settings);
			if (LisLead.SizeSquared() > FMath::Square(10.f)) {
				const FVector LisNow = PredPawn->GetActorLocation();
				DrawDebugSphere(GetWorld(), LisNow + LisLead, 16.f, 8, SteerColor, false, -1.f, 0, 1.5f);
				DrawDebugLine(GetWorld(), LisNow, LisNow + LisLead, SteerColor, false, -1.f, 0, 1.f);
			}
		}
		if (GetOwner()) {
			const FVector SrcLead = ComputeSteeringLead(VelocityScaling.SmoothedSourceVelocity, Settings);
			if (SrcLead.SizeSquared() > FMath::Square(10.f)) {
				const FVector SrcNow = GetOwner()->GetActorLocation();
				DrawDebugSphere(GetWorld(), SrcNow + SrcLead, 16.f, 8, SteerColor, false, -1.f, 0, 1.5f);
				DrawDebugLine(GetWorld(), SrcNow, SrcNow + SrcLead, SteerColor, false, -1.f, 0, 1.f);
			}
		}
	}

	if (bShowVirtualSourceRays) {
		const FVector ActorLoc = GetOwner()->GetActorLocation();
		DrawDebugSphere(GetWorld(), ActorLoc, 20.f, 8, FColor::Magenta, false, -1.f, 0, 1.f);

		// One sphere per audible emitter (pool slot); fading-out slots draw small and grey.
		for (int32 SlotIdx = 0; SlotIdx < VirtualSlots.Num(); ++SlotIdx) {
			const FVirtualSlot& Slot = VirtualSlots[SlotIdx];
			if (Slot.State == FVirtualSlot::EState::Idle || !Slot.bOffsetInit) {
				continue;
			}
			const FVector SlotPos = ActorLoc + Slot.WorldOffset;
			const bool bFadingOut = Slot.State == FVirtualSlot::EState::FadingOut;
			const FColor Color = bFadingOut
				                     ? FColor(90, 90, 90)
				                     : VirtualVoiceDebugColors[SlotIdx % NumVirtualVoiceDebugColors];
			DrawDebugSphere(GetWorld(), SlotPos, bFadingOut ? 12.f : 20.f, 8, Color, false, -1.f, 0, 2.f);
			DrawDebugLine(GetWorld(), ActorLoc, SlotPos, Color, false, -1.f, 0, 0.5f);
		}
	}

	if (bShowEdgePoints) {
		for (const FCachedEdgePoint& EP : CachedEdgePoints) {
			DrawDebugSphere(GetWorld(), EP.EdgePoint, 18.f, 8, FColor::Yellow, false, -1.f, 0, 2.f);
			if (EP.bRelayed) {
				DrawDebugSphere(GetWorld(), EP.RelayPoint, 14.f, 8, FColor::Yellow, false, -1.f, 0, 1.f);
				DrawDebugLine(GetWorld(), EP.EdgePoint, EP.RelayPoint, FColor::Yellow, false, -1.f, 0, 1.f);
			}
		}
	}

	if (bShowShortestPaths) {
		for (const FCachedEdgePoint& EP : CachedEdgePoints) {
			for (int32 i = 0; i + 1 < EP.ShortestPath.Num(); ++i) {
				// Dim = unverified traveled-route prefix (string pull didn't reach the source);
				// the recheck skips those segments too.
				const bool bVerified = i >= EP.ShortestPathVerifiedFrom;
				DrawDebugLine(GetWorld(), EP.ShortestPath[i], EP.ShortestPath[i + 1],
				              bVerified ? FColor::Magenta : FColor(120, 0, 120),
				              false, -1.f, 0, 2.f);
			}
			for (int32 i = 1; i + 1 < EP.ShortestPath.Num(); ++i) {
				DrawDebugSphere(GetWorld(), EP.ShortestPath[i], 8.f, 8,
				                FColor::Magenta, false, -1.f, 0, 1.5f);
			}
			// Relay leg extends the stored path: the RelayDist added to this edge's PathDist
			// is exactly this segment's length.
			if (EP.bRelayed && !EP.ShortestPath.IsEmpty()) {
				DrawDebugLine(GetWorld(), EP.ShortestPath.Last(), EP.RelayPoint, FColor::Magenta,
				              false, -1.f, 0, 2.f);
				DrawDebugSphere(GetWorld(), EP.RelayPoint, 8.f, 8, FColor::Magenta, false, -1.f, 0, 1.5f);
			}
		}
	}

	if (bShowDiffractionPaths) {
		for (const TArray<FVector>& Path : LoSDiffractionPaths) {
			for (int32 i = 0; i + 1 < Path.Num(); ++i) {
				DrawDebugLine(GetWorld(), Path[i], Path[i + 1], FColor::Cyan, false, -1.f, 0, 1.5f);
			}

			for (int32 i = 1; i + 1 < Path.Num(); ++i) {
				DrawDebugSphere(GetWorld(), Path[i], 8.f, 5, FColor::Cyan, false, -1.f, 0, 1.f);
			}
		}
	}

	if (bShowDebugText && GEngine) {
		const uint64 Base = static_cast<uint64>(GetUniqueID()) * 10;

		int32 ScaledRayCount;
		float Prio;
		GetEffectiveRayCounts(ScaledRayCount, Prio);

		// ── Slot 1: Source audio output ──────────────────────────────────────
		// vol  = SourceCrossfade  (1.0 = full volume, fades as occlusion rises)
		// occ  = CurvedOcclusion  (0–1 sent to MetaSound as OcclusionParam, drives LPF internally)
		{
			const FColor SrcColor = SelectThresholdColor(AudioDiag.SourceCrossfade, 0.7f, 0.3f);
			GEngine->AddOnScreenDebugMessage(Base + 1, 0.f, SrcColor,
			                                 FString::Printf(TEXT("  SRC  vol=%3.0f%%  occ_param=%3.0f%%  │  Δvol=%+.2f%%  Δocc=%+.2f%%"),
			                                                 AudioDiag.SourceCrossfade * 100.f,
			                                                 AudioDiag.CurvedOcclusion * 100.f,
			                                                 AudioDiag.DeltaSrcVol * 100.f,
			                                                 AudioDiag.DeltaOcc * 100.f));
		}

		// ── Slot 2: Virtual audio output ─────────────────────────────────────
		// gain = Σ slot gains; each slot's gain = (1−atn) × shr × fade × xfade(slewed gate)
		// bend = the loudest voice's VirtualPathBend (drives HPF/reverb inside the MetaSound)
		// voices = active emitters (+N fading out); per slot: [color state g= shr= atn=],
		//          color names the slot's debug sphere; "out" slots show only their fading gain
		{
			FString VoiceStr;
			int32 NumActive = 0;
			int32 NumFadingOut = 0;
			for (int32 SlotIdx = 0; SlotIdx < VirtualSlots.Num(); ++SlotIdx) {
				const FVirtualSlot& Slot = VirtualSlots[SlotIdx];
				if (Slot.State == FVirtualSlot::EState::Idle) {
					continue;
				}
				const float SlotGain = Slot.FrozenGainScale * Slot.FadeAlpha * CurrentVirtualCrossfade;
				if (Slot.State == FVirtualSlot::EState::FadingOut) {
					++NumFadingOut;
					VoiceStr += FString::Printf(TEXT(" [grey out g=%.0f%%]"), SlotGain * 100.f);
				}
				else if (VirtualVoices.IsValidIndex(Slot.VoiceIndex)) {
					++NumActive;
					const FVirtualVoice& Voice = VirtualVoices[Slot.VoiceIndex];
					VoiceStr += FString::Printf(TEXT(" [%s %s g=%.0f%% shr=%.0f%% atn=%.0f%%]"),
					                            VirtualVoiceDebugColorNames[SlotIdx % NumVirtualVoiceDebugColors],
					                            Slot.State == FVirtualSlot::EState::FadingIn ? TEXT("in") : TEXT("on"),
					                            SlotGain * 100.f,
					                            Voice.CurrentWeightShare * 100.f,
					                            Voice.CurrentPathAttenuation * 100.f);
				}
			}

			const FString VoiceCount = NumFadingOut > 0
				                           ? FString::Printf(TEXT("%d(+%d out)"), NumActive, NumFadingOut)
				                           : FString::Printf(TEXT("%d"), NumActive);

			const FColor VrtColor = SelectThresholdColor(AudioDiag.VirtualGain, 0.4f, 0.15f);
			GEngine->AddOnScreenDebugMessage(Base + 2, 0.f, VrtColor,
			                                 FString::Printf(
				                                 TEXT("  VRT  gain=%3.0f%%  xfade=%3.0f%%  bend=%3.0f%%  Δgain=%+.2f%%  │  voices=%s%s"),
				                                 AudioDiag.VirtualGain * 100.f, CurrentVirtualCrossfade * 100.f,
				                                 AudioDiag.VirtualPathBend * 100.f, AudioDiag.DeltaVrtGain * 100.f,
				                                 *VoiceCount, *VoiceStr));
		}

		// ── Slot 3: Occlusion state ───────────────────────────────────────────
		{
			const FString Bar = BuildProgressBar(CurrentOcclusion);
			const FColor OccColor = SelectThresholdColor(1.f - CurrentOcclusion, 0.7f, 0.3f);
			GEngine->AddOnScreenDebugMessage(Base + 3, 0.f, OccColor,
			                                 FString::Printf(
				                                 TEXT(
					                                 "  Occ %s  cur=%3.0f%%  tgt=%3.0f%%  │  LoS:%s  frac inst=%.0f%% avg=%.0f%% smooth=%.0f%%  edge:%s"),
				                                 *Bar, CurrentOcclusion * 100.f, TargetOcclusion * 100.f,
				                                 bHasDirectLoS ? TEXT("YES") : TEXT("NO"),
				                                 LastOffsetLoSFraction * 100.f,
				                                 WindowedLoSFraction * 100.f,
				                                 LastDirectLoSFraction * 100.f,
				                                 bHasKnownEdge ? TEXT("YES") : TEXT("NO")));
		}

		// ── Slot 5: Edge cache + path attenuation ─────────────────────────────
		{
			int32 NumEvicting = 0, NumPhase0 = 0;
			for (const FCachedEdgePoint& EP : CachedEdgePoints) {
				if (EP.bEvicting) ++NumEvicting;
				if (EP.bPhase0Pending) ++NumPhase0;
			}
			GEngine->AddOnScreenDebugMessage(Base + 5, 0.f, FColor::Orange,
			                                 FString::Printf(
				                                 TEXT(
					                                 "  Edges  %d/%d cached  (evict=%d  ph0=%d)  │  %d stored  │  PathAtten  cur=%3.0f%%  tgt=%3.0f%%"),
				                                 CachedEdgePoints.Num(), Settings.CachedEdgeMaxCount,
				                                 NumEvicting, NumPhase0,
				                                 StoredLoSPaths.Num(),
				                                 CurrentPathAttenuation * 100.f, TargetPathAttenuation * 100.f));
		}

		// ── Slot 6: Sweep pacing + timer ──────────────────────────────────────
		{
			const bool bBothStationary = VelocityScaling.SweepMultiplier > 0.95f && VelocityScaling.EdgeMultiplier > 0.95f;
			const float CoverageFraction = (ScaledRayCount > 0)
				                      ? FMath::Clamp(
					                      static_cast<float>(CachedEdgePoints.Num() + CachedMissDirs.Num()) /
					                      static_cast<float>(ScaledRayCount), 0.f, 1.f)
				                      : 0.f;

			const auto [PacingLabel, PacingColor] = ComputePacingDebugInfo(
				SweepScheduling.GeometryBurstTimer, bBothStationary,
				SweepScheduling.bStationaryIdleMode && !Settings.IsRateThrottlingDisabled(),
				Settings.GeometryChangeBurstMultiplier, StoredEffFullSweepInterval, SweepScheduling.GeometryBurstTimer,
				Settings.StationaryIdleMultiplier, CoverageFraction,
				VelocityScaling.SmoothedSourceSpeed, VelocityScaling.SmoothedListenerSpeed,
				SweepScheduling.CacheFillSweepsRemaining, CountCacheFillEdges(),
				Settings.MovementCacheFillRequiredEdges);

			const int32 CycleCount = FMath::Max(1, Settings.FullSweepCycleCount);
			const float SubInterval = StoredEffFullSweepInterval / CycleCount;

			FString SweepStatus;
			if (bAsyncCastActive) {
				SweepStatus = CycleCount > 1
					? FString::Printf(TEXT("CASTING sub %d/%d"), StaggeredCycleIndex + 1, CycleCount)
					: TEXT("CASTING");
			} else {
				SweepStatus = TEXT("idle");
			}

			const bool bSweepSuspended = bHasDirectLoS && !bAsyncCastActive;
			FString SweepLine;
			if (bSweepSuspended && LastOffsetLoSFraction <= 0.f) {
				SweepLine = FString::Printf(TEXT("  Sweep  SUSPENDED (confirming LoS loss %d/%d)  │  timer=%.2f/%.2fs"),
				                            NoLoSSampleStreak,
				                            FMath::Clamp(Settings.OffsetRingRotationSteps, 1, 8),
				                            TimeSinceFullCast, SubInterval);
			}
			else if (bSweepSuspended) {
				SweepLine = FString::Printf(TEXT("  Sweep  SUSPENDED (clear LoS)  │  timer=%.2f/%.2fs"),
				                            TimeSinceFullCast, SubInterval);
			}
			else {
				SweepLine = FString::Printf(TEXT("  Sweep  %s  │  timer=%.2f/%.2fs  [%s]"),
				                            *PacingLabel, TimeSinceFullCast, SubInterval, *SweepStatus);
			}
			GEngine->AddOnScreenDebugMessage(Base + 6, 0.f, bSweepSuspended ? FColor::Green : PacingColor, SweepLine);
		}

		// ── Slot 9: Audio spike detector ─────────────────────────────────────
		{
			if (AudioDiag.SpikeTimer > 0.f) {
				const float SecsAgo = 3.f - AudioDiag.SpikeTimer;
				GEngine->AddOnScreenDebugMessage(Base + 9, 0.f, FColor::Red,
				                                 FString::Printf(
					                                 TEXT(
						                                 "  *** SPIKE %.1fs ago  SRC vol Δ%+.0f%%  VRT gain Δ%+.0f%%  occ Δ%+.0f%%  ***"),
					                                 SecsAgo,
					                                 AudioDiag.SpikeSrcDelta * 100.f,
					                                 AudioDiag.SpikeVrtGainDelta * 100.f,
					                                 AudioDiag.SpikeOccDelta * 100.f));
			}
			else {
				GEngine->AddOnScreenDebugMessage(Base + 9, 0.f, FColor(70, 70, 70),
				                                 TEXT("  audio stable  (no spike >2% vol/occ in one frame)"));
			}
		}

		// ── Slot 7: Traces + last sweep ───────────────────────────────────────
		GEngine->AddOnScreenDebugMessage(Base + 7, 0.f, FColor::Cyan,
		                                 FString::Printf(
			                                 TEXT(
				                                 "  Traces  ~%.0f/fr  1s=%.0f/s  10s=%.0f/s  │  Last sweep: %d rays (%d cached)  %d fr  %.0fms/%.0fms%s"),
			                                 TraceDiag.SnapshotFrameTraces, TraceDiag.SnapshotTracesPerSec, TraceDiag.Avg10Sec,
			                                 TraceDiag.LastSweepAsyncRays, TraceDiag.LastSweepCachedReplaced, TraceDiag.LastSweepFrames,
			                                 TraceDiag.LastSweepDuration * 1000.f, TraceDiag.LastSweepInterval * 1000.f,
			                                 TraceDiag.LastSweepDuration > TraceDiag.LastSweepInterval ? TEXT("  OVER") : TEXT("")));

		// ── Slot 8: Edge timer activity ───────────────────────────────────────
		if (Settings.bCacheEdgePoints) {
			const float Ph0Interval = Settings.Phase0CheckInterval * VelocityScaling.EdgeMultiplier;

			int32 Ph0Pending = 0;
			for (const FCachedEdgePoint& EP : CachedEdgePoints) {
				if (EP.bPhase0Pending) ++Ph0Pending;
			}

			const FString Ph0Activity = Ph0Pending > 0
				? FString::Printf(TEXT("[CHECKING %d]"), Ph0Pending) : TEXT("idle");

			const FColor EdgeTimerColor = Ph0Pending > 0 ? FColor::Yellow : FColor(180, 180, 180);
			GEngine->AddOnScreenDebugMessage(Base + 8, 0.f, EdgeTimerColor,
			                                 FString::Printf(
				                                 TEXT("  Ph0 timer=%.2f/%.2fs %s"),
				                                 Phase0Timer, Ph0Interval, *Ph0Activity));
		}
	}

	if (bDrawDebugRays && bShowSurfaceCrawl && GEngine) {
		const uint64 LegBase = static_cast<uint64>(GetUniqueID()) * 10;
		GEngine->AddOnScreenDebugMessage(LegBase + 50, 0.f, FColor(220, 220, 220), TEXT("  [7] Surface Crawl Legend ─────────────────────────────────────"));
		GEngine->AddOnScreenDebugMessage(LegBase + 51, 0.f, FColor::White,         TEXT("  White line + sphere         bounce segment / bounce hit point"));
		GEngine->AddOnScreenDebugMessage(LegBase + 52, 0.f, FColor::Cyan,          TEXT("  Cyan sphere                 crawl start (nudged off wall)"));
		GEngine->AddOnScreenDebugMessage(LegBase + 53, 0.f, FColor::White,         TEXT("  White sphere (tiny)         crawl step — wall still continues"));
		GEngine->AddOnScreenDebugMessage(LegBase + 54, 0.f, FColor::Yellow,        TEXT("  Yellow sphere (large)       edge found — wall ended"));
		GEngine->AddOnScreenDebugMessage(LegBase + 55, 0.f, FColor::Orange,        TEXT("  Orange sphere (large)       edge found — perpendicular wall"));
		GEngine->AddOnScreenDebugMessage(LegBase + 56, 0.f, FColor::Red,           TEXT("  Red sphere + red line       back-face hit — ray terminated"));
		GEngine->AddOnScreenDebugMessage(LegBase + 57, 0.f, FColor(255, 80, 80),   TEXT("  Pink sphere                 crawl failed — no edge found"));
		GEngine->AddOnScreenDebugMessage(LegBase + 58, 0.f, FColor::Yellow,        TEXT("  Yellow/Orange line arrow    new ray direction after edge"));
		GEngine->AddOnScreenDebugMessage(LegBase + 59, 0.f, FColor(160, 0, 255),   TEXT("  Purple line  [8]            LoS check attempted (toggle key 8 to show/hide)"));
		GEngine->AddOnScreenDebugMessage(LegBase + 60, 0.f, FColor::Green,         TEXT("  Green sphere                 LoS confirmed clear — trace came back unblocked"));
	}

	if (bDrawDebugRays && bShowLoSChecks && GEngine) {
		const uint64 LegBase = static_cast<uint64>(GetUniqueID()) * 10;
		GEngine->AddOnScreenDebugMessage(LegBase + 70, 0.f, FColor(160, 0, 255), TEXT("  [8] LoS Checks visible — purple lines = probe submitted toward listener"));
	}

	if (bDrawDebugRays && bShowOffsetLoSChecks && GEngine) {
		const uint64 LegBase = static_cast<uint64>(GetUniqueID()) * 10;
		GEngine->AddOnScreenDebugMessage(LegBase + 80, 0.f, FColor(0, 200, 200),
		                                 TEXT("  [9] Offset LoS Checks visible — green/red lines, shown only when one offset ray finds a clear path"));
	}
}

void USpatialAudioComponent::PerformReplaySweep(const USpatialAudioSettings& Settings) {
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

	if (FVector::DistSquared(SourcePos, ListenerPos) > FMath::Square(MaxRayDistance)) {
		return;
	}

	float Priority;
	{
		int32 Unused;
		GetEffectiveRayCounts(Unused, Priority);
	}

	const float DirectDist = FVector::Dist(SourcePos, ListenerPos);
	// Steering-only lead, mirroring the async sweep's aiming axis (traces stay on actual positions).
	const FVector SteerSrc = SourcePos
		+ ComputeSteeringLead(VelocityScaling.SmoothedSourceVelocity, Settings);
	const FVector SteerLis = ListenerPos
		+ ComputeSteeringLead(VelocityScaling.SmoothedListenerVelocity, Settings);
	const float SteerDist = FVector::Dist(SteerSrc, SteerLis);
	const FVector ToListenerDir = SteerDist > 0.f
		                              ? (SteerLis - SteerSrc) / SteerDist
		                              : FVector::ForwardVector;
	const bool bBias = Settings.bBiasRayDirections && DirectDist > 0.f;
	const float UpdateRadius = Settings.DirectLoSSampleRadius;
	const int32 EffMaxBounces = FMath::Max(Settings.MinMaxBounces, FMath::RoundToInt(Settings.MaxBounces * Priority));
	const int32 NumRays = ReplayRayCount;
	const float MaxPathDist = MaxRayDistance * Settings.TotalPathBudgetMultiplier;

	Replay.Paths.Reset();
	Replay.Paths.Reserve(NumRays);

	for (int32 i = 0; i < NumRays; ++i) {
		FReplayRayPath Path;
		Path.ListenerPos = ListenerPos;
		Path.bFoundLoS = false;
		Path.TotalDist = 0.f;
		Path.LoSCumDist = 0.f;
		Path.LoSPoint = FVector::ZeroVector;

		FVector Origin = SourcePos;
		float CumDist = 0.f;
		int32 BounceIdx = 0;
		bool bNextHitCrawls = (i % 2 == 0);

		FVector Dir = FMath::VRand();
		if (bBias) {
			for (int32 Attempt = 0; Attempt < 30; ++Attempt) {
				const FVector Candidate = FMath::VRand();
				if (FMath::FRand() < ComputeRayDirectionWeight(Candidate, ToListenerDir, LastDirectLoSFraction,
				                                               UpdateRadius, SteerDist)) {
					Dir = Candidate;
					break;
				}
			}
		}

		Path.Waypoints.Add({SourcePos, 0.f, false, false, 0});

		while (true) {
			const float LoSSegDist = FVector::Dist(Origin, ListenerPos);
			if (!Path.bFoundLoS && CumDist + LoSSegDist <= MaxPathDist) {
				FHitResult LoSHit;
				if (!TraceLine(World, LoSHit, Origin, ListenerPos)) {
					Path.bFoundLoS = true;
					Path.LoSPoint = Origin;
					Path.LoSCumDist = CumDist;
					if (Path.Waypoints.Num() > 0) {
						Path.Waypoints.Last().bHasLoS = true;
					}
				}
			}
			if (Path.bFoundLoS) {
				break;
			}
			// Best-case prune, mirroring TickAsyncCast — the replay must die where the async ray dies.
			if (CumDist + LoSSegDist > MaxPathDist) {
				break;
			}

			float RemainingBudget = FMath::Min(MaxRayDistance, MaxPathDist - CumDist);
			if (Settings.MaxStraightFlightDistance > 0.f) {
				RemainingBudget = FMath::Min(RemainingBudget, Settings.MaxStraightFlightDistance);
			}
			if (RemainingBudget < 1.f) {
				break;
			}

			const FVector OriginAtBounceStart = Origin;
			FHitResult RayHit;
			const FVector RayEnd = Origin + Dir * RemainingBudget;
			if (!TraceLine(World, RayHit, Origin, RayEnd)) {
				const float TermDist = FVector::Dist(Origin, RayEnd);
				if (!Path.bFoundLoS && TermDist > 1.f) {
					FVector LoSPt;
					float LoSCumDist;
					if (StepSampleSegmentForLoS(Origin, Dir, TermDist, CumDist, ListenerPos,
					                            MaxPathDist, Settings, World, LoSPt, LoSCumDist)) {
						Path.bFoundLoS = true;
						Path.LoSPoint = LoSPt;
						Path.LoSCumDist = LoSCumDist;
						Path.Waypoints.Add({LoSPt, LoSCumDist, false, true, BounceIdx});
					}
				}
				if (!Path.bFoundLoS) {
					Path.Waypoints.Add({RayEnd, CumDist + TermDist, false, false, BounceIdx});
				}
				CumDist += TermDist;
				if (Path.bFoundLoS) {
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

			if (!Path.bFoundLoS && SegLen > 1.f) {
				FVector LoSPt;
				float LoSCumDist;
				const FVector SegDir = (RayHit.Location - OriginAtBounceStart) / SegLen;
				if (StepSampleSegmentForLoS(OriginAtBounceStart, SegDir, SegLen, SegStartCumDist,
				                            ListenerPos, MaxPathDist, Settings, World, LoSPt, LoSCumDist)) {
					Path.bFoundLoS = true;
					Path.LoSPoint = LoSPt;
					Path.LoSCumDist = LoSCumDist;
					Path.Waypoints.Add({LoSPt, LoSCumDist, false, true, BounceIdx});
				}
			}
			if (Path.bFoundLoS) {
				break;
			}

			if (BounceIdx >= EffMaxBounces) {
				break;
			}

			const int32 BounceIdxAtHit = BounceIdx;
			const float CumDistAtHit = CumDist;
			FRayHitOutput HitOut;
			ProcessRayHit(RayHit, Origin, Dir, CumDist, BounceIdx, bNextHitCrawls,
			              Path.bFoundLoS, bBias, ListenerPos, MaxPathDist, Settings, World, HitOut);

			if (HitOut.bCrawlSucceeded) {
				Path.Waypoints.Add({RayHit.Location, CumDistAtHit, false, false, BounceIdxAtHit});
				const float StepSize = HitOut.CrawlDist / FMath::Max(1, HitOut.CrawlSteps.Num());
				for (int32 StepIdx = 0; StepIdx < HitOut.CrawlSteps.Num(); ++StepIdx) {
					Path.Waypoints.Add({
						HitOut.CrawlSteps[StepIdx], CumDistAtHit + (StepIdx + 1) * StepSize, true, false, BounceIdxAtHit
					});
				}
				Path.CrawlProbeEnds.Append(HitOut.CrawlProbeEnds);

				if (!Path.bFoundLoS && HitOut.bLoSFound) {
					Path.bFoundLoS = true;
					Path.LoSPoint = HitOut.LoSPoint;
					Path.LoSCumDist = HitOut.LoSCumDist;
					break;
				}

				if (HitOut.bPerpWall) {
					Path.Waypoints.Add({HitOut.PerpWallEdgePoint, CumDist, false, false, BounceIdxAtHit});
				}
			}
			else {
				Path.Waypoints.Add({RayHit.Location, CumDistAtHit, false, false, BounceIdxAtHit});
			}
		}

		Path.TotalDist = CumDist;
		Replay.Paths.Add(MoveTemp(Path));
	}

	{
		int32 RaysReached = 0;
		float TotalDist = TNumericLimits<float>::Max();
		FVector WeightedPos = FVector::ZeroVector;
		float TotalW = 0.f;

		StoredLoSPaths.Reset();

		for (const FReplayRayPath& RPPath : Replay.Paths) {
			if (!RPPath.bFoundLoS) {
				continue;
			}
			++RaysReached;
			TotalDist = FMath::Min(TotalDist, RPPath.LoSCumDist + FVector::Dist(RPPath.LoSPoint, ListenerPos));

			const float GeomDist = FVector::Dist(SourcePos, RPPath.LoSPoint);
			const float DistW = 1.f / (1.f + Settings.CandidateDistanceFalloff * GeomDist
				/ FMath::Max(MaxRayDistance, 1.f));
			WeightedPos += RPPath.LoSPoint * DistW;
			TotalW += DistW;
			{
				FStoredLoSPath ReplayPath;
				ReplayPath.LoSOrigin = RPPath.LoSPoint;
				ReplayPath.LoSBounces = 0;
				ReplayPath.LoSCumulativeDistance = GeomDist;
				ReplayPath.PathDist = RPPath.LoSCumDist;
				StoredLoSPaths.Add(MoveTemp(ReplayPath));
			}
		}

		Replay.CastRaysReached = RaysReached;
		Replay.CastTotalDist = TotalDist;

		FAsyncCastManager::FRayAccumulatorInput AccumIn;
		AccumIn.RaysReached = RaysReached;
		AccumIn.MinLoSDist = TotalDist;
		AccumIn.WeightedPos = WeightedPos;
		AccumIn.TotalWeight = TotalW;
		AccumIn.DirectDist = DirectDist;
		AccumIn.MaxRayDistance = MaxRayDistance;
		AccumIn.bDirectLoSFound = false;
		const FAsyncCastManager::FRayAccumulatorOutput AccumOut = FAsyncCastManager::ComputeAudioFromRayAccumulator(AccumIn, Settings);

		if (AccumOut.bHasVirtualSource) {
			TargetVirtualSourceLocation = AccumOut.VirtualSourcePos;
		}
	}

	Replay.AnimTimer = 0.f;
	Replay.PauseTimer = 0.f;
	Replay.bAnimating = true;
}


void USpatialAudioComponent::TickReplayDebug(float DeltaTime, const USpatialAudioSettings& Settings) {
	if (!bDrawDebugRays || !bReplayDebug) {
		return;
	}

	UWorld* World = GetWorld();
	if (!World) {
		return;
	}

	if (Replay.Paths.IsEmpty()) {
		PerformReplaySweep(Settings);
		return;
	}

	const float AnimDuration = ReplayAnimDuration;
	const float PauseDuration = ReplayPauseDuration;
	const float MaxPathDist = FMath::Max(1.f, MaxRayDistance * Settings.TotalPathBudgetMultiplier);

	if (Replay.bAnimating) {
		Replay.AnimTimer += DeltaTime;
		if (Replay.AnimTimer >= AnimDuration) {
			Replay.bAnimating = false;
			Replay.PauseTimer = 0.f;
		}
	}
	else {
		Replay.PauseTimer += DeltaTime;
		if (Replay.PauseTimer >= PauseDuration) {
			PerformReplaySweep(Settings);
			return;
		}
	}

	const float t = Replay.bAnimating ? Replay.AnimTimer : AnimDuration;

	const int32 MaxBounces = Settings.MaxBounces;

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;

	if (GetOwner()) {
		DrawDebugSphere(World, GetOwner()->GetActorLocation(), 12.f, 8, FColor::Magenta, false, 0.f, 0, 1.5f);
	}
	if (Pawn) {
		DrawDebugSphere(World, Pawn->GetActorLocation(), 12.f, 8, FColor::Yellow, false, 0.f, 0, 1.5f);
	}

	for (const FReplayRayPath& Path : Replay.Paths) {
		const int32 N = Path.Waypoints.Num();

		int32 CrawlProbeIdx = 0;

		for (int32 WaypointIdx = 0; WaypointIdx + 1 < N; ++WaypointIdx) {
			const FReplayWaypoint& WA = Path.Waypoints[WaypointIdx];
			const FReplayWaypoint& WB = Path.Waypoints[WaypointIdx + 1];

			const float SegT0 = (WA.CumDist / MaxPathDist) * AnimDuration;
			const float SegT1 = (WB.CumDist / MaxPathDist) * AnimDuration;

			if (t < SegT0) {
				continue;
			}

			FVector DrawEnd;
			if (SegT1 <= SegT0 || t >= SegT1) {
				DrawEnd = WB.Position;
			}
			else {
				DrawEnd = FMath::Lerp(WA.Position, WB.Position, (t - SegT0) / (SegT1 - SegT0));
			}

			const FColor LineColor = WB.bIsCrawlStep
				                         ? FColor(0, 220, 255)
				                         : ComputeBounceGradientColor(WB.BounceIndex, MaxBounces);
			DrawDebugLine(World, WA.Position, DrawEnd, LineColor, false, 0.f, 0, 1.5f);

			if (t >= SegT1 && !WB.bIsCrawlStep) {
				DrawDebugSphere(World, WB.Position, 6.f, 6, LineColor, false, 0.f, 0, 1.f);
			}

			if (WB.bIsCrawlStep) {
				if (t >= SegT1 && CrawlProbeIdx < Path.CrawlProbeEnds.Num()) {
					DrawDebugLine(World, WB.Position, Path.CrawlProbeEnds[CrawlProbeIdx], FColor::Magenta, false, 0.f,
					              0, 1.0f);
				}
				++CrawlProbeIdx;
			}
		}

		if (Path.bFoundLoS) {
			const float LosT = (Path.LoSCumDist / MaxPathDist) * AnimDuration;
			if (t >= LosT) {
				DrawDebugLine(World, Path.LoSPoint, Path.ListenerPos, FColor::Green, false, 0.f, 0, 1.5f);
				DrawDebugSphere(World, Path.LoSPoint, 8.f, 8, FColor::Green, false, 0.f, 0, 1.f);
			}
		}
	}
}
