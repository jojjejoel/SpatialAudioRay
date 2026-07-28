# SpatialAudioRay — First-Time Reading Guide

A guided tour for reading this codebase for the first time. It tells you **in what order** to read the code, **what question** each stop answers, and **what mental model** you should have before moving to the next stop. The companion document `CodeFlow.md` is the deep per-system reference — come back to it when you need every detail of one subsystem; this guide is the on-ramp.

---

## What the system does (read this first)

A sound source sits somewhere in the world. The player walks behind a wall. This plugin answers two questions, continuously:

1. **How occluded is the direct sound?** → a 0–1 `Occlusion` value sent to the source's MetaSound, which muffles/attenuates itself accordingly.
2. **Where should the sound appear to come from instead?** → rays cast from the source bounce and crawl around geometry until they find line of sight (LoS) to the listener. The points where they broke free are **diffraction edges** (door frames, wall corners). Virtual emitters — real, playing `UAudioComponent`s — are physically placed at those edges and faded in, so the sound audibly wraps around the corner.

Everything else in the codebase is machinery to answer those two questions accurately, cheaply, and without audible glitches.

### The three loops

The system is three cooperating loops running at different rates:

| Loop | Rate | Job |
|---|---|---|
| Direct-LoS sampling | every frame | 5 sync traces → occlusion value |
| Full async sweep | every ~0.5s (adaptive) | 64 rays, multi-frame, finds diffraction edges |
| Edge cache maintenance | every frame | keeps previously-found edges alive/validated between sweeps |

Sweeps are *expensive and slow* (a sweep takes several frames and its result is already stale when it lands). The design compensates with the *cheap and fast* loops: per-frame occlusion sampling that never waits for a sweep, and a persistent edge cache so the virtual emitters don't blink out between sweeps.

### Two rules to keep in mind while reading

- **Listener independence:** `VirtualGain`, `PathAttenuation`, and `VirtualPathBend` (the "how loud/muffled is the diffracted sound" values) must depend only on source→edge geometry, never on listener position. Listener proximity loudness is handled exclusively by the engine's native `SoundAttenuation` on the emitter component, which works because the emitter is *physically moved* to the edge. Occlusion is the one deliberate exception (it is by definition "does the listener see the source"). When you see two weights computed side by side (`SrcW`/`PosW` in several places), this rule is why.
- **Single-writer ownership:** `TargetOcclusion` is written by exactly one place — the per-frame LoS sampler. The sweep readback and the LoS-break sweep deliberately do *not* write it. When you see a comment saying "deliberately not written here", it's guarding this rule.

---

## Stop 1 — `SpatialAudioTypes.h`: the vocabulary

Read the whole file (~260 lines). You're learning the nouns:

- `FSpatialRayState` — one in-flight ray during an async sweep: origin, direction, bounce count, its pending async trace handles, and `BounceWaypoints` (every point where it changed direction — needed later for path shortening).
- `FCachedEdgePoint` — a confirmed diffraction edge that survives across sweeps. Note `ShortestPath` + `ShortestPathSegmentVerified` (the polyline its path distance was measured along) and `EmitterPoint()` (where the audible emitter actually sits — walked back along that polyline). The long comments on `bRelayed` and `bSourceSideEviction` will make sense after Stop 6; skim them now.
- `FVirtualVoice` / `FVirtualSlot` — a *voice* is the logical "sound coming from cluster X"; a *slot* is a pooled `UAudioComponent` that renders it. Voices hand slots off so positions can jump without audible pops (old slot fades out in place, new slot fades in).

Then skim `SpatialAudioSettings.h` — don't read every property, just the category names. Every tunable in the system lives here, in one shared `UDataAsset`.

## Stop 2 — `SpatialAudioComponent.h`: where all state lives

Key structural fact: `FAsyncCastManager`, `FUpdater`, and `FEdgeCache` are stateless helper classes made of static functions. They are `friend`s of the component and all actual state lives on `USpatialAudioComponent`. So this header is the state map for the entire system.

Read the private section from `AsyncRays` down. Groups worth registering:

