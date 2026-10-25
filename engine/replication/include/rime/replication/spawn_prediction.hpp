// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rime/replication/input_v2.hpp"
#include "rime/replication/prediction.hpp"

// Speculative spawn (m20.5, ADR-0105): predicting an entity the server has not yet said exists.
//
// m20.4's Reconciler (prediction.hpp) can restore and replay a predicted set whose members already
// have server identities. A projectile launched on the tick the player presses the button has none:
// the server may accept it and name it, reject it (out of charges, still cooling down), or accept
// it and end it before the client ever hears. Three techniques make that predictable, and each
// exists because of one specific way the naive version goes wrong:
//
//   1. SPECULATIVE SPAWN WITH CLIENT-CHOSEN KEYS. The client names its guess with a SpawnKey --
//      (session incarnation, input sequence, slot) -- which the server echoes with its verdict. The
//      key exists before any server id does, a replay of the same command recreates the SAME key,
//      and a reconnect (a new incarnation) can never collide with a key from the old session. A
//      client-allocated id alone would fail all three: it differs per replay, means nothing to the
//      server, and repeats across sessions.
//   2. AUTHORITATIVE ROSTER + PROVISIONAL OVERLAY. A checkpoint carrying the spawn extension
//      installs the server's whole predicted roster atomically -- membership, epoch, states, and
//      which roster entity answers which key -- and the client's guesses sit beside it as an
//      overlay that never changes the epoch. The epoch is the server's statement of membership; if
//      a client guess could advance it, every checkpoint would look like the wrong set. Each
//      reconciliation restores the roster, resolves the overlay against the server's outcomes
//      (accepted: ADOPT the replica's identity; rejected or ended: remove), and replays unretired
//      commands under the same keys and local ids. A skipped replay keeps its guesses; a local
//      removal cannot cancel presentation before the server has given its verdict.
//   3. AN EXACTLY-ONCE PRESENTATION LEDGER. `PredictionStep::replaying` can silence effects during
//      a replay, but it cannot stop a launch sound playing a second time when the authoritative
//      replica arrives under a new id. The ledger remembers (key, event kind, ordinal) triples
//      OUTSIDE rewindable state, so no restore or replay can make it forget, and confirmation
//      adopts the presentation already playing instead of starting another.
//
// The worst failure this exists to prevent is invisible to a final digest (ADR-0105 §5): two
// identities surviving one confirmation -- a ghost projectile, or an effect played twice. Every
// skip, drop and overflow path below has a counter for that reason (guardrail 5): a proof that
// cannot see what was skipped still reads as passing.
//
// A pure leaf like prediction.hpp: no ECS, no sockets, no clock. m20.5b wires it to NetDriver,
// app::TickContext's command provenance and hello-game's Throw.
namespace rime::replication {

// ---------------------------------------------------------------------------------------------
// Keys and outcomes
// ---------------------------------------------------------------------------------------------

struct SpawnKey {
    std::uint32_t incarnation = 0;
    std::uint32_t sequence = 0;
    std::uint8_t slot = 0;
    [[nodiscard]] bool operator==(const SpawnKey&) const = default;
    // Lexicographic (incarnation, sequence, slot): the order the server produces outcomes in, which
    // is what lets one "acknowledged through" key stand for a whole prefix of them. Raw uint32
    // sequence comparison assumes no wrap (about 2.27 years at 60 Hz); m20.4 shares that limit.
    [[nodiscard]] auto operator<=>(const SpawnKey&) const = default;
};

struct SpawnKeyHash {
    [[nodiscard]] std::size_t operator()(const SpawnKey& key) const noexcept;
};

enum class SpawnOutcomeKind : std::uint8_t { Accepted = 1, Rejected = 2, AcceptedEnded = 3 };

// The server's durable verdict on one key (ADR-0105 §1), retained in every checkpoint until the
// client has acknowledged holding it. `id` is the roster identity of an Accepted spawn (m20.5b maps
// it to the replica's NetId; this leaf only knows PredictedEntityId); `reason` is the game's own
// code for a Rejected one. Each field is zero when its kind does not use it -- the decoder refuses
// anything else, so a malformed verdict cannot half-apply.
struct SpawnOutcome {
    SpawnKey key{};
    SpawnOutcomeKind kind = SpawnOutcomeKind::Accepted;
    PredictedEntityId id{};
    std::uint8_t reason = 0;
    [[nodiscard]] bool operator==(const SpawnOutcome&) const = default;
};

// "Roster entity `id` is the server's answer to `key`" -- for as long as it lives, not only until
// the outcome is acknowledged, so a client that lost its guess can still bind the replica.
struct SpawnBinding {
    SpawnKey key{};
    PredictedEntityId id{};
    [[nodiscard]] bool operator==(const SpawnBinding&) const = default;
};

// ---------------------------------------------------------------------------------------------
// The checkpoint extension and its codec
// ---------------------------------------------------------------------------------------------
//
// Wire layout, appended after the m20.4 checkpoint bytes, little-endian:
//
//   u8  tag = kSpawnExtensionTag      u8  version = kSpawnExtensionVersion
//   u16 body_bytes (everything below)
//   u8  binding_count (<= 16)         u8  outcome_count (<= 16)
//   u8  frontier_present (0 | 1)      SpawnKey frontier (9 bytes; all zero when absent)
//   binding_count x { SpawnKey key, u32 index, u32 generation }                      17 bytes each
//   outcome_count x { SpawnKey key, u8 kind, u32 index, u32 generation, u8 reason }  19 bytes each
//
// where a SpawnKey is { u32 incarnation, u32 sequence, u8 slot }. The extension's own bound is
// 16 + 16*17 + 16*19 = 592 bytes, and base + extension together stay within m20.4's 1200-byte
// kCheckpointMaxMessageBytes -- the encoder refuses (MessageTooLarge) rather than exceed it, so a
// server that must report more outcomes sends the oldest first and the rest next tick.
//
// NEGOTIATED, NOT MERELY APPENDED: an m20.4 decoder refuses trailing bytes (BadLength), so a peer
// that did not ask for the extension must never be sent it. This decoder reads an m20.4-only
// checkpoint unchanged (no extension: `spawns` is empty).

inline constexpr std::uint8_t kSpawnExtensionTag = 0xE5;
inline constexpr std::uint8_t kSpawnExtensionVersion = 1;
inline constexpr std::size_t kSpawnExtensionMaxBindings = 16;
inline constexpr std::size_t kSpawnExtensionMaxOutcomes = 16;
inline constexpr std::size_t kSpawnKeyBytes = 9;
inline constexpr std::size_t kSpawnBindingBytes = kSpawnKeyBytes + 8;
inline constexpr std::size_t kSpawnOutcomeBytes = kSpawnKeyBytes + 1 + 8 + 1;
inline constexpr std::size_t kSpawnExtensionHeaderBytes = 4;
inline constexpr std::size_t kSpawnExtensionFixedBodyBytes = 3 + kSpawnKeyBytes;
inline constexpr std::size_t kSpawnExtensionMaxBytes =
    kSpawnExtensionHeaderBytes + kSpawnExtensionFixedBodyBytes +
    kSpawnExtensionMaxBindings * kSpawnBindingBytes +
    kSpawnExtensionMaxOutcomes * kSpawnOutcomeBytes;

// Provisional ids live in the upper half of the index space so they can never be mistaken for a
// roster entity: a roster that uses one is refused (ProvisionalIdInRoster). m20.5b's server must
// allocate predicted-set indices below this bit.
inline constexpr std::uint32_t kProvisionalIndexBit = 0x8000'0000u;

struct SpawnCheckpointExtension {
    std::vector<SpawnBinding> bindings;
    // Strictly ascending by key, all above `acknowledged_through`.
    std::vector<SpawnOutcome> outcomes;
    // The newest key whose outcome the server knows the client holds; nothing at or below it is
    // ever sent again, which is what lets the client forget its tombstones for those keys.
    std::optional<SpawnKey> acknowledged_through;
};

struct SpawnCheckpoint {
    OwnerCheckpoint base{};
    // Empty when the message was an m20.4-only checkpoint.
    std::optional<SpawnCheckpointExtension> spawns;
};

enum class SpawnExtensionRefusal : std::uint8_t {
    None,
    Truncated,
    BadLength,
    BadTag,
    UnknownVersion,
    TooManyBindings,
    TooManyOutcomes,
    BadFrontierFlag,
    BadOutcomeKind,
    ReservedNonZero,
    DuplicateBinding,
    BindingNotInRoster,
    ProvisionalIdInRoster,
    OutcomesNotAscending,
    OutcomeAtOrBelowFrontier,
    OutcomeBindingMismatch,
    MessageTooLarge,
    Count, // not a refusal: the number of reasons, for counters
};

inline constexpr std::size_t kSpawnExtensionRefusalCount =
    static_cast<std::size_t>(SpawnExtensionRefusal::Count);

// The semantic rules a well-formed extension obeys against its own checkpoint, shared by the
// encoder, the decoder and SpawnReconciler (which may be handed a struct that never touched a
// wire): bindings name distinct keys and distinct roster entities; an Accepted outcome's key is
// bound to that same id, while a Rejected or AcceptedEnded key is bound to nothing; outcomes ascend
// strictly and sit above the frontier; unused fields are zero; no roster id is in the provisional
// range.
[[nodiscard]] SpawnExtensionRefusal validate_spawn_extension(const OwnerCheckpoint& base,
                                                             const SpawnCheckpointExtension& ext);

struct SpawnCheckpointEncode {
    std::size_t bytes_written = 0;
    CheckpointRefusal base = CheckpointRefusal::None;
    SpawnExtensionRefusal extension = SpawnExtensionRefusal::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return base == CheckpointRefusal::None && extension == SpawnExtensionRefusal::None;
    }
};

