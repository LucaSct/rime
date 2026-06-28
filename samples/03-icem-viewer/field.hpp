// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Reads ICEM's native .icef field file and prepares a scalar field for the viewer: a dense node
// grid of values, packed into an RGBA32F volume (R = value, G = validity) for a 3-D texture, plus
// the affine map from world position to texture coordinate and the value range for the colormap.
//
// The two repos share only the *file format*, never code — this is an independent reader of the
// little-endian layout documented in ICEM's docs/math/field-io.md (and Rime's
// docs/math/colormap.md):
//   magic "ICEF" | u32 version(=1) | u32 flags | origin xyz f64 | h f64 | nx,ny,nz u32 (node
//   counts) | u32 field_count | per field: name(u32 len+bytes) unit(str) components(i32) then
//     nx*ny*nz*components f64 (node-major, x fastest; absent nodes = canonical NaN)
//   | nx*ny*nz u8 validity mask | u64 content hash
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace rime::viewer {

// A scalar field, prepared for upload as an RGBA32F 3-D texture and for the colormap.
struct ScalarField {
    std::string name;
    std::string unit;
    std::uint32_t nx = 0, ny = 0, nz = 0;
    std::vector<float>
        rgba; // nx*ny*nz * 4: R = value (dilated into the absent shell), G = valid(0/1)
    float vmin = 0.0f, vmax = 0.0f; // value range over the solid (the colormap domain)
    // World→texcoord, per component: uvw = world * scale + bias. Folds origin, spacing and the
    // half-texel offset so the shader just does a multiply-add then samples (see colormap.md).
    float scale[3] = {0, 0, 0};
    float bias[3] = {0, 0, 0};

    [[nodiscard]] bool usable() const { return nx > 0 && ny > 0 && nz > 0 && vmax > vmin; }

    [[nodiscard]] std::uint32_t depth() const { return nz; }
};

namespace detail {

inline std::uint32_t rd_u32(std::istream& is) {
    unsigned char b[4];
    is.read(reinterpret_cast<char*>(b), 4);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

inline std::int32_t rd_i32(std::istream& is) {
    const std::uint32_t u = rd_u32(is);
    std::int32_t v;
    std::memcpy(&v, &u, 4);
    return v;
}

inline std::uint64_t rd_u64(std::istream& is) {
    unsigned char b[8];
    is.read(reinterpret_cast<char*>(b), 8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    return v;
}

inline double rd_f64(std::istream& is) {
    const std::uint64_t bits = rd_u64(is);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

inline std::string rd_str(std::istream& is) {
    const std::uint32_t n = rd_u32(is);
    std::string s(n, '\0');
    if (n)
        is.read(s.data(), n);
    return s;
}

} // namespace detail

// Load the first scalar (1-component) field from a .icef — or the one named `want`, if given.
// Returns nullopt on I/O error, bad magic/version, or if no matching scalar field exists.
[[nodiscard]] inline std::optional<ScalarField> load_icef_scalar(const std::string& path,
                                                                 const std::string& want = "") {
    std::ifstream is(path, std::ios::binary);
    if (!is)
        return std::nullopt;

    char magic[4];
    is.read(magic, 4);
    if (std::memcmp(magic, "ICEF", 4) != 0)
        return std::nullopt;
    if (detail::rd_u32(is) != 1u)
        return std::nullopt;  // version
    (void)detail::rd_u32(is); // flags

    const double ox = detail::rd_f64(is), oy = detail::rd_f64(is), oz = detail::rd_f64(is);
    const double h = detail::rd_f64(is);
    const std::uint32_t nx = detail::rd_u32(is), ny = detail::rd_u32(is), nz = detail::rd_u32(is);
    const std::size_t count = static_cast<std::size_t>(nx) * ny * nz;

    const std::uint32_t nfields = detail::rd_u32(is);
    std::string chosen_name, chosen_unit;
    std::vector<double> chosen; // the selected scalar field's dense values (NaN where absent)
    for (std::uint32_t f = 0; f < nfields; ++f) {
        const std::string name = detail::rd_str(is);
        const std::string unit = detail::rd_str(is);
        const int comp = detail::rd_i32(is);
        std::vector<double> data(count * static_cast<std::size_t>(comp));
        for (double& v : data)
            v = detail::rd_f64(is);
        const bool want_this = chosen.empty() && comp == 1 && (want.empty() || name == want);
        if (want_this) {
            chosen = std::move(data);
            chosen_name = name;
            chosen_unit = unit;
        }
    }
    if (chosen.empty() || count == 0)
        return std::nullopt;

    std::vector<std::uint8_t> mask(count);
    if (count)
        is.read(reinterpret_cast<char*>(mask.data()), static_cast<std::streamsize>(count));
    if (!is)
        return std::nullopt;

    ScalarField sf;
    sf.name = chosen_name;
    sf.unit = chosen_unit;
    sf.nx = nx;
    sf.ny = ny;
    sf.nz = nz;

    // Value range over the solid (skip absent/NaN nodes).
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!mask[i] || std::isnan(chosen[i]))
            continue;
        const float v = static_cast<float>(chosen[i]);
        if (!any) {
            sf.vmin = sf.vmax = v;
            any = true;
        } else {
            sf.vmin = std::min(sf.vmin, v);
            sf.vmax = std::max(sf.vmax, v);
        }
    }
    if (!any)
        return std::nullopt;
    if (sf.vmax <= sf.vmin)
        sf.vmax = sf.vmin + 1.0f; // constant field → a non-degenerate domain

    // Dilate the value channel one voxel into the absent shell: an absent node takes the average of
    // its present 6-neighbours. Linear texture filtering near the boundary then blends real values
    // instead of a fill constant, so the surface colour reads true right up to the edge. The
    // validity channel keeps the *original* mask (the true solid), for the slice clip that lands
    // with the cap.
    const auto idx = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return static_cast<std::size_t>(i) +
               static_cast<std::size_t>(nx) *
                   (static_cast<std::size_t>(j) + static_cast<std::size_t>(ny) * k);
    };
    sf.rgba.assign(count * 4, 0.0f);
    for (std::uint32_t k = 0; k < nz; ++k)
        for (std::uint32_t j = 0; j < ny; ++j)
            for (std::uint32_t i = 0; i < nx; ++i) {
                const std::size_t gi = idx(i, j, k);
                float value;
                if (mask[gi] && !std::isnan(chosen[gi])) {
                    value = static_cast<float>(chosen[gi]);
                } else {
                    double sum = 0.0;
                    int n = 0;
                    const int di[6] = {-1, 1, 0, 0, 0, 0};
                    const int dj[6] = {0, 0, -1, 1, 0, 0};
                    const int dk[6] = {0, 0, 0, 0, -1, 1};
                    for (int d = 0; d < 6; ++d) {
                        const long ni = static_cast<long>(i) + di[d];
                        const long nj = static_cast<long>(j) + dj[d];
                        const long nk = static_cast<long>(k) + dk[d];
                        if (ni < 0 || nj < 0 || nk < 0 || ni >= nx || nj >= ny || nk >= nz)
                            continue;
                        const std::size_t gn = idx(static_cast<std::uint32_t>(ni),
                                                   static_cast<std::uint32_t>(nj),
                                                   static_cast<std::uint32_t>(nk));
                        if (mask[gn] && !std::isnan(chosen[gn])) {
                            sum += chosen[gn];
                            ++n;
                        }
                    }
                    value = n ? static_cast<float>(sum / n) : sf.vmin; // no solid neighbour → clamp
                }
                sf.rgba[gi * 4 + 0] = value;
                sf.rgba[gi * 4 + 1] = mask[gi] ? 1.0f : 0.0f; // validity (true solid)
                sf.rgba[gi * 4 + 3] = 1.0f;
            }

    // World→texcoord affine, per axis: uvw = ((p - origin)/h + 0.5)/dims = p*scale + bias.
    const double org[3] = {ox, oy, oz};
    const std::uint32_t dim[3] = {nx, ny, nz};
    for (int c = 0; c < 3; ++c) {
        const double hd = h * static_cast<double>(dim[c]);
        sf.scale[c] = static_cast<float>(1.0 / hd);
        sf.bias[c] = static_cast<float>(0.5 / static_cast<double>(dim[c]) - org[c] / hd);
    }
    return sf;
}

// A vector (vec3) field — a displacement or modal mode shape — prepared as an RGBA32F volume (xyz =
// vector, w = validity) for the warp: the vertex shader fetches xyz to displace the surface, and
// the fragment shader colours by |vector| / vmag_max. Same world→uvw affine as the scalar field.
struct VectorField {
    std::string name;
    std::string unit;
    std::uint32_t nx = 0, ny = 0, nz = 0;
    std::vector<float>
        rgba;              // nx*ny*nz * 4: xyz = vector (dilated into the absent shell), w = valid
    float vmag_max = 0.0f; // max |vector| over the solid (warp + colormap normalization)
    float scale[3] = {0, 0, 0};
    float bias[3] = {0, 0, 0};

    [[nodiscard]] bool usable() const { return nx > 0 && ny > 0 && nz > 0 && vmag_max > 0.0f; }

    [[nodiscard]] std::uint32_t depth() const { return nz; }
};

// Load the first vec3 (3-component) field from a .icef — or the one named `want`. nullopt on error
// or if no matching vector field exists.
[[nodiscard]] inline std::optional<VectorField> load_icef_vector(const std::string& path,
                                                                 const std::string& want = "") {
    std::ifstream is(path, std::ios::binary);
    if (!is)
        return std::nullopt;

    char magic[4];
    is.read(magic, 4);
    if (std::memcmp(magic, "ICEF", 4) != 0)
        return std::nullopt;
    if (detail::rd_u32(is) != 1u)
        return std::nullopt;
    (void)detail::rd_u32(is);

    const double ox = detail::rd_f64(is), oy = detail::rd_f64(is), oz = detail::rd_f64(is);
    const double h = detail::rd_f64(is);
    const std::uint32_t nx = detail::rd_u32(is), ny = detail::rd_u32(is), nz = detail::rd_u32(is);
    const std::size_t count = static_cast<std::size_t>(nx) * ny * nz;

    const std::uint32_t nfields = detail::rd_u32(is);
    std::string cname, cunit;
    std::vector<double> chosen; // the selected vec3 field, 3 per node (NaN where absent)
    for (std::uint32_t f = 0; f < nfields; ++f) {
        const std::string name = detail::rd_str(is);
        const std::string unit = detail::rd_str(is);
        const int comp = detail::rd_i32(is);
        std::vector<double> data(count * static_cast<std::size_t>(comp));
        for (double& v : data)
            v = detail::rd_f64(is);
        if (chosen.empty() && comp == 3 && (want.empty() || name == want)) {
            chosen = std::move(data);
            cname = name;
            cunit = unit;
        }
    }
    if (chosen.empty() || count == 0)
        return std::nullopt;

    std::vector<std::uint8_t> mask(count);
    if (count)
        is.read(reinterpret_cast<char*>(mask.data()), static_cast<std::streamsize>(count));
    if (!is)
        return std::nullopt;

    VectorField vf;
    vf.name = cname;
    vf.unit = cunit;
    vf.nx = nx;
    vf.ny = ny;
    vf.nz = nz;

    const auto idx = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return static_cast<std::size_t>(i) +
               static_cast<std::size_t>(nx) *
                   (static_cast<std::size_t>(j) + static_cast<std::size_t>(ny) * k);
    };
    const auto solid = [&](std::size_t gi) { return mask[gi] && !std::isnan(chosen[gi * 3 + 0]); };

    // Max magnitude over the solid (for warp + colour normalization).
    for (std::size_t gi = 0; gi < count; ++gi) {
        if (!solid(gi))
            continue;
        const double x = chosen[gi * 3 + 0], y = chosen[gi * 3 + 1], z = chosen[gi * 3 + 2];
        vf.vmag_max = std::max(vf.vmag_max, static_cast<float>(std::sqrt(x * x + y * y + z * z)));
    }
    if (vf.vmag_max <= 0.0f)
        vf.vmag_max = 1.0f; // a zero field → avoid divide-by-zero

    // Dilate the vector one voxel into the absent shell (component-wise mean of solid neighbours),
    // so vertex-fetch + trilinear sampling near the boundary read real vectors. Validity keeps the
    // mask.
    vf.rgba.assign(count * 4, 0.0f);
    for (std::uint32_t k = 0; k < nz; ++k)
        for (std::uint32_t j = 0; j < ny; ++j)
            for (std::uint32_t i = 0; i < nx; ++i) {
                const std::size_t gi = idx(i, j, k);
                float vec[3] = {0, 0, 0};
                if (solid(gi)) {
                    for (int c = 0; c < 3; ++c)
                        vec[c] = static_cast<float>(chosen[gi * 3 + c]);
                } else {
                    double sum[3] = {0, 0, 0};
                    int n = 0;
                    const int di[6] = {-1, 1, 0, 0, 0, 0}, dj[6] = {0, 0, -1, 1, 0, 0},
                              dk[6] = {0, 0, 0, 0, -1, 1};
                    for (int d = 0; d < 6; ++d) {
                        const long ni = static_cast<long>(i) + di[d],
                                   nj = static_cast<long>(j) + dj[d],
                                   nk = static_cast<long>(k) + dk[d];
                        if (ni < 0 || nj < 0 || nk < 0 || ni >= nx || nj >= ny || nk >= nz)
                            continue;
                        const std::size_t gn = idx(static_cast<std::uint32_t>(ni),
                                                   static_cast<std::uint32_t>(nj),
                                                   static_cast<std::uint32_t>(nk));
                        if (solid(gn)) {
                            for (int c = 0; c < 3; ++c)
                                sum[c] += chosen[gn * 3 + c];
                            ++n;
                        }
                    }
                    if (n)
                        for (int c = 0; c < 3; ++c)
                            vec[c] = static_cast<float>(sum[c] / n);
                }
                vf.rgba[gi * 4 + 0] = vec[0];
                vf.rgba[gi * 4 + 1] = vec[1];
                vf.rgba[gi * 4 + 2] = vec[2];
                vf.rgba[gi * 4 + 3] = mask[gi] ? 1.0f : 0.0f;
            }

    const double org[3] = {ox, oy, oz};
    const std::uint32_t dim[3] = {nx, ny, nz};
    for (int c = 0; c < 3; ++c) {
        const double hd = h * static_cast<double>(dim[c]);
        vf.scale[c] = static_cast<float>(1.0 / hd);
        vf.bias[c] = static_cast<float>(0.5 / static_cast<double>(dim[c]) - org[c] / hd);
    }
    return vf;
}

