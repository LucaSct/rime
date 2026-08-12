// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/net/net_driver.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/replication/snapshot.hpp"

// The client→server input path (m11.6c, ADR-0033 §"m11.6" + amendments A19/A20).
//
// Everything else in M11 flows server→client: the server owns the simulation and the client is a
// follower. This file is the one stream going the other way, and the asymmetry is not merely
// directional — it changes what the bytes MEAN. A snapshot is a fact ("the wall is broken"); an
// input is a request ("I am pushing forward"). The server is free to disbelieve a request, and this
// path is built so that disbelieving one is the default rather than an afterthought.
//
// ── WHAT CROSSES THE WIRE: INTENT, NOT EVENTS ───────────────────────────────────────────────────
//
// The local path (`Application::post_input` / `frame_input`) carries `platform::Event` — raw device
// events, one per keystroke and mouse motion. That is the right shape for a UI and the wrong shape
// for this wire, for three independent reasons:
//
//   1. It is unbounded per tick. A mouse reporting at 1000 Hz produces ~16 MouseMove events per
//      60 Hz tick; a scroll wheel produces a burst. An input packet must be a fixed small size, or
//      the one message a player's responsiveness depends on is the one that gets fragmented.
//   2. It is device-shaped, not game-shaped. `Key::W` is a *binding* — client-side policy, which
//      the player may remap and the server has no business knowing. What the server needs is the
//      resulting intent.
//   3. It puts arithmetic on the wrong side. If the client sent "the camera moved 5 metres" the
//      server would be trusting client arithmetic, which is the definition of a cheat surface
//      (ADR-0033 §1). Sending "I am pushing forward at 1.0" leaves every metre the server's to
//      compute.
//
// So an `InputCommand` is a *sample of the player's intent* at a moment: axes, view angles, and
// action bits. `InputSampler` below integrates the event stream down to it.
//
// The engine already has the other precedent, and it is worth naming so the two are not confused:
// `stream::InputEvent` (Track S) forwards RAW device events to a remote-view server, because there
// the remote end owns the *user interface* — it is a thin client for an editor viewport, and a
// viewport genuinely wants to know about the scroll wheel. Here the remote end owns the
// *simulation*. Different thing on the far side, different thing on the wire.
//
// ── WHICH CHANNEL: UNRELIABLE, WITH REDUNDANCY ──────────────────────────────────────────────────
//
// Unreliable-sequenced, and the argument is the same one snapshot.hpp makes about Delta: an input
// is superseded by a newer input. A reliable channel would retransmit a lost command, so the
// player's *stale* intent would arrive a round trip late and — worse — would hold every fresher
// command behind it in the ordered stream. Head-of-line blocking on the input path is exactly
// backwards: the one packet whose value decays fastest would be the one delivery waits for.
//
// But "drop it" is not good enough on its own, because an input is not purely a level. Holding W is
// self-healing (the next sample says W is still held); *pressing fire* is an edge, and a lost edge
// is a shot that never happened. So each packet carries the last `kInputRedundancy` commands rather
// than just the newest. One packet covers `kInputRedundancy - 1` consecutive losses with no
// retransmit and no round trip, the server deduplicates by sequence number, and the cost is a few
// dozen bytes per tick on the smallest message we send. This is why `InputCommand` splits `held`
// from `pressed`: the two halves have genuinely different loss stories, and a design that had only
// one of them would either lose shots or repeat them.
//
// ── WHAT "TICK-TAGGED" HONESTLY MEANS ───────────────────────────────────────────────────────────
//
// Amendment A11 already ruled, for destruction ops, that "the same tick" cannot mean a local tick
// index: reliable delivery is ordered but never timely, and two peers are not in lockstep. The
// upstream direction inherits the ruling and there is no clock sync in this codebase to soften it.
//
// So the tag is a plain per-client sequence number, and in v1 the server may use it for exactly
// three things: ORDER commands, DEDUPLICATE the redundancy window, and COUNT gaps. It may NOT use
// it to place a command at a server tick, because a client's tick 400 is not a time the server can
// locate — it is a position in that client's own private sequence.
//
// What it would take for the tag to mean more: the server would have to publish its own tick
// alongside `InputAck`, the client would measure the round trip against its send time, and the two
// would agree an offset — the machinery lag compensation and prediction reconciliation are built
// on. That is deliberately NOT built here (see "the prediction seam" below). The `InputAck` echo IS
// built, because it is what makes the seam honest, and because it already has a working sibling in
// tree: `stream::InputEvent` carries `seq` + `client_us` and the remote-view server echoes them on
// the first frame reflecting that input (ADR-0030 §5, the offset-free latency measurement).
namespace rime::replication {

// ── The wire contract ───────────────────────────────────────────────────────────────────────────

// One sample of a client's intent. Hand-serialized like every other net message (never a struct
// memcpy — see snapshot.hpp), so padding and host byte order cannot leak onto the wire.
struct InputCommand {
    // The client's own monotonically increasing tag, stamped by ClientInputSender. Starts at 1, so
    // 0 is unambiguously "no command" — which is what an `acked_through` of 0 means.
    //
    // A u32 at 60 Hz wraps after ~2.2 years of continuous play by one client, so the comparisons
    // below are plain `>` rather than the modular sequence-distance arithmetic net::ReliableChannel
    // needs for its 16-bit sequences. Naming the assumption rather than hiding it: if a session
    // ever plausibly runs that long, this needs the same treatment.
    std::uint32_t sequence = 0;

