// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/spawn_prediction.hpp"

#include <algorithm>
#include <utility>

namespace rime::replication {
namespace {

// Same little-endian discipline as prediction.cpp's checkpoint codec. Duplicated rather than shared
// because prediction.cpp keeps its helpers private, and a codec's byte order should be readable in
// the file that defines its layout.
struct Writer {
    std::span<std::byte> bytes;
    std::size_t at = 0;

    void u8(std::uint8_t value) { bytes[at++] = static_cast<std::byte>(value); }

    void u16(std::uint16_t value) {
        for (unsigned i = 0; i < 2; ++i)
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void u32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void key(const SpawnKey& value) {
        u32(value.incarnation);
        u32(value.sequence);
        u8(value.slot);
    }

    void id(PredictedEntityId value) {
        u32(value.index);
        u32(value.generation);
    }
};

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;

    [[nodiscard]] std::size_t remaining() const { return bytes.size() - at; }

    std::uint8_t u8() { return std::to_integer<std::uint8_t>(bytes[at++]); }

    std::uint16_t u16() {
        std::uint16_t value = 0;
        for (unsigned i = 0; i < 2; ++i)
            value |= static_cast<std::uint16_t>(std::uint16_t{u8()} << (8 * i));
        return value;
    }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= std::uint32_t{u8()} << (8 * i);
        return value;
    }

    SpawnKey key() {
        SpawnKey value{};
        value.incarnation = u32();
        value.sequence = u32();
        value.slot = u8();
        return value;
    }

    PredictedEntityId id() {
        PredictedEntityId value{};
        value.index = u32();
        value.generation = u32();
        return value;
    }
};

[[nodiscard]] std::size_t extension_bytes(const SpawnCheckpointExtension& ext) {
    return kSpawnExtensionHeaderBytes + kSpawnExtensionFixedBodyBytes +
           ext.bindings.size() * kSpawnBindingBytes + ext.outcomes.size() * kSpawnOutcomeBytes;
}

[[nodiscard]] bool in_roster(const OwnerCheckpoint& base, PredictedEntityId id) {
    return std::any_of(base.entities.begin(), base.entities.end(), [&](const auto& entity) {
        return entity.id == id;
    });
}

[[nodiscard]] bool contains(std::span<const PredictedEntityId> ids, PredictedEntityId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

[[nodiscard]] bool valid_kind(std::uint8_t kind) {
    return kind >= static_cast<std::uint8_t>(SpawnOutcomeKind::Accepted) &&
           kind <= static_cast<std::uint8_t>(SpawnOutcomeKind::AcceptedEnded);
}

} // namespace

std::size_t SpawnKeyHash::operator()(const SpawnKey& key) const noexcept {
    // splitmix64's finaliser over the packed key: keys differ mostly in their low sequence bits,
    // and an unordered_map buckets on low bits, so the bits are mixed before they are used.
    std::uint64_t value = (std::uint64_t{key.incarnation} << 32) | key.sequence;
    value ^= std::uint64_t{key.slot} * 0x9E3779B97F4A7C15ull;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31;
    return static_cast<std::size_t>(value);
}

// ---------------------------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------------------------

SpawnExtensionRefusal validate_spawn_extension(const OwnerCheckpoint& base,
                                               const SpawnCheckpointExtension& ext) {
    if (ext.bindings.size() > kSpawnExtensionMaxBindings)
        return SpawnExtensionRefusal::TooManyBindings;
    if (ext.outcomes.size() > kSpawnExtensionMaxOutcomes)
        return SpawnExtensionRefusal::TooManyOutcomes;
    for (const auto& entity : base.entities) {
        if ((entity.id.index & kProvisionalIndexBit) != 0)
            return SpawnExtensionRefusal::ProvisionalIdInRoster;
    }
    for (std::size_t i = 0; i < ext.bindings.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (ext.bindings[i].key == ext.bindings[j].key ||
                ext.bindings[i].id == ext.bindings[j].id)
                return SpawnExtensionRefusal::DuplicateBinding;
        }
        if (!in_roster(base, ext.bindings[i].id))
            return SpawnExtensionRefusal::BindingNotInRoster;
    }
    for (std::size_t i = 0; i < ext.outcomes.size(); ++i) {
        const auto& outcome = ext.outcomes[i];
        if (!valid_kind(static_cast<std::uint8_t>(outcome.kind)))
            return SpawnExtensionRefusal::BadOutcomeKind;
        const bool has_id = !(outcome.id == PredictedEntityId{});
        if ((outcome.kind == SpawnOutcomeKind::Accepted && outcome.reason != 0) ||
            (outcome.kind == SpawnOutcomeKind::Rejected && has_id) ||
            (outcome.kind == SpawnOutcomeKind::AcceptedEnded && (has_id || outcome.reason != 0)))
            return SpawnExtensionRefusal::ReservedNonZero;
        if (i > 0 && !(ext.outcomes[i - 1].key < outcome.key))
            return SpawnExtensionRefusal::OutcomesNotAscending;
        if (ext.acknowledged_through && outcome.key <= *ext.acknowledged_through)
            return SpawnExtensionRefusal::OutcomeAtOrBelowFrontier;
        const auto binding =
            std::find_if(ext.bindings.begin(), ext.bindings.end(), [&](const auto& candidate) {
                return candidate.key == outcome.key;
            });
        // An accepted spawn that is alive IS a roster entity, so its binding must say so; a
        // rejected or ended one is not, so a binding would claim a projectile the server says does
        // not exist.
        const bool consistent = outcome.kind == SpawnOutcomeKind::Accepted
                                    ? binding != ext.bindings.end() && binding->id == outcome.id
                                    : binding == ext.bindings.end();
        if (!consistent)
            return SpawnExtensionRefusal::OutcomeBindingMismatch;
    }
    return SpawnExtensionRefusal::None;
}

