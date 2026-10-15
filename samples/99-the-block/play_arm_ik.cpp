// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_arm_ik.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "rime/core/hash.hpp"

namespace rime::block_play {
namespace {
using namespace rime;

bool finite(core::Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const core::Mat4& m) {
    for (float v : m.m) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

core::Vec3 head(const core::Mat4& m) {
    return {m.m[12], m.m[13], m.m[14]};
}

core::Vec3 axis(const core::Mat4& m, int col) {
    return {m.at(0, col), m.at(1, col), m.at(2, col)};
}

// Uniform positive scale is safe: the skin palette preserves it and the joint-head solve uses
// actual metre distances. Shear, reflections and nonuniform scale need a different solver.
bool uniform_frame(const core::Mat4& m) {
    if (!finite(m) || std::fabs(m.at(3, 0)) > 1e-5f || std::fabs(m.at(3, 1)) > 1e-5f ||
        std::fabs(m.at(3, 2)) > 1e-5f || std::fabs(m.at(3, 3) - 1.0f) > 1e-5f) {
        return false;
    }
    const auto x = axis(m, 0), y = axis(m, 1), z = axis(m, 2);
    const float s2 = core::dot(x, x);
    return s2 > 1e-12f && std::fabs(core::dot(y, y) - s2) < s2 * 1e-3f &&
           std::fabs(core::dot(z, z) - s2) < s2 * 1e-3f &&
           std::fabs(core::dot(x, y)) < s2 * 1e-3f && std::fabs(core::dot(x, z)) < s2 * 1e-3f &&
           std::fabs(core::dot(y, z)) < s2 * 1e-3f && core::dot(core::cross(x, y), z) > 0.0f;
}

bool rigid_frame(const core::Mat4& m) {
    return uniform_frame(m) && std::fabs(core::length(axis(m, 0)) - 1.0f) < 1e-3f;
}

core::Vec3 unit(core::Vec3 v, core::Vec3 fallback) {
    const float l = core::length(v);
    return l > 1e-7f ? v / l : fallback;
}

core::Vec3 perpendicular(core::Vec3 v) {
    // Choose the least parallel cardinal axis, including the exact antiparallel case.
    const core::Vec3 a =
        std::fabs(v.x) < std::fabs(v.y)
            ? (std::fabs(v.x) < std::fabs(v.z) ? core::Vec3{1, 0, 0} : core::Vec3{0, 0, 1})
            : (std::fabs(v.y) < std::fabs(v.z) ? core::Vec3{0, 1, 0} : core::Vec3{0, 0, 1});
    return unit(core::cross(v, a), {0, 1, 0});
}

core::Quat shortest_arc(core::Vec3 from, core::Vec3 to) {
    from = unit(from, {1, 0, 0});
    to = unit(to, from);
    const float d = std::clamp(core::dot(from, to), -1.0f, 1.0f);
    if (d < -0.999999f) {
        const auto a = perpendicular(from);
        return {a.x, a.y, a.z, 0.0f};
    }
    const auto c = core::cross(from, to);
    return core::normalize(core::Quat{c.x, c.y, c.z, 1.0f + d});
}

core::Mat4 moved_segment(const core::Mat4& bind,
                         core::Vec3 old_direction,
                         core::Vec3 new_direction,
                         core::Vec3 position) {
    auto m = core::to_mat4(shortest_arc(old_direction, new_direction)) * bind;
    m.m[12] = position.x;
    m.m[13] = position.y;
    m.m[14] = position.z;
    return m;
}

std::size_t offset(assets::VertexAttribs attrs, assets::VertexAttribs wanted) {
    std::size_t out = 0;
    for (auto a : {assets::VertexAttribs::Position,
                   assets::VertexAttribs::Normal,
                   assets::VertexAttribs::Uv,
                   assets::VertexAttribs::Tangent,
                   assets::VertexAttribs::Joints,
                   assets::VertexAttribs::Weights}) {
        if (a == wanted) {
            return out;
        }
        if (assets::has_attrib(attrs, a)) {
            out += assets::attrib_size(a);
        }
    }
    return out;
}
} // namespace

bool PlayArmIk::initialize(const assets::MeshAsset& mesh, const assets::Skeleton& skeleton) {
    // A failed reload invalidates the instance; it cannot silently keep an unrelated old grip.
    initialized_ = false;
    stats_ = {};
    using A = assets::VertexAttribs;
    if (mesh.vertex_count == 0 || skeleton.joints.empty() ||
        skeleton.joints.size() > assets::kMaxJoints || !skeleton.is_topologically_ordered() ||
        (static_cast<std::uint32_t>(mesh.attribs) & ~assets::kKnownVertexAttribs) != 0 ||
        !assets::has_attrib(mesh.attribs, A::Position | A::Joints | A::Weights) ||
        mesh.vertex_stride != assets::expected_vertex_stride(mesh.attribs) ||
        mesh.vertices.size() != static_cast<std::size_t>(mesh.vertex_count) * mesh.vertex_stride) {
        return false;
    }
    for (auto i : mesh.indices) {
        if (i >= mesh.vertex_count) {
            return false;
        }
    }
    skeleton_ = skeleton;
    const std::size_t n = skeleton.joints.size();
    bind_world_.resize(n);
    local_bind_.resize(n);
    world_.resize(n);
    palette_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& j = skeleton.joints[i];
        const auto q = j.local_bind.rotation;
        if (!std::isfinite(core::dot(q, q)) || std::fabs(core::dot(q, q) - 1.0f) > 1e-3f) {
            return false;
        }
        local_bind_[i] = core::to_matrix(j.local_bind);
        if (!uniform_frame(local_bind_[i]) || !uniform_frame(j.inverse_bind)) {
            return false;
        }
        bind_world_[i] = j.parent < 0
                             ? local_bind_[i]
                             : bind_world_[static_cast<std::size_t>(j.parent)] * local_bind_[i];
        // This catches export/object-space mismatches before they produce plausible wrong arms.
        if (!uniform_frame(bind_world_[i]) ||
            !core::approx_eq(bind_world_[i] * j.inverse_bind, core::Mat4{}, 2e-3f)) {
            return false;
        }
    }
    constexpr std::array<std::array<std::string_view, 3>, 2> names{
        {{"L_arm_01", "L_elbow_02", "L_wrist_03"}, {"R_arm_023", "R_elbow_024", "R_wrist_025"}}};
    for (std::size_t side = 0; side < 2; ++side) {
        auto& a = arms_[side];
        for (std::size_t k = 0; k < 3; ++k) {
            const auto found = skeleton.find(core::fnv1a_64(names[side][k]));
            if (found < 0) {
                return false;
            }
            a.joints[k] = static_cast<std::size_t>(found);
        }
        if (skeleton.joints[a.joints[1]].parent != static_cast<std::int32_t>(a.joints[0]) ||
            skeleton.joints[a.joints[2]].parent != static_cast<std::int32_t>(a.joints[1])) {
            return false;
        }
        const auto s = head(bind_world_[a.joints[0]]);
        const auto e = head(bind_world_[a.joints[1]]);
        const auto w = head(bind_world_[a.joints[2]]);
        a.upper_length = core::length(e - s);
        a.lower_length = core::length(w - e);
        if (!std::isfinite(a.upper_length) || !std::isfinite(a.lower_length) ||
            a.upper_length < 1e-5f || a.lower_length < 1e-5f) {
            return false;
        }
        const auto direction = unit(w - s, unit(e - s, {0, 0, -1}));
        a.bind_bend =
            unit((e - s) - direction * core::dot(e - s, direction), perpendicular(direction));
        bind_wrists_[side] = bind_world_[a.joints[2]];
    }
    influences_.resize(mesh.vertex_count);
    const auto jo = offset(mesh.attribs, A::Joints), wo = offset(mesh.attribs, A::Weights);
    for (std::size_t i = 0; i < mesh.vertex_count; ++i) {
        const auto* b = mesh.vertices.data() + i * mesh.vertex_stride;
        auto& inf = influences_[i];
        std::memcpy(inf.joints.data(), b + jo, sizeof(inf.joints));
        std::memcpy(inf.weights.data(), b + wo, sizeof(inf.weights));
        float sum = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            if (inf.joints[k] >= n || !std::isfinite(inf.weights[k]) || inf.weights[k] < 0.0f) {
                return false;
            }
            sum += inf.weights[k];
        }
        if (!std::isfinite(sum) || sum <= 1e-6f) {
            return false;
        }
        for (auto& weight : inf.weights) {
            weight /= sum;
        }
    }
    bind_mesh_ = render::mesh_from_cooked(mesh);
    for (const auto& v : bind_mesh_.vertices) {
        if (!finite(core::Vec3{v.px, v.py, v.pz}) || !finite(core::Vec3{v.nx, v.ny, v.nz}) ||
            !finite(core::Vec3{v.tx, v.ty, v.tz}) || !std::isfinite(v.u) || !std::isfinite(v.v) ||
            !std::isfinite(v.tw) || core::length(core::Vec3{v.nx, v.ny, v.nz}) < 1e-6f ||
            core::length(core::Vec3{v.tx, v.ty, v.tz}) < 1e-6f) {
            return false;
        }
    }
    vertices_ = bind_mesh_.vertices;
    world_ = bind_world_;
    initialized_ = true;
    return true;
}

