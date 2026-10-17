# Multiplayer development and shipment

Product direction agreed 2026-10-15; [ADR-0085](../adr/0085-multiplayer-development-and-shipment.md).
This plan tracks implementation and evidence, not promised feature parity or dates.

## Target evidence

| Target/profile | Verified starting point | Gate before shipment support |
| --- | --- | --- |
| Linux native player | Build/test CI; playable samples; hello-game export | Verified release inventory, compatibility floor, clean second-machine run |
| Linux dedicated | GPU-free hello-game mode; repository-hidden export proof | Packaged listening server, two real clients, lifecycle/compatibility tests |
| Windows native player | Build/test CI | Native export, dependency inspection, clean-machine execution |
| macOS native player | Build/test CI through MoltenVK | Native export, dependency inspection, clean-machine execution |
| PS4 | Product target; backend absent | SDK/toolchain and hardware access, RHI/runtime port, package/platform proof |
| Xbox | Product target; backend absent | Exact console generation, SDK/toolchain and hardware access, RHI/runtime port, package/platform proof |
| Browser play | Existing native streaming gateway | Exported-game integration, authenticated isolated sessions, reconnect/latency/capacity proof |
| Optional browser development | Editor and browser tooling exist | Creator policy, isolated forks, validation/quotas, preview/build/publish/rollback proof |
| Native browser/WASM | No runtime target | Separate feasibility, renderer/network/runtime port and build proof |

Build support is different from shipment support. Console executables are not
dedicated-server executables: an export can include a compatible Linux/Windows
server alongside a native console player package. The pipeline must report
unsupported targets explicitly, never substitute another target silently.

## Delivery sequence

Every step has an implementation, a reproducible proof, and a pipeline/operations
gate. Unfinished performance and editor work remains tracked in the main roadmap.

| Step | Concrete result | Proof and pipeline gate |
| --- | --- | --- |
| 0 — Playable repair, in progress | Held-RMB second-camera scope with a larger ocular; reliable firing feedback; diagnose disappearing ground darkness | Rendered target magnification, input/fixed-tick regressions, actual accepted-shot feedback, controlled tree comparisons; existing three-OS and sanitizer CI |
| 1 — Auditable Linux export, implemented; CI pending | Versioned payload manifest, content hashes and reviewed redistribution notices for hello-game | 32 local cases and isolated Release execution with HOME-based and outside-HOME checkouts; CI retains revision, tests and artifact identity; clean-machine release compatibility pending |
| 2 — Exported multiplayer | Existing GameDefinition launches a listening authoritative server and separate clients; then port the block | Two client processes from exported artifacts, late join, prescribed loss/reordering, compatibility refusal and convergence; CI captures logs and always cleans up processes |
| 3 — Capacity and operations | Measured physics/destruction/network envelope; operable servers | Named hardware, player/body counts, tick percentiles, bandwidth/memory/corrections/soak; health checks, graceful shutdown, version pinning, staged rollout and rollback rehearsal |
| 4 — More native targets and browser play | Windows export proof, then other proven native exports; exported games through the streaming gateway | Native dependency checks and clean-machine tests; authenticated session isolation, resource limits, reconnect, input/audio latency and concurrency measurements; console gates remain separately blocked |
| 5 — Creator-enabled browser development | Players edit an isolated fork of an enabled game, preview and publish their own version | Server-enforced opt-in; malformed assets/untrusted code/quota controls; immutable approved session versions; edit → preview → validated build → private multiplayer session → rollback |

Steps 4 and 5 are split into further bricks when their prerequisites are measured.
Console access/feasibility tracking starts immediately; a blocked console target
does not block verifiable PC/server work.

## Verified Linux bundle

Keep `scripts/export-game.sh`, its generated CMake export record and the existing
content-root rule. Scope the first implementation to `samples/hello-game` on Linux.
Do not introduce a general package manager or silently claim other target support.

The manifest identifies the game/build target, source revision, architecture,
available launch profiles and explicit content/protocol compatibility identity.
It lists payloads in sorted relative-path order with size and SHA-256 digest.
Required redistribution notices and system requirements accompany the artifact.
An unsigned manifest identifies payload bytes; it does not establish publisher
authenticity or replace server-side compatibility validation.

Acceptance requires:

1. Export outside the repository and preserve the existing dependency checks:
   system libraries only, no RPATH/home/Conan links, no link-time Vulkan.
2. Preserve repository/HOME/Vulkan-hidden dedicated runs at 120 ticks and the win,
   with digests equal to the in-tree runs; preserve missing-content refusal.
3. A trusted verifier rejects changed executable/content bytes, missing files,
   unsafe paths and missing required notices before launching the game.
4. Identical staged bytes produce identical manifest entries. This does not claim
   reproducible compiler outputs.
5. Review the actual redistribution inventory. Unresolved required notices block
   release rather than being filled with guessed licenses.
6. Exercise the verifier and negative controls in CI and retain identifiable
   evidence. A release candidate cannot be labeled supported with unfinished gates.

Dedicated networking follows this bounded packaging brick. The package identity
must not become a reason to postpone a real multiplayer delivery proof.

Implementation is recorded in [ADR-0087](../adr/0087-verified-linux-development-bundle.md).
The next shipment brick is a bounded listening hello-game server plus two
separate exported client processes, with join/leave and convergence evidence.

## Runtime and trust boundaries

Development preview and shipped games use the same authoritative gameplay,
physics, destruction and replication. Clients submit inputs; servers validate
ownership, timing and bounds and resolve damage. Identity/session discovery stays
separate from simulation authority. Capacity comes from measured workloads rather
than comparison names such as Battlefield or Arma.

Browser play initially streams the native runtime; it does not prove a WASM build.
Existing deployment uses starbase's GTX 1060; the workstation RTX 3060 is not a
hosted-session server. Audit that gateway before reusing it for exported games.

Browser editing requires creator enablement enforced by the service. Edits live
in an isolated fork/overlay, without production credentials. Validate assets and
bound resource use before preview/build; publish an immutable version before
other players join it. Arbitrary player native code must not execute in trusted
production servers. Editor/toolchain dependencies need not ship with a player.

## Delivery practices

Keep the existing Linux, Windows and macOS builds, ASan/UBSan, TSan, SDK-consumer,
editor and formatting gates. Each shipping brick adds the relevant artifact proof
instead of treating compilation as shipment. Identify logs and artifacts by source
revision, target, configuration and compatibility identity. Preserve failures and
unmet gates explicitly. External deployment happens only from a concrete tested
artifact through an appropriate release step.

An independent Astra consultation recommended this sequence, especially the
small packaging boundary before a new network launcher. The implementation owner
reviewed that advice and retains responsibility for source changes and proofs.