SpawnCheckpointEncode encode_spawn_checkpoint(const OwnerCheckpoint& base,
                                              const SpawnCheckpointExtension& ext,
                                              std::span<std::byte> out) {
    // Encode the base into scratch first so "writes nothing on refusal" holds for the extension's
    // refusals too, not only for the base's.
    std::array<std::byte, kCheckpointMaxMessageBytes> scratch{};
    const auto encoded = encode_owner_checkpoint(base, scratch);
    if (!encoded)
        return {0, encoded.refusal, SpawnExtensionRefusal::None};
    if (const auto refusal = validate_spawn_extension(base, ext);
        refusal != SpawnExtensionRefusal::None)
        return {0, CheckpointRefusal::None, refusal};
    const auto ext_size = extension_bytes(ext);
    const auto total = encoded.bytes_written + ext_size;
    if (total > kCheckpointMaxMessageBytes)
        return {0, CheckpointRefusal::None, SpawnExtensionRefusal::MessageTooLarge};
    if (out.size() < total)
        return {0, CheckpointRefusal::None, SpawnExtensionRefusal::Truncated};

    std::copy_n(scratch.begin(), encoded.bytes_written, out.begin());
    Writer writer{out, encoded.bytes_written};
    writer.u8(kSpawnExtensionTag);
    writer.u8(kSpawnExtensionVersion);
    writer.u16(static_cast<std::uint16_t>(ext_size - kSpawnExtensionHeaderBytes));
    writer.u8(static_cast<std::uint8_t>(ext.bindings.size()));
    writer.u8(static_cast<std::uint8_t>(ext.outcomes.size()));
    writer.u8(ext.acknowledged_through ? 1 : 0);
    writer.key(ext.acknowledged_through.value_or(SpawnKey{}));
    for (const auto& binding : ext.bindings) {
        writer.key(binding.key);
        writer.id(binding.id);
    }
    for (const auto& outcome : ext.outcomes) {
        writer.key(outcome.key);
        writer.u8(static_cast<std::uint8_t>(outcome.kind));
        writer.id(outcome.id);
        writer.u8(outcome.reason);
    }
    return {writer.at, CheckpointRefusal::None, SpawnExtensionRefusal::None};
}