- Sweep state: `AsyncRays`, `bAsyncCastActive`, `Finalize`, `CycleAccum`, `AsyncSourcePos`/`AsyncListenerPos` (positions frozen at sweep start — the sweep is multi-frame, so "current position" is ambiguous during one).
- The targets: `TargetOcclusion`, `TargetVirtualSourceLocation`, `TargetPathAttenuation`. Casts write targets; `TickComponent` smooths `Current*` toward them. All audible values are smoothed — nothing snaps.
- The cache: `CachedEdgePoints`, plus `CachedMissDirs` (directions that found nothing — sampled less often next sweep) and `CachedEdgeDirs` (directions already covered by a cached edge — skipped entirely).
- LoS sampling state: `LastOffsetLoSFraction` (raw instant), `WindowedLoSFraction` (pattern average), `LastDirectLoSFraction` (smoothed). Three tiers — the distinction matters at Stop 4.

## Stop 3 — `TickComponent` in `SpatialAudioComponent.cpp`: the heartbeat

Read `TickComponent` (~60 lines) and the phase methods it calls, in order. This is the frame skeleton everything hangs off:

```
TickAsyncPipeline          — read back last frame's probes, advance the sweep one step
UpdateVelocityScaling      — smoothed source/listener speeds → interval multipliers
UpdateGeometryBurstAndIdleState
FEdgeCache::TickCachedEdgeEviction   — validate/evict cached edges (Stop 6)
ComputeEffectiveSweepInterval        — how long until the next sweep is allowed
TickMovementSweepTrigger   — listener moved far → request an early sweep
TickNormalSweepDispatch    — per-frame LoS sampling (always), then either
                             start a sweep, or run the cheap update cast
PerformLoSBreakSweep       — only on the frame LoS was just lost: instant sync sweep
                             so the virtual source is seeded before the crossfade opens
SmoothTowardTargets        — interpolate all Current* values
UpdateAudioParameters      — write the final numbers to the AudioComponents (Stop 8)
```

Also read `BeginPlay`: it caches every `UAudioComponent` tagged `AudioComponentSource` (one pipeline serves all co-located sounds on the actor), creates a transient `UAudioBus` the sources write into, and builds the virtual voice pool (2× `MaxVirtualVoices` components, all playing silently from frame one so fade-ins never pay MetaSound startup latency).

## Stop 4 — `UpdaterCast.cpp`: occlusion, the simplest complete subsystem

Read `TickDirectLoSSampling` → `TrySampleOffsetLoS` → `SyncOffsetLoSFraction` → `UpdateSmoothedOcclusionFromSamples`. This is self-contained and shows the house style: heavy geometric comments, exact periodicity arguments, explicit tier separation.

The idea: every `OffsetLoSCheckInterval`, fire 5 sync traces — listener center plus a 4-point ring — toward matching points on the source's inner-radius sphere. `fraction = clear/5`. The ring rotates and its radius ladders through annuli each check, so over one rotation cycle the whole listener disc and source cap get sampled, and a stationary scene retraces *exactly* the same rays every cycle (that's what makes the value wobble-free at rest).

The three tiers from Stop 2, and who consumes each:
- **raw instant** (`LastOffsetLoSFraction`) → gating: `bHasDirectLoS`, sweep suppression. Gaining LoS is instant; losing it while stationary requires a full blank rotation (hysteresis against marginal grazing rays).
- **pattern average** (`WindowedLoSFraction`, mean of per-slot cache) → smoothing target.
- **smoothed** (`LastDirectLoSFraction`) → `TargetOcclusion = 1 − fraction`. This line is the *only* formula-writer of `TargetOcclusion` in the codebase.

## Stop 5 — the async sweep: `AsyncCastManagerSubmit.cpp` + `AsyncCastManagerReadback.cpp`

The core pipeline. Four entry points, called across consecutive frames:

**`StartAsyncFullCast`** — fires once per sweep (or per sub-cycle). Read the phase methods in call order: positions are captured (`CaptureSweepPositions` — note the separate *steering* positions, velocity-led, used only for aiming, never for verification), the ray budget is resolved (cached edges count as free results and reduce it), and `SubmitSweepRays` distributes directions over a Fibonacci sphere, filters them through miss-direction/cached-edge exclusion, biases them toward the lateral band (where diffraction edges live — straight at the listener hits the same wall, straight away never comes back), and submits the first async trace per ray.

