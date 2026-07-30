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

	// ── Tick phases ───────────────────────────────────────────────────────────
	struct FAggregateTraceStats {
		int32 NumSources = 0;
		float TracesPerSec = 0.f;
		float Avg10Sec = 0.f;
		float Avg60Sec = 0.f;
	};
	/** Sums trace diagnostics over all sources, pruning any whose weak pointer has gone stale. */
	FAggregateTraceStats AggregateSourceTraceStats();
	bool ComputeAnyDebugRaysActive() const;
	void HandleCycleKey(const USpatialAudioComponent& First, const APlayerController* PC);
	void HandleActorLabelsToggleAndDraw(const USpatialAudioComponent& First, const APlayerController* PC);
	void HandleSubModeKeyToggles(const USpatialAudioComponent& First, const APlayerController* PC);
	void DrawGlobalDebugHUD(const FAggregateTraceStats& Stats);

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
	 *  displays are both possible. Toggled by ToggleGlobalDebugTextKey. Defaults off like
	 *  the rest of the views; see bDrawDebugRays for why. */
	bool bShowGlobalDebugText = false;
	bool bPrevToggleKeyDown = false;
	bool bPrevCycleKeyDown = false;
	bool bPrevSubModeKeyDown[11] = {};

	/** World-space name labels, toggled by ToggleActorLabelsKey. Only drawn on sources that
	 *  currently have bDrawDebugRays true (the cycle-selected source, or the proximity-limited
	 *  in-range set), not every registered source. The one view left on by default: it names
	 *  what you are looking at rather than drawing over it, and since it needs a source to be
	 *  debug-enabled first, it stays invisible until you have already opted in. */
	bool bShowActorLabels = true;
	bool bPrevActorLabelsKeyDown = false;
};