SpawnCheckpointDecode decode_spawn_checkpoint(std::span<const std::byte> in, SpawnCheckpoint& out) {
    SpawnCheckpoint decoded{};
    std::size_t consumed = 0;
    if (const auto refusal = decode_owner_checkpoint_prefix(in, decoded.base, consumed);
        refusal != CheckpointRefusal::None)
        return {refusal, SpawnExtensionRefusal::None};
    if (consumed == in.size()) {
        // An m20.4-only checkpoint: still a valid message, just one without verdicts.
        out = std::move(decoded);
        return {};
    }

    Reader reader{in.subspan(consumed)};
    if (reader.remaining() < kSpawnExtensionHeaderBytes)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::Truncated};
    if (reader.u8() != kSpawnExtensionTag)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::BadTag};
    if (reader.u8() != kSpawnExtensionVersion)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::UnknownVersion};
    const std::size_t body = reader.u16();
    if (reader.remaining() < body)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::Truncated};
    if (reader.remaining() > body || body < kSpawnExtensionFixedBodyBytes)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::BadLength};
    const std::size_t binding_count = reader.u8();
    const std::size_t outcome_count = reader.u8();
    if (binding_count > kSpawnExtensionMaxBindings)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::TooManyBindings};
    if (outcome_count > kSpawnExtensionMaxOutcomes)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::TooManyOutcomes};
    const auto frontier_flag = reader.u8();
    if (frontier_flag > 1)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::BadFrontierFlag};
    if (body != kSpawnExtensionFixedBodyBytes + binding_count * kSpawnBindingBytes +
                    outcome_count * kSpawnOutcomeBytes)
        return {CheckpointRefusal::None, SpawnExtensionRefusal::BadLength};
    const auto frontier = reader.key();
    if (frontier_flag == 0 && !(frontier == SpawnKey{}))
        return {CheckpointRefusal::None, SpawnExtensionRefusal::ReservedNonZero};

    SpawnCheckpointExtension ext{};
    if (frontier_flag == 1)
        ext.acknowledged_through = frontier;
    ext.bindings.reserve(binding_count);
    for (std::size_t i = 0; i < binding_count; ++i) {
        SpawnBinding binding{};
        binding.key = reader.key();
        binding.id = reader.id();
        ext.bindings.push_back(binding);
    }
    ext.outcomes.reserve(outcome_count);
    for (std::size_t i = 0; i < outcome_count; ++i) {
        SpawnOutcome outcome{};
        outcome.key = reader.key();
        const auto kind = reader.u8();
        // Checked before the cast so an out-of-range byte never becomes an enum value.
        if (!valid_kind(kind))
            return {CheckpointRefusal::None, SpawnExtensionRefusal::BadOutcomeKind};
        outcome.kind = static_cast<SpawnOutcomeKind>(kind);
        outcome.id = reader.id();
        outcome.reason = reader.u8();
        ext.outcomes.push_back(outcome);
    }
    if (const auto refusal = validate_spawn_extension(decoded.base, ext);
        refusal != SpawnExtensionRefusal::None)
        return {CheckpointRefusal::None, refusal};
    decoded.spawns = std::move(ext);
    out = std::move(decoded);
    return {};
}

SpawnCheckpointDecode SpawnCheckpointDecoder::decode(std::span<const std::byte> in,
                                                     SpawnCheckpoint& out) {
    const auto result = decode_spawn_checkpoint(in, out);
    if (result.base != CheckpointRefusal::None)
        ++base_refusals_;
    else if (result.extension != SpawnExtensionRefusal::None)
        ++extension_refusals_[static_cast<std::size_t>(result.extension)];
    else if (out.spawns)
        ++extended_decoded_;
    else
        ++legacy_decoded_;
    return result;
}

// ---------------------------------------------------------------------------------------------
// PresentationLedger
// ---------------------------------------------------------------------------------------------

PresentationLedger::PresentationLedger(LedgerBudget budget) : budget_(budget) {}

PresentationLedger::Entry* PresentationLedger::find_or_open(const SpawnKey& key) {
    if (const auto it = entries_.find(key); it != entries_.end())
        return &it->second;
    if (active_ >= budget_.max_keys) {
        ++key_overflow_;
        return nullptr;
    }
    ++active_;
    return &entries_[key];
}

