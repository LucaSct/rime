# samples/ — example projects

Small, focused projects built **on** Rime. Samples have two jobs:

1. **Prove the engine works** end-to-end (they're the most honest integration test).
2. **Teach** — each sample shows how to use one slice of the engine, with commentary.

## Planned samples (roughly in build order)

| Sample | Demonstrates |
| --- | --- |
| `00-hello-window` | open a window, run the main loop, clear the screen via the RHI |
| `01-hello-triangle` | the classic first triangle — pipelines, buffers, a draw call |
| `02-ecs-playground` | spawn entities/components, run systems in parallel |
| `03-render-graph` | a multi-pass frame described as a render graph |
| `04-first-light` | PBR + a dynamic light through the renderer |
| `10-destructible-wall` | a part-based wall that fractures, spawns debris, emits VFX/sound |
| `99-the-block` | **the vision demo**: a destructible urban block — destruction + dynamic GI + many lights together |

Samples are added as the features they exercise come online. `99-the-block` is the
"vertical slice" of the dream described in [../VISION.md](../VISION.md) — when it runs
and looks/feels right, the core thesis is proven.

> The numbers above are the *planned* milestone ladder; the on-disk samples track the
> features actually built so far (`00-hello-window`, `01-hello-triangle`,
> `02-textured-quad`). One extra app rides along outside that ladder:
>
> | Sample | Demonstrates |
> | --- | --- |
> | `03-icem-viewer` | load a computed part (binary STL) and draw it lit + depth-correct, orbitable; the seed of the ICEM 3-D viewer (depth + camera + push-constant MVP + a studio shade). `--offscreen` renders one frame to a PPM. |
> | `codec_bench` | **benchmark** (Track S / S0.3): encode representative frames with each streaming codec (raw/LZ4/JPEG) and print ratio, throughput, wire bandwidth, and JPEG PSNR — the measurement behind [ADR-0017](../docs/adr/0017-streaming-codec.md). GPU-free: `codec_bench [w h] [frames]`. |
> | `04-remote-view` | **Track S (S0.5)**: the remote-view endpoint. `remote_view server` renders a scene off-screen and streams it (tap → codec → protocol) while applying input a client sends back; `remote_view client --headless` connects, scripts input, and receives/decodes frames. The *windowed* client (present in a real window) is the display-dependent piece still to come. |
> | `05-ecs-playground` | **M4.6 proof** (M4's "done when"): 200k entities stepped **in parallel** through the ECS — a compute-bound integrate kernel run via `Query::par_for_each` and a `System` `Schedule`, timed against a serial baseline (≈10× on 16 cores, Release) and verified bit-for-bit — plus a **transform hierarchy** (tank: hull → turret → barrel → muzzle) composing `world = parent·local` correctly. CPU-only, self-checking (non-zero exit on failure). Run `ecs_playground`. |
