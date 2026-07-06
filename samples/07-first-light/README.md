# 07-first-light — Milestone 5's "done when"

**A lit PBR scene, drawn through the render graph, driven by the application framework.** This is the
sample the whole milestone was building toward: a 6×2 metallic×roughness sphere grid (copper metals
over blue dielectrics, roughness rising left→right) on a mipmapped-checker floor, under one
**orbiting** point light plus a little ambient, viewed with an orbit camera.

It ties every M5 brick together in one binary:

- the **fixed-tick loop** (M5.7, [ADR-0023](../../docs/adr/0023-app-fixed-tick-loop.md)) advances a
  sim system that orbits the light — deterministically, decoupled from the frame rate;
- each frame the render callback draws the world with the **`SceneRenderer`** (M5.6,
  [ADR-0022](../../docs/adr/0022-forward-pbr.md)) — depth pre-pass → Cook-Torrance HDR → tonemap —
  into the app-owned **render graph** (M5.4, [ADR-0019](../../docs/adr/0019-render-graph.md));
- the floor's checker rides the **mipmap/anisotropy** path (M5.3), filtering cleanly into the
  distance;
- and `--serve` streams the result over the **Track S0** stack (S0.2–S0.5).

## Modes

```
build/dev/bin/first_light --headless [--frames 30] [--ppm out.ppm]
build/dev/bin/first_light --serve   [--host 0.0.0.0] [--port 9100] [--codec jpeg|lz4|raw]
```

- **`--headless`** (default) renders off-screen, reads the last frame back, self-checks that the
  scene is genuinely lit, and returns 0/1 — the CI/lavapipe proof (registered as the
  `first_light_headless` ctest). `--ppm` also dumps the image.
- **`--serve`** runs the S0.5 server loop: render → `FrameStreamer` tap → `FrameEncoder` (JPEG on the
  wire, [ADR-0017](../../docs/adr/0017-streaming-codec.md)) → `ProtocolConnection` over TCP, applying
  the **camera-steering input** the client sends back. Point the existing thin client at it:

  ```
  # on the headless GPU box (this dev server):
  first_light --serve --host 0.0.0.0 --port 9100
  # from the viewer (e.g. the Mac):
  remote_view client --host <server> --port 9100 --frames 300 --ppm frame
  ```

  Drag orbits the camera, scroll zooms, a key resets — the same InputEvents 04-remote-view speaks.
  Verified here over loopback: 40 frames of 720p streamed and decoded, the camera responding to the
  client's pointer sweep.
- **windowed** presenting to a swapchain needs a display; that is the
  [ADR-0023](../../docs/adr/0023-app-fixed-tick-loop.md) §4 seam, filled on the Mac build. Here
  `--windowed` falls back to `--headless` and says so.

## Notes

- The scene is ordinary scene-layer content — `MeshRegistry` / `MaterialRegistry` / ECS render
  components — so this file is mostly *scene setup + the three drivers*; the rendering is all engine.
- The light's orbit is **simulation** (a component + a system on the fixed tick), not a frame-time
  animation — turn the frame rate up or down and it traces the same path in the same sim-time.
