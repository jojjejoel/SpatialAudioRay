// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/SpatialAudioComponent.h"
#include "Audio/SpatialAudioSettings.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

namespace {
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
		bool bBothStationary, bool bStationaryIdleMode,
		float MovementCacheFillMultiplier, float StoredEffFullSweepInterval,
		float StationaryIdleMultiplier, float CoverageFraction,
		float SmoothedSourceSpeed, float SmoothedListenerSpeed,
		int32 CacheFillSweepsRemaining, int32 UsableEdges, int32 RequiredEdges) {
		FPacingDebugInfo Info;
		if (bBothStationary && CacheFillSweepsRemaining > 0 && UsableEdges < RequiredEdges) {
			Info.Label = FString::Printf(TEXT("CACHE-FILL (%.2f×  %.2fs  new edges %d/%d  %d sweeps left)"),
			                             MovementCacheFillMultiplier, StoredEffFullSweepInterval,
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

}

void USpatialAudioComponent::DrawSteeringPredictionDebug(const USpatialAudioSettings& Settings) const {
	if (!bShowSteeringPrediction || Settings.SteeringPredictionLeadTime <= 0.f) {
		return;
	}
	const bool bRetroSteer = TimeSinceHadDirectLoS <= Settings.SteeringPredictionLeadTime;
	const FColor SteerColor = bRetroSteer ? FColor(255, 140, 0) : FColor(0, 128, 255);
	const APlayerController* PredPC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (const APawn* PredPawn = PredPC ? PredPC->GetPawn() : nullptr) {
		const FVector LisLead = ComputeSteeringLead(VelocityScaling.SmoothedListenerVelocity, Settings);
		if (LisLead.SizeSquared() > FMath::Square(10.f)) {
			const FVector LisNow = PredPawn->GetActorLocation();
			DrawDebugSphere(GetWorld(), LisNow + LisLead, 16.f, 8, SteerColor, false, -1.f, SDPG_Foreground, 1.5f);
			DrawDebugLine(GetWorld(), LisNow, LisNow + LisLead, SteerColor, false, -1.f, 0, 1.f);
		}
	}
	if (GetOwner()) {
		const FVector SrcLead = ComputeSteeringLead(VelocityScaling.SmoothedSourceVelocity, Settings);
		if (SrcLead.SizeSquared() > FMath::Square(10.f)) {
			const FVector SrcNow = GetOwner()->GetActorLocation();
			DrawDebugSphere(GetWorld(), SrcNow + SrcLead, 16.f, 8, SteerColor, false, -1.f, SDPG_Foreground, 1.5f);
			DrawDebugLine(GetWorld(), SrcNow, SrcNow + SrcLead, SteerColor, false, -1.f, 0, 1.f);
		}
	}
}

void USpatialAudioComponent::DrawVirtualSourceDebug() {
	if (!bShowVirtualSourceRays) {
		return;
	}
	const FVector ActorLoc = GetOwner()->GetActorLocation();
	DrawDebugSphere(GetWorld(), ActorLoc, 20.f, 8, FColor::Magenta, false, -1.f, SDPG_Foreground, 1.f);

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
		const float SphereRadius = bFadingOut ? 12.f : 20.f;
		DrawDebugSphere(GetWorld(), SlotPos, SphereRadius, 8, Color, false, -1.f, SDPG_Foreground, 2.f);
		DrawDebugString(GetWorld(), SlotPos + FVector(0.f, 0.f, SphereRadius + 4.f),
		                 FString::Printf(TEXT("%s_%02d"), *GetOwner()->GetActorNameOrLabel(), SlotIdx),
		                 nullptr, FColor::White, 0.f, false, 1.1f);
	}
}

void USpatialAudioComponent::DrawEdgePointsDebug() {
	if (!bShowEdgePoints) {
		return;
	}
	for (const FCachedEdgePoint& EP : CachedEdgePoints) {
		DrawDebugSphere(GetWorld(), EP.EdgePoint, 18.f, 8, FColor::Yellow, false, -1.f, SDPG_Foreground, 2.f);
		if (EP.bRelayed) {
			DrawDebugSphere(GetWorld(), EP.RelayPoint, 14.f, 8, FColor::Yellow, false,
			                GetSettings().DebugLineDuration, SDPG_Foreground, 1.f);
			DrawDebugLine(GetWorld(), EP.EdgePoint, EP.RelayPoint, FColor::Yellow, false,
			              GetSettings().DebugLineDuration, 0, 1.f);
		}
	}
}

void USpatialAudioComponent::DrawShortestPathsDebug() {
	if (!bShowShortestPaths) {
		return;
	}
	for (const FCachedEdgePoint& EP : CachedEdgePoints) {
		for (int32 i = 0; i + 1 < EP.ShortestPath.Num(); ++i) {
			const bool bVerified = EP.ShortestPathSegmentVerified.IsValidIndex(i)
				                       && EP.ShortestPathSegmentVerified[i];
			DrawDebugLine(GetWorld(), EP.ShortestPath[i], EP.ShortestPath[i + 1],
			              bVerified ? FColor::Magenta : FColor(120, 0, 120),
			              false, -1.f, 0, 2.f);
		}
		for (int32 i = 1; i + 1 < EP.ShortestPath.Num(); ++i) {
			const bool bVerifiedPoint = EP.ShortestPathSegmentVerified.IsValidIndex(i)
				                            && EP.ShortestPathSegmentVerified[i];
			DrawDebugSphere(GetWorld(), EP.ShortestPath[i], 8.f, 8,
			                bVerifiedPoint ? FColor::Magenta : FColor(120, 0, 120),
			                false, -1.f, SDPG_Foreground, 1.5f);
		}
		if (EP.bRelayed && !EP.ShortestPath.IsEmpty()) {
			DrawDebugLine(GetWorld(), EP.ShortestPath.Last(), EP.RelayPoint, FColor::Magenta,
			              false, GetSettings().DebugLineDuration, 0, 2.f);
			DrawDebugSphere(GetWorld(), EP.RelayPoint, 8.f, 8, FColor::Magenta, false,
			                GetSettings().DebugLineDuration, SDPG_Foreground, 1.5f);
		}
	}
}

void USpatialAudioComponent::DrawDiffractionPathsDebug() {
	if (!bShowDiffractionPaths) {
		return;
	}
	for (const TArray<FVector>& Path : LoSDiffractionPaths) {
		for (int32 i = 0; i + 1 < Path.Num(); ++i) {
			DrawDebugLine(GetWorld(), Path[i], Path[i + 1], FColor::Cyan, false, -1.f, 0, 1.5f);
		}

		for (int32 i = 1; i + 1 < Path.Num(); ++i) {
			DrawDebugSphere(GetWorld(), Path[i], 8.f, 5, FColor::Cyan, false, -1.f, SDPG_Foreground, 1.f);
		}
	}
}

void USpatialAudioComponent::DrawSourceAudioDebugText(const uint64 Base) const {
	const FColor SrcColor = SelectThresholdColor(1.f - AudioDiag.CurvedOcclusion, 0.7f, 0.3f);
	GEngine->AddOnScreenDebugMessage(Base + 1, 0.f, SrcColor,
	                                 FString::Printf(TEXT("  SRC  occ_param=%3.0f%%"),
	                                                 AudioDiag.CurvedOcclusion * 100.f));
}

void USpatialAudioComponent::DrawVirtualAudioDebugText(const uint64 Base) const {
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
		                                 TEXT("  VRT  gain=%3.0f%%  xfade=%3.0f%%  bend=%3.0f%%  │  voices=%s%s"),
		                                 AudioDiag.VirtualGain * 100.f, CurrentVirtualCrossfade * 100.f,
		                                 AudioDiag.VirtualPathBend * 100.f,
		                                 *VoiceCount, *VoiceStr));
}

