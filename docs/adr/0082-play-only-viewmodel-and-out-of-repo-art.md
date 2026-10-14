# ADR-0082: a play-only first-person viewmodel, with its art outside the repository

- Status: Accepted for the `99-the-block --play` prototype
- Date: 2026-10-15
- Builds on: ADR-0078's ordering of the temporal resolve (the overlay is drawn *before* TAA, not
  composited after it).

## Context

`--play` let the owner walk the block and shoot it, with nothing in frame to shoot it with. A
held weapon is the first piece of content the engine draws that is neither world geometry nor
UI: it is attached to the camera, it must be lit by the same world lighting as everything else,
and it must not be occluded by the geometry the camera is standing inside.

It is also the first content we cannot author ourselves. Usable first-person weapon and arm art
is third-party work. The three models chosen are CC-BY 4.0 and attributed in
`samples/99-the-block/assets/viewmodel/ASSET-LICENSES.md`.

## Decision

**The viewmodel is presentation only.** `PlayViewmodel` owns meshes, materials and texture
uploads fixed at load time; per frame it only writes world matrices into a borrowed `DrawItem`
span. It is not an entity, it is not replicated, and selecting a different weapon changes neither
the replicated weapon nor movement input. The two keys that switch it (`1`, `2`) are a content
switch, not a gameplay one.

**It draws through the first-person seam, before TAA.** `SceneRenderer::set_first_person_draws`
takes the span and an *epoch*. A model that appears, swaps or disappears must not inherit the
previous model's temporal history; the epoch is what lets the resolve reject it. Drawing after
the temporal resolve instead would have been simpler and would have put an un-anti-aliased object
in the middle of an anti-aliased frame. A dedicated depth-combine pass keeps the weapon in front
of the world it is nominally inside.

**The art lives outside the repository.** `.gitignore` in the asset directory keeps the prepared
GLBs and every source download out; what is committed is the attribution, the provenance and the
preparation scripts under `assets/viewmodel/prepare/`. The whole weapon path is therefore
conditional on the art being present: `BLOCK_HAVE_VIEWMODEL_ART` gates the cook target and the
`the_block_viewmodel_smoke` test, `--play` prints one line and runs without hands, and
`--viewmodel-smoke` skips with status 0. When the art *is* present that test runs with
`RIME_REQUIRE_VIEWMODEL=1`, so a skip there is a failure rather than a quiet pass.

The alternative was committing roughly 20 MB of GLB into a repository whose entire history is
24 MB, permanently and unrevertibly without a history rewrite. The owner chose to keep it out.

## Consequences

**CI cannot prove the weapon path.** `--viewmodel-smoke` is a local gate run by someone who has
the art. A change to `PlayViewmodel` or to the first-person render seam gets no automated
coverage of the drawing half; the seam's own tests in `tests/render/first_person_test.cpp` are
what CI actually runs, and they use synthetic draws. This is a real reduction in coverage and is
the price of the decision above.

A clean clone configures, builds, tests and plays — without hands. The `EXISTS` check that gates
the cook is evaluated at configure time, so art added later needs a re-configure.

**Textures come from the images embedded in the downloaded GLBs, never from Sketchfab's public
media URLs.** Those served images decode as valid PNGs at their advertised dimensions, but their
pixels are sheared about 45 degrees — measured on every texture of all three models. An earlier
preparation pass embedded them and produced a checkerboarded sleeve and blown-out skin that
looked like a shader bug. The embedded images are 1024x1024 and clean. Nothing is upscaled;
chasing higher-resolution "originals" is what caused the bug.
