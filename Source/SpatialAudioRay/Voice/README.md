# NPC voice layer

An NPC whose vocal effort follows the acoustic state of the diffraction system in `../Audio/`, rather than the straight line to the listener. Standing two steps away behind a wall makes it shout, because the sound has to travel around the corner to reach you.

This layer is a consumer. It never traces anything, and nothing in `Audio/` depends on it. It reads `USpatialAudioComponent::GetEffectiveAcousticDistance`, picks an effort bucket from whisper to shout, and plays lines through the same pipeline as any other sound on the actor, so a voice line diffracts and muffles like everything else.

| File | What lives here |
|---|---|
| `NPCVoiceLogic.h` | Every scheduling decision, as pure functions over explicit state. |
| `NPCVoiceTypes.h` | Bank row, resolved runtime line, and the three scheduler state structs. |
| `NPCVoiceComponent.h/.cpp` | Engine wiring only: sibling components, DataTable loading, playback calls. |
| `NPCVoiceSettings.h` | Tunables, following the same asset-or-CDO pattern as the audio settings. |

The split is what makes the interesting parts testable without a component, world or audio device. Effort hysteresis, content selection, sight-change reactions and barge-in ranking are all covered by the 31 tests in `../Tests/VoiceTests.cpp`, registered as `SpatialAudioRay.Voice.*`.

For how it works in detail, see the voice section of [`../Audio/CodeFlow.md`](../Audio/CodeFlow.md#the-npc-voice-layer). For the system it sits on top of, start at the [repository README](../../../README.md).