struct SpawnCheckpointDecode {
    CheckpointRefusal base = CheckpointRefusal::None;
    SpawnExtensionRefusal extension = SpawnExtensionRefusal::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return base == CheckpointRefusal::None && extension == SpawnExtensionRefusal::None;
    }
};

// Writes nothing on refusal. Decode replaces `out` only after the base AND the extension validate:
// a malformed extension refuses the whole message, never "the base without its verdicts", because
// restoring a roster whose bindings were dropped is how a ghost projectile is born.
[[nodiscard]] SpawnCheckpointEncode encode_spawn_checkpoint(const OwnerCheckpoint& base,
                                                            const SpawnCheckpointExtension& ext,
                                                            std::span<std::byte> out);
[[nodiscard]] SpawnCheckpointDecode decode_spawn_checkpoint(std::span<const std::byte> in,
                                                            SpawnCheckpoint& out);

// The decode above, with every refusal counted by reason (guardrail 5).
class SpawnCheckpointDecoder {
public:
    [[nodiscard]] SpawnCheckpointDecode decode(std::span<const std::byte> in, SpawnCheckpoint& out);

    [[nodiscard]] std::uint64_t base_refusals() const noexcept { return base_refusals_; }

    [[nodiscard]] std::uint64_t refusals(SpawnExtensionRefusal reason) const noexcept {
        return reason < SpawnExtensionRefusal::Count
                   ? extension_refusals_[static_cast<std::size_t>(reason)]
                   : 0;
    }

