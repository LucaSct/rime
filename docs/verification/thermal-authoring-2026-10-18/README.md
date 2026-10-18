# Thermal editor authoring verification — 2026-10-18

Branch `feat/thermal-editor-authoring`, based on
`c1ca901f2210a55f09097753738d1f66b6e1b1fe` (`feat/bounded-thermal-field`, draft #339).
Implementation commit is recorded in the following handoff/evidence commit.
Nothing merged. No native hardware benchmark or visible ice surface is claimed.

| Check | Measured result |
| --- | --- |
| Full RelWithDebInfo build | pass, exit 0 |
| New C++ adapter/codec tests | 9 cases / 395 assertions, pass |
| ASan + UBSan, new tests | 9 cases / 395 assertions, pass |
| Actual-flag Clang syntax | preview, codec, tests, host and worldkit TUs pass |
| Full Rust workspace tests | pass; 4 pre-existing ignored tests |
| GUI editor tests | 122 pass |
| Exact Clippy `--all-targets -- -D warnings` | default workspace and GUI editor pass |
| Rustfmt / pinned C++ format / license / whitespace | pass |
| Full lavapipe CTest | **89/91**, exit 8, 287.76 s |
| Isolated Xvfb windowed-input recheck | **1/1 pass**, .75 s |
| Installed SDK smoke | pass, including thermal update and codec |
| Separate installed thermal/codec consumer | pass |
| Headless thermal author/edit/inverse/redo/save | pass |
| Fresh-process viewport reload and numerical lifecycle | pass |
| Combined smoke/fire + thermal authoring/save | pass |

## Real engine behavior

`thermal.rscene` is the actual engine-saved authored graph. A separate viewport
host loaded 8 entities / 19 components / **0 skipped**, retaining remapped cell
references. The smoke receives each Step acknowledgement followed by matching
host-tick/revision diagnostics; it does not substitute an optimistic local mirror.

Two fixed 1 kg cells have a 100 W/K contact. The target starts as ice at 273.15 K,
with explicit +500,000 W. Twelve 1/60 s steps produce liquid mass fraction
**0.300448** at 273.15 K. Pause preserves it. Disabling that source resets the
whole graph; twelve contact-only steps produce fraction **0.000598**, comfortably
below the test's 1/100 sensitivity requirement. Both runs use the same twelve 1/60 s steps and initial states; only the explicit
source is disabled.

Paused edits then initialize the target as liquid and set -500,000 W. Twelve
steps leave fraction **0.700748**, decreasing each step at the same latent
plateau. An excessive positive power refuses the whole step with SourceRefused /
OutOfRange, field tick and accepted-host tick zero, and initial 333,500 J retained.
An invalid authored temperature clears rows. Stop clears diagnostics; fresh Play
has a larger session ID and bit-equal first-step energy to the original Play.
The host recorded 37 accepted thermal ticks, 1 numerical refusal and 1 invalid
build. No partial energy or runtime buffers were saved.

The GUI uses headless AccessKit widget tests for selected-cell numeric labels,
human refusal text and zero authored commands; session tests reject older
sessions/revisions, wrong phase/tick and malformed payloads. This is not a
clicked thermal creation/save/reopen test against the real engine. Existing
smoke/fire authoring also passes alongside the new components in one saved scene.

## Failure analysis and limits

The full suite retains both failures honestly:

- `the_block_headless`: same three inherited collapse/locality assertions (far
  side untouched, most block standing, local damage). Thermal preview is not
  consumed by that sample; no thresholds changed.
- `first_light_windowed_input`: its key-up stop assertion failed in the full run
  on the ambient display. An isolated unchanged-binary run with `DISPLAY=:99`
  passed 1/1. Display/environment sensitivity is observed; the exact cause was
  not established. This does not turn the full run into 90/91 or a green suite.

Read actual parent #339 CI: Linux job 117777298704 and ASan job 117777298823,
run 39162080599, each failed **only** inherited `the_block_headless` claims.
Filtered failures are retained. Parent SDK, TSan and lint checks succeeded;
hosted checks were skipped. No three-OS/new-branch CI success is inferred.

The sanitizer script reconstructs flags from the actual compile database and
instruments the adapter, codec, test TU and all three numerical thermal runtime
TUs. Remaining static engine libraries are ordinary release dependencies; this
is not a whole-engine sanitizer build. New/changed TUs listed above separately
pass Clang syntax with the documented doctest compatibility suppressions.

The installed SDK check first exposed missing `libsvtav1`/`dav1d` imported-target
discovery when linking editorhost. Package config now re-finds both; the repository
SDK consumer itself exercises the thermal API and optional codec. Both it and a
separate installed consumer pass. Merely linking a render-only consumer had not
covered that dependency closure.

Astra response cost and exact shared gateway delta both **$0.04958**; public
prompt/answer/usage are retained. No new Fable consultation. See the
[research/decision note](../../research/2026-10-18-thermal-editor-authoring.md) for
adopted/rejected suggestions. Root completed hunk-by-hunk self-review; Astra did
not review the final code. Initial GUI lint found items after a test module;
functions were moved and exact GUI Clippy rerun passed.

M18 budget/clock qualification, city convergence, visible thermal surfaces,
moving-water mass/energy transport, ice geometry/collision, combustion authority,
clicked creation, asset/export and multiplayer proofs remain open.

## Reproduce and provenance

See [module commands](../../../engine/fluids_scene/README.md#reproduce-the-live-proof).
Boundary commands included `cmake --build --preset release -j 3`, full CTest with
lavapipe at `-j 2`, `cargo test --workspace`, `cargo test -p editor --bins --features
gui`, default and GUI exact Clippy, and `scripts/sdk-smoke.sh --preset release`.
`verify.py` must be copied to a scratch directory before running: it writes ASan
objects/binary/logs beside itself and uses the checkout's real compile database.

`provenance.json` hashes relevant sources and actual tested binaries after the
SDK rebuild. Source and binary artifacts remain at
`/home/next/reports/rime-thermal-authoring-2026-10-18/`; logs and the small authored
scene/fixture are retained here. No private credential/credit files are committed.
