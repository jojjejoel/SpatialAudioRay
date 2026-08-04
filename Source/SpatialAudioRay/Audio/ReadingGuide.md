# SpatialAudioRay Reading Guide

A route through the codebase for the first time: what to read in what order, and what each stop answers. Everything not
stated here is meant to be answerable by reading the code.

---

## What the system does

A sound source sits somewhere in the world. The player walks behind a wall. The plugin answers two questions,
continuously.

**How occluded is the direct sound?** A 0 to 1 `Occlusion` value goes to the source's MetaSound, which muffles and
attenuates itself from it.

**Where should the sound seem to come from instead?** Rays cast from the source bounce and crawl around geometry until
they find line of sight to the listener. The points where they broke free are diffraction edges: door frames, wall
corners. Real playing `UAudioComponent`s are moved to those points and faded in, so you hear the sound arriving around
the corner as well as muffled through the wall.

Everything else is machinery for answering those two accurately, cheaply, and without audible glitches.

### The three loops

| Loop                          | Rate                                                                   | Job                                                |
|-------------------------------|------------------------------------------------------------------------|----------------------------------------------------|
| Direct line-of-sight sampling | on a timed interval                                                    | produces the occlusion value, smoothed every frame |
| Full async sweep              | on a timed interval, while heavily occluded or sight is confirmed lost | finds diffraction edges                            |
| Edge cache maintenance        | one edge at a time, each on a timed interval, while sight is lost      | re-validates edges and fades out the dead          |

All three intervals shorten as either end moves and stretch when both are still. A sweep takes several frames, so its
answer is slightly out of date by the time it lands. The cheap loops compensate: occlusion is sampled on its own short
interval and never waits for a sweep, and found edges are cached so the virtual emitters do not blink out between
sweeps. Both the sweep and the cache go idle while the source is visible, so in that state the only thing still tracing
is the occlusion sampler. "Visible" here is `bHasDirectLoS`, which is true when any one of the five samples reaches the
source, so both can be idle at 80% occlusion. The sweep additionally runs through the near-occluded band, which is what
warms the cache before the last sliver closes.

### Two rules to keep in mind

**Listener independence.** `VirtualGain`, `PathAttenuation` and `VirtualPathBend`, which together decide how loud and
how muffled the diffracted sound is, depend only on source-to-edge geometry. Never on listener position. Listener
proximity is handled entirely by the engine's own `SoundAttenuation` on the emitter, which works because the emitter
really is at the edge. Occlusion is the one deliberate exception, since it means "does the listener see the source" by
definition. Where you see two weights computed side by side, `SrcW` and `PosW`, this rule is why: position and ranking
may use listener distance, gain may not.

**Single-writer ownership.** The occlusion formula, `1 - DirectLoSFraction`, is evaluated in exactly one place, the
per-frame sampler. The sweep deliberately does not derive a competing value from path ratios: whenever a good
diffraction path exists that value sits below the fraction-derived one, so it would drag occlusion off 1.0 every time a
sweep landed. The other writes to `TargetOcclusion` are constants rather than models: 0 when a sweep confirms direct
sight or the listener is out of range, 1 on init and on a blocked centre trace.

---

## Stop 1. `SpatialAudioTypes.h`, the vocabulary

Read the whole file, around 165 lines. You are learning the nouns.

`FSpatialRayState` is one in-flight ray: origin, direction, bounce count, pending trace handles, and `BounceWaypoints`,
every point where it changed direction. The waypoints matter at Stop 5.

`FCachedEdgePoint` is a confirmed diffraction edge that survives across sweeps. Note `ShortestPath` and
`ShortestPathSegmentVerified`, the polyline its path distance was measured along and which of its segments were actually
traced clear, and `EffectivePoint()`, where the audible emitter sits. The names `bRelayed` and `bSourceSideEviction`
will make sense after Stop 6.

`FVirtualVoice` and `FVirtualSlot` are worth separating in your head. A voice is the logical "sound coming from cluster
X". A slot is a pooled `UAudioComponent` that renders it. Voices hand slots off to each other so a position can jump
without a pop, the old slot fading out where it stands while the new one fades in.