std::span<const render::MeshVertex>
PlayArmIk::solve(const core::Mat4& camera_from_weapon,
                 const std::array<core::Mat4, 2>& weapon_from_wrists,
                 const std::array<core::Vec3, 2>& elbow_poles) noexcept {
    stats_ = {};
    if (!initialized_) {
        stats_.valid_input = false;
        return {};
    }
    std::array<core::Mat4, 2> targets{};
    for (std::size_t side = 0; side < 2; ++side) {
        if (!rigid_frame(camera_from_weapon) || !rigid_frame(weapon_from_wrists[side]) ||
            !finite(elbow_poles[side])) {
            stats_.valid_input = false;
            return vertices_;
        }
        targets[side] = camera_from_weapon * weapon_from_wrists[side];
        if (!rigid_frame(targets[side])) {
            stats_.valid_input = false;
            return vertices_;
        }
    }
    std::array<std::array<core::Mat4, 3>, 2> solved{};
    for (std::size_t side = 0; side < 2; ++side) {
        const auto& a = arms_[side];
        const auto s = head(bind_world_[a.joints[0]]);
        const auto old_e = head(bind_world_[a.joints[1]]);
        const auto old_w = head(bind_world_[a.joints[2]]);
        const auto requested = head(targets[side]);
        const float raw_distance = core::length(requested - s);
        if (!std::isfinite(raw_distance)) {
            stats_.valid_input = false;
            return vertices_;
        }
        const auto direction = unit(requested - s, unit(old_w - s, {0, 0, -1}));
        const float minimum = std::max(std::fabs(a.upper_length - a.lower_length), 1e-5f);
        const float d = std::clamp(raw_distance, minimum, a.upper_length + a.lower_length);
        if (d != raw_distance) {
            ++stats_.clamped_targets;
        }
        auto bend =
            (elbow_poles[side] - s) - direction * core::dot(elbow_poles[side] - s, direction);
        const auto fallback = a.bind_bend - direction * core::dot(a.bind_bend, direction);
        bend = unit(bend, unit(fallback, perpendicular(direction)));
        // Intersect the upper-arm sphere with the forearm sphere. Double intermediates keep the
        // folded equal-length case stable when the wrist is almost exactly at the shoulder.
        const double upper = a.upper_length, lower = a.lower_length, distance = d;
        const double along =
            (upper * upper - lower * lower + distance * distance) / (2.0 * distance);
        const float height =
            static_cast<float>(std::sqrt(std::max(0.0, upper * upper - along * along)));
        const auto e = s + direction * static_cast<float>(along) + bend * height;
        const auto w = s + direction * d;
        solved[side][0] = moved_segment(bind_world_[a.joints[0]], old_e - s, e - s, s);
        solved[side][1] = moved_segment(bind_world_[a.joints[1]], old_w - old_e, w - e, e);
        // Wrist orientation is an independent grip constraint; aiming the forearm at the target
        // alone cannot stop a palm rolling away from the gun. Fingers inherit this exact frame.
        solved[side][2] = targets[side];
        const float scale = core::length(axis(bind_world_[a.joints[2]], 0));
        for (int c = 0; c < 3; ++c) {
            for (int row = 0; row < 3; ++row) {
                solved[side][2].at(row, c) *= scale;
            }
        }
        solved[side][2].m[12] = w.x;
        solved[side][2].m[13] = w.y;
        solved[side][2].m[14] = w.z;
        stats_.wrist_position_error[side] = core::length(w - requested);
    }
    for (std::size_t i = 0; i < skeleton_.joints.size(); ++i) {
        const auto p = skeleton_.joints[i].parent;
        world_[i] = p < 0 ? local_bind_[i] : world_[static_cast<std::size_t>(p)] * local_bind_[i];
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t k = 0; k < 3; ++k) {
                if (i == arms_[side].joints[k]) {
                    world_[i] = solved[side][k];
                }
            }
        }
        palette_[i] = world_[i] * skeleton_.joints[i].inverse_bind;
    }
    for (std::size_t side = 0; side < 2; ++side) {
        const auto& reached = world_[arms_[side].joints[2]];
        float trace = 0.0f;
        for (int c = 0; c < 3; ++c) {
            trace += core::dot(unit(axis(reached, c), {}), unit(axis(targets[side], c), {}));
        }
        stats_.wrist_orientation_error[side] =
            std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f));
    }
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        const auto& v = bind_mesh_.vertices[i];
        const core::Vec3 bp{v.px, v.py, v.pz}, bn{v.nx, v.ny, v.nz}, bt{v.tx, v.ty, v.tz};
        core::Vec3 p{}, normal{}, tangent{};
        for (std::size_t k = 0; k < 4; ++k) {
            const auto& m = palette_[influences_[i].joints[k]];
            const auto weight = influences_[i].weights[k];
            p += core::transform_point(m, bp) * weight;
            normal += core::transform_vector(m, bn) * weight;
            tangent += core::transform_vector(m, bt) * weight;
        }
        normal = unit(normal, bn);
        tangent = unit(tangent - normal * core::dot(normal, tangent), perpendicular(normal));
        vertices_[i] = v; // UV and tangent handedness belong to the authored surface.
        auto& out = vertices_[i];
        out.px = p.x;
        out.py = p.y;
        out.pz = p.z;
        out.nx = normal.x;
        out.ny = normal.y;
        out.nz = normal.z;
        out.tx = tangent.x;
        out.ty = tangent.y;
        out.tz = tangent.z;
        stats_.max_vertex_displacement =
            std::max(stats_.max_vertex_displacement, core::length(p - bp));
    }
    return vertices_;
}

} // namespace rime::block_play
