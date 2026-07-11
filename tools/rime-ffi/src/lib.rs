// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Safe Rust bindings to the Rime engine's C ABI (`librime_capi`, `engine/capi`, M6.9).
//!
//! This crate is how Rust tooling drives the engine without touching a C++ symbol — the concrete
//! Rust end of the ADR-0001 stable-C-ABI boundary. The surface is deliberately tiny (see
//! `rime/capi/rime.h`): read the engine version, validate a cooked asset with the engine's OWN
//! reader (cross-language honesty about the RMA1 format), and spin a headless [`App`].
//!
//! Everything here is gated on `cfg(capi_available)`, which `build.rs` sets only when the
//! `RIME_CAPI_DIR` environment variable points at a built `librime_capi`. Without it the crate still
//! compiles and its tests pass (they print a skip) — so a bare `cargo test` in a checkout with no
//! engine build stays green, while CI (which sets `RIME_CAPI_DIR` via `scripts/build.sh`) runs the
//! real linked proof.

#[cfg(capi_available)]
mod sys {
    //! Raw, hand-written `extern "C"` declarations mirroring `rime/capi/rime.h` one-to-one.
    use std::os::raw::{c_char, c_uint};

    #[repr(C)]
    pub struct RimeVersion {
        pub major: u32,
        pub minor: u32,
        pub patch: u32,
    }

    #[repr(C)]
    pub struct RimeAssetInfo {
        pub kind: u32,
        pub schema_hash: u64,
        pub vertex_count: u32,
        pub index_count: u32,
    }

    /// Opaque — the engine owns the `Application`; we only ever hold the pointer.
    #[repr(C)]
    pub struct RimeApp {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn rime_version() -> RimeVersion;
        pub fn rime_asset_validate(path: *const c_char, out_info: *mut RimeAssetInfo) -> i32;
        pub fn rime_app_create_headless() -> *mut RimeApp;
        pub fn rime_app_tick(app: *mut RimeApp, frames: c_uint) -> i32;
        pub fn rime_app_destroy(app: *mut RimeApp);
        pub fn rime_last_error_message() -> *const c_char;
    }
}

#[cfg(capi_available)]
pub use api::*;

#[cfg(capi_available)]
mod api {
    use super::sys;
    use std::ffi::{CStr, CString};

    /// The engine's semantic version.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct Version {
        pub major: u32,
        pub minor: u32,
        pub patch: u32,
    }

    /// What the engine's reader learned about a cooked asset (see [`asset_validate`]).
    #[derive(Debug, Clone)]
    pub struct AssetInfo {
        /// The RMA1 asset kind (1=mesh, 2=texture, 3=material, 4=skeleton, 5=animation clip).
        pub kind: u32,
        /// The payload's `type_schema_hash` — the cross-language format fingerprint.
        pub schema_hash: u64,
        /// Vertex count (meshes only; 0 otherwise).
        pub vertex_count: u32,
        /// Index count (meshes only; 0 otherwise).
        pub index_count: u32,
    }

    /// The message describing the most recent failing call on this thread ("" if none).
    pub fn last_error() -> String {
        // Safety: the C ABI guarantees a non-NULL, NUL-terminated, engine-owned string valid until
        // the next Rime call on this thread; we copy it immediately into an owned String.
        unsafe {
            CStr::from_ptr(sys::rime_last_error_message())
                .to_string_lossy()
                .into_owned()
        }
    }

    /// The engine's compile-time version.
    pub fn version() -> Version {
        // Safety: no arguments, no ownership — a plain by-value struct return.
        let v = unsafe { sys::rime_version() };
        Version {
            major: v.major,
            minor: v.minor,
            patch: v.patch,
        }
    }

    /// Validate a cooked RMA1 file on disk with the engine's own reader. `Ok` with the parsed info
    /// when the file is a well-formed asset this build understands; `Err(message)` otherwise.
    pub fn asset_validate(path: &str) -> Result<AssetInfo, String> {
        let c_path =
            CString::new(path).map_err(|_| "path contains an interior NUL byte".to_string())?;
        let mut info = sys::RimeAssetInfo {
            kind: 0,
            schema_hash: 0,
            vertex_count: 0,
            index_count: 0,
        };
        // Safety: c_path is a valid NUL-terminated string; &mut info is a valid writable pointer for
        // the call's duration. The engine reads the path and writes info; it frees nothing of ours.
        let status = unsafe { sys::rime_asset_validate(c_path.as_ptr(), &mut info) };
        if status == 0 {
            Ok(AssetInfo {
                kind: info.kind,
                schema_hash: info.schema_hash,
                vertex_count: info.vertex_count,
                index_count: info.index_count,
            })
        } else {
            Err(last_error())
        }
    }

    /// A headless engine `Application` handle. Ticks the fixed-step loop; frees the underlying
    /// engine object on `Drop` (RAII over the C `rime_app_destroy`).
    pub struct App {
        raw: *mut sys::RimeApp,
    }

    impl App {
        /// Create a headless (no device, 60 Hz sim) application.
        pub fn create_headless() -> Result<App, String> {
            // Safety: returns an engine-owned pointer or NULL; we take ownership of a non-NULL one
            // and release it exactly once in Drop.
            let raw = unsafe { sys::rime_app_create_headless() };
            if raw.is_null() {
                Err(last_error())
            } else {
                Ok(App { raw })
            }
        }

        /// Advance the fixed-tick loop by `frames` iterations.
        pub fn tick(&mut self, frames: u32) -> Result<(), String> {
            // Safety: self.raw is a live engine pointer for the lifetime of self.
            let status = unsafe { sys::rime_app_tick(self.raw, frames) };
            if status == 0 {
                Ok(())
            } else {
                Err(last_error())
            }
        }
    }

    impl Drop for App {
        fn drop(&mut self) {
            // Safety: raw was created by rime_app_create_headless and not freed elsewhere; destroy is
            // null-safe and called exactly once.
            unsafe { sys::rime_app_destroy(self.raw) };
        }
    }
}