Then skim `SpatialAudioSettings.h` for the category names only. Every tunable lives there in one shared `UDataAsset`.

## Stop 2. `SpatialAudioComponent.h`, where the state lives

`FAsyncCastManager`, `FUpdater` and `FEdgeCache` are stateless helpers made of static functions. They are friends of the
component, and all the actual state sits on `USpatialAudioComponent`, so this header is the state map for the whole
system.

Read the private section from `AsyncRays` down. Five groups are worth registering:

Sweep state is `AsyncRays`, `bAsyncCastActive`, `Finalize`, and `AsyncSourcePos` with `AsyncListenerPos`,
which are positions frozen at sweep start, because "current position" is ambiguous across a multi-frame sweep.

The targets are `TargetOcclusion`, `TargetVirtualSourceLocation` and `TargetPathAttenuation`. Casts write `Target*` and
`TickComponent` smooths `Current*` toward them. Position is the exception: emitters sit exactly on their target, and a
move too large to make in one step is handled by fading a new voice in rather than by easing.

The cache is `CachedEdgePoints`, and a warm one pays off by shrinking the next sweep's ray budget through
`FullCacheRayScale`.

Line-of-sight sampling keeps two values: `LastOffsetLoSFraction`, the raw instant sample, and
`WindowedLoSFraction`, averaged over the rotation pattern. The distinction matters at Stop 4.

`AudioDiag` and `TraceDiag` are debug-only. No audio path reads them.

## Stop 3. `TickComponent`, the frame skeleton

Read `TickComponent`, around 50 lines, and the phase methods it calls in order:

```
TickAsyncPipeline          read back last frame's probes, advance the sweep one step
UpdateVelocityScaling      smoothed source and listener speeds become interval multipliers
UpdateStationaryIdleState  stretch the sweep interval while nothing moves
FEdgeCache::TickCachedEdgeEviction   validate and evict cached edges (Stop 6)
ComputeEffectiveSweepInterval        how long until the next sweep is allowed
TickMovementSweepTrigger   listener moved far, request an early sweep
TickNormalSweepDispatch    per-frame LoS sampling always, then either start a
                           sweep or run the cheap update cast
SmoothTowardTargets        interpolate occlusion and path attenuation
UpdateAudioParameters      write the final numbers to the audio components (Stop 8)
```

One inline branch sits between dispatch and smoothing: the frame direct sight is lost sets `bMovementRequested`,
starting the next sweep immediately instead of waiting out the timer, so the cache refills while the crossfade is still
opening.

Sweep pacing itself is `ComputeEffectiveSweepInterval`, folding velocity scaling, a post-movement cache-fill burst and
stationary idle, in that order. Every branch that lengthens the interval requires both ends stationary, so a moving
listener has no floor beyond velocity scaling.

Read `BeginPlay` too. It caches every `UAudioComponent` tagged `AudioComponentSource`, since one pipeline serves all
co-located sounds on an actor, creates a transient `UAudioBus` for them to write into, and builds the virtual voice
pool. The pool is twice `MaxVirtualVoices` components, all playing silently from frame one so a fade-in never pays
MetaSound startup latency.

## Stop 4. Occlusion, the simplest complete subsystem

In `UpdaterCast.cpp`, read `TickDirectLoSSampling`, then `TrySampleOffsetLoS`, `SyncOffsetLoSFraction` and
`UpdateOcclusionFromSamples`. It is self-contained and shows the house style.

Every `OffsetLoSCheckInterval` it takes five samples, the listener centre plus a four-point ring, toward matching points
on the source's inner-radius sphere. That is up to nine traces, since resolving each ring point costs one of its own.
Seeing any of that sphere counts as seeing the source, because the source plays at full volume anywhere inside it, so a
wide source half-visible past a corner reads as partly occluded rather than clear. The fraction is how many samples came
back clear.

