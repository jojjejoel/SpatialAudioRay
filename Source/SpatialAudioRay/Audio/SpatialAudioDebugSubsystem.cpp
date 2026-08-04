#include "Audio/SpatialAudioDebugSubsystem.h"

#include "Audio/SpatialAudioComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"

void USpatialAudioDebugSubsystem::Register(USpatialAudioComponent* Component) {
	if (!Sources.Contains(Component)) {
		Sources.Add(Component);
		bEligibleForDebugRays.Add(Component->bDrawDebugRays);
	}
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
				DrawDebugString(GetWorld(), Owner->GetActorLocation(), Owner->GetActorNameOrLabel(),
				                 nullptr, FColor::White, 0.f, true);
			}
		}
	}
}

void USpatialAudioDebugSubsystem::HandleSubModeKeyToggles(const USpatialAudioComponent& First, const APlayerController* PC) {
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
	ApplyToggle(First.ToggleDiffractionPathsKey, bPrevSubModeKeyDown[3], &USpatialAudioComponent::bShowDiffractionPaths);
	ApplyToggle(First.ToggleEdgePointsKey, bPrevSubModeKeyDown[4], &USpatialAudioComponent::bShowEdgePoints);
	ApplyToggle(First.ToggleSurfaceCrawlKey, bPrevSubModeKeyDown[5], &USpatialAudioComponent::bShowSurfaceCrawl);
	ApplyToggle(First.ToggleLoSChecksKey, bPrevSubModeKeyDown[6], &USpatialAudioComponent::bShowLoSChecks);
	ApplyToggle(First.ToggleOffsetLoSChecksKey, bPrevSubModeKeyDown[7], &USpatialAudioComponent::bShowOffsetLoSChecks);
	ApplyToggle(First.ToggleShortestPathsKey, bPrevSubModeKeyDown[8], &USpatialAudioComponent::bShowShortestPaths);
	ApplyToggle(First.ToggleSteeringPredictionKey, bPrevSubModeKeyDown[9], &USpatialAudioComponent::bShowSteeringPrediction);

	const bool bDown = First.ToggleGlobalDebugTextKey.IsValid()
		&& PC->IsInputKeyDown(First.ToggleGlobalDebugTextKey);
	if (bDown && !bPrevToggleKeyDown) {
		bShowGlobalDebugText = !bShowGlobalDebugText;
	}
	bPrevToggleKeyDown = bDown;
}

void USpatialAudioDebugSubsystem::DrawGlobalDebugHUD(const FAggregateTraceStats& Stats) {
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

	USpatialAudioComponent* First = Sources.Num() > 0 ? Sources[0].Get() : nullptr;
	if (!First) {
		return;
	}

	const APlayerController* PC =
		FSlateApplication::IsInitialized() ? GetWorld()->GetFirstPlayerController() : nullptr;

	HandleCycleKey(*First, PC);

	if (!bCycleModeActive) {
		ApplyProximityDebugLimit(*First, PC);
	}

	const bool bAnyDebugRays = ComputeAnyDebugRaysActive();

	HandleActorLabelsToggleAndDraw(*First, PC);

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

	const int32 Next = Selected + 1;
	if (Sources.IsValidIndex(Next)) {
		if (USpatialAudioComponent* C = Sources[Next].Get()) {
			C->bDrawDebugRays = true;
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

	for (int32 Rank = 0; Rank < EligibleIndices.Num(); ++Rank) {
		if (USpatialAudioComponent* C = Sources[EligibleIndices[Rank]].Get()) {
			C->bDrawDebugRays = Rank < MaxSources;
		}
	}
}
