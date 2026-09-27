# ADR-0046: Exported games embed the core and pick their own mode; and the GPL boundary around Blender

- Status: Accepted
- Date: 2026-09-27

## Context

[ADR-0045](0045-hosted-front-end-v1.md) made a hosted browser front end committed scope. Luca then
asked for three more things, and one of them cannot be built as stated:

1. a `rime.peekstar.eu` route where users **edit** games and **play** games;
2. games exported from the Rime pipeline shipping as a **standalone product** — the stated ambition
   is a Battlefield-6-class game — that still embeds the Rime core, so the *end user of the exported
   game* picks at launch between playing locally, hosting for a browser, starting a CLI server that
   streams over the network, and hosting a multiplayer session; **any** user must be able to run a
   dedicated server from the exported artifact;
3. asset creation from an LLM plus user input, and a "modified Blender core pulled from the repo so
   we can create assets in the Rime engine", later replaced by an in-house core — with the question
   of whether Rime could be planned from Blender's code directly.

An Opus 5.5 consult produced the architecture proposal behind this ADR; its citations were
re-verified line by line against the tree before anything here was written down, and the five
load-bearing ones held. This ADR records the decisions. The roadmap rows for the new tracks are in
[`docs/ROADMAP.md`](../ROADMAP.md); the delivery order is not settled here beyond "after M18".

## Decision 1: the exported artifact is one binary that selects its mode before it creates a device

An exported game is a **bundle**, and the mode is chosen on the command line (or through a launcher
when there are no arguments) *before* any RHI device exists:

```
mygame [play | stream --port N | browser --port N | host --port N | dedicated --port N | edit-host]
```

Choosing before device creation is the whole point of the shape: **a dedicated server must never
touch Vulkan**, and the only way to guarantee that is for the mode to be decided before the code
that would create a device runs. `rhi::create_device` already returns `nullptr` rather than
aborting when there is no loader or ICD (`engine/rhi/include/rime/rhi/device.hpp:167`), so a
GPU-less box is a supported target rather than a special build.

The bundle holds the binary, an optional gateway binary, the browser client, cooked content with a
manifest and content hash, a `rime-game.toml` naming the game id, engine and protocol versions and
entry scene, and a `LICENSES/` directory. Content resolves **relative to the executable**, which is
a change from today: `samples/99-the-block/main.cpp` takes its cooked directory from the
compile-time `RIME_BLOCK_COOKED_DIR`, so a copied build looks for content where the source tree
used to be.

**What this forces into the public API.** A `rime::app::GameDefinition` — the component registrar
and scene preparer (both already exist on the editor-host path,
`engine/app/include/rime/app/editor_host_app.hpp:40-70`), the server simulation systems, the client
presentation, and the input map — plus `run_game(argc, argv, const GameDefinition&)`, the mode-flag
contract, and the bundle manifest format. Packaging becomes `rime package` in the Rust CLI, which
cooks, validates every asset through the engine's own reader (`rime_asset_validate`, already bound
at `tools/rime-ffi/src/lib.rs:45`), and writes the manifest.

**What is already reachable, stated honestly, because "it embeds the core" is easy to over-claim:**

| mode | status |
|---|---|
| play locally | reachable, but only as each game's own `main` |
| CLI stream server | the pipeline exists (`samples/04-remote-view`), but its scene is a colour clear |
| browser | new; ADR-0045's gateway |
| host / dedicated | `net`, `gameplay_net` and `replication` exist and are proven on loopback and simulated links only. Listening for strangers, join, and session discovery are **new** |

`rime_app_create_headless` is **not** a headless renderer and cannot serve any of these: the C ABI
documents it as "no device, 60 Hz sim" (`engine/capi/include/rime/capi/rime.h:99`). It exists to
prove the FFI can spin the engine loop from another language. `docs/design/hosted-rime.md` listed it
under "Headless rendering is real"; that line is corrected by this ADR's companion edit.

**The riskiest assumption, recorded because it is load-bearing rather than incidental:** that a
game's play loop can be lifted out of its sample `main` into an engine-owned `GameDefinition`
without rewriting the game. The block's loop is ~3000 lines of *sample* code
(`samples/99-the-block/main.cpp:3019` onward). If it does not factor cleanly, both this decision and
ADR-0045's play surface stall. The mitigation is that the first brick uses a small game and not the
block — and ADR-0038 §"what's missing" already named `samples/hello-game` for exactly this purpose
and **it was never built**, so that sample is now a prerequisite of this track rather than a nicety.

