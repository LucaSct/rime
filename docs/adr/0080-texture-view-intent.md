# ADR-0080: a texture declares its VIEW intent, because the layer count cannot infer it

- Status: **Accepted** — shipped in #284; this ADR records the contract, not a proposal
- Date: 2026-10-08

## Context

`rhi::TextureDesc` described a texture's *shape* — extent, mips, `array_layers`, `cube`, `depth` —
and the Vulkan backend derived the sampling view type from that shape alone:

```
layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D
```

That inference is wrong, and not in an edge case. Vulkan requires the descriptor's view type to
match **how the shader declares the binding** (VUID-vkCmdDrawIndexed-viewType-07752), and the two
are independent facts. `pbr_forward_shadowed.frag:62` declares `sampler2DArrayShadow shadow_map` for
*every* cascade count, because one shader serves all of them. The cascade atlas is one layer per
cascade, so `cascade_count = 1` — the low-end preset, not a test shape — produced a one-layer image,
got a plain 2-D view, and was sampled through an arrayed image type. Undefined behaviour.

Two things made it survive. It *worked* on every driver we run (both lavapipe and the RTX 3060
happened to tolerate it), and its only witness was a validation message, logged 13 times across a
full `ctest` run, that nobody was reading. This is the shape of defect the project's verification
rhythm warns about: a proof that cannot see what it skipped reads as passing.

The general class is "a 1-of-N atlas": any texture whose layer count is a *runtime* quantity while
the shader binding's arrayness is a *compile-time* one. Shadow cascades are the instance we hit;
probe capture, VSM and cube-face shadows have the same shape.

## Decision

**The view type is part of the texture's contract, declared by the caller, not inferred from its
shape.** `rhi::TextureDesc` gains:

```cpp
bool array_view = false;   // give me a 2-D-ARRAY view even at one layer
```

and the backend chooses `VK_IMAGE_VIEW_TYPE_2D_ARRAY` when `layers > 1 || desc.array_view`
(`engine/rhi/src/vulkan/resources_vulkan.cpp:157`). `cube` and 3-D volumes are unaffected: their
view types are fixed by their shape, and `array_view` is ignored for them. Per-layer **render** views
stay plain 2-D regardless — this is about the sampling view only.

`render::RGTextureDesc` carries the same field through the render graph, and — load-bearing —
**`array_view` is part of the transient texture cache key**
(`engine/render/src/render_graph.cpp:598`). Without that, an array-viewed one-layer texture could be
handed a pooled plain-2-D one of identical shape and the bug would come back through the allocator
instead of through the descriptor.

The only caller today is the cascade atlas
(`engine/render/src/lighting/shadows.cpp:168`, `/*array_view=*/true`).

### Alternatives considered

- **A `ViewType` enum field instead of a bool.** More general, and it would subsume the existing
  `cube` bool rather than sitting beside it. Rejected *for now*: the honest end states are "all
  shape bools" or "one explicit enum", and a half-migration — an enum beside a surviving `cube`
  bool, with two ways to spell a cube — is worse than either. Recorded here so the eventual
  migration is a deliberate step and not a surprise. The bool is cheap to subsume later; the field
  name says *intent*, so an enum can replace it without re-deciding anything.
- **Two shader variants, one arrayed and one not.** Doubles a pipeline permutation to describe a
  fact about a view, and moves the coupling from a descriptor into the shader build. It also leaves
  the next 1-of-N atlas to rediscover the problem.
- **Always allocate at least two layers for the atlas.** Hides the symptom by making the broken
  inference unreachable in this one caller. Wastes a cascade's worth of depth memory in the preset
  that exists *because* memory is tight, and leaves the RHI contract wrong.

  This one is not hypothetical: **the codebase already did it.** The placeholder bound where a
  shadow type is absent (`engine/render/src/scene_renderer.cpp:266-268`) is declared
  `array_layers = 2` on a 1×1 depth image, with a comment saying the second layer exists only "so it
  takes a 2-D-ARRAY view that satisfies the shadowed pipeline's `sampler2DArrayShadow` bindings".
  So the workaround was invented once already, for the same reason, and nobody noticed it was a
  workaround — because on a 1×1 image the wasted layer costs nothing. That is the strongest evidence
  available that the *inference* was the defect and not the caller: a correct contract would not have
  needed a spare layer to express "sample me as an array".
- **Infer from `usage`.** The RHI cannot know how a shader declares a binding; nothing in `usage`
  carries it.

## What was checked

- `tests/render/shadow_test.cpp:210` renders cascade counts 1, 2 and 4 and asserts the
  `VUID-vkCmdDrawIndexed-viewType-07752` count is **zero**, counted through the existing
  debug-messenger path (`test::VuidCounter`). The validation message became a test failure.
- Falsified by reverting the fix: the case fails with that exact VUID text. A proof that does not
  fail when the defect returns is not a proof.
- `ctest` 82/82 on the merge commit.

## Consequences

- Every future 1-of-N atlas states its view intent at creation and is correct by construction, on
  every driver, rather than by the tolerance of the drivers we happen to test on.
- A validation-layer message is now a failing test for this VUID. The broader lesson is not
  recorded as a decision here, but it stands: validation output that only ever reaches a log is not
  verification. The surrounding 12 occurrences were the same defect, not twelve.
- `TextureDesc` has one more field, and a future `ViewType` enum has one more thing to subsume.
- The placeholder's spare layer (`scene_renderer.cpp:266-268`) is now expressible as intent —
  `array_layers = 1, array_view = true`. Deliberately **not changed** here: it is a 1×1 depth image,
  so the saving is nil and the edit would touch a working path for tidiness alone. Noted so the next
  reader knows it is a survivor of the old contract rather than a second mechanism.
