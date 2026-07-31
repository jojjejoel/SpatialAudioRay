# Audio System Code Flow

How to read the code in `Plugins/SpatialAudioRay/Source/SpatialAudioRay/Audio/`, what each piece does, when it runs, and how results reach the audio output.

First time here? Read `ReadingGuide.md` instead. It is a step-by-step tour. This document is the per-system reference.

---

## File map

| File | What lives here |
|---|---|
| `SpatialAudioComponent.h/.cpp` | Main class and `TickComponent`. All state lives here. |
| `AsyncCastManagerSubmit.cpp` | Sweep launch and the per-frame ray advance. |
| `AsyncCastManagerReadback.cpp` | Finalize readback and the cache merge. |
| `UpdaterCast.cpp` | Per-frame LoS sampling, update cast, LoS-break sweep. |
| `UpdaterAudio.cpp` | Writing the final numbers to the `UAudioComponent`s. |
| `EdgeCache.cpp` | Per-frame validation and eviction of cached diffraction edges. |
| `RayPhysics.cpp` | Shared surface-crawl and bounce helpers, used by both casts. |
| `DebugDrawer.cpp` | Debug overlay. Nothing here affects audio. |
| `SpatialAudioDebugSubsystem.h/.cpp` | Registry of active components, global HUD, all debug key polling. |
| `SpatialAudioTypes.h` | Shared structs: `FSpatialRayState`, `FCachedEdgePoint` and the rest. |
| `Math.h` | Pure stateless helpers: Fibonacci directions, clustering, path maths. |
| `SpatialAudioSettings.h/.cpp` | The `UDataAsset` holding every tunable. |

### Debug subsystem notes

`USpatialAudioDebugSubsystem` is a `UTickableWorldSubsystem`. It registers components in `BeginPlay` and unregisters in `EndPlay`, sums their `TraceDiag` into a global HUD line showing traces per second averaged over 1, 10 and 60 second windows, and draws one line per source with the same stats. `ToggleGlobalDebugTextKey`, default G, controls it, polled once by the subsystem and independent of the per-source key 3.

The subsystem polls all debug keys, reading key config from the first registered source. `CycleDebugSourceKey`, default N, cycles `bDrawDebugRays` from off, through each source in turn, back to off, so exactly one source draws at a time. It works while everything is off, which is the way back in. Sub-mode keys 1 through 0 and P assign the flipped value to every registered source so flags never desync across the cycle.

`MaxUncycledDebugSources` in `USpatialAudioSettings`, 0 for off, caps how many sources draw while not single-source-cycled. `ApplyProximityDebugLimit` ranks the sources that had `bDrawDebugRays` true at registration by live distance to the listener and lets only the closest N draw, re-derived every tick so sources fade in and out as the player moves. The eligibility snapshot `bEligibleForDebugRays` exists because the limiter mutates `bDrawDebugRays` itself, and without it a source suppressed for distance would be indistinguishable from one deliberately disabled. It lives in the shared settings asset rather than per-component key config because it is a global policy value, not a keybinding. It is skipped once a source is cycle-selected, tracked by `bCycleModeActive`, so it cannot fight an explicit choice. Wrapping the cycle back to off clears that flag, so the closest-N view returns immediately rather than going blank.

`ToggleActorLabelsKey`, default L and on by default, draws `GetActorNameOrLabel()` at each source via `DrawDebugString`. Being screen-space text it is camera-facing and visible through geometry for free. The key is polled independent of `bAnyDebugRays`, but a label only draws for sources currently drawing rays.

---

## Frame order

`TickComponent` calls its phases in a fixed order every frame and is short enough to read directly; `ReadingGuide.md` Stop 3 walks it. Two parts of it are not obvious from the call sequence.

**Sweep pacing** is decided by `ComputeEffectiveSweepInterval`, which folds together velocity scaling, geometry burst, stationary idle and the post-movement cache-fill state. Movement-triggered sweeps arm `MovementCacheFillMaxSweeps` and clear every edge's `bNewSinceFillArm`. Until `MovementCacheFillRequiredEdges` new edges are cached, meaning fresh slots or displacements rather than merge-matched re-confirmations, and non-relayed and non-evicting, sweeps keep burst pace even after movement stops. Budget is spent only by sweeps that leave the target short.

**The pre-sweep band** is occlusion at or above `PreSweepOcclusionThreshold` while partial LoS remains. It pre-warms the edge cache before full occlusion, and such casts ignore direct-hit rays and skip cache clears and discards.

---

## The full async cast

The main ray survey. It runs every `EffFullSweepInterval` seconds and takes `MaxBounces + 1` frames. Read the four entry points in order.

### 1. `StartAsyncFullCast`

Fires at the start of a sweep. Distributes `AsyncTotalRays` directions over a Fibonacci sphere, submits the first async trace per ray, and sets `bAsyncCastActive`.

If `FullSweepCycleCount` is above 1 a sweep splits into sub-cycles, tracked by `CycleAccum.Index`. Positions and ray counts are captured on cycle 0 only.

