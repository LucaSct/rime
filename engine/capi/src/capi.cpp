// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Implementation of the Rime C ABI (rime/capi/rime.h). This is the ONE translation unit where the
// engine's C++ world meets the flat C boundary, so it is also where the boundary's discipline is
// enforced: C++ exceptions are caught here and turned into RimeStatus + a thread-local message (an
// exception escaping into C is undefined behavior), and object lifetimes cross as opaque pointers
// the host frees through paired `_destroy` calls. Nothing here is a hot path — it is a thin,
// defensive shim over the real modules (rime::core, rime::assets, rime::app).

#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/capi/rime.h"
#include "rime/core/version.hpp"

namespace {

// The last-error text is THREAD-LOCAL: two threads calling into the ABI keep independent error
// state, so one thread's failure message can never be read (or clobbered) by another. rime_capi
// does not spawn threads; this simply makes the boundary safe for a multi-threaded host to use.
thread_local std::string g_last_error;

void set_error(std::string message) {
    g_last_error = std::move(message);
}

void clear_error() {
    g_last_error.clear();
}

// Read a whole file into bytes. Returns false and sets the error on any I/O failure. Kept local
// (not via rime::platform) so the validate path depends only on core+assets — the smallest surface.
bool read_file_bytes(const char* path, std::vector<std::byte>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        set_error(std::string("cannot open file: ") + path);
        return false;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        set_error(std::string("cannot read file: ") + path);
        return false;
    }
    return true;
}

rime::app::Application* as_app(RimeApp* app) {
    return reinterpret_cast<rime::app::Application*>(app);
}

} // namespace

extern "C" {

RimeVersion rime_version(void) {
    clear_error();
    const auto v = rime::core::kVersion;
    return RimeVersion{v.major, v.minor, v.patch};
}

RimeStatus rime_asset_validate(const char* path, RimeAssetInfo* out_info) {
    clear_error();
    if (path == nullptr || out_info == nullptr) {
        set_error("rime_asset_validate: path and out_info must be non-NULL");
        return RIME_ERR_INVALID_ARGUMENT;
    }
    *out_info = RimeAssetInfo{};
    try {
        std::vector<std::byte> bytes;
        if (!read_file_bytes(path, bytes)) {
            return RIME_ERR_IO;
        }

        // Read + validate the RMA1 header first (magic, version, kind, schema hash, payload size).
        std::span<const std::byte> payload;
        rime::assets::AssetError header_err{};
        const auto header = rime::assets::read_header(bytes, payload, header_err);
        if (!header) {
            set_error(std::string("not a valid RMA1 cooked asset: ") +
                      std::string(rime::assets::to_string(header_err)));
            return RIME_ERR_ASSET_INVALID;
        }
        out_info->kind = static_cast<uint32_t>(header->kind);
        out_info->schema_hash = header->type_schema_hash;

        // For a mesh, decode the whole payload so the counts are real and the trust-nothing reader
        // runs end to end — this is the path the cross-language golden-fixture test exercises.
        // Other kinds are header-validated only in v1 (counts stay 0), per docs/design/ffi.md.
        if (header->kind == rime::assets::AssetKind::Mesh) {
            rime::assets::AssetError err{};
            const auto mesh = rime::assets::read_mesh(bytes, err);
            if (!mesh) {
                set_error(std::string("mesh invalid: ") +
                          std::string(rime::assets::to_string(err)));
                return RIME_ERR_ASSET_INVALID;
            }
            out_info->vertex_count = mesh->vertex_count;
            out_info->index_count = static_cast<uint32_t>(mesh->indices.size());
        }
        return RIME_OK;
    } catch (const std::exception& e) {
        set_error(std::string("internal error: ") + e.what());
        return RIME_ERR_INTERNAL;
    } catch (...) {
        set_error("internal error: unknown exception");
        return RIME_ERR_INTERNAL;
    }
}

RimeApp* rime_app_create_headless(void) {
    clear_error();
    try {
        // The C ABI owns this object and hands the host an opaque pointer; the host returns it via
        // rime_app_destroy. A smart pointer cannot cross a C boundary, so `new`/`delete` here is
        // the correct tool (and the deliberate exception to the engine's no-raw-new rule) — the
        // handoff of ownership is exactly what the ABI expresses.
        return reinterpret_cast<RimeApp*>(new rime::app::Application(rime::app::AppConfig{}));
    } catch (const std::exception& e) {
        set_error(std::string("rime_app_create_headless: ") + e.what());
        return nullptr;
    } catch (...) {
        set_error("rime_app_create_headless: unknown exception");
        return nullptr;
    }
}

RimeStatus rime_app_tick(RimeApp* app, uint32_t frames) {
    clear_error();
    if (app == nullptr) {
        set_error("rime_app_tick: app must be non-NULL");
        return RIME_ERR_INVALID_ARGUMENT;
    }
    try {
        as_app(app)->run_frames(static_cast<int>(frames));
        return RIME_OK;
    } catch (const std::exception& e) {
        set_error(std::string("rime_app_tick: ") + e.what());
        return RIME_ERR_INTERNAL;
    } catch (...) {
        set_error("rime_app_tick: unknown exception");
        return RIME_ERR_INTERNAL;
    }
}

void rime_app_destroy(RimeApp* app) {
    // No clear_error() here: destruction is not a query, and a host may call it while cleaning up
    // after an error it still wants to read. delete on nullptr is a no-op, so this is null-safe.
    delete as_app(app);
}

const char* rime_last_error_message(void) {
    return g_last_error.c_str();
}

} // extern "C"
