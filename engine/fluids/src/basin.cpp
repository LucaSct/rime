// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids/basin.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rime::fluids {
namespace {

[[nodiscard]] bool finite_cell(BasinCell c) noexcept {
    return std::isfinite(c.depth_metres) && std::isfinite(c.momentum_x) &&
           std::isfinite(c.momentum_z) && c.depth_metres >= 0.0 &&
           (c.depth_metres > Basin::dry_depth_metres ||
            (c.momentum_x == 0.0 && c.momentum_z == 0.0));
}

[[nodiscard]] double velocity(double momentum, double depth) noexcept {
    return depth > Basin::dry_depth_metres ? momentum / depth : 0.0;
}

} // namespace

std::optional<Basin> Basin::create(const BasinConfig& c) {
    if (c.width == 0 || c.height == 0 || c.width > max_cells / c.height || c.max_substeps == 0 ||
        c.max_substeps > max_allowed_substeps || !std::isfinite(c.origin_x_metres) ||
        !std::isfinite(c.origin_z_metres) || !std::isfinite(c.cell_spacing_metres) ||
        c.cell_spacing_metres <= 0.0 || !std::isfinite(c.bed_height_metres) ||
        !std::isfinite(c.initial_depth_metres) || c.initial_depth_metres < 0.0 ||
        !std::isfinite(c.gravity_m_s2) || c.gravity_m_s2 <= 0.0 ||
        !std::isfinite(c.density_kg_m3) || c.density_kg_m3 <= 0.0 || !std::isfinite(c.cfl) ||
        c.cfl <= 0.0 || c.cfl > 0.5 ||
        !std::isfinite(c.origin_x_metres + c.width * c.cell_spacing_metres) ||
        !std::isfinite(c.origin_z_metres + c.height * c.cell_spacing_metres) ||
        !std::isfinite(c.bed_height_metres + c.initial_depth_metres) ||
        !std::isfinite(c.cell_spacing_metres * c.cell_spacing_metres) ||
        !std::isfinite(c.gravity_m_s2 * c.initial_depth_metres))
        return std::nullopt;
    return Basin(c);
}

Basin::Basin(BasinConfig config)
    : config_(config), cells_(config.width * config.height, {config.initial_depth_metres, 0, 0}),
      work_(cells_.size()), next_(cells_.size()), x_flux_(config.height * (config.width + 1)),
      z_flux_((config.height + 1) * config.width), outgoing_(cells_.size()),
      drain_scale_(cells_.size()) {}

std::size_t Basin::index(std::size_t x, std::size_t z) const noexcept {
    return z * config_.width + x;
}

std::optional<BasinCell> Basin::cell(std::size_t x, std::size_t z) const noexcept {
    if (x >= config_.width || z >= config_.height)
        return std::nullopt;
    return cells_[index(x, z)];
}

bool Basin::set_cell(std::size_t x, std::size_t z, BasinCell value) noexcept {
    if (x >= config_.width || z >= config_.height || !finite_cell(value) ||
        !std::isfinite(config_.bed_height_metres + value.depth_metres) ||
        !std::isfinite(config_.gravity_m_s2 * value.depth_metres) ||
        !std::isfinite(value.momentum_x * value.momentum_x) ||
        !std::isfinite(value.momentum_z * value.momentum_z))
        return false;
    cells_[index(x, z)] = value;
    return true;
}

std::optional<WaterSurfaceSample> Basin::sample(double x, double z) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(z) || x < config_.origin_x_metres ||
        z < config_.origin_z_metres)
        return std::nullopt;
    const double gx = (x - config_.origin_x_metres) / config_.cell_spacing_metres;
    const double gz = (z - config_.origin_z_metres) / config_.cell_spacing_metres;
    if (!(gx >= 0.0 && gx < static_cast<double>(config_.width) && gz >= 0.0 &&
          gz < static_cast<double>(config_.height)))
        return std::nullopt;
    const auto& c = cells_[index(static_cast<std::size_t>(gx), static_cast<std::size_t>(gz))];
    const double height = config_.bed_height_metres + c.depth_metres;
    const double vx = velocity(c.momentum_x, c.depth_metres);
    const double vz = velocity(c.momentum_z, c.depth_metres);
    const double fmax = std::numeric_limits<float>::max();
    if (!std::isfinite(height) || std::abs(height) > fmax || !std::isfinite(vx) ||
        std::abs(vx) > fmax || !std::isfinite(vz) || std::abs(vz) > fmax ||
        config_.density_kg_m3 > fmax)
        return std::nullopt;
    WaterSurfaceSample result{};
    result.height_metres = static_cast<float>(height);
    result.velocity_m_s = {static_cast<float>(vx), 0.0f, static_cast<float>(vz)};
    result.density_kg_m3 = static_cast<float>(config_.density_kg_m3);
    result.authoritative_tick = tick_;
    return result;
}