Cached edges are snapshotted into `PendingValidCachedPoints` at cycle 0. Direct, non-evicting entries count as free results and reduce the rays needed. Relayed and evicting entries stay in the snapshot for presentation but do not substitute for rays and do not have their directions skipped, which `BuildCachedEdgeSkipIndices` handles. A relay is an audible stopgap rather than a found path, so the sweep keeps searching at full budget, and the region around a relayed edge is exactly where its replacement most likely is.

**Steering prediction.** With `SteeringPredictionLeadTime` above 0 the sweep also captures `AsyncSteeringSourcePos` and `AsyncSteeringListenerPos`, the actual positions led by the smoothed velocity vectors. The lead is signed, in `ComputeSteeringLead`. Within the lead time of losing direct LoS, which covers pre-sweep-band casts and the LoS-break sweep, steering aims at where the listener was, because the aperture just crossed is behind them, between there and the source. Once occlusion is sustained it flips to forward prediction.

Only steering sites read those positions: the Fibonacci aiming axis, the lateral-band bias weight, every listener pull in flight through `BounceListenerBias`, `CrawlListenerBias`, `SelectEdgeDirection` and mid-air turns, plus the launch bias of the LoS-break sweep. Everything that verifies or caches stays on actual positions, including LoS probes, budget and probe gates, occlusion sampling, Phase 0 and readback ranking. A wrong prediction can therefore only aim rays less well, never cache a false edge. Stationary, the lead decays to zero and behaviour including seeded-bias determinism is unchanged.

### 2. `TickAsyncCast`

Runs every frame while `bAsyncCastActive`, advancing every ray in `AsyncRays` one step:

- `DrainPendingLoSProbes` reads back in-flight mid-segment LoS probe results.
- `ProcessCrawlBatch` reads back all step probes if a crawl was submitted last frame. It is all-or-nothing and returns without advancing if any probe is not ready.
- `SubmitSegmentLoSProbes` samples the current segment for LoS to the listener with async traces spaced along it.
- The next step is decided. If the segment hit a surface, `ProcessRayHit` in `RayPhysics.cpp` chooses crawl or bounce and sets the new origin and direction.

When all rays reach `bDone`, `TickAsyncCast` calls `SubmitFinalizeBatch` and clears `bAsyncCastActive`.

**Mid-air turns.** With `MaxStraightFlightDistance` above 0, every segment and the crawl range are capped to that distance, and a segment that flies its full capped length without hitting anything turns instead of terminating. `ComputeMidAirTurnDirection` applies the same roughness scatter and listener bias as a bounce, but around the current direction since there is no surface normal. At zero roughness and zero bias, where the scatter formula would fly straight, it turns 90 degrees at an angle deterministically seeded from the turn point, so stationary scenes replay identical turns. Each turn consumes a bounce and records a `BounceWaypoint` so string pulling sees it, and the ray only terminates on LoS, budget exhaustion or `MaxBounces`. At the default of 0 a miss terminates the ray. `Ray.SegSubmitLen` records each submitted segment length so the miss handler knows the actual flown distance, since recomputing it from the budget formulas would overshoot the clamp.

**Best-case pruning**, always on with no setting. Every LoS probe is gated on `CumDist + dist(point, listener) <= Budget`, and by the triangle inequality that sum can only grow with further travel. So at every decision point, meaning bounce, crawl exit, mid-air turn and the loop head of both sync sweeps, a ray past that bound dies immediately. It cannot produce a result and flying on would only burn traces. This is lossless: it never kills a ray that could still publish anything.

### 3. `SubmitFinalizeBatch`

Runs the frame all rays finish. It does the trace-free computation, meaning weighted position sum, total LoS bounces and distance, and `bDirectLoSFound`, submits refinement probes for every LoS hit, saves everything into `Finalize` and sets `Finalize.bPending`.

`bDirectLoSFound` is computed in a scan over all rays before the per-ray loop. This matters: computed inside the loop, rays past the first direct hit would see a different value than the readback phase does. That was a real bug.

**String pulling.** For each LoS ray, `ComputeStringPulledLeg1` shortens the path, because travelled distance overestimates the acoustic source-to-edge route. Starting at the edge it finds the first anchor with a clear straight segment to the current point, the source first and then `BounceWaypoints` in path order, hops there, and repeats until it reaches the source. `BasePathDist` becomes the summed chain of verified straight segments. Checks use sync traces with reverse hygiene, and nudge only for waypoint anchors.

When nothing is visible from the current point it does not abandon the whole remaining prefix. It consumes exactly one raw travelled hop as an unverified link, since that segment was traced and found blocked and is the actual crawl or bounce leg rather than a straight line, then keeps pulling from the new point. One blocked corner therefore cannot swallow a shortcut genuinely available past it, such as a second unrelated opening beyond a second corner.

