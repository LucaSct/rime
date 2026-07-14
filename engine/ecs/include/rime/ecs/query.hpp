// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
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
// The same iteration runs across all cores with par_for_each, which hands whole chunks to the job
// system — one chunk per task, so no two workers ever touch the same cache line (M4.4a; see that
// method's comment for the grain and the thread-safety contract):
//
//   world.query<Position, Velocity>().par_for_each(jobs, [](Position& p, Velocity& v) { … });
//
// If any of Ts was never registered, no entity can have it, so the query is simply empty.
//
// Structural-change rule: the body may freely read and write component DATA, but must NOT
// add/remove components or spawn/despawn — that would restructure the very archetypes being
// iterated (and, under par_for_each, concurrently). Deferred structural changes arrive with the
// system scheduler (M4.4b).
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

    // Change-tracking for_each (ADR-0018 §4): visit only the entities in chunks where at least one
    // of this query's component columns was written AFTER `since`. The "process only what moved"
    // workhorse — e.g. re-upload to the GPU only the WorldTransforms physics stamped this tick, or
    // sync to the editor only the components an edit touched. The grain is the chunk (that is the
    // change-detection scope), so a chunk with any recent write is scanned in full; pass the
    // version you remembered last time you ran, and the world will have advanced past it
    // (World::version()).
    template <class F> void for_each_changed(Version since, F&& f) {
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
                Chunk& chunk = arch.chunk(ci);
                if (chunk_changed(chunk, since)) {
                    visit_chunk(chunk, f, std::index_sequence_for<Ts...>{});
                }
            }
        }
    }

    // Parallel form of for_each: run `f` over every matching entity across all cores, on the job
    // system. The parallel GRAIN is the chunk (ADR-0018): each chunk is a separate pooled buffer,
    // so handing whole chunks to different workers guarantees that no two tasks ever write the same
    // cache line — data-parallelism with no false sharing and no locks, the payoff the chunk grain
    // was designed for. We flatten every matching chunk (across every matching archetype) into one
    // list and issue a SINGLE parallel_for over it, so the job system load-balances the whole query
    // at once rather than joining once per archetype. `grain` is chunks-per-task (default 1, the
    // ADR's natural unit); raise it when the body is so cheap that per-task overhead would dominate
    // (M4.6 measures and tunes it).
    //
    // Thread-safety contract: the body runs concurrently on DISJOINT rows, so writing through the
    // component references it is handed is always race-free (different chunks ⇒ different memory).
    // Any state the body captures and SHARES across invocations (a counter, a container) is the
    // caller's to synchronize — exactly as with core::JobSystem::parallel_for. The
    // structural-change rule of for_each holds and tightens: the body must not add/remove
    // components or spawn/despawn.
    template <class F> void par_for_each(core::JobSystem& jobs, F&& f, std::uint32_t grain = 1) {
        if (!valid_) {
            return;
        }
        // Snapshot the matching chunks on the calling thread (the world is not restructured during
        // iteration, so this list stays valid for the whole parallel region). Each entry is one
        // task's worth of work.
        std::vector<Chunk*> chunks;
        const std::size_t archetypes = world_->archetype_count();
        for (std::size_t ai = 0; ai < archetypes; ++ai) {
            Archetype& arch = world_->archetype(ai);
            if (!arch.signature().contains_all(signature_)) {
                continue;
            }
            const auto n = static_cast<std::uint32_t>(arch.chunk_count());
            for (std::uint32_t ci = 0; ci < n; ++ci) {
                chunks.push_back(&arch.chunk(ci));
            }
        }
        // One task per `grain` chunks; each task scans its chunk's rows exactly as the serial path
        // does. parallel_for joins before returning, so `chunks`, `f`, and *this outlive every
        // task.
        jobs.parallel_for(chunks.size(), grain, [this, &chunks, &f](std::size_t i) {
            visit_chunk(*chunks[i], f, std::index_sequence_for<Ts...>{});
        });
    }

    // Parallel change-tracking iteration: par_for_each restricted to chunks that changed since
    // `since` (ADR-0018 §4). Only the surviving chunks become tasks, so a mostly-static world
    // spends work proportional to what moved, not to its size — the payoff that pairs with M7.5
    // sleeping (physics writes back, and stamps, only awake bodies, so this visits only awake
    // islands' chunks).
    template <class F>
    void
    par_for_each_changed(core::JobSystem& jobs, Version since, F&& f, std::uint32_t grain = 1) {
        if (!valid_) {
            return;
        }
        std::vector<Chunk*> chunks;
        const std::size_t archetypes = world_->archetype_count();
        for (std::size_t ai = 0; ai < archetypes; ++ai) {
            Archetype& arch = world_->archetype(ai);
            if (!arch.signature().contains_all(signature_)) {
                continue;
            }
            const auto n = static_cast<std::uint32_t>(arch.chunk_count());
            for (std::uint32_t ci = 0; ci < n; ++ci) {
                Chunk& chunk = arch.chunk(ci);
                if (chunk_changed(chunk, since)) {
                    chunks.push_back(&chunk);
                }
            }
        }
        jobs.parallel_for(chunks.size(), grain, [this, &chunks, &f](std::size_t i) {
            visit_chunk(*chunks[i], f, std::index_sequence_for<Ts...>{});
        });
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

    // True iff at least one of this query's component columns on `chunk` was written after `since`
    // — the per-chunk skip test the *_changed iterations use. "Any column" is the useful default:
    // a query asking for what it reads-and-writes wants the chunk if any of it moved.
    [[nodiscard]] bool chunk_changed(const Chunk& chunk, Version since) const {
        for (const ComponentId id : ids_) {
            if (chunk.column_version(id) > since) {
                return true;
            }
        }
        return false;
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
