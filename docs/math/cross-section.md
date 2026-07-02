# Cross-section clip plane — derivation notes (ICEM viewer, B2)

These notes derive the cross-section the ICEM viewer uses to look *inside* a computed part. The code is
in `samples/03-icem-viewer/shaders/mesh.frag` (the per-fragment cut) and `main.cpp` / `mesh_render.hpp`
(the plane parameters). Conventions follow [ADR-0004](../adr/0004-math-conventions.md). GitHub renders
the `$…$` math.

## 1. A plane as a half-space test

A plane in $\mathbb R^3$ is the set of points $\mathbf p$ with

$$\mathbf n\cdot\mathbf p = w,$$

where $\mathbf n$ is a **unit** normal and $w$ is the signed distance from the origin to the plane along
$\mathbf n$ (because for a point $\mathbf p_0$ on the plane, $w=\mathbf n\cdot\mathbf p_0$ is exactly the
length of the projection of $\mathbf p_0$ onto $\mathbf n$). The plane splits space into two half-spaces:

$$\mathbf n\cdot\mathbf p < w \quad(\text{behind})\qquad\text{and}\qquad \mathbf n\cdot\mathbf p > w \quad(\text{in front}).$$

To **section** the part we simply throw away every fragment in the front half-space. In the fragment
shader, with the fragment's world-space position $\mathbf p$:

```glsl
if (dot(n, p) > w) discard;
```

`discard` removes the fragment before it can write color or depth, so the near material vanishes and the
geometry *behind* the plane — the interior walls, channels, cavities — is what remains to be drawn. No
geometry is modified; the cut is purely a per-pixel decision, which is why it is essentially free and
can slide in real time. The disabled state is $\mathbf n=\mathbf 0$, $w$ large: $0>w$ is never true, so
nothing is cut.

## 2. Placing an axis-aligned plane

The viewer exposes the three axis-aligned cuts. Let $\mathbf e$ be the chosen axis ($\hat x$, $\hat y$
or $\hat z$), let $s=\pm1$ choose which side to remove, and let the user slide the plane to the axis
coordinate $o$ (the *offset*). We want the plane to sit at coordinate $o$ and to discard the $s$ side:

$$\text{discard where } s\,p_{\text{axis}} > w .$$

Take $\mathbf n = s\,\mathbf e$ (a unit vector) so $\mathbf n\cdot\mathbf p = s\,p_{\text{axis}}$. The
condition becomes $s\,p_{\text{axis}} > w$. Solving for the two cases shows the single closed form
$w = s\,o$ works for both:

- $s=+1$: discard $p_{\text{axis}} > w$. We want discard $p_{\text{axis}} > o$, so $w = o = s\,o$. ✓
- $s=-1$: discard $-p_{\text{axis}} > w \Leftrightarrow p_{\text{axis}} < -w$. We want discard
  $p_{\text{axis}} < o$, so $-w = o$, i.e. $w = -o = s\,o$. ✓

Hence the plane is $(\mathbf n, w) = (s\,\mathbf e,\; s\,o)$, and sliding $o$ moves the cut along the
axis while $s$ flips which half disappears. The viewer initializes $o$ to the part-center coordinate
$o_0 = \mathbf c\cdot\mathbf e$ so the first cut passes through the middle of the part.

## 3. Lighting the exposed interior (two-sided shading)

The faces a cut newly exposes are the *inside* of the surface: their outward normals point into the
half-space we removed, i.e. roughly **away** from the camera, so a one-sided $\max(\mathbf n\cdot\mathbf l,0)$
shade would leave them black and the section would read as a void rather than a cutaway. The fix is
two-sided shading: if the normal faces away from the viewer, flip it,

$$\mathbf n \leftarrow \begin{cases}-\mathbf n & \mathbf n\cdot\mathbf v < 0\\ \;\;\,\mathbf n & \text{otherwise,}\end{cases}\qquad \mathbf v = \widehat{\mathbf e_{\text{eye}}-\mathbf p},$$

so the interior walls light up like any other surface. This is what makes the inside of the chamber —
its bore, throat, and cooling passages — legible in section.

## 4. Scope and the next step

This is a **clip-only** section: it reveals the interior but leaves the cut *open* (you see through to
the back walls, not a filled solid face). A true **solid cap** — filling the cut plane with the part's
material so the section looks like sawn metal — is the next refinement. The standard route is a stencil
pass (count back/front faces crossing the plane to mark "inside the solid", then draw the plane where
the stencil is set), which needs the RHI's stencil state; ICEM's analytic SDF offers an exact alternative
(evaluate the field on the cut plane and fill where it is inside). Both are deferred to a follow-up brick;
the clip already delivers "see the structures inside".
