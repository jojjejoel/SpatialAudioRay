# Audio System — Code Flow Guide

This document explains how to read the code in `Plugins/SpatialAudioRay/Source/SpatialAudioRay/Audio/` in the order that makes the system easiest to understand. It covers what each piece does, when it runs, and how results flow into the final audio output.

---

## File map

| File | What lives here |
|---|---|
| `SpatialAudioComponent.h/.cpp` | Main class + `TickComponent`. All state lives here. |
| `AsyncCastManager.cpp` | Multi-frame async ray pipeline (full sweep). |
| `Updater.cpp` | Per-frame sync update cast + audio parameter writes. |
| `EdgeCache.cpp` | Per-frame validation/eviction of cached diffraction edges. |
| `RayPhysics.cpp` | Shared surface-crawl and LoS-sampling helpers used by both casts. |
| `DebugDrawer.cpp` | Debug overlay + replay debug sweep. Nothing here affects audio. |
| `SpatialAudioDebugSubsystem.h/.cpp` | `UTickableWorldSubsystem` — registry of all active components (BeginPlay/EndPlay register/unregister); sums their `TraceDiag` into a global HUD line (traces/s averaged over 1s/10s/60s windows) plus one line per source with the same stats. Toggled by `ToggleGlobalDebugTextKey` (default G), polled once by the subsystem and independent of the per-source key 3. Also polls ALL debug keys once (config read from the first registered source): `CycleDebugSourceKey` (default N) cycles `bDrawDebugRays` OFF → source 1 → … → source N → OFF so exactly one source draws at a time (works while everything is off — it is the way back in), and the sub-mode toggle keys (1–0) assign the flipped value to every registered source so the flags never desync across the cycle. Debug-only. |
| `SpatialAudioTypes.h` | All shared structs (`FSpatialRayState`, `FCachedEdgePoint`, etc.). |
| `Math.h` | Pure stateless math helpers (occlusion formula, Fibonacci dirs, bend angle, etc.). |
| `SpatialAudioSettings.h/.cpp` | `UDataAsset` that holds every tunable parameter. |

---

## Start here: `TickComponent`

`SpatialAudioComponent.cpp` — `TickComponent` is the heartbeat. Everything runs from here in a fixed order every frame. Read it first to understand the overall sequence before diving into any individual system.

### Order of operations each frame

```
1. ReadbackFinalizeBatch()      — read async probes submitted last frame
2. TickAsyncCast()              — advance the full cast by one bounce level
3. UpdateVelocityScaling()      — compute smoothed speeds → SweepMultiplier / EdgeMultiplier
4. UpdateGeometryBurstAndIdleState() — update burst timer and stationary-idle mode
5. EdgeCache->TickCachedEdgeEviction() — validate/evict cached diffraction edges
6. ComputeEffectiveSweepInterval()    — decide how long to wait between full sweeps
                                  (velocity scaling, geometry burst, stationary idle, and the
                                  post-movement cache-fill state: movement-triggered sweeps arm
                                  MovementCacheFillMaxSweeps and clear every edge's
                                  bNewSinceFillArm flag; until MovementCacheFillRequiredEdges NEW
                                  edges (found since the trigger — fresh slots or displacements,
                                  not merge-matched re-confirmations; non-relayed, non-evicting)
                                  are cached, sweeps keep burst pace even after movement stops,
                                  budget spent only by sweeps that leave the target short)
7. TickMovementSweepTrigger()   — request an early sweep if listener moved far enough
8. TickDirectLoSSampling()      — offset-LoS fraction + TargetOcclusion, every frame (sweep or not)
9. [branch]
   a. StartAsyncFullCast()      — if interval elapsed, no cast running, and either a full
                                  offset-ring rotation confirmed no LoS OR occlusion is in the
                                  pre-sweep band (>= PreSweepOcclusionThreshold with partial
                                  LoS: pre-warms the edge cache before full occlusion; such
                                  casts ignore direct-hit rays and skip cache clears/discards)
   b. PerformUpdateRayCast()    — if no cast running
   c. (do nothing, cast in progress)
10. PerformLoSBreakSweep()      — if LoS was just lost, fire a quick directional sweep
11. Blend occlusion / virtual source position toward targets
12. UpdateAudioParameters()     — write final values to AudioComponents
```

