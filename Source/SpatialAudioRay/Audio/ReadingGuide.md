# SpatialAudioRay Reading Guide

A route through this codebase for the first time. It says what order to read things in, what question each stop answers, and what you should understand before moving on. `CodeFlow.md` next to it is the per-system reference for when you need every detail of one subsystem. This is the on-ramp.

---

## What the system does

A sound source sits somewhere in the world. The player walks behind a wall. The plugin answers two questions, continuously.

**How occluded is the direct sound?** A 0 to 1 `Occlusion` value goes to the source's MetaSound, which muffles and attenuates itself from it.

**Where should the sound seem to come from instead?** Rays cast from the source bounce and crawl around geometry until they find line of sight to the listener. The points where they broke free are diffraction edges: door frames, wall corners. Real playing `UAudioComponent`s are moved to those points and faded in, so you hear the sound arriving around the corner as well as muffled through the wall.

Everything else is machinery for answering those two accurately, cheaply, and without audible glitches.

### The three loops

| Loop | Rate | Job |
|---|---|---|
| Direct line-of-sight sampling | every frame | a handful of traces, produces the occlusion value |
| Full async sweep | adaptive interval | a ray budget spread over several frames, finds diffraction edges |
| Edge cache maintenance | every frame | keeps found edges validated between sweeps |

A sweep takes several frames, so its answer is slightly out of date by the time it lands. The cheap loops compensate: occlusion is sampled every frame and never waits for a sweep, and found edges are cached so the virtual emitters do not blink out between sweeps.

### Two rules to keep in mind

**Listener independence.** `VirtualGain`, `PathAttenuation` and `VirtualPathBend`, which together decide how loud and how muffled the diffracted sound is, depend only on source-to-edge geometry. Never on listener position. Listener proximity is handled entirely by the engine's own `SoundAttenuation` on the emitter, which works because the emitter really is at the edge. Occlusion is the one deliberate exception, since it means "does the listener see the source" by definition. Where you see two weights computed side by side, `SrcW` and `PosW`, this rule is why.

**Single-writer ownership.** `TargetOcclusion` is written in exactly one place, the per-frame line-of-sight sampler. The sweep readback and the line-of-sight-break sweep deliberately do not write it. Comments saying "deliberately not written here" are guarding this.

---

## Stop 1. `SpatialAudioTypes.h`, the vocabulary

Read the whole file, around 260 lines. You are learning the nouns.

`FSpatialRayState` is one in-flight ray during a sweep: origin, direction, bounce count, pending trace handles, and `BounceWaypoints`, every point where it changed direction. The waypoints matter later for path shortening.

`FCachedEdgePoint` is a confirmed diffraction edge that survives across sweeps. Note `ShortestPath` and `ShortestPathSegmentVerified`, the polyline its path distance was measured along, and `EmitterPoint()`, where the audible emitter actually sits after being walked back along that polyline. The long comments on `bRelayed` and `bSourceSideEviction` will make sense after Stop 6. Skim them for now.

`FVirtualVoice` and `FVirtualSlot` are worth separating in your head. A voice is the logical "sound coming from cluster X". A slot is a pooled `UAudioComponent` that renders it. Voices hand slots off to each other so a position can jump without a pop, with the old slot fading out where it stands while the new one fades in.

Then skim `SpatialAudioSettings.h`. Do not read every property, just the category names. Every tunable in the system lives there in one shared `UDataAsset`.

## Stop 2. `SpatialAudioComponent.h`, where the state lives

`FAsyncCastManager`, `FUpdater` and `FEdgeCache` are stateless helpers made of static functions. They are friends of the component, and all the actual state sits on `USpatialAudioComponent`, so this header is the state map for the whole system.

Read the private section from `AsyncRays` down. Four groups are worth registering:

Sweep state is `AsyncRays`, `bAsyncCastActive`, `Finalize`, `CycleAccum`, and `AsyncSourcePos` with `AsyncListenerPos`, which are positions frozen at sweep start. A sweep spans several frames, so "current position" is ambiguous while one is running.

The targets are `TargetOcclusion`, `TargetVirtualSourceLocation` and `TargetPathAttenuation`. Casts write targets and `TickComponent` smooths the `Current` values toward them. Nothing audible ever snaps.

