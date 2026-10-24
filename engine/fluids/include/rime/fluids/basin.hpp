// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "rime/fluids/buoyancy.hpp"

namespace rime::fluids {

// Teaching note: the cell state is U=(h,hu,hv). Each interior face computes one
// local Lax-Friedrichs/Rusanov flux from its two cells, then each cell subtracts
// the outgoing flux divergence. A wall mirrors its interior normal momentum:
// normal mass/tangent flux are zero, while normal momentum flux is
// h*u_n^2 + g*h^2/2 +/- (|u_n|+sqrt(g*h))*h*u_n (minus at low X/Z,
// plus at high X/Z). A donor limiter scales a shared face flux when
// the requested outgoing water exceeds its depth, retaining nonnegative h and
// equal-and-opposite mass transfer. Explicit substeps obey
// dt <= CFL*dx/max_cells(|u|+|v|+2*sqrt(g*h)), with CFL <= 0.5.
// Momentum is set to zero at h <= 1e-9 m to avoid singular dry velocities;
// depth is never clipped. This flat-bed, nondispersive 2D model has no vertical
// momentum or body displacement, so buoyancy's vertical water reaction cannot
// be applied here and this is not yet a two-way coupling model.
// Flat-bed Cartesian reference basin. X/Z are horizontal; Y is vertical.
// Momentum fields are depth-integrated velocity (h*u, h*v), in m^2/s.
struct BasinCell {
    double depth_metres = 0.0;
    double momentum_x = 0.0;
    double momentum_z = 0.0;
};

struct BasinConfig {
    std::size_t width = 0;
    std::size_t height = 0;
    double origin_x_metres = 0.0;
    double origin_z_metres = 0.0;
    double cell_spacing_metres = 1.0;
    double bed_height_metres = 0.0;
    double initial_depth_metres = 0.0;
    double gravity_m_s2 = 9.81;
    double density_kg_m3 = 1000.0;
    double cfl = 0.45;
    std::size_t max_substeps = 256;
};

enum class BasinStepStatus { Accepted, InvalidDt, SubstepBudgetExceeded, NumericalFailure };

class Basin {
public:
    static constexpr std::size_t max_cells = 256 * 256;
    static constexpr std::size_t max_allowed_substeps = 4096;
    static constexpr double dry_depth_metres = 1.0e-9;

    [[nodiscard]] static std::optional<Basin> create(const BasinConfig& config);

    [[nodiscard]] const BasinConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }

    [[nodiscard]] std::size_t last_substeps() const noexcept { return last_substeps_; }

    [[nodiscard]] std::uint64_t refused_advances() const noexcept { return refused_advances_; }

    [[nodiscard]] std::optional<BasinCell> cell(std::size_t x, std::size_t z) const noexcept;
    // Refuses non-finite values, negative depth, or momentum in a dry cell.
    [[nodiscard]] bool set_cell(std::size_t x, std::size_t z, BasinCell value) noexcept;
    // Cell-center snapshot; nullopt outside the half-open grid or for dry cells.
    // Inspect cell() for dry depth/bed geometry. Sampling never changes the state.
    [[nodiscard]] std::optional<WaterSurfaceSample> sample(double x_metres,
                                                           double z_metres) const noexcept;
    [[nodiscard]] double total_volume_m3() const noexcept;
    // A positive accepted caller step increments tick once; zero dt is a no-op.
    // On refusal all cells, tick, and last_substeps stay unchanged.
    [[nodiscard]] BasinStepStatus advance(double dt_seconds) noexcept;

private:
    explicit Basin(BasinConfig config);

    struct Flux {
        double h = 0.0;
        double mx = 0.0;
        double mz = 0.0;
    };

    [[nodiscard]] std::size_t index(std::size_t x, std::size_t z) const noexcept;
    [[nodiscard]] double stable_dt(const std::vector<BasinCell>& cells) const noexcept;
    [[nodiscard]] bool
    substep(const std::vector<BasinCell>& from, std::vector<BasinCell>& to, double dt) noexcept;

    BasinConfig config_;
    std::vector<BasinCell> cells_;
    std::vector<BasinCell> work_;
    std::vector<BasinCell> next_;
    std::vector<Flux> x_flux_;
    std::vector<Flux> z_flux_;
    std::vector<double> outgoing_;
    std::vector<double> drain_scale_;
    std::uint64_t tick_ = 0;
    std::size_t last_substeps_ = 0;
    std::uint64_t refused_advances_ = 0;
};

} // namespace rime::fluids