double Basin::total_volume_m3() const noexcept {
    double sum = 0.0;
    double correction = 0.0;
    for (const auto& c : cells_) {
        const double y = c.depth_metres - correction;
        const double t = sum + y;
        correction = (t - sum) - y;
        sum = t;
    }
    return sum * config_.cell_spacing_metres * config_.cell_spacing_metres;
}

double Basin::stable_dt(const std::vector<BasinCell>& cells) const noexcept {
    double max_sum_speed = 0.0;
    for (const auto& c : cells) {
        const double wave = std::sqrt(config_.gravity_m_s2 * c.depth_metres);
        const double speed = std::abs(velocity(c.momentum_x, c.depth_metres)) +
                             std::abs(velocity(c.momentum_z, c.depth_metres)) + 2.0 * wave;
        if (!std::isfinite(speed))
            return 0.0;
        max_sum_speed = std::max(max_sum_speed, speed);
    }
    if (max_sum_speed == 0.0)
        return std::numeric_limits<double>::infinity();
    return config_.cfl * config_.cell_spacing_metres / max_sum_speed;
}

bool Basin::substep(const std::vector<BasinCell>& from,
                    std::vector<BasinCell>& to,
                    double dt) noexcept {
    const auto w = config_.width;
    const auto h = config_.height;
    const double g = config_.gravity_m_s2;
    const double factor = dt / config_.cell_spacing_metres;
    std::fill(outgoing_.begin(), outgoing_.end(), 0.0);

    // Rusanov flux: (F(L)+F(R)-a*(R-L))/2, a=max(|u_n|+sqrt(g*h)).
    // The same face value enters both neighbours with opposite signs.
    for (std::size_t z = 0; z < h; ++z) {
        x_flux_[z * (w + 1)] = {
            0.0, 0.5 * g * from[index(0, z)].depth_metres * from[index(0, z)].depth_metres, 0.0};
        x_flux_[z * (w + 1) + w] = {0.0,
                                    0.5 * g * from[index(w - 1, z)].depth_metres *
                                        from[index(w - 1, z)].depth_metres,
                                    0.0};
        for (std::size_t x = 1; x < w; ++x) {
            const auto& l = from[index(x - 1, z)];
            const auto& r = from[index(x, z)];
            const double ul = velocity(l.momentum_x, l.depth_metres);
            const double ur = velocity(r.momentum_x, r.depth_metres);
            const double vl = velocity(l.momentum_z, l.depth_metres);
            const double vr = velocity(r.momentum_z, r.depth_metres);
            const double a = std::max(std::abs(ul) + std::sqrt(g * l.depth_metres),
                                      std::abs(ur) + std::sqrt(g * r.depth_metres));
            auto& f = x_flux_[z * (w + 1) + x];
            f.h = 0.5 * (l.momentum_x + r.momentum_x - a * (r.depth_metres - l.depth_metres));
            f.mx = 0.5 * (l.momentum_x * ul + 0.5 * g * l.depth_metres * l.depth_metres +
                          r.momentum_x * ur + 0.5 * g * r.depth_metres * r.depth_metres -
                          a * (r.momentum_x - l.momentum_x));
            f.mz =
                0.5 * (l.momentum_x * vl + r.momentum_x * vr - a * (r.momentum_z - l.momentum_z));
            outgoing_[f.h >= 0.0 ? index(x - 1, z) : index(x, z)] += std::abs(f.h);
        }
    }
    for (std::size_t x = 0; x < w; ++x) {
        z_flux_[x] = {
            0.0, 0.0, 0.5 * g * from[index(x, 0)].depth_metres * from[index(x, 0)].depth_metres};
        z_flux_[h * w + x] = {0.0,
                              0.0,
                              0.5 * g * from[index(x, h - 1)].depth_metres *
                                  from[index(x, h - 1)].depth_metres};
    }
    for (std::size_t z = 1; z < h; ++z) {
        for (std::size_t x = 0; x < w; ++x) {
            const auto& l = from[index(x, z - 1)];
            const auto& r = from[index(x, z)];
            const double ul = velocity(l.momentum_x, l.depth_metres);
            const double ur = velocity(r.momentum_x, r.depth_metres);
            const double vl = velocity(l.momentum_z, l.depth_metres);
            const double vr = velocity(r.momentum_z, r.depth_metres);
            const double a = std::max(std::abs(vl) + std::sqrt(g * l.depth_metres),
                                      std::abs(vr) + std::sqrt(g * r.depth_metres));
            auto& f = z_flux_[z * w + x];
            f.h = 0.5 * (l.momentum_z + r.momentum_z - a * (r.depth_metres - l.depth_metres));
            f.mx =
                0.5 * (l.momentum_z * ul + r.momentum_z * ur - a * (r.momentum_x - l.momentum_x));
            f.mz = 0.5 * (l.momentum_z * vl + 0.5 * g * l.depth_metres * l.depth_metres +
                          r.momentum_z * vr + 0.5 * g * r.depth_metres * r.depth_metres -
                          a * (r.momentum_z - l.momentum_z));
            outgoing_[f.h >= 0.0 ? index(x, z - 1) : index(x, z)] += std::abs(f.h);
        }
    }

    // A donor scales all of its outgoing shared face fluxes together. This preserves
    // the face's equal-and-opposite mass transfer and prevents a wet cell overdraft.
    for (std::size_t i = 0; i < from.size(); ++i) {
        const double demand = factor * outgoing_[i];
        drain_scale_[i] = demand > from[i].depth_metres ? from[i].depth_metres / demand : 1.0;
    }
    for (std::size_t z = 0; z < h; ++z) {
        for (std::size_t x = 1; x < w; ++x) {
            auto& f = x_flux_[z * (w + 1) + x];
            const double s = drain_scale_[f.h >= 0.0 ? index(x - 1, z) : index(x, z)];
            f.h *= s;
            f.mx *= s;
            f.mz *= s;
        }
    }
    for (std::size_t z = 1; z < h; ++z) {
        for (std::size_t x = 0; x < w; ++x) {
            auto& f = z_flux_[z * w + x];
            const double s = drain_scale_[f.h >= 0.0 ? index(x, z - 1) : index(x, z)];
            f.h *= s;
            f.mx *= s;
            f.mz *= s;
        }
    }
    for (std::size_t z = 0; z < h; ++z) {
        for (std::size_t x = 0; x < w; ++x) {
            const auto i = index(x, z);
            const auto& xl = x_flux_[z * (w + 1) + x];
            const auto& xr = x_flux_[z * (w + 1) + x + 1];
            const auto& zl = z_flux_[z * w + x];
            const auto& zr = z_flux_[(z + 1) * w + x];
            auto& c = to[i];
            c.depth_metres = from[i].depth_metres - factor * (xr.h - xl.h + zr.h - zl.h);
            c.momentum_x = from[i].momentum_x - factor * (xr.mx - xl.mx + zr.mx - zl.mx);
            c.momentum_z = from[i].momentum_z - factor * (xr.mz - xl.mz + zr.mz - zl.mz);
            if (!std::isfinite(c.depth_metres) || c.depth_metres < 0.0 ||
                !std::isfinite(c.momentum_x) || !std::isfinite(c.momentum_z))
                return false;
            if (c.depth_metres <= dry_depth_metres) {
                c.momentum_x = 0.0;
                c.momentum_z = 0.0;
            }
        }
    }
    return true;
}