The cache is `CachedEdgePoints`, alongside `CachedEdgeDirIndices`, the Fibonacci indices of rays that already found a still-valid edge. The next sweep skips those indices outright, which is the only way a warm cache saves trace budget.

Line-of-sight sampling keeps three values: `LastOffsetLoSFraction` raw, `WindowedLoSFraction` averaged over the pattern, and `LastDirectLoSFraction` smoothed. The distinction between them matters at Stop 4.

## Stop 3. `TickComponent`, the frame skeleton

Read `TickComponent`, around 60 lines, and the phase methods it calls in order. Everything hangs off this:

```
TickAsyncPipeline          read back last frame's probes, advance the sweep one step
UpdateVelocityScaling      smoothed source and listener speeds become interval multipliers
UpdateGeometryBurstAndIdleState
FEdgeCache::TickCachedEdgeEviction   validate and evict cached edges (Stop 6)
ComputeEffectiveSweepInterval        how long until the next sweep is allowed
TickMovementSweepTrigger   listener moved far, request an early sweep
TickNormalSweepDispatch    per-frame LoS sampling always, then either start a
                           sweep or run the cheap update cast
SmoothTowardTargets        interpolate all Current values
UpdateAudioParameters      write the final numbers to the AudioComponents (Stop 8)
```

One inline branch sits between the dispatch and the smoothing: the frame direct line of sight is lost sets `bMovementRequested`, which starts the next sweep immediately instead of waiting out the interval timer, so the edge cache refills while the crossfade is still opening.

Read `BeginPlay` too. It caches every `UAudioComponent` tagged `AudioComponentSource`, since one pipeline serves all co-located sounds on an actor, creates a transient `UAudioBus` for the sources to write into, and builds the virtual voice pool. The pool is twice `MaxVirtualVoices` components, all playing silently from frame one so a fade-in never pays MetaSound startup latency.

## Stop 4. Occlusion, the simplest complete subsystem

In `UpdaterCast.cpp`, read `TickDirectLoSSampling`, then `TrySampleOffsetLoS`, `SyncOffsetLoSFraction` and `UpdateSmoothedOcclusionFromSamples`. It is self-contained and it shows the house style: heavy geometric comments and explicit reasoning about why a sampling pattern behaves the way it does.

Every `OffsetLoSCheckInterval` it fires five traces, the listener centre plus a four-point ring, toward matching points on the source's inner-radius sphere. The fraction is simply how many came back clear. The ring rotates and its radius ladders through annuli on each check, so one full rotation samples the whole listener disc and the source cap. A stationary scene retraces exactly the same rays every cycle, which is what keeps the value from wobbling at rest.

The three values from Stop 2 have different consumers, and mixing them up causes real bugs. The raw fraction drives gating: `bHasDirectLoS` and sweep suppression. Gaining sight is instant, while losing it when stationary needs a full blank rotation first, as hysteresis against grazing rays that flicker. The pattern average is the smoothing target. The smoothed value produces `TargetOcclusion`, and that line is the only place in the codebase that computes it.

## Stop 5. The async sweep

`AsyncCastManagerSubmit.cpp` and `AsyncCastManagerReadback.cpp`. This is the core pipeline, four entry points called across consecutive frames.

**`StartAsyncFullCast`** fires once per sweep or sub-cycle. Read its phase methods in call order. Positions are captured by `CaptureSweepPositions`, and note the separate steering positions there: they are velocity-led and used only for aiming, never for verifying a result. The ray budget is resolved next, with cached edges counting as free results that reduce it. Then `SubmitSweepRays` distributes directions over a Fibonacci sphere, skips the indices cached edges already cover, and biases the rest toward the lateral band. That bias exists because straight at the listener hits the same wall and straight away never comes back, so diffraction edges are found to the sides.

**`TickAsyncCast`** runs every frame while a sweep is active and advances each ray one step through `TickSingleRay`. A ray's life goes: drain any finished line-of-sight probes, process a pending crawl batch if there is one, otherwise handle the segment trace that just finished. That trace either missed, in which case the ray terminates or turns mid-air if `MaxStraightFlightDistance` is enabled, or it hit a wall. A hit either starts a surface crawl, where `TrySetupSurfaceCrawl` submits the whole probe batch up front, or a bounce through `Math::ComputeBouncedDirection`, which blends mirror reflection with roughness scatter and a pull toward the listener. Crawls and bounces alternate per ray via `bNextHitCrawls`. Along every segment `SubmitSegmentLoSProbes` asks asynchronously whether that point can see the listener, and the first point that can becomes the ray's `LoSOrigin`, a diffraction edge candidate.