Verified and unverified segments can interleave along the final polyline rather than forming one trailing prefix, so `ShortestPathSegmentVerified[i]` records per segment whether `ShortestPath[i]` to `ShortestPath[i+1]` is verified. If the edge sees the source directly, `PathDist` collapses to the straight-line distance and `VirtualPathBend` goes to 0. The polyline travels with the result onto each cached edge and is drawn per frame under key 0 in magenta, unverified segments dimmed, with a sphere at each intermediate anchor coloured to match its own segment.

### 4. `ReadbackFinalizeBatch`

Runs the frame after `SubmitFinalizeBatch`, at the top of the next `TickComponent`.

If the sweep found no direct LoS, `TryDiscardStaleSweep` re-runs the five-sample LoS check synchronously at current positions. If occlusion has fallen back below `PreSweepOcclusionThreshold` the entire sweep is discarded, because the listener regained sight while the multi-frame cast was in flight and publishing would stomp `bHasDirectLoS` and register edges thrown away a frame later. Results inside the pre-sweep band are deliberately kept, since warming the cache during partial LoS is the point of pre-sweeps.

Otherwise `AccumulateRefineProbesIntoCycle` reads back the refinement probes into `CycleAccum` (accumulated across sub-cycles, published only on the last) and into `StoredLoSPaths` for per-frame recheck in the update cast, `MergeStoredPathsIntoCache` upserts confirmed results into `CachedEdgePoints`, and `PublishSweepAudioTargets` writes `TargetVirtualSourceLocation` and `TargetPathAttenuation`. It does not write `TargetOcclusion`, which the per-frame sampler owns; the one exception is zeroing it the moment a sweep confirms direct LoS.

---

## Per-frame direct-LoS sampling

`TickDirectLoSSampling` in `UpdaterCast.cpp` runs every frame, including while a full cast is in flight, because occlusion must keep updating during sweeps instead of stalling until they finish. It is the sole owner of `TargetOcclusion`. Neither the cast readback nor the LoS-break sweep writes it.

### Sampling the fraction

`SyncOffsetLoSFraction`, on `OffsetLoSCheckInterval`, fires five synchronous traces: centre to centre plus a four-point ring perpendicular to the source-listener axis. The ring rotates by 90 degrees over `OffsetRingRotationSteps` per check while its radius steps through annuli at `((step+1)/steps)^OffsetRingRadiusExponent` of full radius, with the same period. One cycle therefore covers the whole disc rather than just the rim, so a clear centre view through a small opening no longer reads as four fifths occluded. At the 0.5 default the annuli are equal-area, meaning the cycle average estimates visible disc area with radii crowding toward the rim. Raising the exponent pulls them inward to weight the centre more, at identical trace cost. The pattern repeats exactly every `OffsetRingRotationSteps` checks.

Each listener sample pairs with a same-world-direction lateral offset around the source, annulus-laddered like the listener ring, lifted toward the listener onto the source's inner-radius sphere. The radius R is the attenuation inner radius times `SourceLoSSampleRadiusScale` and is never laddered, being fixed geometry, and the lift is `sqrt(R²-r²)`, so the centre pairs with the sphere point on the source-listener line. Head-on, the source targets read as a filled disc of radius R. From the side they wrap the sphere's listener-facing cap.

The source plays at full volume anywhere inside the inner radius, so seeing any of the sphere's surface counts as seeing the source, and a sample already inside the sphere is clear without tracing. A wide source partially visible past a corner reads as partially occluded, and source extent costs nothing extra: five LoS traces plus four listener-point resolve traces, since cap points are computed rather than resolve-traced. Scale 0 means a point source and traces reach the exact centre.

`LastOffsetLoSFraction`, clear over five, is the raw instant sample. The centre is one vote among five, so pinhole LoS reads as mostly occluded.

### Averaging and smoothing

Each check writes its instant sample into a per-slot cache, `LoSSlotFractions`, indexed by the check's position within the rotation, and `WindowedLoSFraction` is recomputed as the average of all slots immediately, every check. It was previously batched once per completed rotation. A stationary scene retraces the same ray per slot every rotation, so a slot really does hold what that ray saw last time, and occlusion can move mid-rotation instead of freezing for a cycle.

An earlier sliding window was reverted for re-perturbing on every check that resampled a marginal grazing direction. This is mathematically the same shape, a length-`RotationSteps` window recomputed every check, and was re-adopted anyway because a stationary ray only flickers from rare floating-point noise, which is accepted.

`LastDirectLoSFraction` then chases the pattern average over `LoSFractionSmoothingTime`, scaled by the same `VelocityScaling.OffsetLoSMultiplier` as the check interval so the softening stays proportional to how often the cycle completes: shorter while moving, full baseline at rest.

`bHasDirectLoS`, used for sweep gating and the edge cache, reads the raw instant sample. Gaining LoS is instant, but a stationary scene only loses it once a full rotation finds nothing, tracked by `NoLoSSampleStreak >= OffsetRingRotationSteps`, so a marginal direction that only some ring orientations catch cannot flip-flop playback between the occluded source and the virtual path. Movement drops LoS immediately.