    [[nodiscard]] std::uint64_t legacy_decoded() const noexcept { return legacy_decoded_; }

    [[nodiscard]] std::uint64_t extended_decoded() const noexcept { return extended_decoded_; }

private:
    std::array<std::uint64_t, kSpawnExtensionRefusalCount> extension_refusals_{};
    std::uint64_t base_refusals_ = 0;
    std::uint64_t legacy_decoded_ = 0;
    std::uint64_t extended_decoded_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The presentation ledger
// ---------------------------------------------------------------------------------------------

// Event kinds are the game's; these names cover a thrown projectile and any value fits the byte.
enum class PresentationEventKind : std::uint8_t { Launch = 0, Trail = 1, Impact = 2, End = 3 };

struct PresentationEvent {
    SpawnKey key{};
    PresentationEventKind kind = PresentationEventKind::Launch;
    std::uint16_t ordinal = 0;
    [[nodiscard]] bool operator==(const PresentationEvent&) const = default;
};

struct LedgerBudget {
    // Keys with live presentation. Past it, should_play answers false: AT MOST once is the safe
    // degradation of exactly once, and the counter says it happened.
    std::size_t max_keys = 64;
    std::size_t max_events_per_key = 16;
    // Cancelled or released keys remembered so a late replay or duplicate cannot replay them; the
    // oldest is forgotten first.
    std::size_t max_tombstones = 128;
};

// Exactly-once presentation keyed by (SpawnKey, kind, ordinal), ADR-0105 §3. Owned by
// presentation, NEVER by rewindable state: restoring a checkpoint must not make it forget what the
// player has already seen and heard.
class PresentationLedger {
public:
    explicit PresentationLedger(LedgerBudget budget = {});

