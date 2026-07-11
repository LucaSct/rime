# The Rime C ABI — `rime_capi` + the Rust FFI (design note)

This note documents the engine's **C ABI** (Milestone 6.9): the stable, language-agnostic boundary
[ADR-0001](../adr/0001-two-toolchains.md) promised, made real and deliberately *tiny*. A shared
library `librime_capi` exposes a handful of `extern "C"` functions (`engine/capi/include/rime/capi/rime.h`),
and a Rust crate [`tools/rime-ffi`](../../tools/rime-ffi/) calls them — proving Rust tooling (the
editor, the pipeline) can drive engine code without touching a single C++ symbol.

## The surface

Six functions, three concerns. That smallness is the point — the boundary is a *seam*, not the whole
engine API (which stays C++).

| Function | Does |
|----------|------|
| `rime_version()` | the engine's semantic version (matches `rime::core::kVersion`) |
| `rime_asset_validate(path, out_info*)` | run the engine's OWN RMA1 reader over a cooked file; report kind, schema hash, and (for meshes) vertex/index counts |
| `rime_app_create_headless()` / `rime_app_tick()` / `rime_app_destroy()` | create, step, and free a headless [`Application`](../adr/0023-app-loop.md) behind an opaque handle |
| `rime_last_error_message()` | the human message for the most recent failing call on this thread |

`rime_asset_validate` is the boundary's first real job, and a pointed one: the Rust *cooker*'s output
verified by the very C++ *loader* that consumes it at runtime — cross-language honesty about the
cooked format, with the same `mesh_schema_hash` (`0x198738A2DDE250AC`) asserted on both sides.

## The rules that keep it safe

These are enforced in `engine/capi/src/capi.cpp` and documented in the header so a binding author can
rely on them:

- **The engine owns all memory.** Every pointer the host receives is engine-owned and freed through a
  paired `_destroy`. The host never frees an engine pointer and never hands the engine host-allocated
  memory to free. Returned strings are engine-owned and valid until the next call on the same thread.
- **No C++ exception crosses the boundary** (that is undefined behavior). Every function catches at
  the seam and reports failure as a `RimeStatus` plus a message on `rime_last_error_message()`.
- **Errors are thread-local.** Two threads calling in keep independent last-error state.
- **The ABI is append-only.** New functions/enumerators may be added; existing signatures, struct
  layouts, and enumerator values never change (a change is a new symbol). Pre-1.0 this is a
  convention, not a frozen guarantee (a [VISION](../../VISION.md) non-goal) — but the shape is chosen
  to be keepable.

## The library shape

`librime_capi` is the **one shared library** in the build (every module is otherwise a static lib).
It links the engine's static modules **PRIVATE**: a shared library resolves those archives' symbols
*into* the `.so`, so nothing about `rime::app`/`rime::assets` appears in `rime_capi`'s link interface —
a consumer of the `.so` needs only the `.so`. Two project-wide CMake settings make this work and keep
it clean (both in the top-level `CMakeLists.txt`):

- **`POSITION_INDEPENDENT_CODE ON`** — linking a static archive into a shared object requires PIC
  objects. (Static linking into tests/samples/the SDK consumer is unaffected.)
- **hidden visibility by default** — the only symbols the `.so` exports are the few marked
  `RIME_CAPI_API` in the header; the C++ innards stay internal. (Visibility doesn't affect static
  linking, so this is free for the rest of the build.) *v1 note:* volk's loader symbols also export,
  because that third-party C is compiled with default visibility — harmless, and export-list polish
  is deferred (see "Out of scope").

The C face installs with the SDK ([M6.8](sdk.md)): `find_package(rime)` gives `rime::capi` (the `.so`
+ `rime/capi/rime.h`) like any other module.

## The Rust side (`tools/rime-ffi`)

Hand-written raw bindings (`sys`) plus a thin safe wrapper (`version()`, `asset_validate()`, an RAII
`App`) — no `bindgen` dependency; the surface is small enough to keep by hand. Discovery is by the
**`RIME_CAPI_DIR`** environment variable, which `scripts/build.sh` exports (pointing at the freshly
built library) before the cargo step:

- When it's set, `build.rs` emits the link search path, the link directive, an rpath so the test
  binary finds the `.so` at runtime, and a **`capi_available`** cfg. The bindings and their live tests
  compile in.
- When it's unset — a bare `cargo test` in a checkout with no engine build — `build.rs` emits nothing,
  the FFI compiles out, and the tests pass by **skipping with a loud message**. So the Rust workspace
  is testable standalone while CI runs the real linked proof.

**Cross-platform status (v1):** Linux and macOS resolve the library at test time via the rpath.
Windows has no rpath equivalent, so `build.sh` sets `RIME_CAPI_DIR` on Linux/macOS only and the Rust
live tests skip on Windows — the C++ `tests/capi` suite still covers the ABI on all three OSes (it
links the library through CMake, which handles the build-tree RPATH/DLL path). Windows DLL-discovery
polish is deferred.

## Protocol reservation (the editor channel)

The 07-02 plan put "reserve editor message-type space" in M6. `engine/stream`'s `MessageType` enum now
reserves `[0x0200, 0x02FF]` (`EditorReservedBegin`/`End`) for the M9 editor channel. M6 only reserves —
no handler exists. The reservation is safe because `recv_message()` carries an unknown type ID
transparently (it never rejects a type it doesn't recognize), so an old peer simply ignores a reserved
message; M9 will bump the handshake `version:u16` when the channel lands. A `tests/stream` case pins
this round-trip today.

## Out of scope for v1 (seams noted)

`bindgen`; callbacks or render/GPU access across the ABI (the boundary is allocation-free and
data-only by design); a stable-forever guarantee (pre-1.0); and Windows `.def`/exported-symbol-list
polish beyond what CI needs.

## Proof

- **cargo** (`tools/rime-ffi`, CI on Linux/macOS): `rime_version` matches the workspace version;
  `asset_validate` on the golden `quad.rmesh` returns kind=mesh, the pinned schema hash, and (4, 6);
  a corrupt file returns an error with a message; a headless app create→tick×3→destroy runs clean.
- **ctest** (`tests/capi`, all 3 OSes + the ASan/UBSan job): the same paths from C++, giving the
  trust-nothing validate path and the app lifecycle a sanitizer pass across the FFI boundary, plus
  null-argument rejection.
- **C99 header compile** (`cc -std=c99 -Wall -Wextra -Werror`) in the lint job — the header is usable
  from plain C, not just C++.

## Files

- [`engine/capi/`](../../engine/capi/) — the header, the implementation, the shared-library target.
- [`tools/rime-ffi/`](../../tools/rime-ffi/) — the Rust bindings + safe wrapper + tests.
- [`tests/capi/`](../../tests/capi/) — the C++ ABI suite.
- `scripts/build.sh` — the `RIME_CAPI_DIR` wiring before the cargo step.