## Decision 2: one hostname, two surfaces, and the surface is a property of the session

`rime.peekstar.eu` serves a catalogue, a play route and an edit route, but the **surface belongs to
the session, not to the hostname or to anything the browser says**. The gateway creates the session,
spawns the engine process, and is the `ProtocolConnection` peer.

**The surface is enforced in the gateway at the protocol level:** a `play` session never forwards
the `0x0200-0x02FF` editor band (`engine/stream/include/rime/stream/protocol.hpp:89-90`). This is
the decision that matters, because the alternative — letting the engine decide from a flag the
client supplied — would make "can this tenant edit?" a question the engine has to answer about an
untrusted peer.

**Two things the engine gains, and nothing more,** which is ADR-0045's "accept one connection on an
fd I hand you" made concrete: a `LocalSocket::adopt(SocketHandle)`, because today the handle
constructor is private and only `LocalListener::accept()` may mint a connected socket
(`engine/platform/include/rime/platform/socket.hpp:197-198`); and a `--serve-fd` flag on the editor
host. Delete the gateway crate and the engine is unchanged — guardrail 2.

**The editor viewport is LZ4 today and must learn AV1.** `engine/app/editor_host_app.cpp:924,926`
hardcodes `Codec::LZ4`, which is right for a lossless local viewport and useless to a browser. This
does **not** reopen ADR-0045 decision 1: the wire still stays AV1, and this is the brick that
*enables* AV1 on a path that never needed it before. AV1 frames are then **repacketised** onto RTP,
never transcoded, and the input-to-photon echo moves to a DataChannel so
`engine/stream/include/rime/stream/latency.hpp`'s ledger still closes offset-free.

## Decision 3: generated content enters as SOURCE assets, through the existing cook

LLM-generated assets arrive as glTF, PNG or `.rscene` fragments written into the cook's input
directory — **never** as cooked payloads, and **never** as code or shaders. That keeps
`hosted-rime.md`'s "there is no scripting engine in `engine/` today — keep it that way for v1"
boundary intact, and it means generated content is validated by the same parsers, budget gates and
readers that a human's Blender export goes through, with a provenance sidecar (model, prompt, user
inputs, seed, output hashes) beside it. Replaying the sidecar reproduces the cooked output
byte-for-byte without a network call: the model is not deterministic, but the cook stays
deterministic over what was recorded.

Every rejection increments a counter, per guardrail 5 — a generator whose bad output is silently
dropped reads exactly like a generator that works.

## Decision 4: no GPL code is linked into Rime, so Blender stays a separate process

**The request as stated cannot be built.** Blender's core — its C/C++ source, DNA/RNA, bmesh,
modifiers, and the `bpy` Python API — is GPL-3.0-or-later. Apache-2.0 code may flow *into* a GPL
work; GPL code may not flow into an Apache-2.0 work. Linking or embedding a modified Blender core —
statically, dynamically, or as in-process `bpy` — makes the distributed combination GPL, and under
decision 1 **every game exported from Rime would inherit that**.

This is not a new principle here. It is the same reasoning that ruled out GPL x264
([ADR-0017](0017-streaming-codec.md)) and that ADR-0030 applied again when it chose AV1 over
H.264/HEVC — `engine/stream/include/rime/stream/video_codec.hpp:20` states it in the tree today.
Reversing it for an authoring tool would be a larger concession than the one this project has twice
refused for a codec.

**"Plan Rime from the Blender code directly", and learning from it.** Luca's answer (2026-09-27) is
that Rime builds its **own** in-house asset generator and learns from Blender rather than embedding
it — which is the outcome this section was going to recommend, so it is the decision and not a
compromise. The boundary that matters is therefore worth stating precisely rather than as a slogan:

Copyright covers **expression**, not ideas, algorithms or behaviour. Learning *how* Blender does
something — from its documentation, its observable behaviour, the papers it cites, or a description
of its approach — is fine and is ordinary engineering. What creates a derivative work is reproducing
its **expression**: translating its source, or writing code that mirrors its structure and naming
because the source was open beside the editor at the time.

So the working rule for the in-house generator, which is a solo-developer-shaped rule rather than the
two-team clean-room of a large organisation: implement from papers, documentation and observed
behaviour; do not have Blender's source open while writing the equivalent function; and name the
reference in the comment, per the teaching rule, so the provenance of an idea is recorded where a
reader will find it. (This is a practical engineering constraint, not legal advice.) That constraint
is cheap now and very expensive to retrofit after someone has read the source and written similar
code.

