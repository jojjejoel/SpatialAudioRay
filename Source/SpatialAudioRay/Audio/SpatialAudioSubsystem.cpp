#include "Audio/SpatialAudioSubsystem.h"

#include "Audio/Math.h"
#include "Audio/SpatialAudioComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"

void USpatialAudioSubsystem::Register(USpatialAudioComponent* Component) {
	if (!Sources.Contains(Component)) {
		Sources.Add(Component);
		bEligibleForDebugRays.Add(Component->bDrawDebugRays);
	}
	UE_LOG(LogTemp, Log, TEXT("SpatialAudioSubsystem: +%s on %s (%d registered)"),
	       *Component->GetName(), *GetNameSafe(Component->GetOwner()), Sources.Num());
}

void USpatialAudioSubsystem::Unregister(USpatialAudioComponent* Component) {
	const int32 Index = Sources.IndexOfByPredicate(
		[Component](const TWeakObjectPtr<USpatialAudioComponent>& Src) { return Src.Get() == Component; });
	if (Index != INDEX_NONE) {
		Sources.RemoveAt(Index);
		bEligibleForDebugRays.RemoveAt(Index);
	}
	UE_LOG(LogTemp, Log, TEXT("SpatialAudioSubsystem: -%s on %s (%d registered)"),
	       *Component->GetName(), *GetNameSafe(Component->GetOwner()), Sources.Num());
}

TStatId USpatialAudioSubsystem::GetStatId() const {
	RETURN_QUICK_DECLARE_CYCLE_STAT(USpatialAudioSubsystem, STATGROUP_Tickables);
}

USpatialAudioSubsystem::FAggregateTraceStats USpatialAudioSubsystem::AggregateSourceTraceStats() {
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

/** Tickable subsystems run after every component, so the measurement is complete and the stretch
 *  lands next frame, which is what stops the loop chasing itself. */
void USpatialAudioSubsystem::ApplyTraceBudget(const FAggregateTraceStats& Stats, const float DeltaTime) {
	USpatialAudioComponent* First = Sources.Num() > 0 ? Sources[0].Get() : nullptr;
	BudgetTracesPerSec = First ? First->GetSettings().MaxTracesPerSecond : 0.f;

	const float Target = Math::ComputeTraceBudgetStretch(GlobalTraceStretch, Stats.TracesPerSec,
	                                                     BudgetTracesPerSec);
	GlobalTraceStretch = BudgetTracesPerSec > 0.f
		                     ? FMath::FInterpTo(GlobalTraceStretch, Target, DeltaTime, 1.f)
		                     : 1.f;

	/** Idle sources would dilute the mean and hand throttling back to the ones doing the work. */
	float WeightSum = 0.f;
	int32 WeightCount = 0;
	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (const USpatialAudioComponent* C = Src.Get(); C && C->bInAudibleRange) {
			WeightSum += Math::ComputeSourceThrottleWeight(C->CurrentPriority);
			++WeightCount;
		}
	}
	const float MeanWeight = WeightCount > 0 ? WeightSum / WeightCount : 1.f;

	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (USpatialAudioComponent* C = Src.Get()) {
			C->TraceBudgetStretch = C->bInAudibleRange
				                        ? Math::ComputeSourceTraceStretch(
					                        GlobalTraceStretch,
					                        Math::ComputeSourceThrottleWeight(C->CurrentPriority), MeanWeight)
				                        : 1.f;
		}
	}
}

bool USpatialAudioSubsystem::ComputeAnyDebugRaysActive() const {
	for (const TWeakObjectPtr<USpatialAudioComponent>& Src : Sources) {
		if (const USpatialAudioComponent* C = Src.Get(); C && C->bDrawDebugRays) {
			return true;
		}
	}
	return false;
}

