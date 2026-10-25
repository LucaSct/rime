// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/interaction.hpp"

#include <algorithm>
#include <cmath>

namespace rime::replication {

using rime::core::Quat;
using rime::core::Vec3;

MembershipDecision InteractionMembershipPolicy::update(const InteractionSample& s) {
    const bool moving = s.speed > params_.rest_speed;
    // The quiet counter runs whether or not we are a member, but only a member reads it; any motion
    // or touch restarts it, so "30 ticks at rest" means 30 CONSECUTIVE ones.
    quiet_ = (moving || s.touching) ? 0u : quiet_ + 1u;

    if (!member_) {
        if (s.distance <= params_.join_radius()) {
            member_ = true;
            ++counters_.joins;
            if (s.touching)
                ++counters_.joined_in_contact;
            return MembershipDecision::Join;
        }
        return MembershipDecision::Stay;
    }
    if (s.distance > params_.leave_radius()) {
        if (quiet_ >= params_.rest_ticks) {
            member_ = false;
            ++counters_.leaves;
            quiet_ = 0;
            return MembershipDecision::Leave;
        }
        if (s.touching)
            ++counters_.leave_deferred_touching;
        else if (moving)
            ++counters_.leave_deferred_moving;
    }
    return MembershipDecision::Stay;
}

float interaction_raw_bound(float v_push_max,
                            float j_max,
                            float mass,
                            std::uint32_t replay_ticks,
                            double fixed_dt) {
    const double speed = static_cast<double>(v_push_max) + static_cast<double>(j_max) / mass;
    return static_cast<float>(speed * replay_ticks * fixed_dt);
}

CorrectionSmoother::Entry* CorrectionSmoother::find(PredictedEntityId id) {
    for (Entry& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

const CorrectionSmoother::Entry* CorrectionSmoother::find(PredictedEntityId id) const {
    for (const Entry& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

CorrectionSmoother::Entry& CorrectionSmoother::find_or_add(PredictedEntityId id) {
    if (Entry* e = find(id))
        return *e;
    entries_.push_back(Entry{id, {}, rime::core::quat_identity()});
    return entries_.back();
}

void CorrectionSmoother::on_correction(PredictedEntityId id, Vec3 before, Vec3 after) {
    Entry& e = find_or_add(id);
    const Vec3 offset = before - after;
    const float len = rime::core::length(offset);
    ++counters_.corrections;
    counters_.max_raw_jump = std::max(counters_.max_raw_jump, len);
    if (len > params_.snap_distance) {
        // Too big to hide: the object visibly jumps. That jump IS a presented jump, so it counts.
        ++counters_.snaps;
        counters_.max_presented_jump = std::max(counters_.max_presented_jump, len);
        e.offset = Vec3{};
    } else {
        // Presented position is unchanged by construction: before == after + offset.
        e.offset = offset;
    }
}

namespace {
// Rotation angle of a unit quaternion, in [0, pi], taking the short way round.
float angle_of(const Quat& q) {
    return 2.0f * std::acos(std::min(1.0f, std::abs(q.w)));
}
} // namespace

void CorrectionSmoother::on_correction(PredictedEntityId id, Quat before, Quat after) {
    Entry& e = find_or_add(id);
    // offset * after == before, so presented = offset * simulated reproduces what was on screen.
    Quat offset = rime::core::normalize(before * rime::core::inverse(after));
    if (offset.w < 0.0f)
        offset = -offset; // q and -q are one rotation; keep the short arc
    const float angle = angle_of(offset);
    ++counters_.rotation_corrections;
    counters_.max_raw_angle = std::max(counters_.max_raw_angle, angle);
    if (angle > params_.snap_radians) {
        ++counters_.rotation_snaps;
        e.rotation = rime::core::quat_identity();
    } else {
        e.rotation = offset;
    }
}

void CorrectionSmoother::advance(double dt) {
    const float k = params_.tau_seconds > 0.0
                        ? static_cast<float>(std::exp(-dt / params_.tau_seconds))
                        : 0.0f; // tau -> 0 means "no smoothing": the offset vanishes at once
    for (Entry& e : entries_) {
        const Vec3 next = e.offset * k;
        counters_.max_presented_jump =
            std::max(counters_.max_presented_jump, rime::core::length(next - e.offset));
        e.offset = next;
        // Scaling the rotation angle by k about the same axis is the exponential decay of a
        // rotation offset.
        const float a = angle_of(e.rotation);
        if (a > 1e-7f) {
            const float s = std::sqrt(std::max(0.0f, 1.0f - e.rotation.w * e.rotation.w));
            if (s > 1e-7f) {
                const Vec3 axis{e.rotation.x / s, e.rotation.y / s, e.rotation.z / s};
                e.rotation = rime::core::quat_from_axis_angle(axis, a * k);
            }
        }
    }
}

Vec3 CorrectionSmoother::presented(PredictedEntityId id, Vec3 simulated) const {
    const Entry* e = find(id);
    return e ? simulated + e->offset : simulated;
}

Quat CorrectionSmoother::presented(PredictedEntityId id, Quat simulated) const {
    const Entry* e = find(id);
    return e ? rime::core::normalize(e->rotation * simulated) : simulated;
}

void CorrectionSmoother::forget(PredictedEntityId id) {
    std::erase_if(entries_, [&](const Entry& e) { return e.id == id; });
}

float CorrectionSmoother::offset_length(PredictedEntityId id) const {
    const Entry* e = find(id);
    return e ? rime::core::length(e->offset) : 0.0f;
}

} // namespace rime::replication
