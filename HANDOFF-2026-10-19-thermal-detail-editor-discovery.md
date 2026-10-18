# Handoff — thermal detail and editor discovery, 2026-10-19

Parent a1fc1d2 (courtyard #341); branch feat/thermal-ripples-and-editor-navigation.
Previous handoffs remain history; this checkpoint adds the user's follow-ups.

Working: blue frost veins, water normal-map ripples that weaken with freezing,
committed cosmetic elapsed time (variable dt supported), Pause/refusal holding and
reset semantics. Immutable GPU maps owned above render/fluids_scene, borrowed only
into private material copies. 74 maps/~6.5 MB startup cost; no shader/geometry ABI.

Editor: wheel dolly, visible hold-right-mouse + WASD help. User launcher actually
opened the correct scene; user released right mouse before movement, explaining
that report. Outliner prioritizes role names over MeshRef/MaterialRef, supports
Effects/Thermal filters, vector type icons and Display → List / Tiles (20/32 px).
Bodyless emitters select by icon/label and move through LocalTransform/gizmo.
Assets is cooked content, not a prefab library. Fixed 960×540 stretched stream,
persistent custom naming, asset thumbnails and emitter viewport icons remain open.
Creator guide: docs/creator-workflow.md, with Unity/EA primary-source workflow,
actual hello-game player/trigger/export foundations and missing authoring links.

Verification: full build pass; CTest92/93 only same inherited block collapse/locality
claims; adapter5/7142; ASan/UBSan samepass (GPU LSan off per CI), CPU LSan3/631pass;
Clang seven TUs; GUI128pass; Rust workspace/default+GUI Clippy/format/license pass.
Real editor lifecycle + native endpoint/controls pass. Final whole smoke passes
with an explicit out-of-viewport negative pick for the fully backed courtyard.
Initial native/software whole smokes failed because black shadow geometry was
mistaken for empty background; both failures retained, not claimed green. Actual GUI source/icon view
captured on OWN Xvfb; those own processes stopped. No performance run on this
shared build/CI host; gates remain open and nothing merged. Evidence:
docs/verification/thermal-ripples-2026-10-19/; scratch matching /home/next/reports/.
Astra consulted $0.03019 (live usage delta matches); root owns complete review.

## Active next user request — not complete at this checkpoint

User now wants city+weapon scene in editor, actual game launch from editor, and a
binary export after these requests. Preserve this objective; don't stop at the
thermal demo. Existing the-block-host previews intact saved block, not full weapon
runtime. the_block --play owns network/player/weapon presentation. --scene strict
loads exactly supplied content; city expansion only happens for empty scene path.
Need saved city generator using SAME city::expand, preview procedural lamp/barrier
patterns, exact-save-confirmed external Run Game, and honest development export.
Existing export-game.sh is audited hello_game-only; don't claim city release-ready.
Read samples/99-the-block/block_host_main.cpp, main.cpp, play_city.* and existing
export manifest/provenance tools. Game viewmodel directory currently compiled in.

Independent new launch/export consults already completed (don't repeat):
/home/next/reports/rime-city-editor-launch-2026-10-19/ has Astra ($.02713) and Fable
($.05224) answers, raw prompts and usage. They recommend serialized fresh save
ack before spawning argv (no shell), explicit --game config, child status/duplicate
refusal, shared city expansion and executable-relative content with notices.
Fable's suggestion to bake procedural previews as plain entities needs scrutiny:
runtime MeshRef/MaterialRef are not persistent content, and loss of destructible
patterns would change the game. Prefer game-specific host preview from same CPU
pattern assets. Own implementation, test every recommendation, no delegation.
Live gateway at pre-call: plan $6.39remaining, premium2.29/5.80used; user taskceiling$7.

Don't touch old untracked notes. Explicit stage, pinned exact-set clang-format,
new-TU actual Clang, proportional checks, self-review, draft PRs via gh api; no
merge while gates open. Global actual rules path:
/home/next/.claude-switch/profiles/default/CLAUDE.md. Keep >=15GB free (17GB now).