Finally, `TargetOcclusion = 1 - smoothed fraction`.

---

## The per-frame update cast

`PerformUpdateRayCast` in `UpdaterCast.cpp` runs only when no full cast is active. It is the fast path keeping the virtual source position and path attenuation responsive between sweeps.

It weights cached edges using source-side weights only, meaning eviction confidence and geometric falloff, and writes `TargetVirtualSourceLocation` while refreshing `CurrentSourceToVirtualDistance` and `TargetPathAttenuation` from the cache. Then it clusters edges into virtual voices through `ClusterEdgePoints` and `SyncVirtualVoicesToClusters`. It casts no new rays of its own.

Both steps stay active while occluded and through the pre-sweep band, gated on `bVirtualPathActive = !bHasDirectLoS || IsPreSweepActive()`. The crossfade gate starts opening before full occlusion, so the voices must already exist, be positioned and carry real path attenuation by then. Gating on `!bHasDirectLoS` alone left the gate opening onto an empty voice list, with crossfade above 0 and gain stuck at 0, until LoS fully dropped.

**Emitter positions** come from `FCachedEdgePoint::EmitterPoint(VirtualSourcePullbackDistance)`: each edge's presentation point walked back that many centimetres along its arrival path, the relay leg first and then the verified `ShortestPath` suffix, never into the unverified prefix whose segments can pass through geometry. An absolute pullback keeps the emitter the same depth behind an opening at any source distance, and the point always lies on the traced acoustic path. This replaced a percentage lerp between source and centroid, which scaled with distance and cut straight through the geometry the path bends around.

Cluster grouping still uses the edge points themselves, because pulled-back points converge toward the source and would wrongly merge distinct openings. Only the output centroid averages pulled-back points, and the gain inputs `PathDist` and `TotalWeight` are unaffected.

---

## Edge cache

`TickCachedEdgeEviction` in `EdgeCache.cpp` runs every frame regardless of sweep state, managing `CachedEdgePoints`. Phase 0 traces target `EffectivePoint()`, which is the relay point while relayed and the edge otherwise.

### Per-edge phases, in order

`TickSingleEdge` runs them in sequence: `TickEvictionFade`, `TickPhase0Readback`, `TickPhase0OffsetReadback`, `TickMovementThresholdEviction`, `TickRelayMaintenance`, `TickPhase0Submission`. What the names do not say:

**A blocked Phase 0 trace does not evict.** It first tries `TryPromoteToInnerAnchor`: if the previous point on the edge's own `ShortestPath`, one step back toward the source, already has direct unobstructed listener LoS, the edge shrinks back to that point. That is strictly better, since no diffraction is needed for what is now a direct leg, and `PathDist` is corrected by the trimmed segment's straight-line length. Only if that fails does it fan out to four listener offset points at `DirectLoSSampleRadius`, and only if all of those are blocked does it try a relay rescue and then evict. The fan fires only on a blocked centre, so it costs nothing while the edge is comfortably visible.

**Restoring after a clear trace only applies to listener-side evictions.** Source-side ones, flagged `bSourceSideEviction` and caused by the movement threshold or the shortest-path recheck, cannot be revalidated by a listener-to-edge trace, which is typically still clear in both cases. Only a sweep rewriting the entry rehabilitates them.

**Listener movement never evicts anything.** Only source movement past `CachedEdgeUpdateMoveThreshold` does. Listener-side validity is entirely Phase 0's job.

### Per-cache phases

**TickShortestPathRecheck and TickShortestPathReadback**, on `ShortestPathRecheckInterval` with 0 for off. A round-robin re-trace of every segment of one edge's stored polyline per interval, with endpoint pull-in by `RaySurfaceBias` so corner grazing does not misfire. All segment traces, forward and reverse each, are submitted async up front into `Component.PathRecheck` and evaluated by the readback the following tick. The checked entry is re-found by exact `EdgePoint` match at readback, so a sweep rewrite, promotion or eviction in between drops the stale result instead of evicting the wrong entry.

This deliberately includes unverified segments, meaning raw crawl or bounce hops the string pull could not shortcut past. Those were already blocked at discovery, so enabling this setting also evicts ordinary multi-corner diffraction paths the moment they are rechecked, not only genuine geometry changes. It is the only guard on the source side of a cached path: Phase 0 watches the listener leg, movement eviction watches source position, and rank hysteresis discards the worse-ranking re-finds a closed path produces. Any single blocked segment triggers an immediate source-side eviction fade and sweep request, flagged so Phase 0's clear-restore cannot resurrect it, since the listener leg is usually still clear when the source leg closes and the restore would outpace the fade every interval.

**TickInnerAnchorPromotion**, on `ShortestPathPromotionInterval` with 0 for off. Round-robin over one non-relayed edge per interval with its own cursor, independent of the recheck's, trying `TryPromoteToInnerAnchor` regardless of the edge's current LoS state. Unlike the promotion inside `TickPhase0Readback`, which only fires as a rescue the instant an edge goes blocked, this runs opportunistically, so an edge with clear LoS since discovery still migrates toward the true minimal diffraction point as the listener moves further past the corner. One step per interval, not a jump to the source.