**What is built instead:** the in-house generator is the destination, and Blender is the bridge that
buys time until it exists. Blender stays a separate process at arm's length, and the integration is
a documented, general-purpose data contract — glTF plus the editor band — never Blender's in-memory
structures. Any GPL add-on lives in a **separate repository**, shipped with its source. Two
consequences worth stating: running Blender server-side for hosted users creates no source
obligation (GPL is not AGPL), but it may never be placed in an exported bundle; and Cycles is
Apache-2.0, so it is usable on its own terms if it is ever wanted.

**What this costs:** seconds-per-edit export/cook round trips instead of live vertex editing, no
shared undo, an add-on that tracks Blender's Python API churn, and two licences to manage. **What it
buys:** Rime and every game exported from it stay Apache-2.0.

The seam is `rime cook --watch` → the host re-reads the manifest → resends `AssetList`
(`engine/editorhost/include/rime/editorhost/editor_host.hpp:44`) and hot-reloads. That same seam is
what an in-house modelling core would later feed, so replacing Blender swaps the *producer* and
nothing else — which is the real argument for this shape, beyond the licence.

## Consequences

- `docs/ROADMAP.md` gains the tracks for exported games/dedicated servers, generated assets, and the
  authoring bridge. All queue after M18; none is scheduled here.
- `docs/design/hosted-rime.md`'s `rime_app_create_headless` claim is corrected.
- The generated-content licence and the model provider remain open for Luca.

## Amendment (2026-09-27): Luca's answers on the two open questions

Both were taken against the recommendation above. Both are recorded with their cost and with the
mitigation that makes them buildable, rather than re-argued.

**The gateway ships in EVERY exported bundle.** The cost is real and unchanged: a single-player game
now carries a web server and a WebRTC stack, which is attack surface a local game never needed.

*The mitigation that keeps the decision and removes most of the cost:* the gateway ships but **does
not listen unless a hosting mode asked it to**. It is a binary in the bundle that is executed only by
`browser` mode (and, later, `host`/`dedicated`), never a service started at launch. Shipped-and-idle
has roughly the attack surface of an unused file; shipped-and-listening does not. So the bundle
carries it, `play` mode never spawns it, and the bundle's own smoke proof asserts that `play` opens
**no listening socket** — which is what stops the distinction eroding the first time someone wires
convenience into the launcher.

**The Blender add-on is built in THIS repository.** This is workable, but only with hygiene that has
to be mechanical rather than remembered, because the failure mode is a downstream consumer believing
the whole repository is Apache-2.0:

1. **It lives outside `engine/` and `tools/`** — a new top-level `integrations/blender-addon/`.
   `tools/` is the Cargo workspace and is Apache-2.0 by convention; `engine/` is the thing that must
   never acquire a GPL dependency. Putting the add-on in either invites exactly the mistake.
2. **The subtree carries its own `LICENSE`** (GPL-3.0-or-later) and every file an SPDX
   `GPL-3.0-or-later` header. The root `NOTICE` and `README` state that the repository contains one
   GPL subtree and name it.
3. **Nothing in the build references it.** No CMake `add_subdirectory`, no Cargo member, no
   `target_link_libraries`, no runtime import from `engine/` or `tools/`. It is a Blender add-on that
   Blender loads; Rime only ever reads the files it exports.
4. **`scripts/check-license-headers.sh` gains the inverse rule.** It scans `engine tests tools` for
   Apache-2.0 SPDX headers on `.cpp/.hpp/.mm/.rs` today, so a Python add-on is simply invisible to
   it — no false failure, and no enforcement either. The gate therefore gains a positive check:
   every file under `integrations/blender-addon/` must carry `GPL-3.0-or-later` and must **not**
   carry `Apache-2.0`. A licence boundary that depends on nobody forgetting is not a boundary.
5. **It is never placed in an exported bundle** (decision 1's `LICENSES/` directory covers the
   bundle's own dependencies; the add-on is an authoring-side tool and stays out).

**Worth knowing before writing the add-on at all:** a Blender add-on is GPL because it imports `bpy`.
If what is actually needed is "export glTF from Blender the way Rime expects", Blender's **built-in**
glTF exporter plus a documented set of export options may cover it with no add-on of our own, and
therefore no GPL code in this repository. That is the cheaper compliant path and worth testing against
`docs/authoring-blender.md`'s existing contract before committing to a custom add-on.

**Also confirmed by Luca (2026-09-27):** the in-house asset generator is the destination, not a
someday option, so the Blender bridge is explicitly a stopgap that buys time until it exists.
