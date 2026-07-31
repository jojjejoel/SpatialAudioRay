#include "Audio/SpatialAudioDebugSubsystem.h"

#include "Audio/SpatialAudioComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"

void USpatialAudioDebugSubsystem::Register(USpatialAudioComponent* Component) {
	// Kept in lockstep with Sources (not AddUnique) so bEligibleForDebugRays's index always
	// lines up — a duplicate registration must add to neither array.
	if (!Sources.Contains(Component)) {
		Sources.Add(Component);
		bEligibleForDebugRays.Add(Component->bDrawDebugRays);
	}
	// Two registrations naming the same owner = the orphaned-Blueprint-component duplicate
	// (stale inherited copy still instantiates alongside the C++ one); fix by reparenting the
	// Blueprint to AActor and back.
	UE_LOG(LogTemp, Log, TEXT("SpatialAudioDebugSubsystem: +%s on %s (%d registered)"),
	       *Component->GetName(), *GetNameSafe(Component->GetOwner()), Sources.Num());
}

void USpatialAudioDebugSubsystem::Unregister(USpatialAudioComponent* Component) {
	const int32 Index = Sources.IndexOfByPredicate(
		[Component](const TWeakObjectPtr<USpatialAudioComponent>& Src) { return Src.Get() == Component; });
	if (Index != INDEX_NONE) {
		Sources.RemoveAt(Index);
		bEligibleForDebugRays.RemoveAt(Index);
	}
	UE_LOG(LogTemp, Log, TEXT("SpatialAudioDebugSubsystem: -%s on %s (%d registered)"),
	       *Component->GetName(), *GetNameSafe(Component->GetOwner()), Sources.Num());
}

TStatId USpatialAudioDebugSubsystem::GetStatId() const {
	RETURN_QUICK_DECLARE_CYCLE_STAT(USpatialAudioDebugSubsystem, STATGROUP_Tickables);
}

USpatialAudioDebugSubsystem::FAggregateTraceStats USpatialAudioDebugSubsystem::AggregateSourceTraceStats() {
	FAggregateTraceStats Stats;
	for (int32 i = Sources.Num() - 1; i >= 0; --i) {
		const USpatialAudioComponent* C = Sources[i].Get();
		if (!C) {
			Sources.RemoveAt(i);
			bEligibleForDebugRays.RemoveAt(i);
			continue;
		}
		++Stats.NumSources;
		Stats.TracesPerSec += C->TraceDiag.SnapshotTracesPerSec;
		Stats.Avg10Sec += C->TraceDiag.Avg10Sec;
		Stats.Avg60Sec += C->TraceDiag.Avg60Sec;
	}
	PeakTracesPerSec = FMath::Max(PeakTracesPerSec, Stats.TracesPerSec);
	Stats.PeakTracesPerSec = PeakTracesPerSec;
	return Stats;
}

bool USpatialAudioDebugSubsystem::ComputeAnyDebugRaysActive() const {
	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (const USpatialAudioComponent* C = Src.Get(); C && C->bDrawDebugRays) {
			return true;
		}
	}
	return false;
}

void USpatialAudioDebugSubsystem::HandleCycleKey(const USpatialAudioComponent& First, const APlayerController* PC) {
	// The cycle key is polled BEFORE the bAnyDebugRays gate — cycling back in from the
	// all-off stop is exactly the state where that gate rejects everything else.
	if (!PC) {
		return;
	}
	const bool bDown = First.CycleDebugSourceKey.IsValid() && PC->IsInputKeyDown(First.CycleDebugSourceKey);
	if (bDown && !bPrevCycleKeyDown) {
		bCycleModeActive = CycleDebugRaySource();
	}
	bPrevCycleKeyDown = bDown;
}

void USpatialAudioDebugSubsystem::HandleActorLabelsToggleAndDraw(const USpatialAudioComponent& First, const APlayerController* PC) {
	// The toggle key itself stays independent of bAnyDebugRays (works even with ray debugging
	// fully off), but which sources actually get a label (below) is restricted to whichever
	// ones bDrawDebugRays is already true for — the cycle-selected source, or the proximity-
	// limited in-range set — so labels only ever appear on sources that are also drawing.
	if (PC) {
		const bool bDown = First.ToggleActorLabelsKey.IsValid()
			&& PC->IsInputKeyDown(First.ToggleActorLabelsKey);
		if (bDown && !bPrevActorLabelsKeyDown) {
			bShowActorLabels = !bShowActorLabels;
		}
		bPrevActorLabelsKeyDown = bDown;
	}

	if (!bShowActorLabels) {
		return;
	}
	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (const USpatialAudioComponent* C = Src.Get(); C && C->bDrawDebugRays) {
			if (AActor* Owner = C->GetOwner()) {
				// DrawDebugString renders as screen-space text at the point's projected
				// position — camera-facing and not depth-tested against geometry for free.
				DrawDebugString(GetWorld(), Owner->GetActorLocation(), Owner->GetActorNameOrLabel(),
				                 nullptr, FColor::White, 0.f, true);
			}
		}
	}
}