This path alone enables sub-segment refinement, via `bAllowSubSegmentRefine`. When the previous vertex is blocked, a binary search along the final segment brackets the LoS transition, which is the actual geometric corner, and moves the edge there. `ShortestPathPromotionBisectSteps` at 0 derives the step count from the segment so the corner lands inside half `CachedEdgeMergeRadius`; a fixed count pins per-check cost at two traces per step but risks leaving the edge short of the corner on a long segment, stranding near-duplicates just outside the merge radius. Guards: verified final segments only, since unverified hops hug geometry; every accepted point traced clear forward and reverse; a minimum-move epsilon of `max(4×RaySurfaceBias, 4cm)` against stationary jitter; `PathDist` trimmed by the exact straight-line cut with the polyline's last vertex moving with the edge, so there is no pop. The rescue call site passes false, since it re-fires every Phase 0 interval while blocked and the bisection traces would recur in pinhole states.

**MergeCoincidentEdges**, last. Collapses entries that have drifted within `CachedEdgeMergeRadius` of each other. The sweep's own merge radius gates only admission, and nothing else compares cached entries against each other, yet relay maintenance and promotion both move an admitted entry's point.

Relay conversion is what actually piles them up, because it is inherently simultaneous: several edges bending around the same physical corner lose LoS on the same tick, each converts along its own edge-to-relay leg, and those legs all terminate at that one corner. Duplicates are not free. Each costs a cache slot, one `SubstituteCount` against the sweep ray budget, one exclusion direction so sweeps under-search a region holding a single corner, and, since `Math::ClusterEdgePoints` sums member weights into `Cluster.TotalWeight`, a larger share of the voice mix than a genuinely distinct opening gets.

The survivor is the shortest `EffectivePathDist()`. At one point the listener leg is shared and the rank score's listener term cancels, so travelled distance is all that separates them. Bounce count deliberately does not enter, unlike the sweep's `OutranksIncumbent`, which compares candidates at different positions. Relayed entries are excluded and the test is on `EdgePoint`, never `EffectivePoint()`: relays rescued through the same clear fan point share a `RelayPoint` while their edges are distinct corners, so keying on the presented point would delete real edges, the same failure mode as the removed relay-yield rule. A converted relay clears `bRelayed` and is picked up on the next tick regardless.

### Relay rescue and conversion

`TryRelayRescue` runs on total listener LoS loss, before evicting. The edge tries to survive routed through `LastLoSListenerPos`, the most recent listener position whose Phase 0 saw it, or the clear fan point when only the fan saw it. If both legs, edge to relay and relay to current listener, verify synchronously with reverse hygiene, the edge stays alive with `bRelayed` set. `RelayPoint` and `RelayDist` are frozen at rescue time so the extended path stays listener-independent. There is one relay level: a relayed edge whose relay goes dark evicts normally.

Rescue depends only on the edge's own geometry. Both the yield rule and the older "skip while any direct edge exists" gate are gone, because each made an edge's survival depend on sibling state. With N edges going dark on the same tick that meant a processing-order lottery, and once the first relay converted into a direct edge it permanently locked out every remaining rescue. Eight simultaneous losses produced one converted edge and seven deaths. Now N losses produce N relays and each converts independently.

`TickRelayMaintenance` runs while relayed. Once per Phase 0 interval it submits four async traces, listener to edge forward and reverse and edge to relay forward and reverse, with handles on `RelayCheckHandles` and `bRelayCheckPending`, and evaluates them the following tick. It un-relays if direct listener-to-edge LoS returned, so the voice snaps back to the true edge and its shorter path, and drops the relay and evicts if the edge-to-relay leg was severed by dynamic geometry.

When the readback instead confirms the relay still valid, it converts the relay into a real edge. The LoS transition along the verified edge-to-relay leg is the actual second corner the relay bends around, so the entry is upgraded in place: the corner is appended to the polyline as a verified sub-segment of the traced-clear leg, `PathDist` is extended by the exact straight piece, `LoSBounces` gains one, and relay state clears. The entry becomes a first-class cached edge again, with standard maintenance, ray substitution and direction exclusion all applying, which makes the relay roughly a one-interval transitional state rather than a resting one.

`BisectListenerLoS` derives its step count from the bracket length so the corner lands within half `CachedEdgeMergeRadius` however long the leg is. At a fixed five steps a 20 metre leg resolved to only about 60 centimetres, so relays converging on one corner from different legs each landed in their own 60 centimetre window and stayed permanently unmergeable. When no midpoint traces clear, the fallback is the relay point itself, which has the same acoustics, and promotion refinement walks it back toward the true corner on later intervals since the appended segment is exactly the bracket it bisects.

## How results reach the audio components