void PresentationLedger::tombstone(const SpawnKey& key) {
    --active_;
    tombstone_order_.push_back(key);
    while (tombstone_order_.size() > budget_.max_tombstones) {
        entries_.erase(tombstone_order_.front());
        tombstone_order_.erase(tombstone_order_.begin());
        ++tombstones_evicted_;
    }
}

bool PresentationLedger::should_play(const SpawnKey& key,
                                     PresentationEventKind kind,
                                     std::uint16_t ordinal) {
    Entry* entry = find_or_open(key);
    if (entry == nullptr)
        return false;
    if (entry->state == State::Cancelled) {
        ++suppressed_cancelled_;
        return false;
    }
    const auto triple = std::pair{kind, ordinal};
    if (std::find(entry->played.begin(), entry->played.end(), triple) != entry->played.end()) {
        ++duplicates_suppressed_;
        return false;
    }
    if (entry->played.size() >= budget_.max_events_per_key) {
        ++event_overflow_;
        return false;
    }
    entry->played.push_back(triple);
    ++played_;
    return true;
}

bool PresentationLedger::adopt(const SpawnKey& key, PredictedEntityId id) {
    Entry* entry = find_or_open(key);
    if (entry == nullptr)
        return false;
    if (entry->bound) {
        ++duplicate_adoptions_;
        return false;
    }
    entry->bound = id;
    ++adoptions_;
    return true;
}

std::vector<PresentationEvent> PresentationLedger::cancel(const SpawnKey& key) {
    std::vector<PresentationEvent> events;
    const auto it = entries_.find(key);
    if (it != entries_.end() && it->second.state == State::Cancelled) {
        ++duplicate_cancels_;
        return events;
    }
    ++cancels_;
    if (it == entries_.end()) {
        // Nothing was played, but the key must still refuse every later should_play: the guess is
        // known to be wrong. Opened past max_keys if need be -- it becomes a tombstone at once.
        ++active_;
        entries_[key].state = State::Cancelled;
        tombstone(key);
        return events;
    }
    for (const auto& [kind, ordinal] : it->second.played)
        events.push_back({key, kind, ordinal});
    const bool was_active = it->second.state == State::Active;
    it->second.state = State::Cancelled;
    if (was_active)
        tombstone(key);
    return events;
}

void PresentationLedger::release(const SpawnKey& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.state != State::Active)
        return;
    it->second.state = State::Released;
    tombstone(key);
}

std::optional<PredictedEntityId> PresentationLedger::bound(const SpawnKey& key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? std::nullopt : it->second.bound;
}

bool PresentationLedger::cancelled(const SpawnKey& key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() && it->second.state == State::Cancelled;
}

// ---------------------------------------------------------------------------------------------
// SpawnReconciler
// ---------------------------------------------------------------------------------------------

namespace {

PredictionHooks forwarding_hooks(PredictionHooks hooks,
                                 std::function<void(std::span<const PredictedEntityId>,
                                                    const InputCommand&,
                                                    const PredictionStep&)> simulate) {
    hooks.simulate = std::move(simulate);
    return hooks;
}

} // namespace

SpawnReconciler::SpawnReconciler(PredictionHooks hooks,
                                 SpawnHooks spawn_hooks,
                                 double fixed_dt,
                                 std::uint32_t incarnation,
                                 PresentationLedger* ledger,
                                 ReconcilerBudget budget,
                                 SpawnBudget spawn_budget)
    : hooks_(std::move(hooks)), spawn_hooks_(std::move(spawn_hooks)), spawn_budget_(spawn_budget),
      ledger_(ledger),
      inner_(
          forwarding_hooks(hooks_,
                           [this](std::span<const PredictedEntityId> roster,
                                  const InputCommand& command,
                                  const PredictionStep& step) { simulate(roster, command, step); }),
          fixed_dt,
          budget),
      incarnation_(incarnation) {}

void SpawnReconciler::set_membership(std::uint32_t epoch, std::vector<PredictedEntityId> members) {
    roster_ = members;
    epoch_ = epoch;
    inner_.set_membership(epoch, std::move(members));
}

