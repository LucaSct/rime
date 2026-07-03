// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/signature.hpp"
#include "rime/ecs/world.hpp"

// A Query<Ts...> is the iterate side of the ECS: it visits every entity that has ALL of the
// components Ts and hands the body a reference to each. It works by finding the archetypes whose
// signature is a superset of {Ts...} and scanning their chunks **column-wise** — the whole point of
// archetype storage. Each component is a contiguous array, so the body runs over packed data with
// no per-row lookups: the column base pointers are fetched once per chunk, then strided.
//
//   world.query<Position, Velocity>().for_each([](Position& p, Velocity& v) { p.x += v.dx; });
//   world.query<Position>().for_each([](Entity e, Position& p) { /* Entity first, optional */ });
//
// If any of Ts was never registered, no entity can have it, so the query is simply empty.
//
// Structural-change rule: the body may freely read and write component DATA, but must NOT
// add/remove components or spawn/despawn — that would restructure the very archetypes being
// iterated. Deferred structural changes arrive with the system scheduler (M4.4).
namespace rime::ecs {

template <class... Ts> class Query {
public:
    explicit Query(World& world) : world_(&world) {
        // Resolve each component id. Only if every T is registered do we remember the ids and the
        // signature to match against; an unregistered component is on no entity, so the query is
        // then empty.
        valid_ = (world_->is_registered<Ts>() && ...);
        if (valid_) {
            ids_ = std::array<ComponentId, sizeof...(Ts)>{world_->component_id<Ts>()...};
            signature_ = ComponentSignature{world_->component_id<Ts>()...};
        }
    }

    // Invoke `f` for every matching entity. `f` may be callable as f(Ts&...) or f(Entity, Ts&...).
    template <class F> void for_each(F&& f) {
        if (!valid_) {
            return;
        }
        const std::size_t archetypes = world_->archetype_count();
        for (std::size_t ai = 0; ai < archetypes; ++ai) {
            Archetype& arch = world_->archetype(ai);
            if (!arch.signature().contains_all(signature_)) {
                continue;
            }
            const auto chunks = static_cast<std::uint32_t>(arch.chunk_count());
            for (std::uint32_t ci = 0; ci < chunks; ++ci) {
                visit_chunk(arch.chunk(ci), f, std::index_sequence_for<Ts...>{});
            }
        }
    }

    // The number of entities the query matches.
    [[nodiscard]] std::size_t count() const {
        if (!valid_) {
            return 0;
        }
        std::size_t n = 0;
        const std::size_t archetypes = world_->archetype_count();
        for (std::size_t ai = 0; ai < archetypes; ++ai) {
            const Archetype& arch = world_->archetype(ai);
            if (arch.signature().contains_all(signature_)) {
                n += arch.entity_count();
            }
        }
        return n;
    }

    [[nodiscard]] bool empty() const { return count() == 0; }

private:
    template <class F, std::size_t... Is>
    void visit_chunk(Chunk& chunk, F& f, std::index_sequence<Is...>) {
        // Fetch each requested column's packed base pointer once, typed — then stride. (Ts and Is
        // are expanded in lockstep, so column Is is the base of component Ts[Is].)
        [[maybe_unused]] const std::tuple<Ts*...> columns{
            static_cast<Ts*>(chunk.column(ids_[Is]))...};
        [[maybe_unused]] const Entity* entities = chunk.entities();
        const std::uint32_t rows = chunk.size();
        for (std::uint32_t r = 0; r < rows; ++r) {
            if constexpr (std::is_invocable_v<F&, Entity, Ts&...>) {
                f(entities[r], std::get<Is>(columns)[r]...);
            } else {
                static_assert(std::is_invocable_v<F&, Ts&...>,
                              "query body must be callable as f(Ts&...) or f(Entity, Ts&...)");
                f(std::get<Is>(columns)[r]...);
            }
        }
    }

    World* world_;
    std::array<ComponentId, sizeof...(Ts)> ids_{};
    ComponentSignature signature_;
    bool valid_ = false;
};

// Out-of-line definition of World::query (Query is incomplete at the point of declaration in
// world.hpp). Include this header wherever you build queries.
template <class... Ts> Query<Ts...> World::query() {
    return Query<Ts...>(*this);
}

} // namespace rime::ecs