**`TickAsyncCast`** — every frame while active, advances each ray one step via `TickSingleRay`. A ray's life: drain finished LoS probes → if a crawl batch is pending, process it → otherwise its segment trace finished: it either **missed** (terminate, or turn mid-air if `MaxStraightFlightDistance` is on) or **hit a wall**, and a hit either sets up a **surface crawl** (walk along the wall probing for its edge — `TrySetupSurfaceCrawl` submits the whole probe batch up front) or **bounces** (`Math::ComputeBouncedDirection`: mirror reflection blended with roughness scatter and listener bias). Crawl and bounce alternate per ray (`bNextHitCrawls`). Along every segment, `SubmitSegmentLoSProbes` asynchronously asks "can this point see the listener?" — the first point that can becomes the ray's `LoSOrigin`: a diffraction edge candidate.

Note the **best-case prune** used at every decision point: a ray dies as soon as `traveled + straight-line-to-listener > budget`. By the triangle inequality that sum only grows, and every LoS probe is gated on it — so past the bound the ray provably cannot produce a result. Lossless, and a good example of the codebase's habit of stating *why* an optimization is safe.

**`SubmitFinalizeBatch`** — the frame all rays finish. For each LoS ray, `ComputeStringPulledLeg1` shortens the traveled path: the raw route (crawl steps, bounce detours) overestimates the acoustic source→edge distance, so it string-pulls through the recorded `BounceWaypoints` — hop to the farthest waypoint you can see straight, repeat toward the source, keeping raw hops only where nothing is visible. The result (`PathDist` + the polyline) is what all downstream gain math uses.

**`ReadbackFinalizeBatch`** (other file) — the frame after. First `TryDiscardStaleSweep`: the sweep ran against frozen positions, so re-sample LoS at *current* positions and throw the whole sweep away if the listener regained sight meanwhile. Then results accumulate into the cycle, and `MergeStoredPathsIntoCache` upserts edges into `CachedEdgePoints` — merge-by-radius, rank-scored replacement with hysteresis, never evicting an entry just because a sweep didn't re-find it (rays bounce differently every time; absence of evidence isn't eviction-worthy). A find landing *inside* the merge radius is the one case that skips the rank score entirely: same corner means a shared listener leg, so the shorter travelled path simply wins. `FEdgeCache::MergeCoincidentEdges` applies that same rule to entries already in the cache, which drift together as relay conversion and inner-anchor promotion move their points.

## Stop 6 — `EdgeCache.cpp`: keeping edges alive between sweeps

Read `TickCachedEdgeEviction` top-down; it's a per-edge phase sequence. The problem it solves: sweeps are seconds apart, but the listener moves continuously — a cached edge must be *continuously* validated from the listener side, and dropped gracefully when it stops being real.

- **Phase 0**: one async listener→edge trace per edge per interval. Blocked → try promoting the edge back to an inner anchor of its own polyline (`TryPromoteToInnerAnchor` — if a point closer to the source now sees the listener directly, the outer diffraction point is obsolete); then a 4-point offset fan around the listener; then a **relay rescue** (`TryRelayRescue`: route the edge through the last listener position that *did* see it — frozen at rescue time so gain stays listener-independent); only then eviction — which is a fade (`EvictionAlpha`), never a cut.
- **Movement eviction**: the *source* moving beyond a threshold evicts (its paths are stale). Listener movement never does — Phase 0 owns listener-side validity.
- **Shortest-path recheck**: round-robin, re-traces one edge's stored polyline per interval to catch geometry closing on the *source* side (a door closing between source and edge — nothing else watches that leg).

Evictions are one-way or restorable depending on *which side* failed: listener-side evictions un-evict the moment Phase 0 sees the edge again; source-side evictions (`bSourceSideEviction`) can only be rehabilitated by a fresh sweep, because the listener leg is typically still clear and would resurrect them forever.

## Stop 7 — `RayPhysics.cpp` + the update cast: the sync mirror