void SpawnReconciler::simulate(std::span<const PredictedEntityId> roster,
                               const InputCommand& command,
                               const PredictionStep& step) {
    // The inner Reconciler only knows the roster; the game's step runs over the roster PLUS the
    // live guesses, so a provisional projectile flies exactly as an authoritative one would. A
    // guess the replay has not yet recreated (Dropped) is left out: stepping it would advance a
    // projectile whose launch, under the corrected state, may never happen.
    std::vector<PredictedEntityId> members(roster.begin(), roster.end());
    for (const auto& guess : provisional_) {
        if (guess.life == Life::Live)
            members.push_back(guess.id);
    }
    in_step_ = true;
    step_sequence_ = command.sequence;
    hooks_.simulate(members, command, step);
    in_step_ = false;
    // Snapshot every live guess AFTER this command, keyed by its sequence. When a guess's command
    // retires before its verdict arrives, replay cannot recreate it -- so it is restored to its own
    // state as of `retired_through` and replayed forward like everything else.
    for (auto& guess : provisional_) {
        if (guess.life != Life::Live)
            continue;
        guess.snapshots.push_back({command.sequence, hooks_.capture(guess.id)});
        if (guess.snapshots.size() > spawn_budget_.max_snapshots) {
            guess.snapshots.erase(guess.snapshots.begin());
            ++snapshots_evicted_;
        }
    }
}

std::optional<PredictedEntityId>
SpawnReconciler::spawn_provisional(const SpawnKey& key, std::span<const std::byte> initial_state) {
    if (!in_step_) {
        ++spawn_outside_step_;
        return std::nullopt;
    }
    // A key that does not name the executing command could not be recreated by a replay of it, and
    // the server, which builds the key from the command it executed, would never echo it.
    if (key.incarnation != incarnation_ || key.sequence != step_sequence_) {
        ++spawn_bad_key_;
        return std::nullopt;
    }
    // Already resolved -- expired locally while its command was still unretired, typically. The
    // verdict, not a replay, decides what the player sees from here.
    if (resolution(key) != nullptr) {
        ++respawn_suppressed_;
        return std::nullopt;
    }
    if (Provisional* guess = find(key)) {
        if (guess->life == Life::Dropped) {
            // The replay recreating its own guess: same key, SAME local id, so presentation keyed
            // by either never sees a second projectile.
            hooks_.restore(guess->id, initial_state);
            guess->life = Life::Live;
            ++recreated_;
            if (active_outcome_ != nullptr)
                ++active_outcome_->recreated;
            return guess->id;
        }
        ++spawn_duplicate_;
        return std::nullopt;
    }
    if (provisional_.size() >= spawn_budget_.max_provisional) {
        ++provisional_overflow_;
        return std::nullopt;
    }
    const auto serial = next_provisional_++;
    const PredictedEntityId id{kProvisionalIndexBit | (serial & ~kProvisionalIndexBit),
                               1 + (serial >> 31)};
    spawn_hooks_.create(id);
    hooks_.restore(id, initial_state);
    provisional_.push_back({key, id, Life::Live, {}});
    return id;
}

void SpawnReconciler::predict(const InputCommand& command) {
    inner_.predict(command);
    last_sequence_ = command.sequence;
    expire(last_sequence_);
}

