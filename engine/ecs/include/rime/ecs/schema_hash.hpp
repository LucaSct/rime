// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

namespace rime::ecs {

class World;

// A fingerprint of the world's registered component SCHEMA — the set of reflected component types —
// for the m11.2 connect handshake (ADR-0033 §4): two processes may replicate to each other only if
// they agree on the shape of every component either might serialize. A client running a different
// build is then rejected at connect with an actionable diagnostic, instead of silently misreading
// every snapshot that follows (the same bet the M9 editor protocol already proved over its wire).
//
// The fold is over each registered component's `TypeInfo::type_hash`, SORTED ASCENDING and then
// FNV-1a'd in that order. Sorting is the load-bearing step, and it is easy to get wrong: a
// component's `ComponentId` is assigned in REGISTRATION order, and registration order is not a
// contract — two processes running the identical binary can register the same set in a different
// sequence (a different call order, a system registering lazily on first use). The hash of a SET
// must depend on the set, never on the history of how it was built. editorhost's snapshot format
// keys components by `type_hash` rather than `ComponentId` for exactly this reason.
//
// Nested types need no separate walk: `compute_type_hash` already folds each nested struct's
// fingerprint recursively, and (since ADR-0033 amendment A2) the type's own registered name, so a
// component's `type_hash` is a fingerprint of its full transitive shape. That amendment is what
// makes this hash trustworthy at all — before it, two structurally identical components
// (`LocalTransform` and `WorldTransform`, both `{ core::Transform value; }`) shared one
// fingerprint, so a schema could change in a way this number could not see.
//
// UNREFLECTED components are deliberately skipped. They carry no inspectable state and can never
// cross the wire, so their presence or absence cannot make two builds incompatible — and including
// them would make the handshake fail over a purely local tag component.
//
// This is `ecs`'s function, not `net`'s: `engine/net` treats the schema hash as an opaque
// `std::uint64_t` supplied by the app (`NetDriver::Config::schema_hash`), which keeps the net
// module free of an ecs dependency and lets a non-ECS tool drive a session with 0. A game that
// wants its own content version in the handshake combines it into that u64 before handing it over.
[[nodiscard]] std::uint64_t component_schema_hash(const World& world) noexcept;

} // namespace rime::ecs
