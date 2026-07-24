# Audio System — Code Flow Guide

This document explains how to read the code in `Plugins/SpatialAudioRay/Source/SpatialAudioRay/Audio/` in the order that makes the system easiest to understand. It covers what each piece does, when it runs, and how results flow into the final audio output.

First time here? Start with `ReadingGuide.md` instead — a step-by-step tour for newcomers. This document is the deep per-system reference.

---

## File map

| File | What lives here |
|---|---|
| `SpatialAudioComponent.h/.cpp` | Main class + `TickComponent`. All state lives here. |
| `AsyncCastManager.cpp` | Multi-frame async ray pipeline (full sweep). |
| `Updater.cpp` | Per-frame sync update cast + audio parameter writes. |
| `EdgeCache.cpp` | Per-frame validation/eviction of cached diffraction edges. |
| `RayPhysics.cpp` | Shared surface-crawl and LoS-sampling helpers used by both casts. |
| `DebugDrawer.cpp` | Debug overlay. Nothing here affects audio. Every `DrawDebugSphere` call across the plugin (added 2026-07-21, user-requested) passes `SDPG_Foreground` as its DepthPriority so spheres render through walls, same as `DrawDebugString`'s screen-space labels already did; `DrawDebugLine` calls are unaffected (still depth-tested). `DebugLineDuration` (added 2026-07-21, user-requested) moved from a component UPROPERTY to `USpatialAudioSettings` — call sites use `Settings.DebugLineDuration` where a `Settings` param is already in scope, `Component.GetSettings().DebugLineDuration` in the two functions that don't take one (`FUpdater::SyncOffsetLoSFraction`, `FEdgeCache::TickPhase0OffsetReadback`). |
| `SpatialAudioDebugSubsystem.h/.cpp` | `UTickableWorldSubsystem` — registry of all active components (BeginPlay/EndPlay register/unregister); sums their `TraceDiag` into a global HUD line (traces/s averaged over 1s/10s/60s windows) plus one line per source with the same stats. Toggled by `ToggleGlobalDebugTextKey` (default G), polled once by the subsystem and independent of the per-source key 3. Also polls ALL debug keys once (config read from the first registered source): `CycleDebugSourceKey` (default N) cycles `bDrawDebugRays` OFF → source 1 → … → source N → OFF so exactly one source draws at a time (works while everything is off — it is the way back in), and the sub-mode toggle keys (1–0, P) assign the flipped value to every registered source so the flags never desync across the cycle. `MaxUncycledDebugSources` (`USpatialAudioSettings`, 0 = off, added 2026-07-21) caps how many originally-enabled sources actually draw while NOT single-source-cycled: `ApplyProximityDebugLimit` ranks the sources that had `bDrawDebugRays` true at registration (`bEligibleForDebugRays`, a snapshot — otherwise a source suppressed for being far away would be indistinguishable from one the user genuinely disabled) by live distance to the listener and lets only the closest N keep drawing, re-derived every tick so sources fade in/out as the player moves. Lives in the shared settings asset (`First->GetSettings()`) rather than the per-component key config, since it's a global policy value, not a keybinding. Skipped entirely once a single source is cycle-selected (`bCycleModeActive`, true from the first N press until cycling wraps back to OFF) so it can't fight that explicit choice; note that wrapping back to OFF clears `bCycleModeActive`, so the closest-N view reappears immediately rather than going fully blank. `ToggleActorLabelsKey` (default L, on by default) toggles `bShowActorLabels`, drawing `GetActorNameOrLabel()` via `DrawDebugString` at each source's actor location — screen-space text, so it's camera-facing and not depth-tested against geometry (visible through walls) for free. The toggle key itself is polled independent of `bAnyDebugRays`, but a label is only actually drawn for sources whose `bDrawDebugRays` is currently true (the cycle-selected source, or the proximity-limited in-range set) — not every registered source, and none at all while ray debugging is fully off. Debug-only. |
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