`UpdateAudioParameters` and `UpdateDualModeAudio` in `UpdaterAudio.cpp`, called at the very end of every `TickComponent`. By then `CurrentOcclusion` has been smoothed toward `TargetOcclusion` and `CurrentVirtualSourceLocation` interpolated toward its target.

**The source components.** Every component tagged `AudioComponentSource`, since an object can carry several co-located sounds all writing the shared diffraction bus, plus any live `PlaySoundThroughSpatialBus` one-shots, receives `CurvedOcclusion` through `OcclusionParamName`. Each MetaSound graph shapes its own volume and filtering from it continuously, with no external crossfade. Finished one-shots auto-destroy and their stale entries are pruned in the same loop. `ReadAttenuationSettings` uses the widest-range source for `AttenuationInnerRadius` and for the ray range, which is that source's audible range times `MaxRayDistanceScale`.

**The virtual crossfade gate**, running `ComputeVirtualCrossfadeRamp` into a low-pass into `ComputeVirtualCrossfadeTarget` into `ComputeVirtualCrossfadeSlew`. With `VirtualCrossfadeStartOcclusion` below 1, an occlusion-keyed ramp fades the virtual in through the band between that threshold and full occlusion, which is the pinhole and pre-sweep state, keyed to the same smoothed `CurrentOcclusion` the source's muffling follows. The ramp is low-passed over `VirtualCrossfadeSmoothingTime` because its band mapping amplifies occlusion-sampling wobble by `1/(1-Start)`.

A completed blank ring cycle forces the target to 1, meaning the full rotation found nothing rather than one blocked sample. The exception is a stationary scene with the ramp enabled, where the hard term is suppressed, because a marginal pinhole can blank a full rotation and would pump the gate. The ramp still reaches 1 as smoothed occlusion does. Any clear sample resets the streak so the gate releases instantly on regain. At the default start of 1 the ramp is off and the gate is the original hard on and off. The result is slewed by `VirtualCrossfadeFadeInTime` and `FadeOutTime`.

**Per-slot values.** `VirtualGain` is `VAP.VirtualGain × WeightShare × slot fade envelope × crossfade gate`. `VirtualPathBend` is the detour ratio, travelled over straight minus one, saturating at `VirtualPathBendFullExcess`, plus a travelled-distance term of `VirtualPathBendDistanceStrength × traveled / MaxRayDistance` with default strength 0. That term makes far-away clusters sound duller even on straight single-corner paths, an air-absorption analogue of `PathAttenuation` on the same Leg1 basis, listener-independent. The MetaSound derives HPF and reverb from the single bend value internally.

### Curve-shaped path attenuation

`ComputePathAttenuationCurved` evaluates the blended Leg1 distance against the virtual template's attenuation curve, which is the exact curve the engine applies to the pooled emitters' own listener leg. Both legs therefore stay coherent when the virtual and source assets differ, including the inner-radius hold and the asset's falloff model, captured in `ReadAttenuationSettings` after overrides with the widest source as fallback. One minus the resulting volume, scaled by `PathAttenuationStrength`, becomes the attenuation.

This makes the source-to-emitter leg cost what the engine charges for the same distance on the emitter-to-listener leg. With the old flat linear ramp, an emitter close to the listener with a long acoustic path behind it out-shouted a farther emitter with a shorter path, because the native curve restarted at full volume at the emitter while Leg1 was barely penalised.

The blend toward `Leg1Geom` through `PathAttenuationGeomBlend`, 0 for pure travelled and the default, is unchanged and applied to the distance before curve evaluation. `Leg1Geom` is computed per call site from that site's own source and virtual-position pair: the per-frame cache-weighted path in `UpdaterCast.cpp`, the cluster target in `BuildDesiredVoices`, and the sweep readback. The existing `FInterpTo` smoothing toward the target is untouched. When no attenuation asset can be found, `EvaluateVirtualAttenuationVolumeAt` falls back to a linear ramp over the ray range.

All of these are written to `AudioDiag` for the debug HUD and sent through `SetFloatParameter` to the `UAudioComponent`s.

---

## Key state groups on the component

Every group is documented at its declaration in `SpatialAudioComponent.h`; read the private section top to bottom. The distinction that is not local to any one of them: `Target*` members are what the casts write and `Current*` members are what `TickComponent` smooths toward them, so nothing audible ever snaps. `AudioDiag` and `TraceDiag` are debug-only and no audio path reads them.

---

## The ray physics helpers

`CrawlSurfaceToEdge` and `ProcessRayHit` in `RayPhysics.cpp` are shared by the async cast and the sync LoS-break sweep, so reading them once explains both. `ReadingGuide.md` Stop 7 covers what they do. The one number worth knowing is the crawl range: `MaxCrawlSteps × CrawlStepSize`, capped further by `MaxStraightFlightDistance` when set, with a minimum of one step. A crawl that exhausts it without finding an edge bounces off the wall from the original hit point.

---

## The NPC voice layer

`UNPCVoiceComponent` in `Voice/` consumes the acoustic state above rather than being part of the pipeline. It never traces, and nothing in `Audio/` reads from it.