    // True exactly once per triple, however many replays and confirmations ask. Always false for a
    // cancelled key. A released key (its entity ended) still answers once for a triple it never
    // played -- the authoritative end may carry an impact the guess never reached.
    [[nodiscard]] bool
    should_play(const SpawnKey& key, PresentationEventKind kind, std::uint16_t ordinal);
    // Binds the key to the replica's identity; the presentation already playing continues as that
    // entity's. False (counted) when already bound or cancelled: cancellation cannot be undone.
    bool adopt(const SpawnKey& key, PredictedEntityId id);
    // The key's guess was wrong: returns the events already played so the game can stop removable
    // effects, and makes every later should_play for the key false.
    [[nodiscard]] std::vector<PresentationEvent> cancel(const SpawnKey& key);
    // The key's entity ended normally; its played events stay played.
    void release(const SpawnKey& key);

    [[nodiscard]] std::optional<PredictedEntityId> bound(const SpawnKey& key) const;
    [[nodiscard]] bool cancelled(const SpawnKey& key) const;

    [[nodiscard]] std::uint64_t played() const noexcept { return played_; }

    [[nodiscard]] std::uint64_t duplicates_suppressed() const noexcept {
        return duplicates_suppressed_;
    }

    [[nodiscard]] std::uint64_t suppressed_cancelled() const noexcept {
        return suppressed_cancelled_;
    }

    [[nodiscard]] std::uint64_t adoptions() const noexcept { return adoptions_; }

    [[nodiscard]] std::uint64_t duplicate_adoptions() const noexcept {
        return duplicate_adoptions_;
    }

    [[nodiscard]] std::uint64_t adopt_after_cancel() const noexcept { return adopt_after_cancel_; }

    [[nodiscard]] std::uint64_t cancels() const noexcept { return cancels_; }

    [[nodiscard]] std::uint64_t duplicate_cancels() const noexcept { return duplicate_cancels_; }

    [[nodiscard]] std::uint64_t key_overflow() const noexcept { return key_overflow_; }

    [[nodiscard]] std::uint64_t event_overflow() const noexcept { return event_overflow_; }

    [[nodiscard]] std::uint64_t tombstones_evicted() const noexcept { return tombstones_evicted_; }

private:
    enum class State : std::uint8_t { Active, Cancelled, Released };

    struct Entry {
        State state = State::Active;
        std::optional<PredictedEntityId> bound;
        std::vector<std::pair<PresentationEventKind, std::uint16_t>> played;
    };

    Entry* find_or_open(const SpawnKey& key);
    void tombstone(const SpawnKey& key);

