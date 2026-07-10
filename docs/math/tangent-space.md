# Tangent space & normal mapping — derivation notes (M6.4)

These notes derive the basis a **normal map** is read in, why we ship a tangent *per vertex*, why the
handedness sign in its fourth component is load-bearing, and why the tangents are generated with
**MikkTSpace** specifically. They are the companion to two pieces of code: the cooker's
`tools/asset-pipeline/src/tangent.rs` (which *generates* the tangents) and the forward-PBR shaders
`engine/render/shaders/pbr_forward.{vert,frag}` (which *consume* them). Both are terse because the
*why* lives here.

Conventions follow [ADR-0004](../adr/0004-math-conventions.md): right-handed world, column-vector
math, linear color in the shader. This file is the tangent-space companion to [pbr.md](pbr.md), which
derives the BRDF the perturbed normal then feeds. GitHub renders the `$…$` / `$$…$$` math below.

---

## 1. The problem: a normal map is written in a moving frame

A normal map stores, per texel, the surface normal of the *detail* geometry — the bumps, pores, and
scratches an artist wants without the triangles to carry them. It cannot store that normal in world
space: the same brick-wall texture is pasted onto walls facing every direction, so a fixed world
normal would be wrong everywhere but one wall. It cannot store it in object space either, or the
texture could not be reused across UV islands or mirrored.

So it stores the normal in **tangent space**: a per-point orthonormal frame $(\mathbf T, \mathbf B,
\mathbf N)$ glued to the surface and *oriented by the texture's own UV axes*. In that frame the
un-perturbed surface normal is always $(0,0,1)$, and a bump that leans "toward increasing $u$" is
always $(+,0,+)$ — independent of how the surface sits in the world. A flat texel is therefore
$(0,0,1)$, which after the standard encoding $c = \tfrac{1}{2}(n+1)$ is the byte triple
$(128,128,255)$ — the pale blue of every normal map, and the value of our `normal.png` fixture.

The renderer's job is the inverse map: take the tangent-space normal out of the texture and rotate it
into world space with the *right* frame, so the detail normal lands where the surface actually faces.
Everything below is about constructing that frame correctly and cheaply.

## 2. The TBN frame from position and UV derivatives

Two of the three axes are fixed by meaning. $\mathbf N$ is the interpolated surface normal we already
have. $\mathbf T$ (the **tangent**) is the world-space direction in which the texture coordinate $u$
increases along the surface; $\mathbf B$ (the **bitangent**) is the direction $v$ increases. Formally
they are the partial derivatives of the surface position $\mathbf p$ with respect to the texture
coordinates:

$$
\mathbf T = \frac{\partial \mathbf p}{\partial u}, \qquad
\mathbf B = \frac{\partial \mathbf p}{\partial v}.
$$

On a triangle these derivatives are constant and solvable from the corners. With vertices
$\mathbf p_0,\mathbf p_1,\mathbf p_2$ and their UVs $\mathbf w_0,\mathbf w_1,\mathbf w_2$, form two
edges and their UV deltas:

$$
\mathbf e_1 = \mathbf p_1-\mathbf p_0,\quad \mathbf e_2 = \mathbf p_2-\mathbf p_0,\qquad
(\Delta u_1,\Delta v_1) = \mathbf w_1-\mathbf w_0,\quad (\Delta u_2,\Delta v_2) = \mathbf w_2-\mathbf w_0 .
$$

Because $\mathbf T,\mathbf B$ are the constant basis in which UV steps become position steps, each
edge is its UV delta expressed in that basis:

$$
\mathbf e_1 = \Delta u_1\,\mathbf T + \Delta v_1\,\mathbf B,\qquad
\mathbf e_2 = \Delta u_2\,\mathbf T + \Delta v_2\,\mathbf B .
$$

That is a $2\times2$ linear system; inverting the UV-delta matrix gives the tangent and bitangent:

$$
\begin{bmatrix}\mathbf T\\[2pt]\mathbf B\end{bmatrix}
= \frac{1}{\Delta u_1\Delta v_2-\Delta u_2\Delta v_1}
\begin{bmatrix}\Delta v_2 & -\Delta v_1\\[2pt] -\Delta u_2 & \Delta u_1\end{bmatrix}
\begin{bmatrix}\mathbf e_1\\[2pt]\mathbf e_2\end{bmatrix}.
$$

The scalar $\det = \Delta u_1\Delta v_2-\Delta u_2\Delta v_1$ is twice the *signed* area of the
triangle in UV space. Its sign is the whole story of §4; hold onto it.

## 3. Why per-vertex, not per-pixel

We could recompute $\mathbf T$ per fragment from screen-space derivatives (`dFdx`/`dFdy`) — and a
few engines do — but it is noisier (derivatives are 2×2 quad finite differences), costs ALU every
pixel, and, most importantly, will not match the tool that *baked* the normal map (§5). Instead we
compute one tangent per **vertex** at cook time, store it in the vertex, and let the rasterizer
interpolate $\mathbf T$ across the triangle exactly as it interpolates $\mathbf N$. A vertex is shared
by several triangles, so its tangent is an area/angle-weighted average of their per-face tangents —
the same averaging that makes vertex *normals* smooth, applied to the tangent. The fragment then only
renormalizes and does one small orthonormalization (§6).

This is why the vertex layout grows a tangent attribute (`ATTR_TANGENT`, a 4×`f32`), and why it is
**optional**: a mesh no normal map touches never pays the extra 16 bytes/vertex (see
[assets.md](../design/assets.md) on the additive attribute design).

## 4. Handedness: the fourth component, and mirrored UVs

Store $\mathbf T$ and $\mathbf N$ in the vertex and you might think to also store $\mathbf B$. Don't:
$\mathbf B$ is (up to sign) determined by the other two, since the frame is orthonormal —

$$
\mathbf B = s\,(\mathbf N \times \mathbf T),\qquad s\in\{+1,-1\}.
$$

Storing the single sign $s$ instead of the whole vector saves 12 bytes/vertex *and* is more correct
across a seam, because $s$ is exactly the piece of information that a cross product cannot recover on
its own: the **handedness** of the UV chart.

Where does a left-handed chart come from? Mirroring. Character artists routinely UV-map one half of a
symmetric model and mirror the shell onto the other half to reuse texture space — the left boot samples
the same pixels as the right. On the mirrored half the UV winding is reversed, so $\det$ in §2 flips
sign, so $\mathbf B$ must flip to stay a right-handed-consistent frame. That flip *is* $s$. We encode
it as the tangent's `w`:

$$
\mathbf B = \texttt{tangent.w}\,\bigl(\mathbf N \times \mathbf T\bigr),\qquad
\texttt{tangent.w} = \operatorname{sign}(\det).
$$