Cached edge points from `CachedEdgePoints` are snapshotted into `PendingValidCachedPoints` at cycle 0 — direct, non-evicting entries count as free results and reduce the number of actual rays needed. Relayed and evicting entries stay in the snapshot for presentation but do NOT substitute for rays and do NOT exclude their directions (`BuildCachedEdgeExclusionDirs` skips both): a relay is an audible stopgap, not a found path, so the sweep keeps searching at full budget — and the region around the relayed edge is exactly where the replacement path that lets it yield most likely is.

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

For each LoS ray it also string-pulls Leg1 (`ComputeStringPulledLeg1`): the ray's traveled distance overestimates the acoustic source→edge path, so starting at the edge point it finds the first anchor (source first, then `BounceWaypoints` in path order) with a clear straight segment to the current point (sync traces with reverse hygiene; nudge only for waypoint anchors), hops there, and repeats from that anchor until it reaches the source — `BasePathDist` becomes the summed chain of verified straight segments. When nothing is visible from the current point, it doesn't give up on the whole remaining prefix: it consumes exactly one raw traveled hop as an unverified link (that segment was traced and found blocked — it's the actual crawl/bounce leg, not a straight line) and keeps pulling from the new point, so one blocked corner can't swallow a shortcut that's genuinely available past it (e.g. a second, unrelated opening beyond a second corner). Verified and unverified segments can therefore interleave along the final polyline, not just form a single trailing prefix — `ShortestPathSegmentVerified[i]` records per-segment whether `ShortestPath[i]→ShortestPath[i+1]` is a verified straight line. Edge→source visible directly means `PathDist` collapses to the straight-line distance and `VirtualPathBend` → 0. The polyline travels with the result onto each cached edge (`ShortestPath` + `ShortestPathSegmentVerified`) and is drawn per frame under `bShowShortestPaths` (key 0, magenta, unverified segments dimmed; sphere at each intermediate anchor colored to match its own segment).

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
2. **Average and smooth the fraction** — each check writes its instant sample into a per-slot cache (`LoSSlotFractions`, index = the check's position within the rotation) and `WindowedLoSFraction` is recomputed as the average of all slots' current values immediately, every check (2026-07-21: was batched once per completed rotation). A stationary scene retraces the exact same ray per slot every rotation, so a slot really does hold "what this ray saw last time" — occlusion can move mid-rotation instead of freezing for a full cycle. An earlier sliding-window attempt was reverted for re-perturbing on every check that resampled a marginal grazing direction; this is mathematically the same shape (a length-`RotationSteps` window recomputed every check) but was re-adopted anyway since a stationary ray only flickers from rare floating-point noise, which is accepted. `LastDirectLoSFraction` then chases the pattern average over `LoSFractionSmoothingTime` (scaled by the same `VelocityScaling.OffsetLoSMultiplier` as `OffsetLoSCheckInterval` itself, so the softening stays proportional to how often the cycle actually completes — shorter while moving, full baseline at rest) to soften the steps. `bHasDirectLoS` (sweep gating, edge cache) uses the raw instant sample — gaining LoS is instant, but a *stationary* scene only loses LoS once a full rotation pattern finds nothing (`NoLoSSampleStreak >= OffsetRingRotationSteps`), so a marginal direction that only some ring orientations catch can't flip-flop playback between the occluded source and the virtual path. Movement drops LoS immediately.
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
2. **TickPhase0Readback** — read back the async listener→edge trace submitted last frame. A blocking hit first tries `TryPromoteToInnerAnchor`: if the previous point on the edge's own `ShortestPath` (one step back toward the source) already has direct, unobstructed listener LoS, the edge shrinks back to that point instead (a strictly better outcome — no diffraction needed for what's now a direct leg — `PathDist` is corrected by the trimmed segment's straight-line length). Only if that fails does it fan out to the listener offset points (below) instead of evicting immediately; a clear hit after blocking restores the edge — but only from listener-side evictions. Source-side evictions (`bSourceSideEviction`: movement threshold, shortest-path recheck) can't be revalidated by listener→edge LoS, which is typically still clear in both cases; only a sweep rewriting the entry rehabilitates them.
3. **TickPhase0OffsetReadback** — read back the offset fan submitted on a blocked center trace: 4 points around the listener (`DirectLoSSampleRadius`, resolved with `ResolveOffsetPoint`) traced to the edge. Any clear point keeps the edge alive; all blocked attempts a relay rescue (below), and only if that fails starts eviction. The fan only fires when the center is blocked, so it costs nothing while the edge is comfortably visible.
4. **TickMovementThresholdEviction** — if the *source* moved beyond `CachedEdgeUpdateMoveThreshold`, begin eviction. Listener movement never evicts — listener-side validity is entirely Phase 0's job.
5. **TickRelayMaintenance** — while relayed: once per Phase 0 interval, submit four async traces (listener↔edge forward+reverse, edge↔relay forward+reverse; handles on `RelayCheckHandles`/`bRelayCheckPending`) and evaluate them the following tick: un-relay if direct listener→edge LoS returned (the voice snaps back to the true edge and its shorter path), drop the relay and evict if the edge→relay leg was severed by dynamic geometry. When the readback instead confirms the relay still valid, it **converts the relay into a real edge** (`BisectListenerLoS`): the LoS transition along the verified edge→relay leg is the actual second corner the relay bends around, so the entry is upgraded in place — corner appended to the polyline (verified sub-segment of the traced-clear leg), `PathDist` extended by the exact straight piece, `LoSBounces + 1`, relay state cleared. The entry becomes a first-class cached edge (standard maintenance, ray substitution, direction exclusion all apply again), making the relay a ~one-interval transitional state rather than a resting one. Fallback when no midpoint traces clear: the relay point itself — same acoustics, and the promotion refinement (item 8) walks it back toward the true corner on later intervals since the appended segment is exactly the bracket it bisects. Each relayed edge converts independently — there is deliberately no yield-to-direct-edge eviction anymore (removed with the conversion): when N edges relay at once, the first conversion would otherwise have evicted the other N−1 mid-conversion instead of letting each land its own corner.
6. **TickPhase0Submission** — submit a new listener→`EffectivePoint()` async trace for next frame's readback.
7. **TickShortestPathRecheck / TickShortestPathReadback** (per cache, not per edge; `ShortestPathRecheckInterval`, 0 = off) — round-robin re-trace of *every* segment of one edge's stored string-pulled source→edge polyline per interval, with endpoint pull-in (`RaySurfaceBias`) so corner grazing doesn't misfire. All segment traces (forward+reverse each) are submitted async up front into `Component.PathRecheck` and evaluated by the readback the following tick; the checked entry is re-found by exact `EdgePoint` match at readback, so a sweep rewrite, promotion, or eviction in between drops the stale result instead of evicting the wrong entry. This deliberately includes unverified segments (`ShortestPathSegmentVerified[i] == false` — a raw crawl/bounce hop the string pull couldn't shortcut past): those were already blocked at discovery, so enabling this setting also evicts ordinary multi-corner diffraction paths the moment they're rechecked, not just genuine geometry changes. This is the only guard on the source side of a cached path: Phase 0 watches the listener leg, movement eviction watches source *position*, and rank hysteresis discards the worse-ranking re-finds a closed path produces. Any single blocked segment → immediate source-side eviction fade + sweep request (`bSourceSideEviction` — Phase 0's clear-restore must not resurrect it, since the listener leg is usually still clear when the source leg closed and the restore would out-pace the fade every interval).
8. **TickInnerAnchorPromotion** (per cache, not per edge; `ShortestPathPromotionInterval`, 0 = off) — round-robin, one non-relayed edge per interval (its own cursor, independent of the recheck's), tries `TryPromoteToInnerAnchor` regardless of the edge's current LoS state. Unlike the promotion attempt inside `TickPhase0Readback` (which only fires as a rescue the instant the edge itself goes blocked), this runs opportunistically so an edge that's had clear listener LoS since discovery still migrates toward the true minimal diffraction point as the listener moves further past the corner — one step per interval, not a jump straight to the source. This opportunistic path alone also enables **sub-segment refinement** (`bAllowSubSegmentRefine`): when the previous vertex is blocked, a 5-step binary search along the final segment brackets the LoS transition — the actual geometric corner — and moves the edge there (verified final segments only, since unverified hops hug geometry; every accepted point traced clear forward+reverse; minimum-move epsilon of `max(4×RaySurfaceBias, 4cm)` prevents stationary jitter; `PathDist` trimmed by the exact straight-line cut, polyline's last vertex moves with the edge, no pop). The rescue call site passes false — it re-fires every Phase 0 interval while blocked, and the bisection traces would recur in pinhole states.

**Relay rescue** (`TryRelayRescue`): on total listener LoS loss, before evicting, the edge tries to survive routed through `LastLoSListenerPos` — the most recent listener position whose Phase 0 saw the edge (or the clear fan point, when only the fan saw it). If both legs (edge→relay, relay→current listener) verify sync with reverse hygiene, the edge stays alive with `bRelayed`; `RelayPoint`/`RelayDist` are frozen at rescue time so the extended path stays listener-independent. Single relay level: a relayed edge whose relay goes dark evicts normally. Rescue depends only on the edge's own geometry — both the yield rule and the old "skip while any direct edge exists" gate are gone: each made an edge's survival depend on sibling state, and with N edges going dark the same tick that meant processing-order lotteries and, once the first relay converted into a direct edge, a permanent lockout of every remaining rescue (8 simultaneous losses produced 1 converted edge and 7 deaths). Now N total losses produce N relays, and each converts independently into a real corner edge (item 5) — the old wrong-side-of-corner concern about persistent relays no longer applies to a ~one-interval transitional state.

Phase 0 does NOT set `SweepScheduling.bGeometryChangeDetected`. Because edge points sit on geometry surfaces, a blocking hit does not reliably indicate that geometry changed — it fires inconsistently even with static geometry, and triggering a burst would cause permanent re-sweeping.

Valid cached edges are snapshotted into `PendingValidCachedPoints` at the start of each full sweep and injected as free confirmed results during `ReadbackFinalizeBatch`. Only direct, non-evicting entries reduce the sweep's ray budget or exclude ray directions — relayed/evicting ones present but don't stop the search for their replacement.

---

## How results reach the audio components

`UpdateAudioParameters` / `UpdateDualModeAudio` — `UpdaterAudio.cpp`

Called at the very end of every `TickComponent`. By this point:
- `CurrentOcclusion` has been smoothed toward `TargetOcclusion`.
- `CurrentVirtualSourceLocation` has been interpolated toward `TargetVirtualSourceLocation`.

`UpdateDualModeAudio` drives:
- **The Source components** — every component tagged `AudioComponentSource` (an object can carry several co-located sounds; they all write the shared diffraction bus) plus any live `PlaySoundThroughSpatialBus` one-shots receives `CurvedOcclusion` via `OcclusionParamName`; each MetaSound graph shapes its own volume/filtering continuously from it. No external crossfade. Finished one-shots auto-destroy and their stale entries are pruned in this same loop. `ReadAttenuationSettings` uses the widest-range source (inner + falloff) for `AttenuationInnerRadius`/auto `MaxRayDistance`.
- **The virtual crossfade gate** (`ComputeVirtualCrossfadeRamp` → low-pass → `ComputeVirtualCrossfadeTarget` → `ComputeVirtualCrossfadeSlew`) — with `VirtualCrossfadeStartOcclusion` below 1, an occlusion-keyed ramp fades the virtual in through the band between that threshold and full occlusion (the pinhole/pre-sweep states), keyed to the same smoothed `CurrentOcclusion` the Source's muffling follows. The ramp is low-passed over `VirtualCrossfadeSmoothingTime` because its band mapping amplifies occlusion-sampling wobble by `1/(1-Start)`. A completed blank ring cycle (`NoLoSSampleStreak >= OffsetRingRotationSteps` — the full rotation found nothing, not just one blocked sample) forces the target to 1 — except in a stationary scene with the ramp enabled, where the hard term is suppressed (a marginal pinhole can blank a full sampling rotation and would pump the gate; the ramp still reaches 1 as smoothed occlusion does). Any clear sample resets the streak, so the gate releases instantly on regain. At the default Start of 1 the ramp is off and the gate is the original hard on/off. The result is slewed at `VirtualCrossfadeFadeInTime`/`FadeOutTime`.
- **Per-slot VirtualGain** — `VAP.VirtualGain × WeightShare × slot fade envelope × crossfade gate`, plus **VirtualPathBend** (the MetaSound derives HPF and reverb from it internally). Bend = detour ratio (`traveled/straight − 1`, saturating at `VirtualPathBendFullExcess`) **plus** a traveled-distance term (`VirtualPathBendDistanceStrength × traveled / MaxRayDistance`, default strength 0 = off) so far-away clusters sound duller even on straight single-corner paths — the air-absorption analog of `PathAttenuation`, same Leg1 basis, listener-independent.

`Math::ComputePathAttenuation` itself blends the traveled distance toward the straight-line source→virtual distance (`Leg1Geom`, the same quantity Bend's ratio uses) via `PathAttenuationGeomBlend` (0 = pure traveled, default; 1 = pure straight-line) before applying `PathAttenuationStrength`. A hedge against the ray sweep failing to find the true shortest path in complex geometry and caching an artificially long substitute — not a correctness improvement, since it equally softens genuinely long, correctly-found detours. `Leg1Geom` is computed at each of `ComputePathAttenuation`'s three call sites (the per-frame cache-weighted path in `UpdaterCast.cpp`, the cluster→voice target in `SyncVirtualVoicesToClusters`, and the full-sweep `ComputeAudioFromRayAccumulator`) from that site's own source/virtual-position pair, so the blend is applied to the *target* everywhere — the existing `FInterpTo` smoothing toward that target is untouched.

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

These are called identically by both the async cast (`TickAsyncCast` → `ProcessRayHit`) and the sync LoS-break sweep (`FUpdater::TraceSingleLoSBreakRay`). Reading them together explains both.

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

---

## The NPC voice layer (`../Voice/`)

`UNPCVoiceComponent` (`Voice/NPCVoiceComponent.h/.cpp`) is a *consumer* of the acoustic state above, not part of the pipeline: it never traces, and nothing here reads from it.

Per tick it maps listener distance + `CurrentOcclusion` to a vocal-effort bucket (`ENPCVoiceEffort`, whisper → shout; distance bands select inversely — whisper close, shout far — and occlusion ≥ `OcclusionShiftThreshold` shifts one step toward Shout), commits the bucket through dwell-time hysteresis, and schedules lines from a `FNPCVoiceLineRow` DataTable (bank generated by `Tools/VoiceGen/`): category keyed to occlusion state (Clear vs Occluded content), cooldown groups, no-immediate-repeat. Playback injects the line's wave into the owner's voice `UAudioComponent` (tagged both `AudioComponentSource` — so the pipeline above feeds it bus + occlusion — and `NPCVoiceAudio`) via `SetWaveParameter` before `Play`, so a voice line diffracts and muffles exactly like any other co-located sound. A playing line is never modified mid-flight; bucket changes apply to the next line. Tunables live in `UNPCVoiceSettings` (same asset-or-CDO pattern as `USpatialAudioSettings`).