    LedgerBudget budget_;
    std::unordered_map<SpawnKey, Entry, SpawnKeyHash> entries_;
    std::vector<SpawnKey> tombstone_order_;
    std::size_t active_ = 0;
    std::uint64_t played_ = 0;
    std::uint64_t duplicates_suppressed_ = 0;
    std::uint64_t suppressed_cancelled_ = 0;
    std::uint64_t adoptions_ = 0;
    std::uint64_t duplicate_adoptions_ = 0;
    std::uint64_t adopt_after_cancel_ = 0;
    std::uint64_t cancels_ = 0;
    std::uint64_t duplicate_cancels_ = 0;
    std::uint64_t key_overflow_ = 0;
    std::uint64_t event_overflow_ = 0;
    std::uint64_t tombstones_evicted_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The reconciler with a provisional overlay
// ---------------------------------------------------------------------------------------------

// What the game does when the overlay or the roster changes shape. Synchronous, must not throw.
struct SpawnHooks {
    // A new, empty entity slot; PredictionHooks::restore fills it immediately after.
    std::function<void(PredictedEntityId)> create;
    std::function<void(PredictedEntityId)> destroy;
    // Rename provisional -> authoritative, keeping whatever presentation hangs off it. Restore of
    // the authoritative state follows in the same reconciliation.
    std::function<void(PredictedEntityId provisional, PredictedEntityId authoritative)> adopt;
};

struct SpawnBudget {
    // Live guesses at once. Past it a spawn is refused (counted): the server still decides, and its
    // replica binds later as an unpredicted outcome.
    std::size_t max_provisional = 8;
    // A guess older than this many input sequences without a verdict expires locally (counted). The
    // "predicted projectile already expired" of ADR-0105 §3: set it at least as long as the
    // projectile's own lifetime.
    std::uint32_t max_unresolved_sequences = 120;
    // Per-guess state snapshots by input sequence, needed to restore a guess whose command retired
    // before its verdict arrived. Oldest evicted first (counted).
    std::size_t max_snapshots = 128;
    // Resolved keys remembered, so a duplicate verdict is recognised and an expired guess is never
    // respawned by replay. Oldest evicted first (counted); a watermark still blocks older keys.
    std::size_t max_tombstones = 64;
};

struct SpawnReconcileOutcome {
    ReconcileOutcome base{};
    std::size_t adopted = 0;
    std::size_t rejected = 0;
    std::size_t ended = 0;
    std::size_t retired_without_launch = 0;
    std::size_t pending = 0;
    std::size_t recreated = 0;
    std::size_t not_recreated = 0;
    // Events the ledger cancelled in this reconciliation; the game stops their removable effects.
    std::vector<PresentationEvent> cancelled;
};

struct ProvisionalEntity {
    SpawnKey key{};
    PredictedEntityId id{};
};

// m20.4's Reconciler plus a provisional overlay (ADR-0105 §2). It owns a Reconciler and forwards
// to it, so the m20.4 restore/replay/reseed rules -- and their counters -- are unchanged; what it
// adds happens around each call.
//
// Spawning is a STEP-CONTEXT CALL: the game's simulate hook calls spawn_provisional() on this
// object. Chosen over a new PredictionHooks member or a PredictionStep field because it leaves both
// m20.4 types untouched, and because only the reconciler knows, at that moment, which command is
// executing (so it can refuse a key that replay could not recreate) and whether it is replaying (so
// it revives the dropped guess under its old id instead of minting a second one).
class SpawnReconciler {
public:
    SpawnReconciler(PredictionHooks hooks,
                    SpawnHooks spawn_hooks,
                    double fixed_dt,
                    std::uint32_t incarnation,
                    PresentationLedger* ledger,
                    ReconcilerBudget budget = {},
                    SpawnBudget spawn_budget = {});
    // The inner Reconciler's hooks call back into this object.
    SpawnReconciler(const SpawnReconciler&) = delete;
    SpawnReconciler& operator=(const SpawnReconciler&) = delete;
    SpawnReconciler(SpawnReconciler&&) = delete;
    SpawnReconciler& operator=(SpawnReconciler&&) = delete;
    ~SpawnReconciler() = default;

    // The initial roster, as m20.4's Reconciler::set_membership.
    void set_membership(std::uint32_t epoch, std::vector<PredictedEntityId> members);
    void predict(const InputCommand& command);

    // Only from inside the simulate hook. The key must be (this incarnation, the executing
    // command's sequence, any slot). Returns the provisional id -- the same one on every replay of
    // the same command -- or nothing, in which case the game must not create the entity.
    [[nodiscard]] std::optional<PredictedEntityId>
    spawn_provisional(const SpawnKey& key, std::span<const std::byte> initial_state);

    // Validates everything before changing anything; a refused checkpoint changes nothing.
    [[nodiscard]] SpawnReconcileOutcome apply_checkpoint(const OwnerCheckpoint& checkpoint,
                                                         const SpawnCheckpointExtension& ext);

    [[nodiscard]] const Reconciler& base() const noexcept { return inner_; }

    [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }

    [[nodiscard]] std::span<const PredictedEntityId> roster() const noexcept { return roster_; }

    [[nodiscard]] std::vector<ProvisionalEntity> provisional() const;
    [[nodiscard]] std::optional<PredictedEntityId> provisional_id(const SpawnKey& key) const;

    // Refusal tests inspect this bounded memory because losing it can resurrect a replayed guess.
    [[nodiscard]] bool has_tombstone(const SpawnKey& key) const noexcept;