Get this sign wrong and the mirror seam lights **inside-out**: bumps that should catch a light cast
shadow instead, and the two halves of the face disagree under the same lamp. It is the classic
tangent-space bug, and it is exactly what `tangent.rs`'s handedness test and (in the render brick) the
mirrored-UV pixel proof pin down. The convention above — bitangent $= w\,(\mathbf N\times\mathbf T)$
with $w=\pm1$ — is the one [glTF](https://registry.khronos.org/glTF/) mandates, so our decode matches
the data.

## 5. Why MikkTSpace, and not "some tangent"

Here is the subtlety that makes tangent generation a *convention* problem, not just a math problem.
A normal map is not neutral data: it was **baked** — a tool projected a high-poly model's normals into
the low-poly model's tangent space and wrote the result. Whatever basis the baker used is now encoded
into the pixels. If the renderer reconstructs a *different* basis at each vertex, the mismatch shows up
as faint faceting, seams that glow, and lighting that slides as the model deforms — even though every
individual tangent is a "valid" $\partial\mathbf p/\partial u$.

The industry's answer is to standardize the algorithm on both sides. **MikkTSpace** (Morten S.
Mikkelsen's tangent-space standard) is that algorithm: a specific weighting of per-face tangents onto
shared vertices and a specific orthonormalization, defined so that *any* two implementations produce
bit-comparable tangents from the same mesh. Blender, Substance, Unity, Unreal, and glTF's reference
generators all bake against it; glTF explicitly specifies it for meshes that omit tangents. So the
cooker uses the `mikktspace` crate rather than rolling the §2 formula by hand — not because our formula
would be *wrong*, but because "matches the baker" beats "locally correct" here. The cost of getting it
independently right is unbounded; the cost of adopting the convention is one dependency.

Two v1 simplifications are documented at their code:
- **Indexed, last-write-wins.** MikkTSpace may want to *split* a vertex when two faces genuinely
  disagree on its tangent (a hard tangent seam), growing the vertex count. We keep the mesh indexed
  and write each face-vertex's tangent onto the shared vertex, keeping the last. For smooth-normal,
  continuous-UV meshes the faces agree and this is exact; the de-index/re-weld for hard seams is a
  noted seam, taken only when an asset shows the artifact.
- **Tangents only when needed.** We generate them for a mesh only if its material carries a normal map
  (`cook_gltf`'s policy) — no map, no tangent attribute, no wider vertex.

## 6. Putting it back together in the shader

The consumption side (in `pbr_forward.vert`/`.frag`) is the inverse of §1, and short now that the
basis is trustworthy. In the **vertex shader**, transform the stored tangent into world space and hand
the fragment the pieces of the frame:

$$
\mathbf T_w = \operatorname{normalize}\!\big(\mathbf M_{3\times3}\,\mathbf t_{xyz}\big),\qquad
\mathbf N_w = \operatorname{normalize}\!\big(\mathbf M^{-\top}_{3\times3}\,\mathbf n\big),
$$

carrying `tangent.w` through unchanged (a sign is invariant under the transform). $\mathbf T$ rides the
model matrix like a direction on the surface; $\mathbf N$ rides the inverse-transpose like the covector
it is (the same reason vertex normals do — see [pbr.md](pbr.md) §7).

In the **fragment shader**, re-orthonormalize (interpolation across the triangle leaves $\mathbf T$
slightly non-perpendicular to $\mathbf N$) with one Gram–Schmidt step, rebuild the bitangent from the
sign, sample and decode the map, and rotate it to world space:

$$
\mathbf T' = \operatorname{normalize}(\mathbf T - (\mathbf N\!\cdot\!\mathbf T)\,\mathbf N),\qquad
\mathbf B = \texttt{tangent.w}\,(\mathbf N \times \mathbf T'),
$$

$$
\mathbf n_t = \big(\text{scale}\cdot(2\,\mathbf c_{xy}-1),\; 2c_z-1\big),\qquad
\mathbf N' = \operatorname{normalize}\!\big([\,\mathbf T'\ \mathbf B\ \mathbf N\,]\,\mathbf n_t\big),
$$

where $\mathbf c$ is the sampled texel and $[\,\mathbf T'\ \mathbf B\ \mathbf N\,]$ is the TBN matrix
whose columns are the frame. The `scale` is glTF's `normalScale` / the material's `normal_scale`: it
multiplies only the tangent-plane ($xy$) components, so a value below 1 flattens the bumps and 0
recovers the geometric normal. $\mathbf N'$ is the perturbed normal every term of the BRDF in
[pbr.md](pbr.md) then uses in place of $\mathbf N$.

## 7. What this buys, and where it goes next

With §6 in place, a flat quad under a grazing light shows the *bumps* of its normal map — intra-face
shading variance where a flat control has none (the render brick's normal-mapping pixel proof). The
same frame is what the deferred G-buffer will store per pixel at M10, and what parallax/height mapping
would perturb further; both are left as clean seams here. The one convention that must never drift is
$\mathbf B = w\,(\mathbf N\times\mathbf T)$ with MikkTSpace-generated $w$ — cooker and shader agree on
it by construction, and the handedness proof is the guard.