---

## The full async cast (multi-frame pipeline)

This is the main ray survey. It runs every `EffFullSweepInterval` seconds and takes `MaxBounces + 1` frames to complete. Read these three methods in order:

### 1. `StartAsyncFullCast` — `AsyncCastManager.cpp`

Fires at the start of a new sweep. Distributes `AsyncActualRays` directions over a Fibonacci sphere, submits the first async trace for each ray, and sets `bAsyncCastActive = true`.

With `SteeringPredictionLeadTime` > 0, the sweep also captures `AsyncSteeringSourcePos`/`AsyncSteeringListenerPos` — the actual positions led by the smoothed velocity vectors (`VelocityScaling.Smoothed*Velocity`). The lead is **signed** (`ComputeSteeringLead`): within the lead time of losing direct LoS (`TimeSinceHadDirectLoS <= LeadTime`, which covers pre-sweep-band casts and the LoS-break sweep), steering aims at the position lead-time in the *past* — the aperture just crossed is behind, between there and the source — and flips to forward prediction once occlusion is sustained. **Steering sites only** read these: the Fibonacci aiming axis, the lateral-band bias weight, and every listener pull in flight (`BounceListenerBias`, `CrawlListenerBias`, `SelectEdgeDirection`, mid-air turns), plus the launch bias of the LoS-break and replay sweeps. Everything that verifies or caches — LoS probes, budget/probe gates, occlusion sampling, Phase 0, readback ranking — stays on the actual positions, so a wrong prediction can only aim rays less well, never cache a false edge. Stationary, the lead decays to zero and behavior (including seeded-bias determinism) is unchanged.

If `FullSweepCycleCount > 1`, a single sweep is split into multiple sub-cycles. `CycleAccum.Index` tracks which cycle is running. Positions and ray counts are only captured on cycle 0.

Cached edge points from `CachedEdgePoints` are snapshotted into `PendingValidCachedPoints` at cycle 0 — they count as free results and reduce the number of actual rays needed.

### 2. `TickAsyncCast` — `AsyncCastManager.cpp`

Called every frame while `bAsyncCastActive`. Advances every ray in `AsyncRays` by one step:

- **DrainPendingLoSProbes** — reads back any in-flight mid-segment LoS probe results.
- **ProcessCrawlBatch** — if a crawl was submitted last frame, reads all step probes back. All-or-nothing: returns without advancing if any probe isn't ready yet.
- **SubmitSegmentLoSProbes** — samples the current segment for LoS to listener using async traces spaced along the segment.
- **Decide next step** — if the segment hits a surface, call `ProcessRayHit` (in `RayPhysics.cpp`) to decide crawl vs. bounce and set the new origin/direction.

With `MaxStraightFlightDistance` > 0 every segment (and the crawl range) is additionally capped to that distance, and a segment that flies its full capped length without hitting anything **turns mid-air** instead of terminating (`ComputeMidAirTurnDirection` — same roughness scatter + listener bias as a bounce, but around the current direction since there is no surface normal; at zero roughness **and** zero bias, where the scatter formula would fly straight, it turns 90° at an angle deterministically seeded from the turn point, so stationary scenes replay identical turns). Each turn consumes a bounce and records a `BounceWaypoint`, so string pulling sees it; the ray only terminates on LoS, budget exhaustion, or `MaxBounces`. At the default of 0 a miss terminates the ray as before. `Ray.SegSubmitLen` records each submitted segment length so the miss handler knows the actual flown distance — recomputing it from budget formulas would overshoot the clamp.

**Best-case pruning** (always on, no setting): every LoS probe is gated on `CumDist + dist(point, listener) <= Budget`, and by the triangle inequality that sum can only grow with further travel. So at every decision point (bounce, crawl exit, mid-air turn, and the loop head of both sync sweeps) a ray past that bound dies immediately — it is mathematically incapable of producing a result, and flying on would only burn traces. This is lossless: it never kills a ray that could still publish anything.

When all rays reach `bDone`, `TickAsyncCast` calls `SubmitFinalizeBatch` and clears `bAsyncCastActive`.

### 3. `SubmitFinalizeBatch` — `AsyncCastManager.cpp`

