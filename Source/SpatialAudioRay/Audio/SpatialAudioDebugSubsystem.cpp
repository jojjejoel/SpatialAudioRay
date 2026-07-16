#include "Audio/SpatialAudioDebugSubsystem.h"

#include "Audio/SpatialAudioComponent.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"

void USpatialAudioDebugSubsystem::Register(USpatialAudioComponent* Component) {
	Sources.AddUnique(Component);
	// Two registrations naming the same owner = the orphaned-Blueprint-component duplicate
	// (stale inherited copy still instantiates alongside the C++ one); fix by reparenting the
	// Blueprint to AActor and back.
	UE_LOG(LogTemp, Log, TEXT("SpatialAudioDebugSubsystem: +%s on %s (%d registered)"),
	       *Component->GetName(), *GetNameSafe(Component->GetOwner()), Sources.Num());
}

void USpatialAudioDebugSubsystem::Unregister(USpatialAudioComponent* Component) {
	Sources.Remove(Component);
	UE_LOG(LogTemp, Log, TEXT("SpatialAudioDebugSubsystem: -%s on %s (%d registered)"),
	       *Component->GetName(), *GetNameSafe(Component->GetOwner()), Sources.Num());
}

TStatId USpatialAudioDebugSubsystem::GetStatId() const {
	RETURN_QUICK_DECLARE_CYCLE_STAT(USpatialAudioDebugSubsystem, STATGROUP_Tickables);
}

void USpatialAudioDebugSubsystem::Tick(float DeltaTime) {
	Super::Tick(DeltaTime);
	if (!GEngine) {
		return;
	}

	int32 NumSources = 0;
	int32 NumSweeping = 0;
	float TracesPerSec = 0.f;
	float Avg10Sec = 0.f;
	float Avg60Sec = 0.f;
	bool bAnyDebugRays = false;

	for (int32 i = Sources.Num() - 1; i >= 0; --i) {
		const USpatialAudioComponent* C = Sources[i].Get();
		if (!C) {
			Sources.RemoveAt(i);
			continue;
		}
		++NumSources;
		NumSweeping += C->bAsyncCastActive ? 1 : 0;
		TracesPerSec += C->TraceDiag.SnapshotTracesPerSec;
		Avg10Sec += C->TraceDiag.Avg10Sec;
		Avg60Sec += C->TraceDiag.Avg60Sec;
		bAnyDebugRays |= C->bDrawDebugRays;
	}

	// All key config is read from the first registered source, like the G key always was —
	// per-source rebinding is not supported.
	USpatialAudioComponent* First = Sources.Num() > 0 ? Sources[0].Get() : nullptr;
	if (!First) {
		return;
	}

	const APlayerController* PC =
		FSlateApplication::IsInitialized() ? GetWorld()->GetFirstPlayerController() : nullptr;

	// The cycle key is polled BEFORE the bAnyDebugRays gate — cycling back in from the
	// all-off stop is exactly the state where that gate rejects everything else.
	if (PC) {
		const bool bDown = First->CycleDebugSourceKey.IsValid()
			&& PC->IsInputKeyDown(First->CycleDebugSourceKey);
		if (bDown && !bPrevCycleKeyDown) {
			bAnyDebugRays = CycleDebugRaySource();
		}
		bPrevCycleKeyDown = bDown;
	}

	// Gated on the master debug switch only, NOT on per-source bShowDebugText — key 3 hides
	// the per-source blocks without taking the global line with them.
	if (!bAnyDebugRays) {
		return;
	}

	// Polled here rather than per component: with the source cycle at most one component draws
	// (and the old per-component poll only ran while drawing), so hidden sources would stop
	// seeing presses and their flags would desync from the visible one. One edge-detector
	// assigning !First to every source keeps all flags identical, so the cycled-to source
	// always shows the same sub-mode set just toggled on the previous one.
	if (PC) {
		auto ApplyToggle = [&](const FKey& Key, bool& bPrevDown, bool USpatialAudioComponent::* Flag) {
			const bool bDown = Key.IsValid() && PC->IsInputKeyDown(Key);
			if (bDown && !bPrevDown) {
				const bool bNew = !(First->*Flag);
				for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
					if (USpatialAudioComponent* C = Src.Get()) {
						C->*Flag = bNew;
					}
				}
			}
			bPrevDown = bDown;
		};

		ApplyToggle(First->ToggleVirtualSourceKey, bPrevSubModeKeyDown[0], &USpatialAudioComponent::bShowVirtualSourceRays);
		ApplyToggle(First->ToggleBounceRaysKey, bPrevSubModeKeyDown[1], &USpatialAudioComponent::bShowBounceRays);
		ApplyToggle(First->ToggleDebugTextKey, bPrevSubModeKeyDown[2], &USpatialAudioComponent::bShowDebugText);
		ApplyToggle(First->ToggleReplayDebugKey, bPrevSubModeKeyDown[3], &USpatialAudioComponent::bReplayDebug);
		ApplyToggle(First->ToggleDiffractionPathsKey, bPrevSubModeKeyDown[4], &USpatialAudioComponent::bShowDiffractionPaths);
		ApplyToggle(First->ToggleEdgePointsKey, bPrevSubModeKeyDown[5], &USpatialAudioComponent::bShowEdgePoints);
		ApplyToggle(First->ToggleSurfaceCrawlKey, bPrevSubModeKeyDown[6], &USpatialAudioComponent::bShowSurfaceCrawl);
		ApplyToggle(First->ToggleLoSChecksKey, bPrevSubModeKeyDown[7], &USpatialAudioComponent::bShowLoSChecks);
		ApplyToggle(First->ToggleOffsetLoSChecksKey, bPrevSubModeKeyDown[8], &USpatialAudioComponent::bShowOffsetLoSChecks);
		ApplyToggle(First->ToggleShortestPathsKey, bPrevSubModeKeyDown[9], &USpatialAudioComponent::bShowShortestPaths);
		ApplyToggle(First->ToggleSteeringPredictionKey, bPrevSubModeKeyDown[10], &USpatialAudioComponent::bShowSteeringPrediction);

		const bool bDown = First->ToggleGlobalDebugTextKey.IsValid()
			&& PC->IsInputKeyDown(First->ToggleGlobalDebugTextKey);
		if (bDown && !bPrevToggleKeyDown) {
			bShowGlobalDebugText = !bShowGlobalDebugText;
		}
		bPrevToggleKeyDown = bDown;
	}

	if (!bShowGlobalDebugText) {
		return;
	}

	// Fixed keys 1..N+1 — the per-component slots key off GetUniqueID() * 10, which never
	// lands this low.
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Yellow,
	                                 FString::Printf(
		                                 TEXT("GLOBAL  %d source%s (%d sweeping)  │  traces 1s=%.0f/s  10s=%.0f/s  60s=%.0f/s"),
		                                 NumSources, NumSources == 1 ? TEXT("") : TEXT("s"), NumSweeping,
		                                 TracesPerSec, Avg10Sec, Avg60Sec));

	int32 LineKey = 2;
	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (const USpatialAudioComponent* C = Src.Get()) {
			GEngine->AddOnScreenDebugMessage(LineKey++, 0.f, FColor(255, 255, 160),
			                                 FString::Printf(
				                                 TEXT("  %s  │  traces 1s=%.0f/s  10s=%.0f/s  60s=%.0f/s"),
				                                 *GetNameSafe(C->GetOwner()),
				                                 C->TraceDiag.SnapshotTracesPerSec,
				                                 C->TraceDiag.Avg10Sec,
				                                 C->TraceDiag.Avg60Sec));
		}
	}
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