    // Movement intent on the ground plane, nominally within the unit disc: +y is forward, +x is
    // right. NOT a velocity and not a displacement — how fast this makes anything move is the
    // server's arithmetic, per §1 above.
    float move_x = 0.0f;
    float move_y = 0.0f;

    // ABSOLUTE view angles in radians, not deltas. A delta stream loses aim permanently on any
    // dropped packet — the rotation that packet carried is simply gone and every later angle is
    // wrong by it — whereas an absolute angle is repaired by the very next sample that arrives.
    // Self-healing under loss is the whole reason the unreliable channel is acceptable here.
    float yaw = 0.0f;
    float pitch = 0.0f;

    // Actions currently HELD (a level: self-healing, since the next sample restates it) and actions
    // that went down since the previous command (an EDGE: it exists in exactly one command, which
    // is what the redundancy window above is protecting). Bit meanings are the game's; the engine
    // only transports them.
    std::uint32_t held = 0;
    std::uint32_t pressed = 0;
};

// [sequence:4][move_x:4][move_y:4][yaw:4][pitch:4][held:4][pressed:4]
inline constexpr std::size_t kInputCommandBytes = 4 + 4 + 4 + 4 + 4 + 4 + 4;

// How many recent commands ride in each packet (see "redundancy" above). Three covers two
// consecutive losses: at the 20% independent loss the scripted-loss harness runs, that is a
// residual ~0.8% per command instead of 20%, for 56 extra bytes a tick.
inline constexpr std::size_t kInputRedundancy = 3;

// The parse bound on an arriving packet. Larger than kInputRedundancy so a client that ticks faster
// than it sends can widen its own window, and hard-capped so a malformed or hostile count field
// cannot make the server loop.
inline constexpr std::size_t kMaxCommandsPerPacket = 16;

// How many un-acked commands a client keeps for replay (the prediction seam below). At 60 Hz this
// is two seconds of input — well past any round trip a playable session has, so reaching the bound
// means the server has effectively stopped answering, which the session's own timeout handles.
inline constexpr std::size_t kMaxUnackedCommands = 128;

// How many accepted commands a server buffers per client between drains. The game normally drains
// every tick and finds one; this covers a burst arriving after a hitch. Bounded because the peer
// chooses how fast to send — an unbounded buffer a peer can grow is a denial of service wearing a
// resilience feature's clothes (the same rule as kMaxDeferredRecords).
inline constexpr std::size_t kMaxBufferedCommands = 64;

// Clamp a command into the contract its fields advertise, in place. Returns true if anything was
// out of contract, so the caller can count it: a client whose commands need clamping is either
// buggy or lying, and both are worth a number rather than a silent correction.
//
// This is not "game rules" creeping into the engine. It enforces exactly what the field comments
// promise and nothing else — the unit disc, a sane pitch, finite numbers — because a NaN that
// reaches physics poisons a whole solve, and `move_x = 1e30` is not an intent any binding can
// produce. Everything beyond that (how fast, what a bit means, whether this player may act at all)
// is the game's to decide, and it decides after this.
bool sanitize(InputCommand& command) noexcept;

// ── Sampling: the event stream is edge-shaped, the wire wants level-shaped ───────────────────────
//
// This is the load-bearing conversion of the whole brick and it is easy to miss. `frame_input()`
// hands out EVENTS — "W went down", "the mouse moved by (3, -1)". An InputCommand is STATE — "W is
// held, the view is at this angle". Nobody can derive the second from one frame of the first: held
// state only exists if somebody integrates the down/up edges across frames, and a view angle only
// exists if somebody accumulates the relative motion. That somebody is this class.
//
// A game with its own input mapping replaces this outright — it is a convenience, not a contract.
// The contract is `InputCommand`. What is NOT optional is the integration: any replacement has to
// do the same edge→level work, or it will send held bits that flicker off on every frame the key
// happened not to repeat.
class InputSampler {
public:
    // Bind a key to an action bit (held while down, and once in `pressed` on the way down).
    struct ActionBinding {
        platform::Key key = platform::Key::Unknown;
        std::uint32_t bit = 0;
    };