void USpatialAudioComponent::DrawOcclusionDebugText(const uint64 Base) const {
	const FString Bar = BuildProgressBar(CurrentOcclusion);
	const FColor OccColor = SelectThresholdColor(1.f - CurrentOcclusion, 0.7f, 0.3f);
	GEngine->AddOnScreenDebugMessage(Base + 3, 0.f, OccColor,
	                                 FString::Printf(
		                                 TEXT(
			                                 "  Occ %s  cur=%3.0f%%  tgt=%3.0f%%  │  LoS:%s  frac inst=%.0f%% avg=%.0f%% smooth=%.0f%%"),
		                                 *Bar, CurrentOcclusion * 100.f, TargetOcclusion * 100.f,
		                                 bHasDirectLoS ? TEXT("YES") : TEXT("NO"),
		                                 LastOffsetLoSFraction * 100.f,
		                                 WindowedLoSFraction * 100.f,
		                                 LastDirectLoSFraction * 100.f));
}

void USpatialAudioComponent::DrawEdgeCacheDebugText(const uint64 Base, const USpatialAudioSettings& Settings) const {
	int32 NumEvicting = 0, NumPhase0 = 0;
	for (const FCachedEdgePoint& EP : CachedEdgePoints) {
		if (EP.bEvicting) ++NumEvicting;
		if (EP.bPhase0Pending) ++NumPhase0;
	}
	GEngine->AddOnScreenDebugMessage(Base + 5, 0.f, FColor::Orange,
	                                 FString::Printf(
		                                 TEXT(
			                                 "  Edges  %d/%d cached  (evict=%d  ph0=%d)  │  PathAtten  cur=%3.0f%%  tgt=%3.0f%%"),
		                                 CachedEdgePoints.Num(), Settings.CachedEdgeMaxCount,
		                                 NumEvicting, NumPhase0,
		                                 CurrentPathAttenuation * 100.f, TargetPathAttenuation * 100.f));
}