Worth noticing at every decision point is the prune: a ray dies as soon as travelled distance plus straight-line distance to the listener exceeds the budget. By the triangle inequality that sum only ever grows, and every line-of-sight probe is gated on the same bound, so past it the ray cannot produce a result no matter what happens next.

**`SubmitFinalizeBatch`** runs on the frame all rays have finished. For each ray that found line of sight, `ComputeStringPulledLeg1` shortens the travelled path, because crawl steps and bounce detours make the raw route longer than the acoustic distance. It hops from the edge to the furthest recorded waypoint that is directly visible, repeats toward the source, and keeps raw hops only where nothing is visible. The resulting `PathDist` and polyline are what all the downstream gain maths uses.

**`ReadbackFinalizeBatch`** runs the frame after. It opens with `TryDiscardStaleSweep`: the sweep ran against frozen positions, so line of sight is re-sampled at current positions and the whole sweep is thrown away if the listener regained sight while it was in flight. Results then accumulate into the cycle and `MergeStoredPathsIntoCache` upserts edges into `CachedEdgePoints`, merging by radius with rank-scored replacement and hysteresis. An entry is never evicted merely because a sweep failed to re-find it, since rays bounce differently every time and a miss is not evidence of absence. A find landing inside the merge radius skips the rank score entirely: same corner means a shared listener leg, so the shorter travelled path just wins. `FEdgeCache::MergeCoincidentEdges` applies the same rule to entries already in the cache, which drift toward each other as relay conversion and inner-anchor promotion move their points.

## Stop 6. `EdgeCache.cpp`, keeping edges alive

Read `TickCachedEdgeEviction` top down. It is a per-edge phase sequence, and the problem it solves is that sweeps are seconds apart while the listener moves continuously, so a cached edge has to be validated from the listener's side the whole time and dropped gracefully when it stops being real.

Phase 0 is one async listener-to-edge trace per edge per interval. When it comes back blocked, several things are tried before giving up. First `TryPromoteToInnerAnchor`, in case a point closer to the source now sees the listener directly, which would make the outer diffraction point obsolete. Then a four-point fan around the listener, because one centre trace grazing a corner is thin evidence. Then a relay rescue, routing the edge through the last listener position that could still see it, frozen at rescue time so the gain stays listener-independent. That one submits its four traces and rules the following tick, leaving the edge playing untouched while they fly. Only after all of that does eviction start, and eviction is a fade through `EvictionAlpha` rather than a cut, since an emitter vanishing instantly is audible.

Movement eviction is separate and applies to the source. If the source moves beyond a threshold its paths are stale, so its edges go. Listener movement never evicts anything, because Phase 0 already owns listener-side validity.

The shortest-path recheck is round-robin, re-tracing one edge's stored polyline per interval to catch geometry closing on the source side, such as a door shutting between the source and the edge. Nothing else watches that leg.

Whether an eviction can be undone depends on which side failed. Listener-side evictions un-evict the moment Phase 0 sees the edge again. Source-side ones, flagged `bSourceSideEviction`, can only be rehabilitated by a fresh sweep, because the listener leg is usually still clear and would otherwise resurrect them forever.

## Stop 7. The crawl mechanic, and the cheap path

Surface crawling deserves its own read, since it is what actually finds most diffraction edges. The mechanic is simple: step along the wall away from the hit point, and at each step probe back toward the surface. While the back-probe hits, the wall is still there. The step where it misses is past the end of the wall, and that point is the edge.

The code splits that across two phases, because every probe is asynchronous. `TrySetupSurfaceCrawl` computes the crawl direction, blending the ray's slide vector along the wall with the wall-projected direction to the listener by `CrawlListenerBias`, then submits every step's three traces in one batch: the back-probe, a line-of-sight probe to the listener, and a forward probe for a perpendicular wall in the way. Submitting the whole batch up front is the point. A crawl costs one frame of latency however many steps it runs, instead of one frame per step.