SpawnReconcileOutcome SpawnReconciler::apply_checkpoint(const OwnerCheckpoint& checkpoint,
                                                        const SpawnCheckpointExtension& ext) {
    SpawnReconcileOutcome out{};
    // --- Validate everything before changing anything. ---------------------------------------
    // The inner Reconciler validates too, but only AFTER this object would already have resolved
    // guesses and reshaped the roster; checking here first is what makes a refusal change nothing.
    if (has_checkpoint_ && checkpoint.server_tick <= last_server_tick_) {
        ++stale_checkpoints_;
        return out;
    }
    bool duplicate = false;
    for (std::size_t i = 0; i < checkpoint.entities.size() && !duplicate; ++i) {
        for (std::size_t j = 0; j < i; ++j)
            duplicate = duplicate || checkpoint.entities[i].id == checkpoint.entities[j].id;
    }
    if (checkpoint.result.server_tick != checkpoint.server_tick || duplicate ||
        checkpoint.entities.size() > kCheckpointMaxEntities) {
        ++invalid_base_;
        return out;
    }
    if (const auto refusal = validate_spawn_extension(checkpoint, ext);
        refusal != SpawnExtensionRefusal::None) {
        ++invalid_extension_[static_cast<std::size_t>(refusal)];
        return out;
    }

    const auto append_cancel = [&](const SpawnKey& key) {
        auto events = cancel(key);
        out.cancelled.insert(out.cancelled.end(), events.begin(), events.end());
    };

    std::vector<PredictedEntityId> new_roster;
    new_roster.reserve(checkpoint.entities.size());
    for (const auto& entity : checkpoint.entities)
        new_roster.push_back(entity.id);

    // --- 1. Adoption: a guess the server answered with a roster entity BECOMES that entity. ----
    std::vector<PredictedEntityId> adopted_ids;
    std::vector<SpawnKey> adopted_keys;
    for (const auto& binding : ext.bindings) {
        Provisional* guess = find(binding.key);
        if (guess == nullptr)
            continue;
        spawn_hooks_.adopt(guess->id, binding.id);
        if (ledger_ != nullptr)
            ledger_->adopt(binding.key, binding.id);
        remove(static_cast<std::size_t>(guess - provisional_.data()));
        resolve(binding.key, Resolution::Adopted);
        adopted_ids.push_back(binding.id);
        adopted_keys.push_back(binding.key);
        ++out.adopted;
        ++adopted_;
    }

    // --- 2. Verdicts. Retained until acknowledged, so most arrivals are repeats: ignored, counted.
    for (const auto& outcome : ext.outcomes) {
        if (std::find(adopted_keys.begin(), adopted_keys.end(), outcome.key) != adopted_keys.end())
            continue; // the first confirmation, already adopted above
        Provisional* guess = find(outcome.key);
        const Resolution* prior = resolution(outcome.key);
        const bool lost_locally =
            prior != nullptr && (*prior == Resolution::Expired || *prior == Resolution::Dropped ||
                                 *prior == Resolution::NoLaunch);
        switch (outcome.kind) {
            case SpawnOutcomeKind::Accepted:
                if (prior == nullptr) {
                    ++unpredicted_outcomes_;
                } else if (*prior == Resolution::Adopted) {
                    ++duplicate_outcomes_ignored_;
                    break;
                } else if (lost_locally) {
                    // The guess is gone but the replica lives: bind it (step 4 creates it), and the
                    // ledger's memory of the launch keeps the launch from playing again.
                    if (*prior == Resolution::NoLaunch)
                        ++contradictory_outcomes_;
                    else
                        ++confirmed_after_expiry_alive_;
                } else {
                    ++contradictory_outcomes_;
                    break;
                }
                resolve(outcome.key, Resolution::Adopted);
                if (ledger_ != nullptr)
                    ledger_->adopt(outcome.key, outcome.id);
                break;
            case SpawnOutcomeKind::Rejected:
                if (guess != nullptr) {
                    spawn_hooks_.destroy(guess->id);
                    remove(static_cast<std::size_t>(guess - provisional_.data()));
                    ++out.rejected;
                } else if (prior == nullptr) {
                    ++unpredicted_outcomes_;
                } else if (*prior == Resolution::Rejected) {
                    ++duplicate_outcomes_ignored_;
                    break;
                } else if (!lost_locally) {
                    ++contradictory_outcomes_;
                    break;
                }
                ++rejected_launches_;
                resolve(outcome.key, Resolution::Rejected);
                append_cancel(outcome.key);
                break;
            case SpawnOutcomeKind::AcceptedEnded:
                if (guess != nullptr) {
                    spawn_hooks_.destroy(guess->id);
                    remove(static_cast<std::size_t>(guess - provisional_.data()));
                    ++out.ended;
                    ++accepted_ended_;
                } else if (prior == nullptr) {
                    ++unpredicted_outcomes_;
                } else if (*prior == Resolution::Ended) {
                    ++duplicate_outcomes_ignored_;
                    break;
                } else if (*prior == Resolution::Adopted) {
                    ++accepted_ended_; // the adopted replica ended before its verdict was
                                       // acknowledged
                } else if (*prior == Resolution::Expired || *prior == Resolution::Dropped) {
                    ++confirmed_after_expiry_ended_; // stays absent; nothing is restarted
                } else {
                    ++contradictory_outcomes_;
                    break;
                }
                resolve(outcome.key, Resolution::Ended);
                release(outcome.key);
                break;
        }
    }

    // --- 3. Guesses with no verdict, read against the execution acknowledgement. -------------
    const auto& result = checkpoint.result;
    for (std::size_t i = provisional_.size(); i-- > 0;) {
        Provisional& guess = provisional_[i];
        const bool retired = result.any_retired && guess.key.sequence <= result.retired_through;
        if (!retired) {
            // The replay below runs this guess's command again and will recreate it if the
            // corrected state still launches -- drop it until then.
            guess.life = Life::Dropped;
            guess.snapshots.clear();
            continue;
        }
        const auto offset = result.retired_through - guess.key.sequence;
        const bool known = offset < 128;
        const bool applied =
            known && (result.applied_mask[offset / 64] & (std::uint64_t{1} << (offset % 64))) != 0;
        if (known && !applied) {
            // Retired SYNTHESIZED: the server never ran the command, so there was no launch and
            // there never will be (a retired command is never replayed, ADR-0104 §3). This assumes
            // a synthesized command cannot launch -- true of an edge-triggered throw, since
            // synthesis clears `pressed`; a launch on a held button would have to arrive as a
            // verdict, which step 2 has already given precedence.
            spawn_hooks_.destroy(guess.id);
            const auto key = guess.key;
            remove(i);
            resolve(key, Resolution::NoLaunch);
            append_cancel(key);
            ++retired_without_launch_;
            ++out.retired_without_launch;
            continue;
        }
        // Retired applied (or beyond the mask's reach) with no verdict yet: it stays provisional,
        // restored to its own state as of the retired command, until the verdict or the
        // unresolved bound arrives.
        const auto snapshot = std::find_if(
            guess.snapshots.begin(), guess.snapshots.end(), [&](const auto& candidate) {
                return candidate.sequence == result.retired_through;
            });
        if (snapshot == guess.snapshots.end()) {
            spawn_hooks_.destroy(guess.id);
            const auto key = guess.key;
            remove(i);
            resolve(key, Resolution::Expired);
            ++pending_without_snapshot_;
            continue;
        }
        hooks_.restore(guess.id, snapshot->state);
        auto kept = std::move(*snapshot);
        guess.snapshots.clear();
        guess.snapshots.push_back(std::move(kept));
        ++out.pending;
    }

    // --- 4. Install the authoritative roster: membership, epoch and bindings, atomically. ------
    for (const auto& id : roster_) {
        if (contains(new_roster, id))
            continue;
        spawn_hooks_.destroy(id);
        const auto binding = std::find_if(
            bindings_.begin(), bindings_.end(), [&](const auto& b) { return b.id == id; });
        if (binding != bindings_.end()) {
            if (const Resolution* prior = resolution(binding->key);
                prior != nullptr && *prior == Resolution::Adopted)
                resolve(binding->key, Resolution::Ended);
            release(binding->key);
        }
    }
    for (const auto& id : new_roster) {
        if (!contains(roster_, id) && !contains(adopted_ids, id))
            spawn_hooks_.create(id);
    }
    roster_ = new_roster;
    bindings_ = ext.bindings;
    epoch_ = checkpoint.membership_epoch;
    inner_.set_membership(epoch_, std::move(new_roster));

    // --- 5. Restore and replay through m20.4's rules; the replay may recreate dropped guesses. --
    // It cannot refuse: staleness, epoch and completeness were settled above for this very roster.
    active_outcome_ = &out;
    out.base = inner_.apply_checkpoint(checkpoint);
    active_outcome_ = nullptr;
    last_server_tick_ = checkpoint.server_tick;
    has_checkpoint_ = true;

    // --- 6. Guesses the replay did not make again: under the corrected state, no launch. ------
    for (std::size_t i = provisional_.size(); i-- > 0;) {
        if (provisional_[i].life != Life::Dropped)
            continue;
        spawn_hooks_.destroy(provisional_[i].id);
        const auto key = provisional_[i].key;
        remove(i);
        resolve(key, Resolution::Dropped);
        append_cancel(key);
        ++not_recreated_;
        ++out.not_recreated;
    }

    // --- 7. Acknowledgement and forgetting. ----------------------------------------------------
    if (!ext.outcomes.empty() && (!held_through_ || *held_through_ < ext.outcomes.back().key))
        held_through_ = ext.outcomes.back().key;
    if (ext.acknowledged_through) {
        // Nothing at or below the frontier is ever sent again, and every such key's command has
        // retired (a verdict exists only for an executed command), so no replay can ask either.
        std::erase_if(tombstones_,
                      [&](const auto& entry) { return entry.first <= *ext.acknowledged_through; });
    }
    expire(last_sequence_);
    return out;
}

