# SpatialAudioRay

Real-time acoustic diffraction and occlusion for Unreal Engine 5, written in C++.

Rays are cast from a sound source and bounce and crawl around geometry until one of them finds line of sight to the listener. Wherever a ray broke free is a diffraction edge, usually a door frame or a wall corner, and the plugin moves a real audio component there.

So when you walk behind a wall you hear two things at once. The source is still where it always was, muffled by however much geometry is between you and it. On top of that you hear it arriving around every corner it found a way through, each from its own direction. How those two balance shifts as you move. With the direct line fully blocked you are hearing only the paths around the geometry.

The NPC voice system on top of it reads how far the sound actually has to travel instead of the straight line distance, and picks a vocal effort from that. Standing two steps from an NPC but around a corner makes it shout.

The lines are generated offline by `Tools/VoiceGen/`, a local text-to-speech pipeline that renders each one at four vocal efforts from a single reference recording per effort. No text tag tells the model to whisper or shout. The performance comes entirely from which reference conditions it, and the runtime chooses between them from the acoustic distance.

| Diffraction | NPC voice |
|---|---|
| [![Real-time audio diffraction](https://img.youtube.com/vi/FY_Q5QJGMjQ/mqdefault.jpg)](https://www.youtube.com/watch?v=FY_Q5QJGMjQ) | [![NPC voice driven by diffraction](https://img.youtube.com/vi/0SEGsmSWudY/mqdefault.jpg)](https://www.youtube.com/watch?v=0SEGsmSWudY) |

## How it works

Three loops run at different rates:

| Loop | Rate | Job |
|---|---|---|
| Direct line-of-sight sampling | on a timed interval | produces the occlusion value, smoothed every frame |
| Full async sweep | on a timed interval, while heavily occluded or sight is confirmed lost | finds diffraction edges, a ray budget spread over several frames |
| Edge cache maintenance | one edge at a time, each on a timed interval, while sight is lost | re-validates edges and fades out the dead |

A sweep takes several frames, so by the time it lands its answer is slightly out of date. That is why occlusion is sampled on its own short interval and never waits for a sweep, and why found edges are cached and kept alive independently instead of being re-derived each time. Both of the lower two stop entirely while any sightline to the source survives, and that bar is lower than it sounds: one clear sample out of the five is enough, so they can be idle at 80% occlusion. Below that, the only thing still tracing is the occlusion sampler. The sweep gets one exception, running through the near-occluded band as well, so the cache is already warm when the last sliver closes. The sampler and the sweep also idle when the listener is out of the source's audible range.

How much a sweep costs and how often it runs both adapt at runtime. The ray budget scales down with distance. A warm cache thins it further: once it is full it can only improve by displacing an entry, so most of a full-budget sweep into it is wasted. All three intervals stretch when nothing is moving and tighten when something is, so a stationary scene settles into cheap upkeep instead of re-surveying itself.

What all three loops produce is numbers handed to MetaSound graphs: one occlusion value for the source, and a gain and a path-bend value for each diffracted emitter. The graphs derive their own filtering and reverb from those, so the shaping lives in the MetaSound rather than in the C++.

## Design notes

The sweep advances one bounce level per frame. Every trace is submitted asynchronously and read back the frame after, so a sweep costs a small amount of work spread over many frames rather than a spike on one.

Most of the complexity is in the edge cache, because the listener keeps moving between sweeps and a cached edge has to be checked continuously from the listener's side. A blocked check does not evict straight away. It escalates:

1. Promote the edge inward along its own path, in case a point closer to the source is now directly visible and the outer corner is obsolete.
2. Fan out four offset points around the listener, since a single centre trace grazing a corner is not enough evidence.
3. Route the edge through the last listener position that could still see it, which buys time while you walk.

Only after all of that does it start fading, and it fades rather than cutting, because a virtual emitter disappearing instantly is audible.

The distance a ray physically travelled is longer than the acoustic path, since crawling along a wall and bouncing adds detours. Before that distance is used for anything audible it gets string-pulled: from the edge, hop back to the furthest recorded waypoint that is directly visible, repeat until reaching the source. Rays also die as soon as travelled distance plus straight-line distance to the listener exceeds the budget, which by the triangle inequality means they provably cannot reach it.

One rule runs through all of it: how loud and how muffled the diffracted sound is depends only on source-to-edge geometry, never on where the listener stands. Listener proximity is the engine's own attenuation doing its job, which works because the emitter really is at the corner. It is an easy rule to break by accident, because a listener distance is usually right there in scope when you reach for one, and each break presents as an unrelated bug somewhere else. That is why it is stated at the top of the reading guide rather than left implied.

## Cost

Measured in the NPC voice test scene with one source, using the plugin's own trace counters, over a session mixing running and standing still, both behind cover and in the open. Tracing averaged **227 traces a second**, about 4 a frame at 60 fps. Split by state, moving cost 327/s and standing still 116/s, so the resting scene runs at roughly a third of the moving one. Peaks reach 1010/s, a single sweep frame, and include the sweep every source fires at level load.

The floor stays low for two reasons. Standing in the open, the sweep and the edge cache are gated off entirely and only the occlusion sampler traces. And the upkeep that does run is bounded by the number of cached edges rather than by scene complexity, so it does not grow with the level.

## Reading the code

Start with [`Source/SpatialAudioRay/Audio/ReadingGuide.md`](Source/SpatialAudioRay/Audio/ReadingGuide.md). It is a nine-stop tour that says what to read in what order, what question each stop answers, and the handful of things the code cannot tell you itself.

```
Source/SpatialAudioRay/
├── Audio/     diffraction and occlusion
├── Voice/     the NPC voice system built on it
└── Tests/     94 automation tests
Tools/VoiceGen/  offline voice bank generation
```

`USpatialAudioComponent` owns all the state. `FAsyncCastManager`, `FUpdater` and `FEdgeCache` hold none of their own and are static functions over it, split by which of the three loops they belong to. `Math.h` and `Voice/NPCVoiceLogic.h` sit below both with the pure functions, no engine or component state in either, and 79 of the 94 tests are on those two files. Every tunable lives in one `UDataAsset` instead of as constants in the code.

## Tests

94 tests, run from Session Frontend, Automation, filtering on `SpatialAudioRay`:

| Suite | Tests | Covers |
|---|---|---|
| `SpatialAudioRay.Math.*` | 48 | reflection, attenuation, clustering, path shortening |
| `SpatialAudioRay.Voice.*` | 31 | efforts, hysteresis, barge-in, content selection |
| `SpatialAudioRay.Async.*` | 13 | sweep accumulation, ray budget, turn determinism |
| `SpatialAudioRay.EdgeCache.*` | 2 | cache merge candidacy |

## Trying it

**[Download the demo build](https://drive.google.com/file/d/1ELhPIuckoraKKf0xNHkx_-oMoyQMAEec/view?usp=sharing)** (Windows, 1 GB). Walk around, listen to a source through walls and around corners, and move between rooms while the NPC talks to hear its effort and its lines follow where you are.

The number keys turn on the debug views in the build: bounce rays, crawl steps, edge points, string-pulled paths, virtual emitters, and per-source and global stats. Rays and paths together, while walking behind a wall, show most of the system at once. Controls are listed in the readme next to the executable.

That is the intended way to hear it. The plugin is built against Unreal Engine 5.7 and the source here is meant for reading rather than for dropping into a project, since a working setup also needs a tuned settings asset and the generated voice bank, neither of which is committed.

## Author

Joel Schultz, audio programmer. [joelschultz.net](https://joelschultz.net)
