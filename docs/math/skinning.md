# Skeletal skinning — derivation notes (M6.7, part 1: the CPU sampler)

These notes derive **linear blend skinning** (LBS) and the clip-sampling that feeds it: what an
*inverse bind matrix* is, how a joint's animated *world* pose is composed down the skeleton, what the
per-joint *palette* matrix means, and how a clip's keyframes are interpolated at a time $t$ — including
why quaternions need a *neighborhood* fix. They are the companion to the code that implements them:
`engine/assets/include/rime/assets/{skeleton_asset,clip_asset}.hpp` and the sampler
`engine/assets/src/clip_sampler.cpp`. Those files are terse because the *why* lives here.

This is **part 1**: import-side data and the **CPU** sampler that produces a palette (AN0, correctness
first). **Part 2** — GPU palette skinning in the vertex shader, and blending between clips — arrives
with AN1 in the M7 window and extends §4–§5 below.

Conventions follow [ADR-0004](../adr/0004-math-conventions.md): right-handed world, column vectors,
$M v$ applies $M$ to $v$; and [ADR-0005](../adr/0005-rotation-representation.md): rotations are unit
quaternions, composing right-to-left like matrices. Companions:
[quaternions-transforms.md](quaternions-transforms.md) (the quaternion algebra reused here) and
[transform-hierarchy.md](transform-hierarchy.md) (the parent→child compose). GitHub renders the
`$…$` / `$$…$$` math.

---

## 1. The problem: one vertex, several bones

A skinned mesh is authored once, in a **bind pose** (the rest position — think a character in a
T-pose). Every vertex $v$ is painted with up to four *(joint, weight)* pairs: "this belly vertex is
70% spine, 30% hip." When the skeleton moves, the vertex must follow its joints — weighted.

The naïve idea "store each vertex relative to its joint, then re-place it" is exactly right; the whole
derivation is just making "relative to its joint" and "re-place it" precise as matrices.

## 2. The bind pose and the inverse bind matrix

Each joint $j$ has a **bind world transform** $B_j$: where that joint sits, in model space, in the
rest pose. $B_j$ is itself the composition of local bind transforms down the tree (§3, evaluated at
rest).

A vertex $v$ is given in **model space**. To express it *relative to joint $j$* — in the frame where
$j$ sits at its rest position — we undo $j$'s bind placement:

$$ v^{(j)} = B_j^{-1}\, v . $$

$B_j^{-1}$ is the **inverse bind matrix**. It is a property of the *bind pose only*, so the cooker bakes
it once per joint and ships it in the `Skeleton` (`Joint::inverse_bind`). glTF stores exactly this
array (`skin.inverseBindMatrices`); we keep it verbatim.

Intuition: $v^{(j)}$ says "if joint $j$ were the origin at rest, here is the vertex." Whatever $j$ does
next, the vertex is welded to that frame.

## 3. The animated world pose (compose down the tree)

Animation supplies, per joint and per time $t$, a **local** transform $L_j(t)$ — the joint's placement
*in its parent's frame* (a TRS: translation, a unit-quaternion rotation, scale). The joint's **world**
transform is the running product from the root:

$$ W_j(t) = W_{\mathrm{parent}(j)}(t)\; L_j(t), \qquad W_{\text{root}}(t) = L_{\text{root}}(t). $$

Because joints are stored in **topological order** (a parent's index precedes its children'), this is a
single forward pass — each joint reads its already-computed parent (`clip_sampler.cpp`; the ordering is
asserted by `Skeleton::is_topologically_ordered`). We compose in **matrix** space, not by multiplying
TRS structs: a parent's *non-uniform* scale shears a child in a way a single $T\!\cdot\!R\!\cdot\!S$
cannot represent, and matrices carry that correctly (see the note in `transform.hpp::operator*`).

## 4. The palette and the skinning sum

Put §2 and §3 together. Take the vertex into joint $j$'s rest frame ($B_j^{-1}$), then move with that
joint to its animated pose ($W_j(t)$):

$$ P_j(t) \;=\; W_j(t)\, B_j^{-1} . $$

$P_j$ is joint $j$'s **palette matrix** (a.k.a. skinning / skin matrix). It is precisely what
`sample_clip` writes — one $\mathrm{Mat4}$ per joint, in joint order. A vertex bound to joints
$\{j\}$ with weights $\{w_j\}$ (with $\sum_j w_j = 1$) deforms to

$$ v' \;=\; \Big(\sum_j w_j\, P_j(t)\Big) v \;=\; \sum_j w_j\, W_j(t)\, B_j^{-1}\, v . $$

This is **linear blend skinning**. Two sanity checks the tests pin:

- **Bind pose ⇒ identity.** At rest, $W_j = B_j$, so $P_j = B_j B_j^{-1} = I$ and $v' = v$ — the mesh
  is unmoved. (`sample_clip: a static clip yields the identity palette`.)