void USpatialAudioDebugSubsystem::HandleSubModeKeyToggles(const USpatialAudioComponent& First, const APlayerController* PC) {
	// Polled here rather than per component: with the source cycle at most one component draws
	// (and the old per-component poll only ran while drawing), so hidden sources would stop
	// seeing presses and their flags would desync from the visible one. One edge-detector
	// assigning !First to every source keeps all flags identical, so the cycled-to source
	// always shows the same sub-mode set just toggled on the previous one.
	if (!PC) {
		return;
	}
	auto ApplyToggle = [&](const FKey& Key, bool& bPrevDown, bool USpatialAudioComponent::* Flag) {
		const bool bDown = Key.IsValid() && PC->IsInputKeyDown(Key);
		if (bDown && !bPrevDown) {
			const bool bNew = !(First.*Flag);
			for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
				if (USpatialAudioComponent* C = Src.Get()) {
					C->*Flag = bNew;
				}
			}
		}
		bPrevDown = bDown;
	};

	ApplyToggle(First.ToggleVirtualSourceKey, bPrevSubModeKeyDown[0], &USpatialAudioComponent::bShowVirtualSourceRays);
	ApplyToggle(First.ToggleBounceRaysKey, bPrevSubModeKeyDown[1], &USpatialAudioComponent::bShowBounceRays);
	ApplyToggle(First.ToggleDebugTextKey, bPrevSubModeKeyDown[2], &USpatialAudioComponent::bShowDebugText);
	ApplyToggle(First.ToggleDiffractionPathsKey, bPrevSubModeKeyDown[4], &USpatialAudioComponent::bShowDiffractionPaths);
	ApplyToggle(First.ToggleEdgePointsKey, bPrevSubModeKeyDown[5], &USpatialAudioComponent::bShowEdgePoints);
	ApplyToggle(First.ToggleSurfaceCrawlKey, bPrevSubModeKeyDown[6], &USpatialAudioComponent::bShowSurfaceCrawl);
	ApplyToggle(First.ToggleLoSChecksKey, bPrevSubModeKeyDown[7], &USpatialAudioComponent::bShowLoSChecks);
	ApplyToggle(First.ToggleOffsetLoSChecksKey, bPrevSubModeKeyDown[8], &USpatialAudioComponent::bShowOffsetLoSChecks);
	ApplyToggle(First.ToggleShortestPathsKey, bPrevSubModeKeyDown[9], &USpatialAudioComponent::bShowShortestPaths);
	ApplyToggle(First.ToggleSteeringPredictionKey, bPrevSubModeKeyDown[10], &USpatialAudioComponent::bShowSteeringPrediction);

	const bool bDown = First.ToggleGlobalDebugTextKey.IsValid()
		&& PC->IsInputKeyDown(First.ToggleGlobalDebugTextKey);
	if (bDown && !bPrevToggleKeyDown) {
		bShowGlobalDebugText = !bShowGlobalDebugText;
	}
	bPrevToggleKeyDown = bDown;
}

void USpatialAudioDebugSubsystem::DrawGlobalDebugHUD(const FAggregateTraceStats& Stats) {
	// Fixed keys 1..N+1 — the per-component slots key off GetUniqueID() * 10, which never
	// lands this low.
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Yellow,
	                                 FString::Printf(
		                                 TEXT("GLOBAL  %d source%s  │  traces 1s=%.0f/s  10s=%.0f/s  60s=%.0f/s  peak=%.0f/s"),
		                                 Stats.NumSources, Stats.NumSources == 1 ? TEXT("") : TEXT("s"),
		                                 Stats.TracesPerSec, Stats.Avg10Sec, Stats.Avg60Sec,
		                                 Stats.PeakTracesPerSec));

	int32 LineKey = 2;
	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (const USpatialAudioComponent* C = Src.Get()) {
			const AActor* Owner = C->GetOwner();
			GEngine->AddOnScreenDebugMessage(LineKey++, 0.f, FColor(255, 255, 160),
			                                 FString::Printf(
				                                 TEXT("  %s  │  traces 1s=%.0f/s  10s=%.0f/s  60s=%.0f/s  peak=%.0f/s  │  moving=%.0f/s  rest=%.0f/s"),
				                                 Owner ? *Owner->GetActorNameOrLabel() : TEXT("None"),
				                                 C->TraceDiag.SnapshotTracesPerSec,
				                                 C->TraceDiag.Avg10Sec,
				                                 C->TraceDiag.Avg60Sec,
				                                 C->TraceDiag.PeakTracesPerSec,
				                                 C->TraceDiag.MovingTracesPerSec(),
				                                 C->TraceDiag.RestTracesPerSec()));
		}
	}
}