The ring rotates and its radius ladders through annuli on each check, so one full rotation samples the whole disc rather
than only the rim, and a clear centre view through a small opening does not read as four fifths occluded. A stationary
scene retraces exactly the same rays every cycle, which is what keeps the value from wobbling at rest.

The two values from Stop 2 have different consumers, and mixing them up causes real bugs. The raw fraction drives
gating: `bHasDirectLoS` and sweep suppression. Gaining sight is instant, while losing it when stationary needs a full
blank rotation first, as hysteresis against grazing rays that flicker. The pattern average produces `TargetOcclusion`,
which `OcclusionBlendTime` then smooths on the way into `CurrentOcclusion`. Never gate on that smoothed value: it
lags a real break by design.

## Stop 5. The async sweep

`AsyncCastManagerSubmit.cpp` and `AsyncCastManagerReadback.cpp`, four entry points called across consecutive frames.

**`StartAsyncFullCast`** fires once per sweep, and does two separable things: decide where to aim, then decide how much
to spend.

Aiming starts at `CaptureSweepPositions`. Note the separate steering positions, which are velocity-led and used only for
aiming, never for verifying a result. That split is the point: a wrong prediction can only aim rays less well, it can
never cache a false edge. `SubmitSweepRays` then distributes directions over a Fibonacci sphere, biased toward the
lateral band, because straight at the listener hits the same wall and straight away never comes back.

The budget comes from distance priority and then `ApplyCacheFullnessRayScale`, which thins it as the cache fills, on the
reasoning that a saturated cache can only improve by displacing an entry. It scales rather than stops, because a cache
that stopped searching could never find the corner that would displace something.

**`TickAsyncCast`** runs every frame while a sweep is active, advancing each ray one step: drain finished line-of-sight
probes, process a pending crawl batch, else handle the segment trace that just finished. That trace either missed,
terminating the ray or turning it mid-air when `MaxStraightFlightDistance` is set, or hit a wall, which starts a crawl
or a bounce, alternating per ray via `bNextHitCrawls`. Along every segment `SubmitSegmentLoSProbes` asks asynchronously
whether that point sees the listener, and the first that does becomes the ray's `LoSOrigin`.

Worth noticing at every decision point is the prune: a ray dies once travelled distance plus straight-line distance to
the listener exceeds the budget. By the triangle inequality that sum only grows, and every probe is gated on the same
bound, so past it the ray cannot produce a result no matter what happens next. It is lossless.

**`SubmitFinalizeBatch`** runs the frame all rays finish. `bDirectLoSFound` is computed in a scan before the per-ray
loop, which matters: derived inside it, rays past the first direct hit would see a different value than the readback
does. Then `ComputeStringPulledLeg1` shortens each path, because crawl steps and bounce detours make the travelled route
longer than the acoustic one. From the edge it hops to the furthest recorded waypoint that is directly visible and
repeats toward the source. Where nothing is visible it consumes exactly one raw travelled hop rather than abandoning the
rest, so a single blocked corner cannot swallow a genuine shortcut beyond it. Verified and unverified segments therefore
interleave, which is what `ShortestPathSegmentVerified` records.

**`ReadbackFinalizeBatch`** runs the frame after. It opens with `TryDiscardStaleSweep`: the sweep ran against frozen
positions, so sight is re-sampled now and the whole sweep discarded if the listener regained it in flight. Results then
accumulate and `MergeStoredPathsIntoCache` upserts edges, merging by radius with rank-scored replacement and hysteresis.
An entry is never evicted merely because a sweep failed to re-find it, since rays bounce differently every time and a
miss is not evidence of absence. A find inside the merge radius skips rank scoring entirely: same corner means a shared
listener leg, so the shorter travelled path simply wins.

## Stop 6. `EdgeCache.cpp`, keeping edges alive

Read `TickCachedEdgeEviction` top down. The problem it solves is that sweeps are seconds apart while the listener moves
continuously, so a cached edge has to be validated from the listener's side the whole time and dropped gracefully when
it stops being real.

