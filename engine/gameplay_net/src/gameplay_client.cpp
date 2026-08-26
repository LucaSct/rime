// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay_net/gameplay_client.hpp"

#include "rime/core/byte_cursor.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay_net/components.hpp"

namespace rime::gameplay_net {

std::size_t GameplayClient::apply_messages(std::span<const net::Received> messages) {
    std::size_t consumed = 0;
    for (const net::Received& message : messages) {
        core::ByteReader reader{message.bytes};
        std::uint8_t tag = 0;
        if (!reader.u8(tag)) {
            ++malformed_;
            continue;
        }
        if (!owns_tag(tag)) {
            continue; // replication's, destruction_net's, or a module that does not exist yet
        }
        if (static_cast<MessageTag>(tag) != MessageTag::AssignPlayer) {
            ++malformed_; // our block, but not a message we define
            continue;
        }

        replication::NetId id{};
        if (!reader.u32(id.index) || !reader.u32(id.generation)) {
            ++malformed_;
            continue;
        }
        // Accepted verbatim, including a re-send of the id we already hold. There is deliberately
        // no "already assigned" rejection: the server re-announces by diffing, so a benign
        // retransmit is normal traffic, and refusing one would turn it into a broken session.
        local_player_ = id;
        ++assignments_received_;
        ++consumed;
    }
    return consumed;
}

std::size_t GameplayClient::apply_inbound(net::NetDriver& driver) {
    std::size_t consumed = 0;
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr) {
            continue;
        }
        inbox_.clear();
        (void)session->drain_received(inbox_);
        consumed += apply_messages(inbox_);
    }
    return consumed;
}

ecs::Entity GameplayClient::local_player(const replication::NetIdMap& map) const noexcept {
    if (!local_player_.is_valid()) {
        return ecs::kNullEntity;
    }
    return map.resolve(local_player_);
}

std::uint32_t GameplayClient::last_processed_input(const ecs::World& world,
                                                   const replication::NetIdMap& map) const {
    const ecs::Entity player = local_player(map);
    if (!player.is_valid()) {
        return 0;
    }
    const LastProcessedInput* last = world.get<LastProcessedInput>(player);
    return last != nullptr ? last->sequence : 0u;
}

void GameplayClient::reset() noexcept {
    local_player_ = replication::kNullNetId;
}

} // namespace rime::gameplay_net
