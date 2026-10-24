// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "../../engine/render/src/material_binding_cache.hpp"

using namespace rime;

namespace {
struct Draw {
    std::array<rhi::TextureHandle, 5> maps{};
    rhi::SamplerHandle sampler{};
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 36;
    std::uint32_t flags = 0;
    bool accepted = true;
};

struct Recorder {
    std::array<rhi::TextureHandle, 5> maps{};
    std::array<rhi::SamplerHandle, 5> samplers{};
    std::uint64_t bindings = 0;
    std::uint64_t draws = 0;
    std::uint64_t hash = 14695981039346656037ull;

    void fold(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 255u;
            hash *= 1099511628211ull;
        }
    }

    // Observe EFFECTIVE pending state at every draw, not a hash of the binding calls: the
    // optimized stream deliberately contains fewer calls. Include the original draw's UBO
    // offset/range/flags to make a changed draw order or a skipped draw fail the same witness.
    void draw(const Draw& draw, std::size_t index) {
        fold(index * 256);
        fold(draw.first_index);
        fold(draw.index_count);
        fold(draw.flags);
        for (std::size_t i = 0; i < maps.size(); ++i) {
            fold(maps[i].index);
            fold(maps[i].generation);
            fold(samplers[i].index);
            fold(samplers[i].generation);
        }
        ++draws;
    }
};

void record_partition(Recorder& out, std::span<const Draw> draws, bool cached) {
    render::detail::MaterialBindingCache bindings;
    const auto issue =
        [&out](std::uint32_t binding, rhi::TextureHandle texture, rhi::SamplerHandle sampler) {
            const auto slot = binding - 2;
            out.maps[slot] = texture;
            out.samplers[slot] = sampler;
            ++out.bindings;
        };
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const auto& draw = draws[i];
        if (!draw.accepted)
            continue;
        for (std::size_t slot = 0; slot < draw.maps.size(); ++slot) {
            if (cached)
                bindings.bind(slot, draw.maps[slot], draw.sampler, issue);
            else
                issue(static_cast<std::uint32_t>(slot + 2), draw.maps[slot], draw.sampler);
        }
        out.draw(draw, i);
    }
}

std::vector<Draw> repeated(std::size_t count) {
    std::vector<Draw> draws(count);
    for (std::size_t i = 0; i < draws.size(); ++i) {
        draws[i].maps = {{{1, 0}, {1, 0}, {2, 0}, {1, 0}, {1, 0}}};
        draws[i].sampler = {3, 0};
        draws[i].first_index = static_cast<std::uint32_t>(i % 4) * 36;
        draws[i].index_count += static_cast<std::uint32_t>(i % 3) * 3;
        draws[i].flags = static_cast<std::uint32_t>(i % 4);
    }
    return draws;
}

Recorder verify(const char* name, std::span<const Draw> draws) {
    Recorder reference, candidate;
    record_partition(reference, draws, false);
    record_partition(candidate, draws, true);
    CHECK(candidate.hash == reference.hash);
    CHECK(candidate.draws == reference.draws);
    CHECK(candidate.bindings <= reference.bindings);
    std::printf("[material-binding-proof] %s draws=%llu reference=%llu cached=%llu hash=%016llx\n",
                name,
                static_cast<unsigned long long>(candidate.draws),
                static_cast<unsigned long long>(reference.bindings),
                static_cast<unsigned long long>(candidate.bindings),
                static_cast<unsigned long long>(candidate.hash));
    return candidate;
}
} // namespace

TEST_CASE("material bindings retain effective draw state with repeated and changing maps") {
    auto draws = repeated(2206);
    CHECK(verify("repeated", draws).bindings == 5);

    for (std::size_t i = 0; i < draws.size(); ++i)
        for (auto& map : draws[i].maps)
            map.index += static_cast<std::uint32_t>(i % 2) * 8;
    CHECK(verify("alternating", draws).bindings == draws.size() * 5);

    draws = repeated(2206);
    for (std::size_t i = 0; i < draws.size(); ++i)
        draws[i].maps[2].index += static_cast<std::uint32_t>(i % 2) * 8;
    CHECK(verify("normal-only", draws).bindings == draws.size() + 4);
}

TEST_CASE("material bindings include generation sampler changes and filtered draw gaps") {
    auto draws = repeated(2206);
    for (std::size_t i = 0; i < draws.size(); ++i) {
        draws[i].maps[0].generation = static_cast<std::uint32_t>(i / 16);
        draws[i].sampler.index += static_cast<std::uint32_t>((i / 32) % 2);
        draws[i].sampler.generation = static_cast<std::uint32_t>(i / 64);
        draws[i].accepted = i % 3 != 0;
    }
    const auto result = verify("generation-sampler-filter", draws);
    CHECK(result.draws == 1470);
    CHECK(result.bindings < result.draws * 5);

    // A stale-generation sensitivity control must change the effective-state witness. Likewise,
    // changing a range or the input order must not be hidden by the recorder's semantic hash.
    auto stale = draws;
    stale[1].maps[0].generation += 1;
    Recorder changed;
    record_partition(changed, stale, false);
    CHECK(changed.hash != result.hash);
    stale = draws;
    stale[1].first_index += 1;
    changed = {};
    record_partition(changed, stale, false);
    CHECK(changed.hash != result.hash);
    stale = draws;
    std::swap(stale[1], stale[2]);
    changed = {};
    record_partition(changed, stale, false);
    CHECK(changed.hash != result.hash);
}

TEST_CASE("material bindings restart every invocation including null first attachments") {
    const auto draws = repeated(1);
    Recorder reference, candidate;
    record_partition(reference, draws, false);
    record_partition(candidate, draws, true);
    // An intervening pipeline/partition/callback may leave arbitrary pending attachments. A new
    // local cache must issue all five again, even though its input equals the preceding draw.
    for (auto& map : candidate.maps)
        map = {91, 17};
    record_partition(reference, draws, false);
    record_partition(candidate, draws, true);
    CHECK(candidate.bindings == 10);
    CHECK(candidate.hash == reference.hash);
    CHECK(candidate.draws == reference.draws);

    std::array<Draw, 1> null_draw{};
    for (auto& map : candidate.maps)
        map = {92, 19};
    record_partition(reference, null_draw, false);
    record_partition(candidate, null_draw, true);
    CHECK(candidate.bindings == 15);
    CHECK(candidate.hash == reference.hash);
    CHECK(verify("empty", {}).bindings == 0);
}