    // Bind a key to a movement axis: `axis` 0 = move_x, 1 = move_y; `scale` is normally ±1.
    struct AxisBinding {
        platform::Key key = platform::Key::Unknown;
        std::uint8_t axis = 0;
        float scale = 1.0f;
    };

    // Copies, so the caller may bind from a temporary.
    void set_bindings(std::span<const ActionBinding> actions, std::span<const AxisBinding> axes);

    // Radians of view rotation per pixel of raw mouse motion.
    void set_mouse_sensitivity(float radians_per_pixel) noexcept {
        mouse_sensitivity_ = radians_per_pixel;
    }

    // Fold one frame's events into the running state. Call every frame, including frames on which
    // no command is built — an event dropped because it landed between samples is a keystroke the
    // player made and the game never saw.
    void accumulate(std::span<const platform::Event> events);

    // Snapshot the running state as the command numbered `sequence`, and consume what must not be
    // reported twice: the `pressed` edges (an edge belongs to exactly one command) and nothing
    // else. Held state and view angles persist, because they are levels — that is the difference
    // this class exists to maintain.
    [[nodiscard]] InputCommand build(std::uint32_t sequence);

    // Whether a bound key is currently down. Exposed for tests and for a game that wants to read
    // the integrated state without minting a command.
    [[nodiscard]] bool is_held(platform::Key key) const noexcept;

private:
    // A key held when the window loses focus is never released — the OS delivers the KeyUp to
    // whoever has focus now, not to us. Left alone, that is the classic stuck-key bug, and on this
    // path it is worse than an annoyance: the client would keep telling the server it is walking
    // forward while the player alt-tabs. Focus loss therefore clears the held set.
    void clear_held() noexcept;

    static constexpr std::size_t kKeyWords =
        (static_cast<std::size_t>(platform::Key::Count) + 63) / 64;

    std::array<std::uint64_t, kKeyWords> held_keys_{}; // bitset over platform::Key
    std::vector<ActionBinding> actions_;
    std::vector<AxisBinding> axes_;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float mouse_sensitivity_ = 0.0025f;
    std::uint32_t pending_pressed_ = 0; // edges seen since the last build()
};

// WASD + Space/Ctrl, mouse-look. A starting point for samples and tests, not a policy: bit 0 is
// "primary action", bit 1 "secondary", and a real game names its own.
[[nodiscard]] std::span<const InputSampler::ActionBinding> default_action_bindings() noexcept;
[[nodiscard]] std::span<const InputSampler::AxisBinding> default_axis_bindings() noexcept;

// ── The client end ──────────────────────────────────────────────────────────────────────────────

class ClientInputSender {
public:
    // Stamp the next sequence onto `command`, keep it for replay, and return it as sent. Call once
    // per tick from PreSim (after sampling, before the local tick runs), so the sequence advances
    // at the simulation's cadence rather than the renderer's.
    const InputCommand& record(InputCommand command);

    // Put the redundancy window on the unreliable channel of every connected session. Call from
    // Publish. A client has exactly one session, but the loop matches ClientReplicator::send_ack
    // rather than assuming that.
    void send(net::NetDriver& driver, std::uint64_t now_ms);