Called the frame all rays finish. Does the trace-free computation (weighted position sum, total LoS bounces/distance, `bDirectLoSFound`) and submits refinement async probes for every LoS hit found. Saves everything into `Finalize` and sets `Finalize.bPending = true`.

For each LoS ray it also string-pulls Leg1 (`ComputeStringPulledLeg1`): the ray's traveled distance overestimates the acoustic source→edge path, so starting at the edge point it finds the first anchor (source first, then `BounceWaypoints` in path order) with a clear straight segment to the current point (sync traces with reverse hygiene; nudge only for waypoint anchors), hops there, and repeats from that anchor until it reaches the source — `BasePathDist` becomes the summed chain of verified straight segments. A level where nothing is visible keeps the traveled route for the remaining prefix — those prefix segments are *not* straight clear lines (crawl legs hug the wall between recorded turn points), so `ShortestPathVerifiedFrom` records the polyline index where the verified chain begins. Edge→source visible directly means `PathDist` collapses to the straight-line distance and `VirtualPathBend` → 0. The polyline travels with the result onto each cached edge (`ShortestPath` + `ShortestPathVerifiedFrom`) and is drawn per frame under `bShowShortestPaths` (key 0, magenta, unverified prefix dimmed; sphere at each intermediate anchor).

`bDirectLoSFound` is pre-computed in a scan over all rays **before** the per-ray loop. This matters: if it were computed inside the loop, rays past the first direct-LoS hit would see a different value than the readback phase does.

### 4. `ReadbackFinalizeBatch` — `AsyncCastManager.cpp`

Called the frame **after** `SubmitFinalizeBatch`, at the top of the next `TickComponent`. If the sweep found no direct LoS, it first re-checks the four listener offset points synchronously at *current* positions — if any has LoS to the source, the entire sweep result is discarded (the listener gained partial LoS while the multi-frame cast was in flight; publishing would stomp `bHasDirectLoS` and register edges that get thrown away a frame later). Otherwise it reads back all refinement probes, picks the best virtual source position, upserts confirmed results into `CachedEdgePoints`, and writes the final targets:

- `TargetOcclusion`
- `TargetVirtualSourceLocation`
- `TargetPathAttenuation`
- `CycleAccum` (accumulated across sub-cycles; published only on the last cycle)
- `StoredLoSPaths` (saved for per-frame recheck in the update cast)

---

## Per-frame direct-LoS sampling

`TickDirectLoSSampling` — `UpdaterCast.cpp`

Runs **every frame**, including while a full cast is in flight — occlusion must keep draining during sweeps instead of stalling until they finish. Sole owner of `TargetOcclusion`; neither the full cast readback nor the LoS-break sweep writes it.

1. **Sample direct LoS** (`SyncOffsetLoSFraction`, on `OffsetLoSCheckInterval`) — 5 synchronous traces: center↔center plus a 4-point ring perpendicular to the source↔listener axis that rotates 90°/`OffsetRingRotationSteps` per check, while the ring radius steps through annuli (`((step+1)/steps)^OffsetRingRadiusExponent` of full radius) with the same period — one cycle covers the whole disc, not just the rim (a clear centre view through a small opening no longer reads as 4/5 occluded). At the 0.5 default the annuli are equal-area (cycle average estimates *visible disc area*, radii crowding toward the rim); raising the exponent pulls the annuli inward to weight the centre view more, at identical trace cost. The pattern repeats exactly every `OffsetRingRotationSteps` checks. Each listener sample pairs with a same-world-direction lateral offset around the **source** (annulus-laddered like the listener ring) that is *lifted toward the listener onto the source's inner-radius sphere* (radius R = attenuation inner radius × `SourceLoSSampleRadiusScale`, itself never laddered — fixed geometry; lift = `sqrt(R²−r²)`, so the center pairs with the sphere point on the source↔listener line): head-on the source targets read as the familiar filled disc of radius R, from the side they wrap the sphere's listener-facing cap. The source plays at full volume anywhere inside the inner radius, so seeing any of the sphere's surface counts as seeing the source, and a sample already inside the sphere is clear without tracing. A wide source partially visible past a corner reads as partially occluded, and source extent costs nothing — 5 LoS traces plus the 4 listener-point resolve traces (cap points are computed, not resolve-traced; scale 0 = point source, traces reach the exact center). `LastOffsetLoSFraction` = clear/5 is the raw instant sample; the center is just one vote, so pinhole LoS reads as mostly occluded.
2. **Average and smooth the fraction** — the instant samples accumulate over one rotation cycle and publish their average (`WindowedLoSFraction`) only when the cycle **completes**, so the value can step at most once per rotation; a sliding window was re-perturbed on every check that resampled a marginal grazing direction (traces there flicker hit/miss), which pumped occlusion while standing still. `LastDirectLoSFraction` then chases the cycle average over `LoSFractionSmoothingTime` to soften the steps. `bHasDirectLoS` (sweep gating, edge cache) uses the raw instant sample — gaining LoS is instant, but a *stationary* scene only loses LoS once a full rotation pattern finds nothing (`NoLoSSampleStreak >= OffsetRingRotationSteps`), so a marginal direction that only some ring orientations catch can't flip-flop playback between the occluded source and the virtual path. Movement drops LoS immediately.
3. **Write `TargetOcclusion = 1 − smoothed fraction`.**

