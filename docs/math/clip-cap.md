# Cross-section: clip plane + solid cap (viewer bricks B2 / B2b)

## What this is

The viewer cuts a part open with a plane so you can see inside it (B2), and fills the **cut face** so the
section reads as sawn-solid metal — painting the computed field on that face (B2b). Two pieces:

1. a **clip plane** that discards the half of the part in front of the cut;
2. a **stencil cap** that fills the cut plane exactly where it passes through the solid.

## 1. The clip plane

A plane is $\{\mathbf p : \mathbf n\cdot\mathbf p = w\}$ with unit normal $\mathbf n$ and signed offset
$w$. The mesh fragment shader discards a fragment whose world position is on the front side,

$$ \mathbf n\cdot\mathbf p > w \;\Rightarrow\; \text{discard}, $$

so the kept geometry is the half-space behind the plane — and the interior surfaces the cut exposes are
shaded two-sided so they are lit, not black. An axis-aligned cut along axis $a$ at coordinate $o$ uses
$\mathbf n = \pm\hat e_a$, $w = \pm o$; the sign picks which half is removed. Disabled is
$\mathbf n=\mathbf 0,\,w=+\infty$ (nothing discarded).

## 2. Why a hollow cut needs a cap

Discarding the front half leaves the solid's interior **open**: along a view ray that passes through
material at the cut, the entry (front) face was in the discarded half, so you see straight through to the
back wall. To read as solid, the cut plane must be **filled** wherever it lies inside the solid. That
"inside the solid, at the plane, at this pixel" test is what the stencil buffer computes.

## 3. Stencil cap by parity

Render order, in one pass into a colour + depth-**stencil** target (ADR-0014), after the part's colour pass:

**Marking pass.** Re-render the part with the *same clip discard*, **no colour, no depth**, and a stencil
op of **Invert** on bit 0 for every fragment (both faces — culling off). Invert flips the parity bit, so
after the pass a pixel's stencil LSB is

$$ \text{stencil}_0 \;=\; \Big(\#\{\text{kept surfaces along the view ray}\}\Big) \bmod 2. $$

Consider the ray from a point **on the cut plane** going away from the camera (into the kept half). If
that point is **inside** the solid, the ray must cross the boundary an **odd** number of times to escape
to infinity ⇒ parity 1. If it is **outside** (e.g. down the open bore), it crosses an **even** number ⇒
parity 0. So $\text{stencil}_0 = 1$ marks exactly the solid part of the cut plane.

Parity (Invert) is chosen over front/back **counting** (increment back faces, decrement front faces)
because it is **winding-independent**: ICEM's STL is a triangle "soup" whose per-triangle winding is not
guaranteed, which would break a count but never a parity (every crossing flips the bit regardless of
orientation).

**Cap pass.** Draw a quad lying on the cut plane and spanning the part (a square of side $\sim\!2r$ about
the part centre — the stencil trims it, so it need only over-cover). The stencil test keeps only fragments
where $\text{stencil}_0 = 1$ (compare Equal, reference 1, mask 0x1) — the solid cross-section — and
`cap.frag` colours them. Depth test is off and the cap is drawn last: it is the frontmost kept surface, so
it correctly overlays the interior walls behind it. The quad's corners are generated in the vertex shader
from the push constant (the two in-plane axis extents + the plane offset and axis) — no vertex buffer.

## 4. The cut face *is* the field slice

`cap.frag` samples the field volume at the cut-face world position $\mathbf p$ (the same world→uvw affine
and colormap as the surface, see [colormap.md](colormap.md)) and shades it **flat** (unlit), so the
computed temperature/etc. reads true across the section plane. This is the **flat field slice** deferred
from C1: the cap and the slice are the same surface (the cut face), so they are produced together. With no
field bound the cap is a flat "machined metal" grey, a touch darker than the lit surface so the cut reads
as solid.

## Verification

- **Mechanism** — `tests/rhi/stencil_cap_test` (ADR-0014): mark a centred triangle into stencil with
  colour masked off, then fill only where the stencil test passes; the centre fills, a corner stays clear.
- **Cap** — `tests/rhi/cap_offscreen_test`: a unit cube cut in half, rendered with and without the cap; the
  cap visibly changes the cut-face pixel (the stencil-filled quad painted it), and the capped section shows
  the field colormap (cold-blue + hot-red) on the cut — i.e. the slice. No collapsed-normal black holes.
- **End to end** — the brick6 thrust chamber, cut along its axis: the cap fills the annular wall as flat
  machined metal while the bore stays open (capped vs `--no-cap` differ exactly on the wall cross-section).