    // How many recent commands ride in each packet, clamped to [1, kMaxCommandsPerPacket]. A knob
    // rather than a constant because the right answer depends on the link: 1 is the degenerate
    // "send only the newest" design, which is also how the proof gets a negative control — the same
    // scenario, same seed, same losses, and strictly more of the player's input never arrives.
    void set_redundancy(std::size_t commands_per_packet) noexcept;

    [[nodiscard]] std::size_t redundancy() const noexcept { return redundancy_; }

    // Consume InputAck out of an already-drained batch, ignoring everything else. The client drains
    // its session ONCE and hands the same span to each reader (ClientReplicator, DestructionClient,
    // this) — drain_received moves messages out, so a second drain would find an empty inbox.
    void apply_messages(std::span<const net::Received> messages);

    // The newest command the server reports having consumed. Monotonic: acks ride the unreliable
    // channel and so may arrive reordered, and letting this go backwards would resurrect commands
    // already retired.
    [[nodiscard]] std::uint32_t acked_through() const noexcept { return acked_through_; }

    // ── The prediction seam ─────────────────────────────────────────────────────────────────────
    //
    // Commands the server has not confirmed consuming, oldest first: exactly the set a predicting
    // client replays on top of a corrected state. This is what the roadmap called "the prediction
    // interface", delivered as the STATE prediction needs rather than as an abstract class nothing
    // implements — deliberately, and the reasoning belongs in the code rather than in a review
    // comment:
    //
    // There is no character controller, no player and no weapon anywhere in engine/ (ADR-0033 A6;
    // that is M12). So a `virtual void replay(...) = 0` declared today would be guessing at its own
    // signature — does it take a World, an entity, a physics scene, a dt? — and a wrong guess in a
    // header is worse than an absence, because the next person inherits it as a constraint instead
    // of a blank page. What is NOT a guess is that any predictor needs the un-acked commands and a
    // way to retire them. That much is real, it is exercised by the tests, and it is here.
    //
    // What is honestly missing, so nobody mistakes this for prediction: the replay itself, the
    // rollback of local state to the server's version, the comparison that decides a correction is
    // needed, and the clock offset that would let a command be placed at a server tick.
    //
    // One asymmetry a future predictor must know about rather than discover: this list retires on
    // the server's CONSUMPTION frontier, which jumps over commands the server dropped (loss, or an
    // overflowed buffer). So a command leaving this list means "the server will never act on it",
    // NOT "the server acted on it". That is why reconciliation has to compare resulting STATE and
    // can never simply replay a diff of command lists.
    [[nodiscard]] std::span<const InputCommand> unacked() const noexcept { return unacked_; }

    // Counters — the house rule from m11.1: a loss test in which nothing was dropped proves
    // nothing, so every skip path gets a number a proof can assert on.
    [[nodiscard]] std::uint64_t commands_sent() const noexcept { return commands_sent_; }

    [[nodiscard]] std::uint64_t packets_sent() const noexcept { return packets_sent_; }

    [[nodiscard]] std::uint64_t acks_received() const noexcept { return acks_received_; }

    // Un-acked commands evicted because the history filled — i.e. commands a predictor would have
    // replayed and now cannot. Should be zero against a server that is answering at all.
    [[nodiscard]] std::uint64_t commands_evicted() const noexcept { return commands_evicted_; }

    [[nodiscard]] std::uint64_t malformed_messages() const noexcept { return malformed_; }

private:
    std::vector<InputCommand> unacked_;
    std::vector<std::byte> scratch_;
    std::size_t redundancy_ = kInputRedundancy;
    std::uint32_t next_sequence_ = 1;
    std::uint32_t acked_through_ = 0;
    std::uint64_t commands_sent_ = 0;
    std::uint64_t packets_sent_ = 0;
    std::uint64_t acks_received_ = 0;
    std::uint64_t commands_evicted_ = 0;
    std::uint64_t malformed_ = 0;
};

// ── The server end ──────────────────────────────────────────────────────────────────────────────

class ServerInputReceiver {
public:
    // Read the input out of an already-drained batch for one session. Same shared-inbox contract as
    // everywhere else: tags that are not ours are left alone, never consumed and never errors.
    // Returns how many commands were newly accepted.
    std::size_t apply_messages(net::SessionId id, std::span<const net::Received> messages);

    // Convenience for a server whose sessions carry only replication traffic: drain each session
    // and apply. A server that also runs destruction_net must drain once itself and share the span.
    std::size_t apply_inbound(net::NetDriver& driver);