// Derive the scalar **speed** |v| from a loaded vec3 velocity field, as a ScalarField the colormap,
// legend, isosurface and DVR (which all consume a ScalarField) can show volumetrically (D2·V). The
// streamlines trace the flow's *direction* and colour it by speed; the speed scalar lets the same
// speed be rendered as a *volume* — the viscous **boundary layer** as a cloud (slow at the no-slip
// walls, fast in the core), an isotach surface, or a slice on the cut plane. It reuses the vector
// field's grid, the world→uvw affine, and its one-voxel dilation: the magnitude of the
// already-dilated vector is the dilated speed, and the validity channel carries straight over, so
// boundary sampling stays clean.
[[nodiscard]] inline ScalarField speed_field(const VectorField& vf) {
    ScalarField sf;
    sf.name = "speed";
    sf.unit = vf.unit; // |v| has the velocity's unit (e.g. m/s)
    sf.nx = vf.nx;
    sf.ny = vf.ny;
    sf.nz = vf.nz;
    for (int c = 0; c < 3; ++c) {
        sf.scale[c] = vf.scale[c];
        sf.bias[c] = vf.bias[c];
    }
    const std::size_t count = static_cast<std::size_t>(vf.nx) * vf.ny * vf.nz;
    sf.rgba.assign(count * 4, 0.0f);
    bool any = false;
    for (std::size_t gi = 0; gi < count; ++gi) {
        const float x = vf.rgba[gi * 4 + 0], y = vf.rgba[gi * 4 + 1], z = vf.rgba[gi * 4 + 2];
        const float valid = vf.rgba[gi * 4 + 3];
        const float s = std::sqrt(x * x + y * y + z * z);
        sf.rgba[gi * 4 + 0] = s;     // R = speed
        sf.rgba[gi * 4 + 1] = valid; // G = validity (true solid), exactly like load_icef_scalar
        sf.rgba[gi * 4 + 3] = 1.0f;
        if (valid > 0.5f) {
            if (!any) {
                sf.vmin = sf.vmax = s;
                any = true;
            } else {
                sf.vmin = std::min(sf.vmin, s);
                sf.vmax = std::max(sf.vmax, s);
            }
        }
    }
    if (!any) {
        sf.vmin = 0.0f;
        sf.vmax = 1.0f;
    } else if (sf.vmax <= sf.vmin) {
        sf.vmax = sf.vmin + 1.0f; // a uniform-speed field → a non-degenerate colormap domain
    }
    return sf;
}

} // namespace rime::viewer
