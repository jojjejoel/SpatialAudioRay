# SpatialAudioRay

Real-time acoustic diffraction and occlusion for Unreal Engine 5.

Rays are cast from a sound source and bounce and crawl around geometry until one of them finds line of sight to the listener. Wherever a ray broke free is a diffraction edge, usually a door frame or a wall corner, and the plugin moves a real audio component there.

So when you walk behind a wall you hear two things at once. The source is still where it always was, muffled by however much geometry is between you and it. On top of that you hear it arriving around every corner it found a way through, each from its own direction. How those balance shifts as you move, and with the direct line fully blocked you are hearing only the paths around the geometry.

The NPC voice system on top of it reads how far the sound actually has to travel instead of the straight line distance, and picks a vocal effort from that. Standing two steps from an NPC but around a corner makes it shout.

| Diffraction | NPC voice |
|---|---|
| [![Real-time audio diffraction](https://img.youtube.com/vi/FY_Q5QJGMjQ/mqdefault.jpg)](https://www.youtube.com/watch?v=FY_Q5QJGMjQ) | [![NPC voice driven by diffraction](https://img.youtube.com/vi/0SEGsmSWudY/mqdefault.jpg)](https://www.youtube.com/watch?v=0SEGsmSWudY) |

## How it works

Three loops run at different rates:

| Loop | Rate | Job |
|---|---|---|
| Direct line-of-sight sampling | every frame | a handful of traces, produces the occlusion value |
| Full async sweep | adaptive interval | a ray budget spread over several frames, finds diffraction edges |
| Edge cache maintenance | every frame | keeps found edges validated between sweeps |

A sweep takes several frames, so by the time it lands its answer is slightly out of date. That is why occlusion is sampled separately every frame and never waits for a sweep, and why found edges are cached and kept alive independently instead of being re-derived each time.

How much a sweep costs and how often it runs are both tunable and both adapt at runtime. The ray budget scales down with distance, shrinks further when cached edges already cover a direction, and can be split across several frames' worth of cycles. The interval stretches when nothing is moving and tightens when something is, so a stationary scene settles into cheap upkeep instead of re-surveying itself.

## Design notes

The sweep advances one bounce level per frame. Every trace is submitted asynchronously and read back the frame after, so a sweep costs a small amount of work spread over many frames rather than a spike on one.

Most of the complexity is in the edge cache, because the listener keeps moving between sweeps and a cached edge has to be checked continuously from the listener's side. A blocked check does not evict straight away. First it tries promoting the edge inward along its own path, in case a point closer to the source is now directly visible and the outer corner is obsolete. Then it fans out four offset points around the listener, since a single centre trace grazing a corner is not enough evidence. Then it tries routing the edge through the last listener position that could still see it, which buys time while you walk. Only after all of that does it start fading, and it fades rather than cutting, because a virtual emitter disappearing instantly is audible.

The distance a ray physically travelled is longer than the acoustic path, since crawling along a wall and bouncing adds detours. Before that distance is used for anything audible it gets string-pulled: from the edge, hop back to the furthest recorded waypoint that is directly visible, repeat until reaching the source. Rays also die as soon as travelled distance plus straight-line distance to the listener exceeds the budget, which by the triangle inequality means they provably cannot reach it.

One rule runs through all of it: how loud and how muffled the diffracted sound is depends only on source-to-edge geometry, never on where the listener stands. Listener proximity is the engine's own attenuation doing its job, which works because the emitter really is at the corner. This got violated six separate times during development, each looking like an unrelated bug until it was traced back, which is why it is now written at the top of the reading guide.

## Reading the code

Start with [`Source/SpatialAudioRay/Audio/ReadingGuide.md`](Source/SpatialAudioRay/Audio/ReadingGuide.md). It is a nine-stop tour that says what to read in what order and what question each stop answers. [`CodeFlow.md`](Source/SpatialAudioRay/Audio/CodeFlow.md) next to it is the per-system reference for when you need the detail.

```
Source/SpatialAudioRay/
├── Audio/     diffraction and occlusion
├── Voice/     the NPC voice system built on it
└── Tests/     101 automation tests
Tools/VoiceGen/  offline voice bank generation
```

`Math.h` and `Voice/NPCVoiceLogic.h` hold the pure functions, with no engine or component state in either.

## Tests

101 tests, run from Session Frontend, Automation, filtering on `SpatialAudioRay`:

| Suite | Tests | Covers |
|---|---|---|
| `SpatialAudioRay.Math.*` | 54 | reflection, attenuation, clustering, path shortening |
| `SpatialAudioRay.Voice.*` | 31 | effort buckets, hysteresis, barge-in, content selection |
| `SpatialAudioRay.Async.*` | 16 | sweep accumulation and miss-direction state |

## Using it

Built against Unreal Engine 5.7. Copy the plugin into a project's `Plugins/` folder and rebuild.

Add a `USpatialAudioComponent` to an actor and tag the audio components you want it to drive with `AudioComponentSource`. One component serves every co-located sound on its actor, so they share a ray budget and an edge cache rather than each running their own.

Every tunable lives in a single `USpatialAudioSettings` data asset, so a whole project is tuned from one place rather than per source. Tuning is specific to a project's scale and geometry, so none is shipped here. Create your own and assign it to each component. Until you do, the class defaults in `SpatialAudioSettings.h` apply, and those are starting points rather than the tuning used in the videos above.

Set `bDrawDebugRays` for the debug views. Number keys toggle sub-modes at runtime: bounce rays, crawl steps, edge points, string-pulled paths, virtual emitters, and per-source and global stats. Turning on rays and paths and then walking behind a wall shows most of the system at once.

## The voice bank

The rendered voice lines are not committed, because they are build output. What produces them is here: the four reference recordings in `Tools/VoiceGen/refs/`, one per effort level, the line list in `lines_showcase.csv`, and `generate_bank.py`, which clones the voice from those references using Chatterbox. There are no text tags telling it to whisper or shout. The performance comes entirely from which reference clip it is conditioned on, which is why the four recordings are tracked and the renders are not.

`export_to_unreal.py` then writes the wavs and a CSV of DataTable rows. Import the wavs to `/SpatialAudioRay/Voice/NPC01/` and the CSV as a DataTable using the `FNPCVoiceLineRow` struct, and the bank is rebuilt.

Everything else under `Content/` is committed, since none of it can be regenerated.

## Author

Joel Schultz, audio programmer. [joelschultz.net](https://joelschultz.net)
