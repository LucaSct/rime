# rime::audio — the audio seam (M8.4)

The smallest audio module that does one useful thing: give the rest of the engine a **stable boundary
to call** — `play(sound, position, gain)` — while the real mixer is still unwritten. It ships one
interface and one backend.

## The idea

- **`AudioBackend`** — the interface. One virtual call, `play(SoundId, position, gain)`: a
  fire-and-forget one-shot at a world position. Virtual dispatch is deliberate and free here — audio
  events are sparse (an impact, a footstep), not a per-vertex hot loop.
- **`NullAudioBackend`** — the v1 backend. Instead of making sound it **logs** each `play()`, so audio
  is testable and demoable with no device: a headless test (and the M8.6 sample's self-check) asserts
  a break played the right sounds, here, this loud.

The point of introducing the interface now — before there is a mixer — is the **swap**: the real
spatializing/mixing backend (track **au1**) replaces `NullAudioBackend` without touching a single call
site. M8.4's destruction fan-out is the first caller (a `PartDied` → `play(impact, …)`).

Removable feature module (guardrail 2): it depends on `rime::core` only (for `Vec3`), and nothing
depends on it — the engine builds with it gone.

## Building & testing

Built as part of the engine (`scripts/build.sh`). The test is pure-CPU and runs on every CI OS plus
ASan/UBSan and TSan:

```bash
ctest --preset dev -R rime_audio_tests
```
