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

	/** While NOT single-source-cycled (see bCycleModeActive) and MaxUncycledDebugSources > 0,
	 *  suppresses bDrawDebugRays on every originally-enabled source (bEligibleForDebugRays)
	 *  beyond the closest N to the listener, and restores it as sources move back into range. */
	void ApplyProximityDebugLimit(const USpatialAudioComponent& First, const APlayerController* PC);

	/** Weak so a component destroyed without unregistering is dropped harmlessly next tick. */
	TArray<TWeakObjectPtr<USpatialAudioComponent>> Sources;

	/** Snapshot of each Sources[i]'s bDrawDebugRays at registration time — the editor/designer's
	 *  original intent, independent of later runtime suppression by the proximity limit (which
	 *  would otherwise be indistinguishable from the user genuinely disabling a source). Parallel
	 *  to Sources. */
	TArray<bool> bEligibleForDebugRays;

	/** True from the first N press (a single source selected) until cycling wraps back to OFF.
	 *  While true, ApplyProximityDebugLimit is skipped entirely so it can't fight the user's
	 *  explicit single selection. */
	bool bCycleModeActive = false;

	/** Independent of the per-source bShowDebugText so global-only and per-source-only
	 *  displays are both possible. Toggled by ToggleGlobalDebugTextKey. */
	bool bShowGlobalDebugText = true;
	bool bPrevToggleKeyDown = false;
	bool bPrevCycleKeyDown = false;
	bool bPrevSubModeKeyDown[11] = {};

	/** World-space name labels at every source, toggled by ToggleActorLabelsKey. Independent of
	 *  bDrawDebugRays so it's usable purely for scouting/filming without the ray-debug overlay. */
	bool bShowActorLabels = false;
	bool bPrevActorLabelsKeyDown = false;
};
