# 06-render-graph — the render graph, demonstrated (M5.8)

A small procedural PBR scene — five metallic spheres of rising roughness on a checkered floor under
one point light — drawn through the M5.6 pass library on a `render::RenderGraph`, off-screen. It is
the runnable companion to [ADR-0019](../../docs/adr/0019-render-graph.md) (the graph) and
[ADR-0022](../../docs/adr/0022-forward-pbr.md) (the PBR passes): it shows the graph **compiling a
frame**, **timing each pass**, and — the milestone's headline — how **little** it takes to add one.

```
build/dev/bin/render_graph_sample                 # depth → PBR → tonemap, off-screen, self-checked
build/dev/bin/render_graph_sample --vignette      # + one extra post pass, spliced in
build/dev/bin/render_graph_sample --ppm out.ppm    # also dump the image
```

It prints the compiled pass order and each pass's GPU time, then self-checks that the scene is
genuinely lit (non-zero exit on failure — it doubles as a CI smoke test on lavapipe):

```
06-render-graph: compiled 3 passes:
  [depth-prepass ] 3.556 ms
  [forward-pbr   ] 4.320 ms
  [tonemap       ] 7.532 ms
  self-check: 174680 lit / 2431 bright of 307200 px
```

## "Adding a pass is easy"

That is M5's promise, and this is the proof. With `--vignette`, the frame gains a fourth pass. The
**entire** cost of adding it is one `add_raster_pass` call that names what it reads (`src`) and
writes (`dst`) — the graph re-derives the execution order and inserts the read-after-write barrier
between tonemap and the vignette on its own:

```cpp
RGTexture add_vignette_pass(RenderGraph& graph, const PostPipeline& pp, RGTexture src,
                            rhi::Extent2D extent) {
    const RGTexture dst = graph.create_texture({extent, kLdrFormat, "vignette-out"});
    const RGColorAttachment color{dst, LoadOp::DontCare, StoreOp::Store, {}};
    const RGTexture sampled[] = {src};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = {&color, 1};
    desc.sampled = sampled;                     // ← declaring this read is what orders + barriers it
    graph.add_raster_pass("vignette", desc, [&](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pp.pipeline);
        cmd.bind_texture(0, graph.physical(src), pp.sampler);
        cmd.draw(3);
    });
    graph.export_texture(dst);
    return dst;
}
```

No manual barrier, no touching the other passes, no fixed pipeline order — declare the access and
the graph does the rest. That is the whole point of describing a frame as data.

## Notes

- **Off-screen only.** Windowed presentation is the ADR-0023 §4 seam that `07-first-light` fills on a
  display; this sample is about the graph, so it renders to an image and reads it back — runnable on
  lavapipe and in CI.
- The scene is the same scene-layer + PBR path everything else uses (`MeshRegistry` /
  `MaterialRegistry` / ECS render components → `SceneRenderer`); the sample adds only the demo pass.