Note the first line: the whole thing returns early while `bHasDirectLoS`, since a visible source has no diffraction
paths worth maintaining. Everything below runs only while occluded. `TickSingleEdge` then runs six phases in order:
`TickEvictionFade`, `TickRelayRescueReadback`, `TickPhase0Readback`, `TickPhase0OffsetReadback`, `TickRelayMaintenance`,
`TickPhase0Submission`.

Phase 0 is one async listener-to-edge trace, submitted for a single entry per slice rather than the whole cache. It
shares `CachedEdgeCheckInterval` with the polyline recheck and the promotion step, and `EdgeCheckSlice` divides that by
cache size, so the setting names the period an individual edge is checked at and the cost arrives evenly instead of as
a burst of N. The three passes take their own turns through the cache, so they never fire on the same edge together.

A blocked Phase 0 trace does not evict. It escalates:

1. `TryPromoteToInnerAnchor`, in case the previous point on the edge's own polyline already has direct listener sight,
   which makes the outer corner obsolete and shortens `PathDist` by the trimmed segment.
2. A four-point fan around the listener, since one centre trace grazing a corner is thin evidence. It fires only on a
   blocked centre, so it costs nothing while the edge is comfortably visible.
3. A relay rescue, routing the edge through the last listener position that could still see it, frozen at rescue time so
   gain stays listener-independent.

Only then does eviction start, and it is a fade through `EvictionAlpha` rather than a cut, since an emitter vanishing
instantly is audible. The rescue submits its four traces and rules the following tick, leaving the edge playing
untouched meanwhile; pre-fading would break outright at fade time 0, where the entry is removed before any readback
could land.

Two design points worth carrying away. Rescue depends only on the edge's own geometry, never on whether sibling edges
are healthy. That matters because edges around one corner tend to go dark on the same tick, and any rule referring to
sibling state would let whichever edge happened to be processed first decide the fate of the rest. As written, N edges
going dark produce N relays that each convert independently. And a confirmed relay is upgraded in place into a real
edge, bisecting its verified leg to find the second corner, which makes the relay a transitional state rather than a
resting one.

Source movement is handled by the shortest-path recheck, not a distance threshold. It re-traces one edge's stored
polyline per slice, skipping unverified segments, which were blocked at discovery by construction and would otherwise
evict every multi-corner path on sight. The first segment is special: retraced from the live source position, and a
clear result re-anchors the entry and re-measures `PathDist`. Listener movement never evicts anything, because Phase 0
already owns listener-side validity.

Whether an eviction can be undone depends on which side failed. Listener-side ones un-evict the moment Phase 0 sees the
edge again. Source-side ones, flagged `bSourceSideEviction`, need a fresh sweep, because the listener leg is usually
still clear and a restore would outpace the fade forever.

## Stop 7. The crawl mechanic, and the cheap path

Crawling is what actually finds most diffraction edges. Step along the wall away from the hit point and probe back
toward the surface at each step. While the back-probe hits, the wall is still there. The step where it misses is past
the end of the wall, and that point is the edge.

`TrySetupSurfaceCrawl` submits every step's three traces in one batch: the back-probe, a line-of-sight probe, and a
forward probe for a perpendicular wall. Batching is the point, since a crawl then costs one frame of latency however
many steps it runs. `EvaluateCrawlSteps` reads the batch back next frame and asks three questions per step, each its own
named function. It is all-or-nothing, returning without advancing if any probe is not ready, since a partial read would
evaluate steps out of order.

Then `PerformUpdateRayCast`, the cheap per-frame path used when no sweep is running. It casts nothing. It re-weights the
cache in `AccumulateCachedEdgeWeights`, where the `SrcW` and `PosW` split enforces listener independence, refreshes the
position target and path attenuation, then clusters edges into voices.