    // What the client may acknowledge upstream: the newest outcome key it has PROCESSED in an
    // applied checkpoint, never one it merely received (guardrail 5) -- a refused or stale
    // checkpoint never advances it. A contradictory verdict counts as processed: it can never be
    // applied, and leaving it unacknowledged would make the server resend it forever. Valid as a
    // prefix because outcomes ascend.
    [[nodiscard]] std::optional<SpawnKey> outcomes_held_through() const noexcept {
        return held_through_;
    }

    // ADR-0105 §5's hard cases. "Launch recovered by redundancy" is NOT visible here: InputV2's
    // redundant window recovers a lost copy before the server executes it, and the leaf only ever
    // sees the result -- an ordinary acceptance. m20.5b counts it from the InputSchedule.
    [[nodiscard]] std::uint64_t rejected_launches() const noexcept { return rejected_launches_; }

    [[nodiscard]] std::uint64_t rejected_unpredicted() const noexcept {
        return rejected_unpredicted_;
    }

    // Local entity removal and the server's cancellation are separate: only the latter stops
    // removable effects, since a later acceptance may still name a replica for a removed guess.
    [[nodiscard]] std::uint64_t local_removals() const noexcept { return local_removals_; }

    [[nodiscard]] std::uint64_t local_releases() const noexcept { return local_releases_; }

    [[nodiscard]] std::uint64_t authoritative_cancels() const noexcept {
        return authoritative_cancels_;
    }

    [[nodiscard]] std::uint64_t unreplayed_guesses() const noexcept { return unreplayed_guesses_; }

    [[nodiscard]] std::uint64_t frontier_ahead_of_client() const noexcept {
        return frontier_ahead_of_client_;
    }

    [[nodiscard]] std::uint64_t epoch_roster_mismatch() const noexcept {
        return epoch_roster_mismatch_;
    }

    [[nodiscard]] std::uint64_t retired_without_launch() const noexcept {
        return retired_without_launch_;
    }

    [[nodiscard]] std::uint64_t duplicate_outcomes_ignored() const noexcept {
        return duplicate_outcomes_ignored_;
    }

    [[nodiscard]] std::uint64_t confirmed_after_expiry_alive() const noexcept {
        return confirmed_after_expiry_alive_;
    }

    [[nodiscard]] std::uint64_t confirmed_after_expiry_ended() const noexcept {
        return confirmed_after_expiry_ended_;
    }

    [[nodiscard]] std::uint64_t adopted() const noexcept { return adopted_; }

    [[nodiscard]] std::uint64_t accepted_ended() const noexcept { return accepted_ended_; }

    [[nodiscard]] std::uint64_t unpredicted_outcomes() const noexcept {
        return unpredicted_outcomes_;
    }

    [[nodiscard]] std::uint64_t contradictory_outcomes() const noexcept {
        return contradictory_outcomes_;
    }

    [[nodiscard]] std::uint64_t provisional_expired() const noexcept {
        return provisional_expired_;
    }

    [[nodiscard]] std::uint64_t pending_without_snapshot() const noexcept {
        return pending_without_snapshot_;
    }

    [[nodiscard]] std::uint64_t not_recreated() const noexcept { return not_recreated_; }

    [[nodiscard]] std::uint64_t recreated() const noexcept { return recreated_; }

    [[nodiscard]] std::uint64_t respawn_suppressed() const noexcept { return respawn_suppressed_; }

    [[nodiscard]] std::uint64_t spawn_outside_step() const noexcept { return spawn_outside_step_; }

    [[nodiscard]] std::uint64_t spawn_bad_key() const noexcept { return spawn_bad_key_; }

    [[nodiscard]] std::uint64_t spawn_duplicate() const noexcept { return spawn_duplicate_; }

    [[nodiscard]] std::uint64_t provisional_overflow() const noexcept {
        return provisional_overflow_;
    }

    [[nodiscard]] std::uint64_t snapshots_evicted() const noexcept { return snapshots_evicted_; }

    [[nodiscard]] std::uint64_t tombstones_evicted() const noexcept { return tombstones_evicted_; }

    [[nodiscard]] std::uint64_t stale_checkpoints() const noexcept { return stale_checkpoints_; }

    [[nodiscard]] std::uint64_t invalid_base() const noexcept { return invalid_base_; }