void USpatialAudioSubsystem::HandleCycleKey(const USpatialAudioComponent& First, const APlayerController* PC) {
	if (!PC) {
		return;
	}
	const bool bDown = First.CycleDebugSourceKey.IsValid() && PC->IsInputKeyDown(First.CycleDebugSourceKey);
	if (bDown && !bPrevCycleKeyDown) {
		bCycleModeActive = CycleDebugRaySource();
	}
	bPrevCycleKeyDown = bDown;
}

void USpatialAudioSubsystem::HandleActorLabelsToggleAndDraw(const USpatialAudioComponent& First,
                                                                 const APlayerController* PC) {
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

void USpatialAudioSubsystem::HandleSubModeKeyToggles(const USpatialAudioComponent& First,
                                                          const APlayerController* PC) {
	if (!PC) {
		return;
	}
	auto ApplyToggle = [&](const FKey& Key, bool& bPrevDown, bool USpatialAudioComponent::* Flag)
	{
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
	ApplyToggle(First.ToggleDiffractionPathsKey, bPrevSubModeKeyDown[3],
	            &USpatialAudioComponent::bShowDiffractionPaths);
	ApplyToggle(First.ToggleEdgePointsKey, bPrevSubModeKeyDown[4], &USpatialAudioComponent::bShowEdgePoints);
	ApplyToggle(First.ToggleSurfaceCrawlKey, bPrevSubModeKeyDown[5], &USpatialAudioComponent::bShowSurfaceCrawl);
	ApplyToggle(First.ToggleLoSChecksKey, bPrevSubModeKeyDown[6], &USpatialAudioComponent::bShowLoSChecks);
	ApplyToggle(First.ToggleOffsetLoSChecksKey, bPrevSubModeKeyDown[7], &USpatialAudioComponent::bShowOffsetLoSChecks);
	ApplyToggle(First.ToggleShortestPathsKey, bPrevSubModeKeyDown[8], &USpatialAudioComponent::bShowShortestPaths);
	ApplyToggle(First.ToggleSteeringPredictionKey, bPrevSubModeKeyDown[9],
	            &USpatialAudioComponent::bShowSteeringPrediction);

	const bool bDown = First.ToggleGlobalDebugTextKey.IsValid()
		&& PC->IsInputKeyDown(First.ToggleGlobalDebugTextKey);
	if (bDown && !bPrevToggleKeyDown) {
		bShowGlobalDebugText = !bShowGlobalDebugText;
	}
	bPrevToggleKeyDown = bDown;
}

FColor USpatialAudioSubsystem::ThrottleTint(const float Stretch) {
	const float Span = FMath::Max(Math::MaxTraceBudgetStretch - 1.f, KINDA_SMALL_NUMBER);
	const float T = FMath::Clamp((Stretch - 1.f) / Span, 0.f, 1.f);
	return FLinearColor::LerpUsingHSV(FLinearColor(1.f, 1.f, 0.63f), FLinearColor(1.f, 0.18f, 0.1f), T)
		.ToFColor(false);
}

void USpatialAudioSubsystem::DrawGlobalDebugHUD(const FAggregateTraceStats& Stats) {
	const bool bThrottling = GlobalTraceStretch > 1.01f;
	const FString BudgetLabel = BudgetTracesPerSec <= 0.f
		                            ? TEXT("  │  budget off")
		                            : FString::Printf(TEXT("  │  budget %.0f/s  stretch %.2f×"),
		                                              BudgetTracesPerSec, GlobalTraceStretch);

	/** Ascending, because the engine lays these messages out bottom-up. */
	TArray<int32> Order;
	Order.Reserve(Sources.Num());
	for (int32 i = 0; i < Sources.Num(); ++i) {
		if (Sources[i].IsValid()) {
			Order.Add(i);
		}
	}
	Order.Sort([this](const int32 A, const int32 B) {
		return Sources[A]->CurrentPriority < Sources[B]->CurrentPriority;
	});

	/** Clear last frame's surplus, or a line outlives the source that wrote it. */
	constexpr int32 GlobalHudKey = 100;
	int32 LineKey = GlobalHudKey - 1;

	for (const int32 Index : Order) {
		const USpatialAudioComponent* C = Sources[Index].Get();
		const AActor* Owner = C->GetOwner();
		if (!C->bInAudibleRange) {
			GEngine->AddOnScreenDebugMessage(LineKey--, 0.f, FColor(120, 120, 120),
			                                 FString::Printf(TEXT("  %s  │  out of range, idle"),
			                                                 Owner ? *Owner->GetActorNameOrLabel() : TEXT("None")));
			continue;
		}

		const FString ThrottleLabel = BudgetTracesPerSec > 0.f
			                              ? FString::Printf(
				                              TEXT("  swp %.2f×  edge %.2f×  los %.2f×"),
				                              Math::ShareOfBudgetStretch(
					                              C->TraceBudgetStretch, Math::SweepBudgetShare),
				                              Math::ShareOfBudgetStretch(
					                              C->TraceBudgetStretch, Math::EdgeCheckBudgetShare),
				                              Math::ShareOfBudgetStretch(
					                              C->TraceBudgetStretch, Math::DirectLoSBudgetShare))
			                              : FString();
		GEngine->AddOnScreenDebugMessage(LineKey--, 0.f, ThrottleTint(C->TraceBudgetStretch),
		                                 FString::Printf(
			                                 TEXT("  %s  │  1s=%.0f/s  60s=%.0f/s  │  prio %.2f%s"),
			                                 Owner ? *Owner->GetActorNameOrLabel() : TEXT("None"),
			                                 C->TraceDiag.SnapshotTracesPerSec,
			                                 C->TraceDiag.Avg60Sec,
			                                 C->CurrentPriority,
			                                 *ThrottleLabel));
	}

	for (int32 Stale = Order.Num(); Stale < PrevSourceLineCount; ++Stale) {
		GEngine->RemoveOnScreenDebugMessage(GlobalHudKey - 1 - Stale);
	}
	PrevSourceLineCount = Order.Num();

	/** Higher keys draw above lower ones, so the summary takes the largest key to sit on top. */
	GEngine->AddOnScreenDebugMessage(GlobalHudKey, 0.f, bThrottling ? FColor::Orange : FColor::Yellow,
	                                 FString::Printf(
		                                 TEXT("GLOBAL  %d source%s  │  1s=%.0f/s  60s=%.0f/s  peak=%.0f/s%s"),
		                                 Stats.NumSources, Stats.NumSources == 1 ? TEXT("") : TEXT("s"),
		                                 Stats.TracesPerSec, Stats.Avg60Sec,
		                                 Stats.PeakTracesPerSec, *BudgetLabel));
}

void USpatialAudioSubsystem::Tick(float DeltaTime) {
	Super::Tick(DeltaTime);

	const FAggregateTraceStats Stats = AggregateSourceTraceStats();
	ApplyTraceBudget(Stats, DeltaTime);

	USpatialAudioComponent* First = Sources.Num() > 0 ? Sources[0].Get() : nullptr;
	if (!GEngine || !First) {
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

bool USpatialAudioSubsystem::CycleDebugRaySource() {
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

void USpatialAudioSubsystem::ApplyProximityDebugLimit(const USpatialAudioComponent& First,
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

	EligibleIndices.Sort([this, &ListenerPos](const int32 A, const int32 B)
	{
		const AActor* OwnerA = Sources[A]->GetOwner();
		const AActor* OwnerB = Sources[B]->GetOwner();
		const float DistA = OwnerA
			                    ? FVector::DistSquared(OwnerA->GetActorLocation(), ListenerPos)
			                    : TNumericLimits<float>::Max();
		const float DistB = OwnerB
			                    ? FVector::DistSquared(OwnerB->GetActorLocation(), ListenerPos)
			                    : TNumericLimits<float>::Max();
		return DistA < DistB;
	});

	for (int32 Rank = 0; Rank < EligibleIndices.Num(); ++Rank) {
		if (USpatialAudioComponent* C = Sources[EligibleIndices[Rank]].Get()) {
			C->bDrawDebugRays = Rank < MaxSources;
		}
	}
}