---

## The per-frame update cast

`PerformUpdateRayCast` — `UpdaterCast.cpp`

Runs **only when no full cast is active** (`!bAsyncCastActive && !Finalize.bPending`). It is the fast path that keeps the virtual source position and path attenuation responsive between full sweeps.

What it does each frame:

1. **Weight cached edges** — source-side weights only (eviction confidence + geometric falloff); writes `TargetVirtualSourceLocation`, refreshes `CurrentSourceToVirtualDistance` and `TargetPathAttenuation` from the cache.
2. **Cluster edges into virtual voices** (`ClusterEdgePoints` + `SyncVirtualVoicesToClusters`).

Voice/emitter positions come from `FCachedEdgePoint::EmitterPoint(VirtualSourcePullbackDistance)`: each edge's presentation point walked back that many cm along its arrival path (relay leg first, then the verified `ShortestPath` suffix — never into the unverified prefix, whose segments can pass through geometry). An absolute cm pullback keeps the emitter the same depth behind the opening at any source distance, and the point always lies on the traced acoustic path — this replaced the old `VirtualSourceMaxOffset` percentage lerp between source and centroid, which scaled with distance and cut straight through the geometry the path bends around. Cluster *grouping* still uses the edge points themselves (pulled-back points converge toward the source and would wrongly merge distinct openings); only the output centroid averages the pulled-back points. `PathDist`/`TotalWeight` (gain inputs) are unaffected by the pullback.

Steps 1–2 are active while occluded **and through the pre-sweep band** (`bVirtualPathActive = !bHasDirectLoS || IsPreSweepActive()`): the crossfade gate starts opening before full occlusion, so the voices must already exist, be positioned, and carry real path attenuation then — gating them on `!bHasDirectLoS` alone left the gate opening onto an empty voice list (`xfade` > 0 with `gain` stuck at 0) until LoS fully dropped.
3. **Write `AudioDiag.*`** — captures intermediate values for the debug HUD.

---

## Edge cache

`TickCachedEdgeEviction` — `EdgeCache.cpp`

Runs **every frame** regardless of sweep state. Manages `CachedEdgePoints`, each of which is a confirmed diffraction edge from a previous full sweep.

