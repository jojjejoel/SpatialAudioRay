// World-wide registry of active USpatialAudioComponents — aggregates their trace diagnostics
// into one global debug HUD line.
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
	/** Advances bDrawDebugRays through OFF → source 1 → … → source N → OFF (registration
	 *  order), forcing at most one source on so its rays are inspected without the others
	 *  drawing over them. Returns whether any source is drawing afterwards. */
	bool CycleDebugRaySource();

	/** Weak so a component destroyed without unregistering is dropped harmlessly next tick. */
	TArray<TWeakObjectPtr<USpatialAudioComponent>> Sources;

	/** Independent of the per-source bShowDebugText so global-only and per-source-only
	 *  displays are both possible. Toggled by ToggleGlobalDebugTextKey. */
	bool bShowGlobalDebugText = true;
	bool bPrevToggleKeyDown = false;
	bool bPrevCycleKeyDown = false;
	bool bPrevSubModeKeyDown[10] = {};
};