- **Rigid follower.** A child joint with no animation of its own inherits its parent's motion exactly,
  because $W_{\text{child}} = W_{\text{parent}} L_{\text{child}}^{\text{bind}}$ carries the parent's
  delta through. (`… LINEAR translation … propagates down the tree`.)

Normals use the same $P_j$ but as its inverse-transpose (the §2 argument applied to covectors, exactly
as in [tangent-space.md](tangent-space.md)); that belongs to part 2, where normals are skinned on the
GPU. LBS's known artifact — the "collapsing elbow" at large bends, because a blend of rotations is not
a rotation — is likewise a part-2 concern (dual-quaternion skinning is the usual fix; recorded as a
seam, not built).

## 5. Sampling a channel at time $t$

Each local transform $L_j(t)$ is read from keyframed **channels** — separate tracks for translation,
rotation, and scale, each a list of times $t_0 < t_1 < \dots$ and values. A silent channel falls back
to the joint's bind-local value, so a clip only costs what it actually moves.

**Bracket.** Find the interval $[t_i, t_{i+1}]$ with $t_i \le t < t_{i+1}$ (a binary search) and the
fraction $\alpha = (t - t_i)/(t_{i+1} - t_i)$. Outside the track we hold the nearest endpoint — no
extrapolation.

**STEP** holds the earlier key: value $= \text{values}[i]$. Right for discrete/mechanical motion.

**LINEAR, vectors** (translation, scale) is the plain lerp $ (1-\alpha)\,a + \alpha\,b $.

**LINEAR, rotations** is where quaternions bite. A unit quaternion $q$ and its negation $-q$ encode the
**same rotation** (the double cover $\mathrm{SU}(2)\to\mathrm{SO}(3)$: rotating by $\theta$ about an
axis and by $2\pi-\theta$ about the opposite axis land the same orientation, and their quaternions
differ by sign). If two neighboring keys sit on opposite hemispheres — $q_i \cdot q_{i+1} < 0$ — a
straight blend swings the **long way** around the sphere, and for exact opposites it passes through the
zero quaternion (an undefined rotation). The fix is the **neighborhood** test: flip the sign of the
second key when the dot is negative, then normalized-lerp:

$$ q(\alpha) = \operatorname{normalize}\!\big((1-\alpha)\,q_i + \alpha\,\hat q_{i+1}\big), \qquad
   \hat q_{i+1} = \operatorname{sign}(q_i \cdot q_{i+1})\; q_{i+1}. $$

We use **nlerp**, not slerp: it is cheap, and its slightly non-constant angular velocity is
imperceptible between typical keyframes. slerp (constant angular velocity, [quaternions-transforms.md
§slerp](quaternions-transforms.md)) is the *quality seam* AN1 can opt into per clip. The
`… shortest arc across the quaternion double cover` test pins the neighborhood fix with the adversarial
$q, -q$ pair: the correct result is $q$ (rotation preserved); the buggy one collapses to identity.

## 6. Time policy: loop vs clamp

The raw play-head time is mapped into $[0, \text{duration}]$ before sampling. **Clamp** holds the
endpoints — a one-shot that stops on its last pose. **Loop** wraps modulo the duration (folding a
negative time back in), so $t$ and $t + \text{duration}$ sample identically — a cycle. This is a
*sample-time* choice, not baked into the clip, so one clip plays either way.

---

### Status & what part 2 adds

Implemented and proven CPU-side (AN0): the palette equation §4, all of §5–§6, on a hand-computable
2-bone fixture (`tests/assets/clip_sampler_test.cpp`). The **import and cook** side is complete too —
glTF skins/animations cook to `SkeletonAsset` + `ClipAsset` + a skinned mesh (`JOINTS_0`/`WEIGHTS_0`
vertices), the C++ RMA1 readers ingest them under the trust-nothing discipline, and a cross-language
fixture (`skinned.gltf` → committed `.rskel`/`.ranim`, read and sampled by `tests/assets/fixture_test.cpp`)
is the drift alarm. One import subtlety the math above assumes: the cooker **topologically reorders**
glTF's arbitrarily-ordered skin joints so §3's single forward pass is valid. The cooked byte layouts are
[docs/design/assets.md](../design/assets.md) and [tools/asset-pipeline/FORMAT.md](../../tools/asset-pipeline/FORMAT.md).

Part 2 (AN1, M7) adds: the palette uploaded and applied in the vertex shader (consuming those
`JOINTS_0`/`WEIGHTS_0` attributes — the reserved vertex-layout bits from
[ADR-0024](../adr/0024-asset-model.md) §6), normal skinning by the inverse-transpose, and cross-clip
blending. The palette this file derives is unchanged by that move — it is the interface between the two
halves.
