# ADR-0013: 3-D (volume) textures

- Status: Accepted
- Date: 2026-06-27

## Context

[ADR-0010](0010-textures-and-descriptors.md) added 2-D textures, samplers, and a one-entry
combined-image-sampler descriptor — enough for a textured quad and the lit mesh. The ICEM viewer's
next brick (C1) shows a **computed simulation field** — a temperature, a displacement, a velocity —
as a colormap on the part and on the cross-section. ICEM exports that field as a **dense regular grid
of node values** (`.icef`; the sparse FEM hex mesh re-expressed on its lattice). The natural GPU home
for a regular 3-D grid of values is a **3-D texture**: the shader samples it at a fragment's world
position with hardware **trilinear** interpolation, which is exactly the field-at-a-point query a
surface colormap (and later a volume raymarch / isosurface, C2) needs.

The RHI has no notion of a third texture dimension: `TextureDesc` carries only `Extent2D`, and the
Vulkan backend hard-codes `VK_IMAGE_TYPE_2D` / `VK_IMAGE_VIEW_TYPE_2D` and a depth-1 copy region.

## Decision

**Add volume textures as a minimal extension of the existing texture path — one new field.**

- **`TextureDesc::depth` (default 1).** `depth == 1` is the existing 2-D image, bit-for-bit unchanged;
  `depth > 1` makes a `width × height × depth` 3-D image. No new descriptor type, no new enum — a 3-D
  image is bound through the *same* combined-image-sampler (`GraphicsPipelineDesc::sampled_texture`);
  the shader simply declares `sampler3D` instead of `sampler2D`, and the bound image's view type
  matches at draw time.
- **Backend:** when `depth > 1`, `create_texture` uses `VK_IMAGE_TYPE_3D` and `VK_IMAGE_VIEW_TYPE_3D`
  and sets the image extent's depth; `write_texture` copies a region covering the full `w×h×d` extent
  (the staged-upload path is otherwise identical). The texture remembers its depth so the copy and any
  later transition use it.
- **Sampler reuse.** Volumes need clamp-to-edge addressing on all three axes and linear filtering —
  both already expressed by `SamplerDesc` (`AddressMode::ClampToEdge`, `Filter::Linear`), and the
  backend already sets `addressModeW`. Nothing to add.
- **Format.** `Format::RGBA32Float` (already mapped to `VK_FORMAT_R32G32B32A32_SFLOAT`) carries a scalar
  field in R plus a validity flag in G with no quantization — the viewer's first field volume.

## Consequences

**Good**
- C1 can upload a field once and trilinear-sample it per fragment; C2's raymarched isosurface / DVR and
  C3's vector-field glyphs reuse the same volume with no further RHI change.
- Purely additive: `depth = 1` is the default, so every existing 2-D texture (offscreen targets, the
  quad, the depth buffer) is untouched — the whole RHI suite stays green.
- Proven the M3 way: `tests/rhi/volume_texture_test` uploads a 1×1×2 RGBA32F volume (red slice, green
  slice) and samples it down the **w axis** across a full-screen quad, asserting the top reads red and
  the bottom green — i.e. the 3-D image, 3-D view, depth-aware upload, and `sampler3D` all worked.
  Off-screen + readback, GPU-free on lavapipe in CI.

**Costs we accept**
- **No mipmaps / no array textures.** A single mip and a true 3-D image (not a 2-D array) are all the
  field viewer needs; mip generation and image arrays are added when a real workload wants them.
- **Whole-image upload only.** `write_texture` still replaces the entire image (no sub-region / streamed
  bricks). Fine for a field that is loaded once; partial updates wait for the transfer path / render graph.
- **`RGBA32Float` linear filtering** is relied upon. It is universally supported on desktop and on
  lavapipe; if a future target lacks linear-filterable 32-bit float, the field can fall back to a
  16-bit float volume (a format addition, not an architecture change).

## Alternatives considered

- **A 2-D texture atlas of z-slices.** Avoids a new image type but forces manual slice addressing and
  manual trilinear blending in the shader (sampling two slices and lerping) — more shader complexity and
  no hardware trilinear, for no real saving. Rejected; native 3-D images are the right primitive.
- **A storage buffer of grid values, indexed in the shader.** Works, but throws away free hardware
  filtering and clamp addressing and bloats the shader with manual interpolation. A sampled volume is
  simpler and faster. Rejected (a buffer path may still appear later for unstructured fields).