Order of per-edge operations (Phase 0 traces target `EffectivePoint()` — the relay point while relayed, the edge otherwise):
1. **TickEvictionFade** — if `bEvicting`, count down `EvictionAlpha`. Remove when it reaches zero.
2. **TickPhase0Readback** — read back the async listener→edge trace submitted last frame. A blocking hit fans out to the listener offset points (below) instead of evicting immediately; a clear hit after blocking restores the edge — but only from listener-side evictions. Source-side evictions (`bSourceSideEviction`: movement threshold, shortest-path recheck) can't be revalidated by listener→edge LoS, which is typically still clear in both cases; only a sweep rewriting the entry rehabilitates them.
3. **TickPhase0OffsetReadback** — read back the offset fan submitted on a blocked center trace: 4 points around the listener (`DirectLoSSampleRadius`, resolved with `ResolveOffsetPoint`) traced to the edge. Any clear point keeps the edge alive; all blocked attempts a relay rescue (below), and only if that fails starts eviction. The fan only fires when the center is blocked, so it costs nothing while the edge is comfortably visible.
4. **TickMovementThresholdEviction** — if the *source* moved beyond `CachedEdgeUpdateMoveThreshold`, begin eviction. Listener movement never evicts — listener-side validity is entirely Phase 0's job.
5. **TickRelayMaintenance** — while relayed: every frame, yield if any directly-visible cached edge exists (drop the relay, start the eviction fade — the real edge carries the sound now); once per Phase 0 interval, un-relay if direct listener→edge LoS returned (the voice snaps back to the true edge and its shorter path) and re-verify the edge→relay leg, dropping the relay and evicting if dynamic geometry severed it.
6. **TickPhase0Submission** — submit a new listener→`EffectivePoint()` async trace for next frame's readback.
7. **TickShortestPathRecheck** (per cache, not per edge; `ShortestPathRecheckInterval`, 0 = off) — round-robin re-trace of one edge's stored string-pulled source→edge polyline per interval, sync with endpoint pull-in (`RaySurfaceBias`) so corner grazing doesn't misfire. Only segments from `ShortestPathVerifiedFrom` onward are traced — the unverified traveled-route prefix (string pull that never reached the source) was blocked at discovery already, so re-tracing it evicts good edges forever without indicating any geometry change. This is the only guard on the source side of a cached path: Phase 0 watches the listener leg, movement eviction watches source *position*, and rank hysteresis discards the worse-ranking re-finds a closed path produces. `ShortestPathRecheckFailures` consecutive blocked checks → source-side eviction fade + sweep request (`bSourceSideEviction` — Phase 0's clear-restore must not resurrect it, since the listener leg is usually still clear when the source leg closed and the restore would out-pace the fade every interval); any clear check resets the streak, as does a sweep rewriting the entry.

**Relay rescue** (`TryRelayRescue`): on total listener LoS loss, before evicting, the edge tries to survive routed through `LastLoSListenerPos` — the most recent listener position whose Phase 0 saw the edge (or the clear fan point, when only the fan saw it). If both legs (edge→relay, relay→current listener) verify sync with reverse hygiene, the edge stays alive with `bRelayed`; `RelayPoint`/`RelayDist` are frozen at rescue time so the extended path stays listener-independent. Single relay level: a relayed edge whose relay goes dark evicts normally. The relay only bridges an otherwise-empty presentation: rescue is skipped while any non-evicting, non-relayed cached edge exists, and an existing relay yields to one the moment it appears — a voice parked at an old listener position reads as sound from the wrong side of the corner once a real edge is carrying the sound.

Phase 0 does NOT set `SweepScheduling.bGeometryChangeDetected`. Because edge points sit on geometry surfaces, a blocking hit does not reliably indicate that geometry changed — it fires inconsistently even with static geometry, and triggering a burst would cause permanent re-sweeping.

Valid cached edges are snapshotted into `PendingValidCachedPoints` at the start of each full sweep and injected as free confirmed results during `ReadbackFinalizeBatch`.

---

## How results reach the audio components

`UpdateAudioParameters` / `UpdateDualModeAudio` — `UpdaterAudio.cpp`

Called at the very end of every `TickComponent`. By this point:
- `CurrentOcclusion` has been smoothed toward `TargetOcclusion`.
- `CurrentVirtualSourceLocation` has been interpolated toward `TargetVirtualSourceLocation`.

`UpdateDualModeAudio` drives:
- **The Source component** — receives `CurvedOcclusion` via `OcclusionParamName`; its own MetaSound graph shapes volume/filtering continuously from it. No external crossfade.
- **The virtual crossfade gate** (`ComputeVirtualCrossfadeRamp` → low-pass → `ComputeVirtualCrossfadeTarget` → `ComputeVirtualCrossfadeSlew`) — with `VirtualCrossfadeStartOcclusion` below 1, an occlusion-keyed ramp fades the virtual in through the band between that threshold and full occlusion (the pinhole/pre-sweep states), keyed to the same smoothed `CurrentOcclusion` the Source's muffling follows. The ramp is low-passed over `VirtualCrossfadeSmoothingTime` because its band mapping amplifies occlusion-sampling wobble by `1/(1-Start)`. A completed blank ring cycle (`NoLoSSampleStreak >= OffsetRingRotationSteps` — the full rotation found nothing, not just one blocked sample) forces the target to 1 — except in a stationary scene with the ramp enabled, where the hard term is suppressed (a marginal pinhole can blank a full sampling rotation and would pump the gate; the ramp still reaches 1 as smoothed occlusion does). Any clear sample resets the streak, so the gate releases instantly on regain. At the default Start of 1 the ramp is off and the gate is the original hard on/off. The result is slewed at `VirtualCrossfadeFadeInTime`/`FadeOutTime`.
- **Per-slot VirtualGain** — `VAP.VirtualGain × WeightShare × slot fade envelope × crossfade gate`, plus **VirtualPathBend** (the MetaSound derives HPF and reverb from it internally). Bend = detour ratio (`traveled/straight − 1`, saturating at `VirtualPathBendFullExcess`) **plus** a traveled-distance term (`VirtualPathBendDistanceStrength × traveled / MaxRayDistance`, default strength 0 = off) so far-away clusters sound duller even on straight single-corner paths — the air-absorption analog of `PathAttenuation`, same Leg1 basis, listener-independent.

All of these are written to `AudioDiag.*` for the debug HUD and sent via `SetFloatParameter` to the `UAudioComponent`s.

---

## Key state groups on the component

The component holds all shared state. The important groups:

| Member | What it is |
|---|---|
| `AsyncRays` | Active rays during a full sweep. Empty between sweeps. |
| `Finalize` | Batch submitted when rays finish; pending for exactly one frame. |
| `CycleAccum` | Accumulates results across sub-cycles; published on the last cycle. |
| `StoredLoSPaths` | LoS origins saved from the last full sweep for per-frame recheck. |
| `CachedEdgePoints` | Confirmed diffraction edges that survive across sweeps. |
| `TargetOcclusion / TargetVirtualSourceLocation` | The targets both casts write to; `TickComponent` smooths toward them. |
| `VelocityScaling` | Smoothed speeds and per-axis interval multipliers. |
| `SweepScheduling` | Burst timer, stationary-idle mode, movement-trigger state. |
| `AudioDiag` | Debug-only: last audio parameter values written each frame. |
| `TraceDiag` | Debug-only: trace counts, per-sweep stats, snapshot history. |

---

## Reading the ray physics helpers

`RayPhysics.cpp` — `CrawlSurfaceToEdge` and `ProcessRayHit`

These are called identically by both the async cast (`TickAsyncCast` → `ProcessRayHit`) and the replay debug sweep (`PerformReplaySweep`). Reading them together explains both.

`ProcessRayHit` decides what happens when a ray hits a surface:
- Even-numbered bounces crawl; odd bounces reflect (or vice versa, depending on `bNextHitCrawls`).
- On a crawl turn, it calls `CrawlSurfaceToEdge`, which walks along the wall until the back-probe misses, marking the edge.
- On a bounce turn, it reflects the ray direction using `Math::ReflectDir`.

`CrawlSurfaceToEdge` blends the wall-slide direction with the projected listener direction to steer the crawl toward the listener. At each step it fires a back-probe (`-SurfaceNormal`) to detect when the wall ends. The crawl range is `MaxCrawlSteps × CrawlStepSize`, further capped by `MaxStraightFlightDistance` when set (minimum one step); a crawl that exhausts its range without finding an edge bounces off the wall from the original hit point.

---

## Reading order summary

1. `SpatialAudioTypes.h` — learn the struct shapes.
2. `SpatialAudioComponent.h` — learn the state layout.
3. `TickComponent` in `SpatialAudioComponent.cpp` — understand the per-frame sequence.
4. `StartAsyncFullCast` → `TickAsyncCast` → `SubmitFinalizeBatch` → `ReadbackFinalizeBatch` in `AsyncCastManager.cpp` — follow the full cast pipeline.
5. `TickDirectLoSSampling` and `PerformUpdateRayCast` in `UpdaterCast.cpp` — understand the per-frame fast path.
6. `TickCachedEdgeEviction` in `EdgeCache.cpp` — understand edge lifetime.
7. `ProcessRayHit` / `CrawlSurfaceToEdge` in `RayPhysics.cpp` — understand the ray physics.
8. `UpdateAudioParameters` / `UpdateDualModeAudio` in `Updater.cpp` — see how everything becomes sound.