void USpatialAudioComponent::DrawSweepPacingDebugText(const uint64 Base, const USpatialAudioSettings& Settings,
                                                       const int32 ScaledRayCount) const {
	const bool bBothStationary = VelocityScaling.IsStationary();
	const float CoverageFraction = (ScaledRayCount > 0)
		                      ? FMath::Clamp(
			                      static_cast<float>(CachedEdgePoints.Num()) /
			                      static_cast<float>(ScaledRayCount), 0.f, 1.f)
		                      : 0.f;

	const auto [PacingLabel, PacingColor] = ComputePacingDebugInfo(
		bBothStationary, SweepScheduling.bStationaryIdleMode,
		Settings.MovementCacheFillMultiplier, StoredEffFullSweepInterval,
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

	const bool bPreSweepBand = IsPreSweepActive();
	const bool bSweepSuspended = bHasDirectLoS && !bPreSweepBand && !bAsyncCastActive;
	FString SweepLine;
	if (bSweepSuspended && LastOffsetLoSFraction <= 0.f) {
		SweepLine = FString::Printf(TEXT("  Sweep  SUSPENDED (confirming LoS loss %d/%d)  │  timer=%.2f/%.2fs"),
		                            NoLoSSampleStreak,
		                            ResolveRingRotationSteps(),
		                            TimeSinceFullCast, SubInterval);
	}
	else if (bSweepSuspended) {
		SweepLine = FString::Printf(TEXT("  Sweep  SUSPENDED (clear LoS)  │  timer=%.2f/%.2fs"),
		                            TimeSinceFullCast, SubInterval);
	}
	else {
		SweepLine = FString::Printf(TEXT("  Sweep  %s%s  │  timer=%.2f/%.2fs  [%s]"),
		                            bPreSweepBand ? TEXT("PRE-SWEEP ") : TEXT(""),
		                            *PacingLabel, TimeSinceFullCast, SubInterval, *SweepStatus);
	}
	GEngine->AddOnScreenDebugMessage(Base + 6, 0.f, bSweepSuspended ? FColor::Green : PacingColor, SweepLine);
}

void USpatialAudioComponent::DrawTraceStatsDebugText(const uint64 Base) const {
	GEngine->AddOnScreenDebugMessage(Base + 7, 0.f, FColor::Cyan,
	                                 FString::Printf(
		                                 TEXT(
			                                 "  Traces  ~%.0f/fr  1s=%.0f/s  10s=%.0f/s  │  Last sweep: %d rays  %d fr  %.0fms/%.0fms%s"),
		                                 TraceDiag.SnapshotFrameTraces, TraceDiag.SnapshotTracesPerSec, TraceDiag.Avg10Sec,
		                                 TraceDiag.LastSweepAsyncRays, TraceDiag.LastSweepFrames,
		                                 TraceDiag.LastSweepDuration * 1000.f, TraceDiag.LastSweepInterval * 1000.f,
		                                 TraceDiag.LastSweepDuration > TraceDiag.LastSweepInterval ? TEXT("  OVER") : TEXT("")));

	const float* Buckets = TraceDiag.SnapshotBucketTraces;
	GEngine->AddOnScreenDebugMessage(Base + 9, 0.f, FColor::Cyan,
	                                 FString::Printf(
		                                 TEXT("    of which  swp %.1f · occ %.1f · ph0 %.1f · rly %.1f · bis %.1f · pth %.1f · oth %.1f"),
		                                 Buckets[static_cast<int32>(ETraceBucket::Sweep)],
		                                 Buckets[static_cast<int32>(ETraceBucket::Occlusion)],
		                                 Buckets[static_cast<int32>(ETraceBucket::Phase0)],
		                                 Buckets[static_cast<int32>(ETraceBucket::Relay)],
		                                 Buckets[static_cast<int32>(ETraceBucket::Bisect)],
		                                 Buckets[static_cast<int32>(ETraceBucket::PathCheck)],
		                                 Buckets[static_cast<int32>(ETraceBucket::Other)]));
}

void USpatialAudioComponent::DrawEdgeTimerDebugText(const uint64 Base, const USpatialAudioSettings& Settings) const {
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

void USpatialAudioComponent::DrawDebugTextHUD(const USpatialAudioSettings& Settings) const {
	if (!bShowDebugText || !GEngine) {
		return;
	}
	const uint64 Base = static_cast<uint64>(GetUniqueID()) * 10;

	int32 ScaledRayCount;
	float Prio;
	GetEffectiveRayCounts(ScaledRayCount, Prio);

	DrawSourceAudioDebugText(Base);
	DrawVirtualAudioDebugText(Base);
	DrawOcclusionDebugText(Base);
	DrawEdgeCacheDebugText(Base, Settings);
	DrawSweepPacingDebugText(Base, Settings, ScaledRayCount);
	DrawTraceStatsDebugText(Base);
	DrawEdgeTimerDebugText(Base, Settings);
}

void USpatialAudioComponent::DrawDebugLegends() const {
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

void USpatialAudioComponent::DrawDebugVisualization(const USpatialAudioSettings& Settings) {
	DrawSteeringPredictionDebug(Settings);
	DrawVirtualSourceDebug();
	DrawEdgePointsDebug();
	DrawShortestPathsDebug();
	DrawDiffractionPathsDebug();
	DrawDebugTextHUD(Settings);
	DrawDebugLegends();
}
