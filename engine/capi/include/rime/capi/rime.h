/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 The Rime Engine Authors. */
#ifndef RIME_CAPI_RIME_H
#define RIME_CAPI_RIME_H

/*
 * Rime C ABI (M6.9) — the stable, language-agnostic boundary of ADR-0001.
 *
 * This is the ONLY way non-C++ code (the Rust editor/tools, or any FFI host) is meant to drive the
 * engine: a deliberately TINY `extern "C"` surface over `librime_capi`, with C++ names, exceptions,
 * templates, and ownership kept entirely on the far side. It is C99-clean (compiled as C in a test)
 * so any language with a C FFI can bind it.
 *
 * Discipline this boundary keeps (see docs/design/ffi.md):
 *  - The engine owns all memory. Every pointer the host receives is owned by the engine and freed
 *    through a paired `_destroy`; the host never frees an engine pointer, and never passes the host's
 *    own allocations for the engine to free. Strings returned are engine-owned and valid until the
 *    next call on the same thread (see rime_last_error_message).
 *  - No C++ exception may cross this boundary (that is undefined behavior). Every function catches
 *    at the seam and reports failure as a RimeStatus + a human message on rime_last_error_message.
 *  - The ABI is APPEND-ONLY. New functions/enumerators may be added; existing signatures, struct
 *    layouts, and enumerator values never change (a change is a new symbol). Pre-1.0 this is a
 *    convention, not a frozen guarantee (VISION non-goal) — but the shape is chosen to be keepable.
 */

#include <stddef.h>
#include <stdint.h>

/* Visibility/export. When building the shared library we export the marked symbols; when a C/C++
 * consumer includes this header we mark them imported (matters on Windows). Everything else in the
 * library is hidden (the CMake target sets -fvisibility=hidden), so the ABI surface is exactly the
 * functions marked here — nothing of the C++ innards leaks out. */
#if defined(_WIN32)
#  if defined(RIME_CAPI_BUILD)
#    define RIME_CAPI_API __declspec(dllexport)
#  else
#    define RIME_CAPI_API __declspec(dllimport)
#  endif
#else
#  define RIME_CAPI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Status codes ─────────────────────────────────────────────────────────────────────────────
 * A small, stable set. The engine's richer internal error (which RMA1 check failed, etc.) is not
 * flattened into more enumerators — it is put on rime_last_error_message() as text, so the enum
 * stays tiny and append-only while the human detail is never lost. */
typedef enum RimeStatus {
    RIME_OK = 0,
    RIME_ERR_INVALID_ARGUMENT = 1, /* a required pointer was NULL, or an argument was out of range */
    RIME_ERR_IO = 2,               /* the file could not be opened or read */
    RIME_ERR_ASSET_INVALID = 3,    /* the RMA1 reader rejected the file (reason in last-error text) */
    RIME_ERR_INTERNAL = 4          /* an unexpected C++ exception was caught at the boundary */
} RimeStatus;

/* Cooked-asset kinds, mirroring rime::assets::AssetKind. Values are ABI-stable. */
typedef enum RimeAssetKind {
    RIME_ASSET_MESH = 1,
    RIME_ASSET_TEXTURE = 2,
    RIME_ASSET_MATERIAL = 3,
    RIME_ASSET_SKELETON = 4,
    RIME_ASSET_ANIMATION_CLIP = 5
} RimeAssetKind;

/* ── Version ──────────────────────────────────────────────────────────────────────────────────*/
typedef struct RimeVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} RimeVersion;

/* The engine's compile-time version (matches rime::core::kVersion and the CMake project version). */
RIME_CAPI_API RimeVersion rime_version(void);

/* ── Asset validation ─────────────────────────────────────────────────────────────────────────
 * Run the engine's own cooked-asset reader over a file on disk — the cross-language honesty check:
 * a Rust cooker's output verified by the very loader that will consume it at runtime. Fills
 * `out_info` and returns RIME_OK when the file is a well-formed cooked asset this build understands;
 * on failure returns a status and leaves a description on rime_last_error_message(). For a mesh the
 * whole payload is decoded and `vertex_count`/`index_count` are filled; for other kinds the header
 * is validated and the counts are 0 (documented in docs/design/ffi.md). */
typedef struct RimeAssetInfo {
    uint32_t kind;         /* one of RimeAssetKind */
    uint64_t schema_hash;  /* the payload's type_schema_hash from the RMA1 header */
    uint32_t vertex_count; /* meshes only; 0 otherwise */
    uint32_t index_count;  /* meshes only; 0 otherwise */
} RimeAssetInfo;

RIME_CAPI_API RimeStatus rime_asset_validate(const char* path, RimeAssetInfo* out_info);

/* ── Headless application ─────────────────────────────────────────────────────────────────────
 * An opaque handle to a rime::app::Application built GPU-free (ADR-0023). Enough to prove the FFI
 * can spin the real engine loop from another language without any C++ or GPU in the host. */
typedef struct RimeApp RimeApp;

/* Create a headless (no device, 60 Hz sim) Application. Returns NULL on failure (see last-error). */
RIME_CAPI_API RimeApp* rime_app_create_headless(void);

/* Advance the fixed-tick loop by `frames` iterations off the real clock. */
RIME_CAPI_API RimeStatus rime_app_tick(RimeApp* app, uint32_t frames);

/* Destroy an Application created by rime_app_create_headless. Safe to call with NULL. */
RIME_CAPI_API void rime_app_destroy(RimeApp* app);

/* ── Errors ───────────────────────────────────────────────────────────────────────────────────
 * The message describing the most recent failing call ON THIS THREAD (the state is thread-local, so
 * concurrent callers don't clobber each other). Never NULL — "" when there is no error. The pointer
 * is engine-owned and valid until the next Rime call on this thread; copy it if you need to keep it.
 */
RIME_CAPI_API const char* rime_last_error_message(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RIME_CAPI_RIME_H */
