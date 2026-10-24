**Proposal (independent, read-only)**

**Core principle:** the editor never "runs the game"; it saves an exact file, confirms *that* save succeeded, then spawns the existing `--play --scene <path>` binary as an opaque child. All game-specific knowledge (weapons, networking, resources) stays in the game binary.

**1. Authored-map parity (the real risk).**
Parity means: what the editor shows == what `--play --scene` loads. Two divergence points today:
- *Generation:* city expansion lives in `--play` only when `scene_path` is empty. Solution as proposed: a `savedcity` generator calling the same `city::expand` into a plain authored scene file. Add a one-shot test: generate → load via the strict `--scene` path → diff entity list against in-process `expand`. If `expand` later gains runtime-only randomness, parity silently breaks; seed it explicitly in the saved scene.
- *Preview:* the-block-host previews nine CookSpecs but not lamp/barrier patterns. Rather than teaching the host every procedural pattern (ongoing maintenance), have the generator *bake* lamp/barrier instances into the saved scene as ordinary placed entities. Then any host (even generic enginehost) previews them correctly, and extending the game-specific host becomes optional polish, not a parity requirement.

**2. Editor launch.**
- `--game <path>` + repeated `--game-arg`; no shell. Good.
- SaveScene result has no request id; "wait for matching ordered result" is fragile if any other save fires. Minimal fix: add a request id to SaveScene/result (small protocol change, worth it). Otherwise serialize saves in the editor with a single in-flight slot.
- After confirmed success: spawn child with `--play --scene <savedpath>`, surface stderr/exit code in the UI, refuse duplicate launches while child alive. On save failure: show error, launch nothing — never fall back to a stale file.

**3. Development export (honest scope).**
A `bundle_city_dev` script producing: game exe, cooked dir, viewmodel content dir, generated scene, third-party assets with NOTICES/CC-BY attribution. Game must accept `--cooked` / `--viewmodel-content` relative to exe dir (or default to `exe_dir/content/*`); fail loudly if missing. Validate: no absolute paths remain, every referenced asset exists, every third-party file has a notice. Label explicitly "development bundle, not production export".

**Flag as unjustified now:** extending host preview for procedural patterns (baking supersedes it), generic editor play-launch UI beyond one button, any network/weapon runtime in the editor preview, production-grade installer/signing.
