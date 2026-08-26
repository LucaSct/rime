// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"

// The one component that makes a networked player possible (m12.3, ADR-0035 §4).
namespace rime::ecs {
class World;
}

namespace rime::gameplay_net {

// The newest input sequence the server had CONSUMED when it wrote this entity's state.
//
// THIS IS THE PIVOT OF THE WHOLE DESIGN, so it is worth being precise about what it claims. It is
// not "the last packet I received from you" and it is not "how far I have acknowledged". It is: the
// authoritative `CharacterState` you are looking at on this entity is exactly what you get by
// running the mover over your commands up to and including `sequence`, and over nothing after it.
// That pairing — state and input-position, in one record, written in the same tick — is what m12.4
// reconciles against.
//
// WHY IT RIDES THE ORDINARY SNAPSHOT PATH rather than a message of its own. The pairing is only
// worth anything if the two halves cannot come apart. A separate message would have to be ordered
// against the Delta carrying the state it describes, across two channels ADR-0033 §3 deliberately
// gives no ordering between — so a client could hold state from tick T and a sequence from tick
// T+2 and reconcile against a pairing that never existed on the server. Putting the number IN the
// record removes the question: the client either has both or neither, because they are the same
// bytes. This is A15's `DebrisOrigin` move, made for the same reason and reused deliberately.
//
// WHY IT MUST BE ON THE ENTITY AND NOT IN THE ACK. `InputAck` already carries a per-session
// `consumed_through` (input.hpp) and it is tempting to reconcile against that instead. It does not
// work: the ack rides an unreliable stream that supersedes, so the newest ack a client holds is
// routinely FRESHER than the newest snapshot it holds. Pairing "the state I have from tick T" with
// "the frontier you reported at tick T+2" claims the server had applied commands it had not yet
// applied when it wrote that state, and the predictor would then throw away commands it still
// needs to replay. The ack's job is retiring the send buffer; this component's job is naming a
// state. They are different frontiers and collapsing them is the same shape of mistake as
// collapsing `received_through` into `consumed_through`.
//
// A TICK ON WHICH NO COMMAND WAS CONSUMED DOES NOT ADVANCE IT. The server steps the mover exactly
// once per consumed command and not at all on a starved tick (gameplay_server.hpp), so a stale
// sequence beside an unchanged state is the truth: nothing of yours has been acted on since.
struct LastProcessedInput {
    std::uint32_t sequence = 0;
};

// Register the gameplay_net components with a world — id + size + reflection TypeInfo in one shot,
// so replication picks the component up through the ordinary wire-schema build. Idempotent, and
// BOTH PEERS MUST CALL IT: the schema hash is part of the connection handshake, so a server that
// registers this and a client that does not are two different builds and the session is refused
// with a diagnostic rather than silently desynchronised.
void register_gameplay_net_components(ecs::World& world);

} // namespace rime::gameplay_net

// Reflection (outside the namespace — the macros open rime::core themselves).
RIME_REFLECT_BEGIN(rime::gameplay_net::LastProcessedInput)
RIME_REFLECT_FIELD(sequence)
RIME_REFLECT_END()