`ProcessRayHit` / `CrawlSurfaceToEdge` are synchronous versions of the same crawl-or-bounce logic from Stop 5, shared by `FUpdater::TraceSingleLoSBreakRay` (the instant sweep fired on LoS break). Read `CrawlSurfaceToEdge` to see the crawl mechanic in its plainest form: step along the wall, back-probe toward the surface each step; when the back-probe misses, the wall ended — that's the edge.

Then `PerformUpdateRayCast` (in `UpdaterCast.cpp`): the cheap per-frame path when no sweep is running. No new rays — it re-weights the existing cache (`AccumulateCachedEdgeWeights`, note the `SrcW`/`PosW` split enforcing listener independence), refreshes the virtual position target and path attenuation, then clusters edges into voices: `Math::ClusterEdgePoints` groups cache entries by radius, and `SyncVirtualVoicesToClusters` diffs desired-vs-active voices — within glide range a voice keeps its slot and glides; otherwise the old slot fades out in place and a new one fades in.

## Stop 8 — `UpdaterAudio.cpp` + `Math.h`: where numbers become sound

`UpdateDualModeAudio` runs last every frame:

- Every tagged source component receives `CurvedOcclusion` — each MetaSound shapes its own volume/filtering from it; there's no external source crossfade.
- The **virtual crossfade gate** (`UpdateVirtualCrossfadeGate`) controls whether virtual voices are audible at all: hard-opens on a completed blank LoS rotation, and (optionally) ramps open through the near-occluded band keyed to smoothed occlusion, so the diffracted sound bleeds in *before* full occlusion.
- `UpdateVirtualVoiceSlots` writes each slot's final `VirtualGain` (= path attenuation term × weight share × fade envelope × gate — and nothing listener-dependent) and `VirtualPathBend` (detour ratio `traveled/straight − 1` plus a distance term; the MetaSound derives HPF cutoff and reverb wetness from this single parameter internally), and physically moves the slot component — which is what makes the engine's native attenuation do the listener-proximity work.

Finish with `Math.h` end to end (~300 lines, pure stateless functions). Most formulas referenced everywhere else live here, with their reasoning attached.

## Stop 9 — seeing it run

`SpatialAudioDebugSubsystem` (world subsystem) registers every component and polls all debug keys. With `bDrawDebugRays` on a source: **N** cycles which source draws, **2** bounce rays, **7** crawl steps (cyan = crawling, white = flying), **6** edge points, **0** string-pulled shortest paths (magenta; dimmed = unverified segments), **1** virtual emitter spheres, **3** the per-source HUD, **G** the global trace-count HUD. Walking behind a wall while watching keys 2+7+0 is the fastest way to make Stops 5–7 concrete.

The `Voice/` folder (`UNPCVoiceComponent`) is a *consumer* demo: it reads the component's effective acoustic distance (straight line while visible, diffraction path length while occluded) to pick a vocal-effort bucket for NPC voice lines — whisper when acoustically close, shout when far — and plays them through the same pipeline. Nothing in `Audio/` depends on it.

Tests live at the module root (`SpatialAudio.Math.*`, `SpatialAudio.Async.*`, `SpatialAudio.Voice.*` in Session Frontend → Automation) and are a readable spec for the pure helpers — `SpatialAudioMathTests.cpp` is a good final read.

---

## The route, compressed

1. `SpatialAudioTypes.h` — the nouns
2. `SpatialAudioComponent.h` (private section) — the state map
3. `TickComponent` — the frame skeleton
4. `TickDirectLoSSampling` — occlusion (simplest full subsystem)
5. `StartAsyncFullCast` → `TickAsyncCast` → `SubmitFinalizeBatch` → `ReadbackFinalizeBatch` — the sweep
6. `TickCachedEdgeEviction` — edge lifetime
7. `CrawlSurfaceToEdge` / `ProcessRayHit`, then `PerformUpdateRayCast` — the sync mirror + per-frame path
8. `UpdateDualModeAudio` + `Math.h` — numbers → sound
9. Run it with debug keys; skim the tests

Depth-first alternative: after Stop 3, jump straight to Stop 5 if you care most about the ray pipeline, or straight to Stop 8 if you care most about the audio behavior — Stops 4–7 are independent enough to read in either order once you have the frame skeleton.