The layer split mirrors `Audio/`'s own. `Voice/NPCVoiceLogic.h` holds every scheduling decision as pure functions over explicit state, in the `VoiceLogic` namespace, following the same convention as `Math.h`. `Voice/NPCVoiceTypes.h` holds the bank row, the resolved runtime line, and the three scheduler state structs those functions mutate: `FNPCVoiceBucketHysteresis`, `FNPCVoicePlaybackState` and `FNPCVoiceTransitionState`. The component keeps only engine wiring, resolving sibling components, loading the DataTable, and calling `Play`, `FadeOut`, `SetWaveParameter` and `SetAttenuationFalloffScale`. Every decision is therefore unit-testable without a component, world or audio device, under `SpatialAudioRay.Voice.*`.

### Effort from effective acoustic distance

Per tick the layer maps effective acoustic distance to a vocal-effort bucket, `ENPCVoiceEffort` from whisper to shout, with bands selecting inversely so close is a whisper and far is a shout.

`USpatialAudioComponent::GetEffectiveAcousticDistance` supplies it: the straight line while clear, and while occluded the shortest cached diffraction route, the minimum over non-evicting edges of `EffectivePathDist()` plus the edge-to-listener leg. That is the most favourable path, following the same minimum-not-average rule the occlusion accumulators use. The two are blended by smoothed `CurrentOcclusion` in `Math::ComputeEffectiveAcousticDistance`, so a listener just around a small corner gets at most a small step up while one three rooms deep walks the bands toward shout, and effort steps stay graded even though the occlusion fraction itself swings near-binarily around a corner.

The blend takes a detour floor, and the voice passes `OcclusionShiftThreshold`, the same threshold that calls the listener hidden. Below it, effort follows the straight line exactly. Above it, the route phases in across the remaining range, reaching the full path at total occlusion. Edge caching finds routes well before the source is hidden, since the pre-sweep band pre-warms the cache during partial LoS, and blending those in from occlusion 0 charged the NPC for a detour the sound was not taking. It strained at a listener it could plainly see while simultaneously saying "I can see you" lines. One threshold for both keeps effort and content from disagreeing about whether the detour is real, and the remap means nothing jumps at the crossing. Occlusion still never shifts the bucket directly.

The bucket commits through dwell-time hysteresis, and lines come from a `FNPCVoiceLineRow` DataTable generated by `Tools/VoiceGen/`, with content keyed to the acoustic situation, cooldown groups and no immediate repeats.

`LoadBank` resolves each row's wave and lets the wave overrule the manifest's `Duration`, warning on a mismatch past `DurationMismatchTolerance`, because that field is the scheduler's only end-of-line signal. A re-render that never made it back into the CSV would otherwise truncate every following line or leave dead air, invisible until it ruined a take.

### Choosing what to say

`ResolveCategoryPreference` samples `FNPCVoiceAcousticState`, meaning occlusion, direct distance, effective distance and time since the listener last crossed between visible and hidden, and returns an ordered ladder of `ENPCVoiceCategory`, most specific first:

- `BehindWall`: hidden, physically close, but the route is several times the straight line. The signature diffraction state, and the one where effort and proximity openly disagree since effort follows path length.
- `AroundCorner`: hidden, route barely longer than the straight line.
- `PartiallyOccluded`: still visible, but something is genuinely clipping the line, at `PartialOcclusionThreshold`, far below the hidden threshold. "Can they see me at all" and "is the path unobstructed" are different questions, and the second turns false as soon as one offset sample blocks.
- `LostSight` and `SightRegained`: the moment sight broke or came back. Temporal, so they briefly outrank the spatial contexts.

Selection never crosses between the visible and occluded halves, and each ladder ends in that half's generic entry, `Clear` or `Occluded`. Every line asserts something about the world, so an occluded line played to a listener standing in the open, or a "nothing between us but air" line played to one behind a wall, contradicts what they can see. Silence is the correct failure mode, so the bank must carry a generic `Clear` and `Occluded` line at every effort. Missing specific contexts simply fall through.

There is deliberately no "visible but far" context. For a visible listener the effective distance is essentially the straight line, so the effort bucket already partitions the visible half by distance and a `Clear` line at shout is by construction a distant one. The category that used to do this restated the bucket and carried its own threshold that had to be hand-synced with a band edge.

### One sight signal

Both sight reactions read a pending flag from `VoiceLogic::IsSightReactionPending`, off the crossing stamped by `VoiceLogic::AdvanceSightState`. That is the voice layer's sole sight signal, derived from the same `IsListenerHidden` occlusion threshold content selection uses.

The flag is consumed as well as timed. Playing any `LostSight` or `SightRegained` line settles that crossing through `MarkSightReactionDelivered`, called from `PlayLine` so barge-in and ordinary scheduling both count, and the next crossing re-arms it. A reaction is a one-time statement about an event, not a description of a state, but the window has to stay open long enough to survive an in-flight line finishing. On time alone, the line after a reaction re-announces the same crossing, and since what typically schedules that line is the listener crossing an effort band, it re-announces it in a different voice: "there you are" shouted, then again at raised effort.