    [[nodiscard]] std::uint64_t invalid_extension(SpawnExtensionRefusal reason) const noexcept {
        return reason < SpawnExtensionRefusal::Count
                   ? invalid_extension_[static_cast<std::size_t>(reason)]
                   : 0;
    }

private:
    enum class Life : std::uint8_t { Live, Dropped };
    enum class Resolution : std::uint8_t { Adopted, Rejected, Ended, NoLaunch, Expired, Dropped };

    struct Snapshot {
        std::uint32_t sequence = 0;
        std::vector<std::byte> state;
    };

    struct Provisional {
        SpawnKey key{};
        PredictedEntityId id{};
        Life life = Life::Live;
        std::vector<Snapshot> snapshots;
        bool replayed_command = false;
    };

    void simulate(std::span<const PredictedEntityId> roster,
                  const InputCommand& command,
                  const PredictionStep& step);
    void expire(std::uint32_t through_sequence);
    void remove(std::size_t index);
    std::vector<PresentationEvent> cancel(const SpawnKey& key);
    void release(const SpawnKey& key);
    [[nodiscard]] Provisional* find(const SpawnKey& key);
    [[nodiscard]] const Resolution* resolution(const SpawnKey& key) const;
    void resolve(const SpawnKey& key, Resolution resolution);
    void release_local_if_pending(const SpawnKey& key);

    PredictionHooks hooks_;
    SpawnHooks spawn_hooks_;
    SpawnBudget spawn_budget_;
    PresentationLedger* ledger_ = nullptr;
    Reconciler inner_;
    std::uint32_t incarnation_ = 0;
    std::uint32_t epoch_ = 0;
    std::vector<PredictedEntityId> roster_;
    std::vector<SpawnBinding> bindings_;
    std::vector<Provisional> provisional_;
    // Verdict classification must survive tombstone eviction; the acknowledged frontier prunes it.
    std::unordered_set<SpawnKey, SpawnKeyHash> predicted_keys_;
    std::vector<std::pair<SpawnKey, Resolution>> tombstones_;
    std::vector<SpawnKey> pending_local_release_;
    std::optional<SpawnKey> forgotten_through_;
    std::optional<SpawnKey> held_through_;
    std::uint64_t last_server_tick_ = 0;
    std::uint32_t last_sequence_ = 0;
    std::uint32_t next_provisional_ = 0;
    std::uint32_t step_sequence_ = 0;
    bool has_checkpoint_ = false;
    bool in_step_ = false;
    SpawnReconcileOutcome* active_outcome_ = nullptr;
    std::uint64_t rejected_launches_ = 0;
    std::uint64_t rejected_unpredicted_ = 0;
    std::uint64_t local_removals_ = 0;
    std::uint64_t local_releases_ = 0;
    std::uint64_t authoritative_cancels_ = 0;
    std::uint64_t unreplayed_guesses_ = 0;
    std::uint64_t frontier_ahead_of_client_ = 0;
    std::uint64_t epoch_roster_mismatch_ = 0;
    std::uint64_t retired_without_launch_ = 0;
    std::uint64_t duplicate_outcomes_ignored_ = 0;
    std::uint64_t confirmed_after_expiry_alive_ = 0;
    std::uint64_t confirmed_after_expiry_ended_ = 0;
    std::uint64_t adopted_ = 0;
    std::uint64_t accepted_ended_ = 0;
    std::uint64_t unpredicted_outcomes_ = 0;
    std::uint64_t contradictory_outcomes_ = 0;
    std::uint64_t provisional_expired_ = 0;
    std::uint64_t pending_without_snapshot_ = 0;
    std::uint64_t not_recreated_ = 0;
    std::uint64_t recreated_ = 0;
    std::uint64_t respawn_suppressed_ = 0;
    std::uint64_t spawn_outside_step_ = 0;
    std::uint64_t spawn_bad_key_ = 0;
    std::uint64_t spawn_duplicate_ = 0;
    std::uint64_t provisional_overflow_ = 0;
    std::uint64_t snapshots_evicted_ = 0;
    std::uint64_t tombstones_evicted_ = 0;
    std::uint64_t stale_checkpoints_ = 0;
    std::uint64_t invalid_base_ = 0;
    std::array<std::uint64_t, kSpawnExtensionRefusalCount> invalid_extension_{};
};

} // namespace rime::replication