`EvaluateCrawlSteps` reads that batch back the next frame and walks the steps in order, asking three questions per step, each its own named function: did this step see the listener, did the forward probe hit a perpendicular wall, and did the back-probe miss. The first yes wins and ends the crawl. Note that it is all-or-nothing, returning without advancing if any probe in the batch is not ready yet, since a partial read would evaluate steps out of order.

Then `PerformUpdateRayCast` in `UpdaterCast.cpp`, the cheap per-frame path used when no sweep is running. It casts no new rays. It re-weights the existing cache in `AccumulateCachedEdgeWeights`, where the `SrcW` and `PosW` split is what enforces listener independence, refreshes the virtual position target and path attenuation, then clusters edges into voices. `Math::ClusterEdgePoints` groups cache entries by radius and `SyncVirtualVoicesToClusters` diffs desired against active: within glide range a voice keeps its slot and glides, otherwise the old slot fades out where it is and a new one fades in.

## Stop 8. Where numbers become sound

`UpdateDualModeAudio` in `UpdaterAudio.cpp` runs last every frame.

Every tagged source component receives `CurvedOcclusion`, and each MetaSound shapes its own volume and filtering from that. There is no external crossfade on the source side.

`UpdateVirtualCrossfadeGate` decides whether the virtual voices are audible at all. It hard-opens once a full sampling rotation has come back blank, and can optionally ramp open earlier through the near-occluded band, keyed to smoothed occlusion, so the diffracted sound bleeds in before occlusion is total.

`UpdateVirtualVoiceSlots` writes each slot's final numbers. `VirtualGain` is the path attenuation term times weight share times fade envelope times gate, with nothing listener-dependent anywhere in it. `VirtualPathBend` is the detour ratio, travelled over straight minus one, plus a distance term, and the MetaSound derives HPF cutoff and reverb wetness from that single parameter internally. It also physically moves the slot component, which is what lets the engine's own attenuation do the listener-proximity work.

Finish with `Math.h` end to end, around 300 lines of pure stateless functions. Most of the formulas referenced elsewhere live there with their reasoning attached.

## Stop 9. Seeing it run

`SpatialAudioDebugSubsystem` is a world subsystem that registers every component and polls the debug keys. With `bDrawDebugRays` set on a source, N cycles which source draws, 2 shows bounce rays, 7 shows crawl steps with cyan for crawling and white for flying, 6 shows edge points, 0 shows the string-pulled shortest paths in magenta with unverified segments dimmed, 1 shows virtual emitter spheres, 3 is the per-source HUD and G is the global trace-count HUD. Walking behind a wall with 2, 7 and 0 on makes Stops 5 through 7 concrete faster than reading them again.

The `Voice/` folder is a consumer rather than part of the system. `UNPCVoiceComponent` reads the component's effective acoustic distance, which is the straight line while the source is visible and the diffraction path length once it is occluded, and picks a vocal effort bucket from it, whispering when acoustically close and shouting when far. The lines play back through the same pipeline as anything else. Nothing in `Audio/` depends on it.

Tests live in `Source/SpatialAudioRay/Tests/`, registered as `SpatialAudioRay.Math.*`, `SpatialAudioRay.Async.*` and `SpatialAudioRay.Voice.*` under Session Frontend, Automation. They read as a spec for the pure helpers, and `MathTests.cpp` is a good final read.

---

## The route, compressed

1. `SpatialAudioTypes.h`, the nouns
2. `SpatialAudioComponent.h` private section, the state map
3. `TickComponent`, the frame skeleton
4. `TickDirectLoSSampling`, occlusion
5. `StartAsyncFullCast`, `TickAsyncCast`, `SubmitFinalizeBatch`, `ReadbackFinalizeBatch`, the sweep
6. `TickCachedEdgeEviction`, edge lifetime
7. `TrySetupSurfaceCrawl` and `EvaluateCrawlSteps`, then `PerformUpdateRayCast`
8. `UpdateDualModeAudio` and `Math.h`, numbers into sound
9. Run it with the debug keys, skim the tests

If you would rather go depth-first, jump from Stop 3 straight to Stop 5 for the ray pipeline or straight to Stop 8 for the audio behaviour. Stops 4 through 7 are independent enough to read in either order once the frame skeleton is in your head.