#[cfg(test)]
mod tests {
    // Each test skips loudly when the library isn't linked in (RIME_CAPI_DIR unset), so a bare
    // `cargo test` stays green while CI runs the real thing. The mesh fixture is the same one the
    // C++ cross-language test consumes — proving both languages agree on the cooked bytes.
    #[cfg(capi_available)]
    const QUAD_RMESH: &str = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../tests/assets/fixtures/quad.rmesh"
    );
    #[cfg(capi_available)]
    const MESH_SCHEMA_HASH: u64 = 0x198738A2DDE250AC; // pinned in engine/assets + the Rust cooker

    macro_rules! skip_if_unavailable {
        () => {
            #[cfg(not(capi_available))]
            {
                eprintln!(
                    "SKIP: RIME_CAPI_DIR not set — no librime_capi to link, skipping live FFI test"
                );
                return;
            }
        };
    }

    #[test]
    fn version_matches_workspace() {
        skip_if_unavailable!();
        #[cfg(capi_available)]
        {
            let v = super::version();
            assert_eq!(
                (v.major, v.minor, v.patch),
                (0, 0, 1),
                "the C ABI must report the workspace version 0.0.1"
            );
        }
    }

    #[test]
    fn validates_the_golden_mesh_fixture() {
        skip_if_unavailable!();
        #[cfg(capi_available)]
        {
            let info = super::asset_validate(QUAD_RMESH)
                .expect("quad.rmesh must validate through the engine's reader");
            assert_eq!(info.kind, 1, "quad.rmesh is a mesh (AssetKind::Mesh)");
            assert_eq!(
                info.schema_hash, MESH_SCHEMA_HASH,
                "the C ABI must report the same mesh schema hash both languages pinned"
            );
            assert!(
                info.vertex_count > 0 && info.index_count > 0,
                "a mesh has vertices and indices"
            );
            // The quad is two triangles: 4 unique vertices, 6 indices.
            assert_eq!((info.vertex_count, info.index_count), (4, 6));
        }
    }

    #[test]
    fn rejects_a_corrupt_file_with_a_message() {
        skip_if_unavailable!();
        #[cfg(capi_available)]
        {
            let path = std::env::temp_dir().join("rime_ffi_corrupt.bin");
            std::fs::write(&path, b"this is not an RMA1 cooked asset").unwrap();
            let err = super::asset_validate(path.to_str().unwrap())
                .expect_err("garbage bytes must not validate");
            assert!(
                !err.is_empty(),
                "a failure must carry a human-readable message"
            );
            let _ = std::fs::remove_file(&path);
        }
    }

    #[test]
    fn headless_app_create_tick_destroy() {
        skip_if_unavailable!();
        #[cfg(capi_available)]
        {
            let mut app = super::App::create_headless().expect("headless app must create");
            app.tick(3).expect("ticking the headless loop must succeed");
            // Drop frees it; running clean under ASan (the C++ side) is the leak/UAF check.
        }
    }
}
