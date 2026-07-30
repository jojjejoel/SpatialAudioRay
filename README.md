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

## Trying it

**[Download the demo build](https://drive.google.com/file/d/1ELhPIuckoraKKf0xNHkx_-oMoyQMAEec/view?usp=sharing)** (Windows, 1 GB). Walk around, listen to a source through walls and around corners, and move between rooms while the NPC talks to hear its effort and its lines follow where you are.

The number keys turn on the debug views in the build: bounce rays, crawl steps, edge points, string-pulled paths, virtual emitters, and per-source and global stats. Rays and paths together, while walking behind a wall, show most of the system at once. Controls are listed in the readme next to the executable.

That is the intended way to hear it. The plugin is built against Unreal Engine 5.7 and the source here is meant for reading rather than for dropping into a project, since a working setup also needs a tuned settings asset and the generated voice bank, neither of which is committed.

## Author

Joel Schultz, audio programmer. [joelschultz.net](https://joelschultz.net)