void USpatialAudioDebugSubsystem::Tick(float DeltaTime) {
	Super::Tick(DeltaTime);
	if (!GEngine) {
		return;
	}

	const FAggregateTraceStats Stats = AggregateSourceTraceStats();

	// All key config is read from the first registered source, like the G key always was —
	// per-source rebinding is not supported.
	USpatialAudioComponent* First = Sources.Num() > 0 ? Sources[0].Get() : nullptr;
	if (!First) {
		return;
	}

	const APlayerController* PC =
		FSlateApplication::IsInitialized() ? GetWorld()->GetFirstPlayerController() : nullptr;

	HandleCycleKey(*First, PC);

	// Not single-source-cycled (never pressed N, or cycled back around to OFF): cap how many
	// originally-enabled sources actually draw to the closest N, so a level with several
	// debug-enabled sources doesn't draw all of them at once before one is picked via N.
	if (!bCycleModeActive) {
		ApplyProximityDebugLimit(*First, PC);
	}

	const bool bAnyDebugRays = ComputeAnyDebugRaysActive();

	HandleActorLabelsToggleAndDraw(*First, PC);

	// Gated on the master debug switch only, NOT on per-source bShowDebugText — key 3 hides
	// the per-source blocks without taking the global line with them.
	if (!bAnyDebugRays) {
		return;
	}

	HandleSubModeKeyToggles(*First, PC);

	if (!bShowGlobalDebugText) {
		return;
	}

	DrawGlobalDebugHUD(Stats);
}

bool USpatialAudioDebugSubsystem::CycleDebugRaySource() {
	// "Selected" = the first drawing source. Editor setups can start with several enabled;
	// the first press collapses that to single-selection and the cycle proceeds from there.
	int32 Selected = INDEX_NONE;
	for (int32 i = 0; i < Sources.Num(); ++i) {
		const USpatialAudioComponent* C = Sources[i].Get();
		if (C && C->bDrawDebugRays) {
			Selected = i;
			break;
		}
	}

	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (USpatialAudioComponent* C = Src.Get()) {
			C->bDrawDebugRays = false;
		}
	}

	// INDEX_NONE + 1 == 0: the all-off stop advances to the first source.
	const int32 Next = Selected + 1;
	if (Sources.IsValidIndex(Next)) {
		if (USpatialAudioComponent* C = Sources[Next].Get()) {
			C->bDrawDebugRays = true;
			// Message key 0 sits below the fixed global-HUD keys (1..N+1), so repeated
			// presses replace the announcement instead of stacking.
			GEngine->AddOnScreenDebugMessage(0, 2.f, FColor::Cyan,
			                                 FString::Printf(TEXT("Debug rays: %s (%d/%d)"),
			                                                 *GetNameSafe(C->GetOwner()),
			                                                 Next + 1, Sources.Num()));
			return true;
		}
	}

	GEngine->AddOnScreenDebugMessage(0, 2.f, FColor::Cyan, TEXT("Debug rays: OFF"));
	return false;
}

void USpatialAudioDebugSubsystem::ApplyProximityDebugLimit(const USpatialAudioComponent& First,
                                                            const APlayerController* PC) {
	const int32 MaxSources = First.GetSettings().MaxUncycledDebugSources;
	if (MaxSources <= 0 || !PC || !PC->GetPawn()) {
		return;
	}
	const FVector ListenerPos = PC->GetPawn()->GetActorLocation();

	// Rank only the originally-enabled sources (bEligibleForDebugRays, snapshotted at
	// registration) — ineligible ones are never touched here, so a source the user genuinely
	// left off in the editor stays off rather than being pulled in by proximity.
	TArray<int32> EligibleIndices;
	for (int32 i = 0; i < Sources.Num(); ++i) {
		if (bEligibleForDebugRays.IsValidIndex(i) && bEligibleForDebugRays[i] && Sources[i].IsValid()) {
			EligibleIndices.Add(i);
		}
	}

	EligibleIndices.Sort([this, &ListenerPos](const int32 A, const int32 B) {
		const AActor* OwnerA = Sources[A]->GetOwner();
		const AActor* OwnerB = Sources[B]->GetOwner();
		const float DistA = OwnerA ? FVector::DistSquared(OwnerA->GetActorLocation(), ListenerPos)
		                           : TNumericLimits<float>::Max();
		const float DistB = OwnerB ? FVector::DistSquared(OwnerB->GetActorLocation(), ListenerPos)
		                           : TNumericLimits<float>::Max();
		return DistA < DistB;
	});

	// Re-derived every tick from live distances, so a suppressed source comes back on as the
	// player approaches and a drawing one turns off as they move away.
	for (int32 Rank = 0; Rank < EligibleIndices.Num(); ++Rank) {
		if (USpatialAudioComponent* C = Sources[EligibleIndices[Rank]].Get()) {
			C->bDrawDebugRays = Rank < MaxSources;
		}
	}
}