It deliberately does not read the spatial component's direct-line-of-sight timers. That flag is the raw instant sample and drops on a single grazing trace while the listener moves, so a visible listener got told "there you are" about a break that was never announced and they never heard. In a pinhole state, occlusion past the threshold while a sliver of direct sight survives, which is exactly the pre-sweep band, it never breaks at all, which pinned `LostSight` at the head of the ladder for as long as the listener stood in the doorway. One predicate and one edge means content and barge-ins cannot disagree about whether a break happened.

### Playback and barge-in

Playback injects the line's wave into the owner's voice `UAudioComponent`, tagged both `AudioComponentSource` so the pipeline above feeds it bus and occlusion, and `NPCVoiceAudio`, through `SetWaveParameter` before `Play`. A voice line therefore diffracts and muffles exactly like any other co-located sound.

A playing line is never modified mid-flight, and bucket changes apply to the next line, with one exception. `TickSightReaction` handles three moments worth reacting to, ranked by `EvaluateBargeIn`: direct line of sight breaking, sight returning, and the committed bucket drifting at least `TransitionBucketDelta` steps from the bucket the playing line started at. The line is cut with a short declick fade, and one tick later a line from the reason's category fires through `FindBargeInLine`, which prefers the exact target bucket then the nearest rendered one, followed after `PostTransitionLineDelay` by a full line at the new effort.

Visibility outranks effort drift deliberately. Losing sight inflates the acoustic path, which climbs the effort bands, so both trigger on the same tick, and ranking drift first would report a listener "moving away" who only stepped behind a wall.

Guards: a per-barge-in cooldown, a minimum remaining line time, and barge-ins never interrupting barge-ins, tracked by `bActiveIsBargeIn` set from how a line was scheduled rather than its category, since `LostSight` rows serve as both ordinary lines and interruptions. Per-reason content availability comes from `ResolveBargeInAvailability`, cached at load, and each trigger is gated on its own replacement category, so a reason the bank cannot service steps aside for one it can instead of claiming the tick and aborting. A bank with none of the three keeps the plain wait-for-line-end behaviour.

A sight change with nothing playing has no line to interrupt, so the reaction comes from `PullInNextLine` instead. The normal `LineIntervalMin` to `Max` silence easily outlasts `SightChangeReactionWindow`, and without this the NPC would sit through the break and then never mention it, having moved on to describing the new state.

### Reach and gain

Effort sets audible reach, derived from the same distance bands that select it rather than authored separately. An effort exists to be heard across its own band, so a whisper only has to carry as far as the distance at which the NPC would escalate to conversational anyway. `GetEffortReachDistance` returns the bucket's band maximum times `EffortReachHeadroom`, and each `PlayLine` passes it to `USpatialAudioComponent::SetAttenuationOuterRadius`, which converts it to a `FalloffDistance` scale through `Math::ComputeFalloffScaleForOuterRadius`, applied to the source components and the live virtual pool.

Headroom above 1 keeps an effort audible slightly past its band edge, because the bucket only commits after `BucketDwellTime` and a line in flight finishes at its starting effort, so the listener is regularly a little past the boundary while an older effort is still speaking. Two clamps bound the result: the attenuation's inner radius is a hard floor, since volume is full inside it, and the asset's own range is the ceiling, because the captured ray and LoS ranges stay at base scale and a sound audible past them would have no diffraction paths to play through. Shout has no band maximum and defers to the asset's authored range, being the anchor everything else scales down from.

Reach alone cannot carry the contrast, because two falloff curves ending near each other differ by only a decibel or two where they overlap, which made crossing a band boundary inaudible. So effort also sets source gain: `GetEffortGainDb` feeds the voice MetaSound's `EffortGainDb` input at each line start, once per line since a line's effort never changes mid-flight. This is what the bank's single LUFS target was for. Normalising the renders strips the TTS's accidental loudness variation so deliberate variation can be applied in-engine. Reach and gain are orthogonal and both physical: a shout is louder and carries further.

The gain must be applied inside the MetaSound graph, ahead of the Audio Bus Writer. In `MS_Voice` that is Wave Player into a multiply by `DecibelsToLinearGain(EffortGainDb)` into Plate Reverb, because the graph splits after the reverb: one branch filters into the bus for the virtual emitters, the other applies the occlusion gain and exits as `Out Mono`. A component-level `SetVolumeMultiplier` sits outside the graph, so it would reach only direct playback and leave occluded playback at the wrong level, and `FUpdater::ApplySourceOcclusionParams` rewrites that multiplier to 1 every frame anyway. Shout anchors at 0 dB with everything scaling down, since the sources share one mixing bus and boosting above 0 risks clipping it.

Tunables live in `UNPCVoiceSettings`, following the same asset-or-CDO pattern as `USpatialAudioSettings`.
