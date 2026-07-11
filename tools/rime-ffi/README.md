# tools/rime-ffi — safe Rust bindings to the engine's C ABI

`rime-ffi` is how Rust tooling drives the Rime engine **without touching a single C++ symbol** — the
concrete Rust end of [ADR-0001](../../docs/adr/0001-cpp-core-rust-tooling.md)'s stable-C-ABI boundary.
It binds `librime_capi` ([`engine/capi`](../../engine/capi), M6.9) and wraps its raw `extern "C"`
surface in safe, idiomatic Rust.

The surface is deliberately tiny (it mirrors `rime/capi/rime.h` one-to-one in a private `sys` module):

- `version() -> Version` — the engine's semantic version.
- `asset_validate(path) -> Result<AssetInfo, String>` — validate a cooked `RMA1` file with the
  engine's **own** reader (cross-language honesty about the format — Rust cooked it, C++ checks it).
- `App::create_headless()` / `App::tick(frames)` — stand up and step a headless engine app; `Drop`
  calls `rime_app_destroy`, so ownership is RAII-clean on the Rust side even though the engine owns the
  memory.

Failures come back as `Err(String)`, carrying the engine's `rime_last_error_message()` text — no
`RimeStatus` codes leak into the Rust API.

## The `RIME_CAPI_DIR` gate (why a bare `cargo test` is still green)

`librime_capi` is produced by the **CMake** build, not cargo. So everything in this crate is gated on
the `capi_available` cfg, which [`build.rs`](build.rs) emits **only** when the `RIME_CAPI_DIR`
environment variable points at a built library:

- **Set** (CI, and any run through [`scripts/build.sh`](../../scripts/build.sh), which exports it after
  the C++ build): `build.rs` adds the link-search, links `dylib=rime_capi`, and bakes an **rpath** so
  the test binary finds the `.so`/`.dylib` at runtime without `LD_LIBRARY_PATH`. The bindings and
  their **live linked tests** compile in — the real cross-language proof runs.
- **Unset** (a bare `cargo test` in a checkout with no engine build): `build.rs` emits nothing, the
  bindings and tests compile out, and the crate still builds and passes (the tests print a skip). This
  is what lets the Rust workspace be tested standalone.

Windows DLL discovery has no rpath equivalent, so `build.sh` sets `RIME_CAPI_DIR` on Linux/macOS only;
on Windows the crate cleanly skips — a documented v1 gap. See
[docs/design/ffi.md](../../docs/design/ffi.md) for the boundary's full rationale.

## Running the real proof locally

```
# builds the C++ engine (incl. librime_capi), then the Rust workspace with RIME_CAPI_DIR set
scripts/build.sh
```
