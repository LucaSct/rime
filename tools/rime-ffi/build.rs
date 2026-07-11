// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Link discovery for rime-ffi. The engine's C ABI (librime_capi) is produced by the CMake build, not
// cargo — so scripts/build.sh exports its directory as RIME_CAPI_DIR before running the cargo step,
// and we wire the linker up to it here.
//
// The key design choice: EVERYTHING is conditional on RIME_CAPI_DIR being set. When it is, we emit a
// `capi_available` cfg (the FFI bindings + their live tests compile in) plus the link-search, link,
// and an rpath so the test binary finds the .so at runtime without LD_LIBRARY_PATH. When it is NOT
// set — a bare `cargo test` in a checkout with no engine build — we emit nothing: the bindings and
// tests compile out and the crate still builds and "passes" green. That is what lets the Rust
// workspace be tested standalone while CI (which sets RIME_CAPI_DIR) runs the real cross-language
// proof. Windows DLL discovery has no rpath equivalent, so build.sh sets RIME_CAPI_DIR on
// Linux/macOS only; on Windows the crate cleanly skips (a documented v1 gap — see docs/design/ffi.md).
fn main() {
    // Declare the custom cfg so the compiler doesn't warn about it being "unexpected".
    println!("cargo:rustc-check-cfg=cfg(capi_available)");
    println!("cargo:rerun-if-env-changed=RIME_CAPI_DIR");

    if let Ok(dir) = std::env::var("RIME_CAPI_DIR") {
        println!("cargo:rustc-cfg=capi_available");
        println!("cargo:rustc-link-search=native={dir}");
        println!("cargo:rustc-link-lib=dylib=rime_capi");
        // Bake the directory as a runtime search path into the test binary. Linux honours -rpath
        // directly; macOS resolves it for the dylib's @rpath install name the same way.
        #[cfg(not(target_os = "windows"))]
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        println!("cargo:rerun-if-changed={dir}");
    }
}