BasinStepStatus Basin::advance(double dt_seconds) noexcept {
    if (!std::isfinite(dt_seconds) || dt_seconds < 0.0) {
        ++refused_advances_;
        return BasinStepStatus::InvalidDt;
    }
    if (dt_seconds == 0.0)
        return BasinStepStatus::Accepted;
    if (tick_ == std::numeric_limits<std::uint64_t>::max()) {
        ++refused_advances_;
        return BasinStepStatus::NumericalFailure;
    }
    std::copy(cells_.begin(), cells_.end(), work_.begin());
    double remaining = dt_seconds;
    std::size_t steps = 0;
    while (remaining > 0.0) {
        if (steps == config_.max_substeps) {
            ++refused_advances_;
            return BasinStepStatus::SubstepBudgetExceeded;
        }
        const double bound = stable_dt(work_);
        if (!(bound > 0.0) || std::isnan(bound)) {
            ++refused_advances_;
            return BasinStepStatus::NumericalFailure;
        }
        const double step = std::min(remaining, bound);
        if (!substep(work_, next_, step)) {
            ++refused_advances_;
            return BasinStepStatus::NumericalFailure;
        }
        work_.swap(next_);
        ++steps;
        const double reduced = remaining - step;
        if (reduced == remaining) {
            ++refused_advances_;
            return BasinStepStatus::NumericalFailure;
        }
        remaining = reduced;
    }
    cells_.swap(work_);
    last_substeps_ = steps;
    ++tick_;
    return BasinStepStatus::Accepted;
}

} // namespace rime::fluids