void SpawnReconciler::expire(std::uint32_t through_sequence) {
    for (std::size_t i = provisional_.size(); i-- > 0;) {
        const auto& guess = provisional_[i];
        if (through_sequence < guess.key.sequence ||
            through_sequence - guess.key.sequence < spawn_budget_.max_unresolved_sequences)
            continue;
        // The ledger is left alone: what the player saw stays seen, and a late verdict either binds
        // the live replica or leaves the projectile absent -- never a second launch.
        spawn_hooks_.destroy(guess.id);
        const auto key = guess.key;
        remove(i);
        resolve(key, Resolution::Expired);
        ++provisional_expired_;
    }
}

void SpawnReconciler::remove(std::size_t index) {
    provisional_.erase(provisional_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::vector<PresentationEvent> SpawnReconciler::cancel(const SpawnKey& key) {
    return ledger_ != nullptr ? ledger_->cancel(key) : std::vector<PresentationEvent>{};
}

void SpawnReconciler::release(const SpawnKey& key) {
    if (ledger_ != nullptr)
        ledger_->release(key);
}

SpawnReconciler::Provisional* SpawnReconciler::find(const SpawnKey& key) {
    const auto it = std::find_if(provisional_.begin(), provisional_.end(), [&](const auto& guess) {
        return guess.key == key;
    });
    return it == provisional_.end() ? nullptr : &*it;
}

const SpawnReconciler::Resolution* SpawnReconciler::resolution(const SpawnKey& key) const {
    const auto it = std::find_if(tombstones_.begin(), tombstones_.end(), [&](const auto& entry) {
        return entry.first == key;
    });
    return it == tombstones_.end() ? nullptr : &it->second;
}

void SpawnReconciler::resolve(const SpawnKey& key, Resolution resolution) {
    const auto it = std::find_if(tombstones_.begin(), tombstones_.end(), [&](const auto& entry) {
        return entry.first == key;
    });
    if (it != tombstones_.end()) {
        it->second = resolution;
        return;
    }
    tombstones_.emplace_back(key, resolution);
    while (tombstones_.size() > spawn_budget_.max_tombstones) {
        tombstones_.erase(tombstones_.begin());
        ++tombstones_evicted_;
    }
}

std::vector<ProvisionalEntity> SpawnReconciler::provisional() const {
    std::vector<ProvisionalEntity> guesses;
    for (const auto& guess : provisional_) {
        if (guess.life == Life::Live)
            guesses.push_back({guess.key, guess.id});
    }
    return guesses;
}

std::optional<PredictedEntityId> SpawnReconciler::provisional_id(const SpawnKey& key) const {
    const auto it = std::find_if(provisional_.begin(), provisional_.end(), [&](const auto& guess) {
        return guess.key == key && guess.life == Life::Live;
    });
    return it == provisional_.end() ? std::nullopt : std::optional{it->id};
}

} // namespace rime::replication
