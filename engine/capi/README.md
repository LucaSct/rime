# engine/capi — the C ABI (`librime_capi`)

`rime::capi` is Rime's **stable, language-agnostic boundary**: a deliberately tiny `extern "C"`
surface over a shared library that is the *only* supported way non-C++ code — the Rust editor and
tools (M9), or any host with a C FFI — drives the engine at runtime. It is the concrete form of
[ADR-0001](../../docs/adr/0001-cpp-core-rust-tooling.md)'s rule: **tools talk to the engine across a
C ABI, files, or a protocol — never by reaching into C++ internals.**

C++ names, exceptions, templates, and ownership all stay on the far side of this seam. The header is
**C99-clean** (a CI job compiles it as C), so any language that can call C can bind Rime.

## The surface (M6.9)

The whole ABI is a handful of functions in [`include/rime/capi/rime.h`](include/rime/capi/rime.h):

| Function | What it does |
| --- | --- |
| `rime_version()` | the library's semantic version (an ABI-probe you can call before anything else) |
| `rime_asset_validate(path, out_info)` | open + fully validate a cooked `RMA1` file; fill a small info struct (kind, counts) |
| `rime_app_create_headless()` | stand up a headless `Application` (no device), returning an opaque `RimeApp*` |
| `rime_app_tick(app, frames)` | advance the fixed-tick loop N frames |
| `rime_app_destroy(app)` | free the app (paired with `_create`) |
| `rime_last_error_message()` | the human-readable detail behind the last non-OK `RimeStatus` (thread-local) |

The `rime-ffi` Rust crate ([`tools/rime-ffi`](../../tools/rime-ffi)) binds exactly this and drives the
engine's own loader in its tests — the boundary's live proof.

## The discipline this seam keeps

Spelled out at the top of the header and in [docs/design/ffi.md](../../docs/design/ffi.md):

- **The engine owns all memory.** Every pointer the host receives is engine-owned and freed through a
  paired `_destroy`. The host never frees an engine pointer, and never hands the engine host-owned
  allocations to free. Returned strings are valid until the next call on the same thread.
- **No C++ exception may cross the boundary** (that is UB). Every function catches at the seam and
  reports failure as a `RimeStatus` code plus a message on `rime_last_error_message()`. The status
  enum stays tiny and append-only; the rich internal reason (which RMA1 check failed, etc.) is put on
  the error text, never flattened into more enumerators.
- **The ABI is append-only.** New functions and enumerators may be added; existing signatures, struct
  layouts, and enumerator values never change — a change is a *new symbol*. (Pre-1.0 this is a
  convention, not a frozen guarantee — see VISION's non-goals.)

## Build & install notes

This is Rime's **first shared library**, and it set two project-wide policies so the exported surface
is exactly the marked symbols and nothing of the C++ innards leaks:

- **position-independent code** across the engine (a shared lib links static archives that must be PIC),
- **hidden default visibility** — only symbols marked `RIME_CAPI_API` are exported (`-fvisibility=hidden`
  on GCC/Clang; `__declspec(dllexport/dllimport)` on Windows via the `RIME_CAPI_BUILD` define).

It installs as the `rime::capi` CMake target (part of the M6.8 SDK export). At runtime the `rime-ffi`
crate finds the library through `RIME_CAPI_DIR` (an rpath baked by its `build.rs` on Linux/macOS); on
Windows there is no rpath, so those FFI tests skip themselves — a documented v1 gap.