    // Move this client's accepted commands into `out` (appended, not cleared — the same contract as
    // Session::drain_received), oldest first. Returns the count.
    //
    // Draining is what advances the acknowledgement frontier, and that ordering is the point: the
    // ack must mean "the game has this", not "a packet arrived carrying it". The distinction is the
    // upstream twin of the bug docs/design/replication.md was written about.
    std::size_t drain(net::SessionId id, std::vector<InputCommand>& out);

    // Tell each connected client how far its input has been consumed. Call from Publish.
    void send_acks(net::NetDriver& driver, std::uint64_t now_ms);

    // Release a disconnected client's state. A per-session record keyed by a recyclable slot must
    // not outlive its subject — the third bullet of corollary 2 in docs/design/replication.md,
    // which has already produced one bug in this module (instance six).
    void forget(net::SessionId id) noexcept;

    // The frontier reported to this client: the newest command the game has drained. Zero for an
    // unknown session.
    [[nodiscard]] std::uint32_t consumed_through(net::SessionId id) const noexcept;

    // Counters.
    [[nodiscard]] std::uint64_t commands_accepted() const noexcept { return commands_accepted_; }

    // Commands discarded because a newer-or-equal sequence was already accepted — i.e. the
    // redundancy window doing its job. Expected to be roughly (kInputRedundancy - 1) per command on
    // a clean link; a zero here in a lossless test means the window was never actually exercised.
    [[nodiscard]] std::uint64_t commands_duplicate() const noexcept { return commands_duplicate_; }

    // Commands that arrived out of contract and were clamped (see sanitize).
    [[nodiscard]] std::uint64_t commands_sanitized() const noexcept { return commands_sanitized_; }

    // Accepted commands evicted un-drained because the per-client buffer was full — a game that
    // stopped draining, or a client sending far faster than the server ticks. Counted rather than
    // silent, because the consumption frontier steps over them and the client will retire them as
    // if they had been acted on.
    [[nodiscard]] std::uint64_t commands_dropped_overflow() const noexcept {
        return commands_dropped_overflow_;
    }

    // Gaps in a client's sequence: commands that were lost outright (every copy in the redundancy
    // window dropped). The proof asserts this is non-zero under scripted loss — a redundancy window
    // that never had to cover anything proves nothing about redundancy.
    [[nodiscard]] std::uint64_t gaps_observed() const noexcept { return gaps_observed_; }

    [[nodiscard]] std::uint64_t malformed_messages() const noexcept { return malformed_; }

private:
    // Per-client input state. Two frontiers, and collapsing them into one is the bug this design is
    // shaped to avoid:
    //
    //   received_through — the newest sequence ACCEPTED into the buffer. Deduplication reads this,
    //                      and it must advance on arrival or the redundancy window would deliver
    //                      every command three times.
    //   consumed_through — the newest sequence the GAME HAS DRAINED. The ack reports this, and it
    //                      advances nowhere else.
    //
    // If the ack reported `received_through`, a client would retire commands sitting in a buffer
    // that a stalled or overflowing server may still discard — a claim about what the peer holds
    // resting on evidence weaker than holding, which is corollary 1 of the replication invariant,
    // pointed upstream.
    //
    // NOTE what `consumed_through` is NOT: a completeness claim. It steps over permanent gaps,
    // because on an unreliable channel a lost command is never coming, and a frontier that waited
    // for one would stall forever. That is a real difference from AckTracker's watermark, and the
    // invariant's own "what is not covered" section is the precedent for writing it down rather
    // than letting the two look alike.
    struct ClientInput {
        net::SessionId id{};
        bool in_use = false;
        std::uint32_t received_through = 0;
        std::uint32_t consumed_through = 0;
        std::vector<InputCommand> buffered; // accepted, not yet drained; oldest first
    };

    ClientInput& client_for(net::SessionId id);

    std::vector<ClientInput> clients_;
    std::vector<net::Received> inbox_;
    std::vector<std::byte> scratch_;
    std::uint64_t commands_accepted_ = 0;
    std::uint64_t commands_duplicate_ = 0;
    std::uint64_t commands_sanitized_ = 0;
    std::uint64_t commands_dropped_overflow_ = 0;
    std::uint64_t gaps_observed_ = 0;
    std::uint64_t malformed_ = 0;
};

} // namespace rime::replication
