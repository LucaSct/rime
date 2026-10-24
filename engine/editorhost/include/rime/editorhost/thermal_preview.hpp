// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rime::editorhost {
// Optional versioned diagnostics, never authoring blobs. Status mirrors the preview adapter
// without making editorhost depend on fluids/fluids_scene. Old peers skip its unknown message.
struct ThermalPreviewCell {
    std::uint64_t editor_id = 0;
    double temperature_kelvin = 0.0;
    double liquid_mass_fraction = 0.0;
    double energy_joules = 0.0;
};

struct ThermalPreviewMsg {
    static constexpr std::size_t max_cells = 16;
    std::uint8_t status = 0; // Empty, Ready, InvalidDescription, CapacityExceeded, StepRefused
    std::uint8_t phase = 0;  // Edit, Playing, Paused
    std::uint8_t field_status = 0, heat_status = 0, contact_status = 0;
    std::uint64_t session_id = 0, revision = 0, host_tick = 0, accepted_host_tick = 0,
                  field_tick = 0;
    std::uint64_t problem_editor_id = 0;
    std::uint32_t failed_substep = 0, failed_descriptor = 0;
    std::vector<ThermalPreviewCell> cells;
};

// Wire v1: six u8s (version,status,phase,field/heat/contact status), six u64s, two u32s,
// count:u8, then count*(editor_id:u64, temperature:f64, liquid mass fraction:f64, energy:f64).
// Invalid messages serialize to empty; parsing validates into a candidate and commits atomically.
[[nodiscard]] std::vector<std::byte> serialize_thermal_preview(const ThermalPreviewMsg& msg);
[[nodiscard]] bool parse_thermal_preview(std::span<const std::byte> payload,
                                         ThermalPreviewMsg& out);
} // namespace rime::editorhost
