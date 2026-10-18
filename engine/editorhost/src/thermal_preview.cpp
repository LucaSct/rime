// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/editorhost/thermal_preview.hpp"

#include <cmath>

#include "rime/core/byte_cursor.hpp"

namespace rime::editorhost {
namespace {
bool valid(const ThermalPreviewMsg& m) {
    if (m.status > 4 || m.phase > 2 || m.field_status > 8 || m.heat_status > 4 ||
        m.contact_status > 6 || m.cells.size() > ThermalPreviewMsg::max_cells ||
        m.accepted_host_tick > m.host_tick)
        return false;
    if ((m.status == 0 || m.status == 2 || m.status == 3) && !m.cells.empty())
        return false;
    if (m.status == 1 && m.cells.empty())
        return false;
    if (m.phase == 0 && (m.status != 0 || m.field_tick || m.accepted_host_tick || m.host_tick))
        return false;
    for (std::size_t i = 0; i < m.cells.size(); ++i) {
        const auto& c = m.cells[i];
        if (!c.editor_id || !std::isfinite(c.temperature_kelvin) || c.temperature_kelvin <= 0.0 ||
            !std::isfinite(c.liquid_mass_fraction) || c.liquid_mass_fraction < 0.0 ||
            c.liquid_mass_fraction > 1.0 || !std::isfinite(c.energy_joules))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (m.cells[j].editor_id == c.editor_id)
                return false;
    }
    return true;
}
} // namespace

std::vector<std::byte> serialize_thermal_preview(const ThermalPreviewMsg& m) {
    if (!valid(m))
        return {};
    std::vector<std::byte> out;
    out.reserve(63 + 32 * m.cells.size());
    core::ByteWriter w(out);
    w.u8(1);
    w.u8(m.status);
    w.u8(m.phase);
    w.u8(m.field_status);
    w.u8(m.heat_status);
    w.u8(m.contact_status);
    for (auto v : {m.session_id,
                   m.revision,
                   m.host_tick,
                   m.accepted_host_tick,
                   m.field_tick,
                   m.problem_editor_id})
        w.u64(v);
    w.u32(m.failed_substep);
    w.u32(m.failed_descriptor);
    w.u8(static_cast<std::uint8_t>(m.cells.size()));
    for (const auto& c : m.cells) {
        w.u64(c.editor_id);
        w.f64(c.temperature_kelvin);
        w.f64(c.liquid_mass_fraction);
        w.f64(c.energy_joules);
    }
    return out;
}

bool parse_thermal_preview(std::span<const std::byte> payload, ThermalPreviewMsg& out) {
    core::ByteReader r(payload);
    ThermalPreviewMsg m;
    std::uint8_t version = 0, count = 0;
    if (!r.u8(version) || version != 1 || !r.u8(m.status) || !r.u8(m.phase) ||
        !r.u8(m.field_status) || !r.u8(m.heat_status) || !r.u8(m.contact_status) ||
        !r.u64(m.session_id) || !r.u64(m.revision) || !r.u64(m.host_tick) ||
        !r.u64(m.accepted_host_tick) || !r.u64(m.field_tick) || !r.u64(m.problem_editor_id) ||
        !r.u32(m.failed_substep) || !r.u32(m.failed_descriptor) || !r.u8(count) ||
        count > ThermalPreviewMsg::max_cells || r.remaining() != 32u * count)
        return false;
    m.cells.resize(count);
    for (auto& c : m.cells)
        if (!r.u64(c.editor_id) || !r.f64(c.temperature_kelvin) || !r.f64(c.liquid_mass_fraction) ||
            !r.f64(c.energy_joules))
            return false;
    if (!valid(m))
        return false;
    out = std::move(m);
    return true;
}
} // namespace rime::editorhost
