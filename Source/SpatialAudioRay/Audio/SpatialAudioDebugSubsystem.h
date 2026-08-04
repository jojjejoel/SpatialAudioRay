#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SpatialAudioDebugSubsystem.generated.h"

class USpatialAudioComponent;

UCLASS()
class SPATIALAUDIORAY_API USpatialAudioDebugSubsystem : public UTickableWorldSubsystem {
	GENERATED_BODY()

public:
	void Register(USpatialAudioComponent* Component);
	void Unregister(USpatialAudioComponent* Component);

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

private:
	bool CycleDebugRaySource();
	void ApplyProximityDebugLimit(const USpatialAudioComponent& First, const APlayerController* PC);

	struct FAggregateTraceStats {
		int32 NumSources = 0;
		float TracesPerSec = 0.f;
		float Avg10Sec = 0.f;
		float Avg60Sec = 0.f;
		float PeakTracesPerSec = 0.f;
	};

	FAggregateTraceStats AggregateSourceTraceStats();
	bool ComputeAnyDebugRaysActive() const;
	void HandleCycleKey(const USpatialAudioComponent& First, const APlayerController* PC);
	void HandleActorLabelsToggleAndDraw(const USpatialAudioComponent& First, const APlayerController* PC);
	void HandleSubModeKeyToggles(const USpatialAudioComponent& First, const APlayerController* PC);
	void DrawGlobalDebugHUD(const FAggregateTraceStats& Stats);

	TArray<TWeakObjectPtr<USpatialAudioComponent>> Sources;

	TArray<bool> bEligibleForDebugRays;

	bool bCycleModeActive = false;

	bool bShowGlobalDebugText = false;
	bool bPrevToggleKeyDown = false;
	bool bPrevCycleKeyDown = false;
	bool bPrevSubModeKeyDown[10] = {};

	bool bShowActorLabels = true;
	bool bPrevActorLabelsKeyDown = false;

	float PeakTracesPerSec = 0.f;
};