The clustering radius is not a setting. It is the virtual emitter's own attenuation inner radius, read from the asset,
and the reason is geometric: a point joins a cluster only if it is within that radius of the centroid, and the emitter
is placed at the centroid, so the emitter's full-volume sphere covers exactly the openings its voice speaks for. The
inner radius is the engine's own statement that distance stops mattering inside it, which is the same judgement
clustering makes when it calls two corners one sound. It also lets a quieter source group its openings more tightly
than a loud one, which a single shared value could not express. A radius of zero means no grouping, one voice per edge.

Both halves stay active through the pre-sweep band, gated on `bVirtualPathActive` rather than on `!bHasDirectLoS` alone.
The crossfade starts opening before occlusion is total, so the voices have to already exist and carry real attenuation
by then.

## Stop 8. Where numbers become sound

`UpdateDualModeAudio` in `UpdaterAudio.cpp` runs last every frame.

Every tagged source component receives `CurvedOcclusion`, and each MetaSound shapes its own volume and filtering from
it. There is no external crossfade on the source side.

`UpdateVirtualCrossfadeGate` decides whether the virtual voices are audible at all. It hard-opens once a full sampling
rotation comes back blank, and can ramp open earlier through the near-occluded band. While stationary with the ramp
enabled the hard term is suppressed, because a marginal pinhole can blank a rotation and would pump the gate.

`UpdateVirtualVoiceSlots` writes each slot's numbers. `VirtualGain` is path attenuation times weight share times fade
envelope times gate, with nothing listener-dependent in it. Path attenuation is evaluated against the virtual template's
own attenuation curve, so the source-to-emitter leg costs what the engine charges for the same distance on the
emitter-to-listener leg. `VirtualPathBend` is the detour ratio plus a distance term, and the MetaSound derives filter
cutoff and reverb from that single value. The slot component is physically moved, which is what lets the engine's
attenuation do the proximity work.

Finish with `Math.h` end to end, around 310 lines of pure stateless functions.

## Stop 9. The voice layer, and seeing it run

`Voice/` is a consumer, not part of the pipeline. It never traces, and nothing in `Audio/` reads from it.
`UNPCVoiceComponent` reads `GetEffectiveAcousticDistance`, the straight line while visible and the shortest cached route
once occluded, and picks a vocal effort from it, so standing close but around a corner makes the NPC shout. Its own
`Voice/README.md` covers the layer; the split there mirrors this one, with every scheduling decision as pure functions
in `NPCVoiceLogic.h`.

`SpatialAudioDebugSubsystem` registers every component and polls the debug keys. With `bDrawDebugRays` set, N cycles
which source draws, 2 shows bounce rays, 7 crawl steps, 6 edge points, 0 the string-pulled paths with unverified
segments dimmed, 1 virtual emitters, 3 the per-source HUD and G the global trace counts. Walking behind a wall with 2, 7
and 0 on makes Stops 5 through 7 concrete faster than reading them again. 6 and 1 together answer the clustering
question above: each edge draws a line to the emitter it feeds, in that emitter's colour, and an edge with no line is
one no audible voice speaks for.

Tests live in `Source/SpatialAudioRay/Tests/`, 94 of them under `SpatialAudioRay.Math.*`, `.Async.*`, `.Voice.*` and
`.EdgeCache.*` in Session Frontend. They read as a spec for the pure helpers, and `MathTests.cpp` is a good final read.

---

## The route, compressed

1. `SpatialAudioTypes.h`, the nouns
2. `SpatialAudioComponent.h` private section, the state map
3. `TickComponent`, the frame skeleton
4. `TickDirectLoSSampling`, occlusion
5. `StartAsyncFullCast` through `ReadbackFinalizeBatch`, the sweep
6. `TickCachedEdgeEviction`, edge lifetime
7. `TrySetupSurfaceCrawl` and `EvaluateCrawlSteps`, then `PerformUpdateRayCast`
8. `UpdateDualModeAudio` and `Math.h`, numbers into sound
9. Run it with the debug keys, skim the tests

If you would rather go depth-first, jump from Stop 3 straight to Stop 5 for the ray pipeline or straight to Stop 8 for
the audio behaviour. Stops 4 through 7 are independent enough to read in either order once the frame skeleton is in your
head.
