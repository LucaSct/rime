// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/platform/event.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/platform/mouse.hpp"
#include "rime/stream/protocol.hpp"

// `stream::InputEvent` -> `platform::Event` (m18 Track H, ADR-0054).
//
// WHAT WAS MISSING. `stream::InputEvent` has been on the wire since S0.4 and the browser page has
// been sending it since m18 Track H's page brick, but nothing on the server side ever turned one
// back into engine input: the editor host's drain loop reinterpreted the 0x0101 message as an
// `EditorMessage` and did nothing with it, which is a silent no-op that reads exactly like a
// client saying nothing at all. rime.peekstar.eu was therefore watch-only. This is the missing
// half.
//
// WHY A CLASS AND NOT A FUNCTION. Three of the four things a correct translation has to do need
// memory of earlier events:
//
//   * `platform::Event::MouseMove` carries an absolute position AND a delta, and the wire carries
//     only the position (stream-frame pixels), so the delta is `this - previous`. A first-person
//     camera reads the delta and nothing else.
//   * `Event::KeyData::repeat` distinguishes an auto-repeat from a fresh press. The wire has no
//     repeat bit and does not need one: a `KeyDown` for a key we already believe is held IS a
//     repeat, which is a fact only held-state can supply.
//   * Release-all on disconnect. A `Bye` that arrives between a KeyDown and its KeyUp leaves W
//     held forever and the player walking into a wall; the fix is to remember what is down and
//     synthesize the ups. (`Input` has no "forget everything" call, and should not — a released
//     key must look like a released key to every edge-detecting consumer.)
//
// Keeping that state in one named object rather than in the host's frame loop is what makes the
// whole path testable without a socket, a browser, or a GPU: feed it `InputEvent`s, read the
// `platform::Event`s out, fold them into a `platform::Input`, and assert what the game sees.
namespace rime::app {

class StreamInputTranslator {
public:
    // Turn one decoded `InputEvent` into 0..n platform events, APPENDED to `out` (the caller
    // batches a whole drain into one vector, which is the shape `Application::post_input` and
    // `gameplay::FlyCamera::update` both want).
    //
    // Nothing here can fail: every field of an `InputEvent` that survived `decode` is legal input.
    // An unrecognised key usage or button index is counted and dropped, not rejected — the peer is
    // a browser on the public internet and "a key this build does not know" must cost one counter,
    // not a dropped session.
    void translate(const stream::InputEvent& event, std::vector<platform::Event>& out);

    // Append a KeyUp / MouseButton-up for everything this translator believes is still held, and
    // forget it. Call on `Bye`, on a dropped connection, and on anything else that means "the
    // sender is gone" — see the class comment for why a stuck key is the failure that matters.
    void release_all(std::vector<platform::Event>& out);

    // Counters (the engine's rule: every drop path gets one, or a silent stream is undiagnosable).
    [[nodiscard]] std::uint64_t unknown_usages() const noexcept { return unknown_usages_; }

    [[nodiscard]] std::uint64_t unknown_buttons() const noexcept { return unknown_buttons_; }

    // Held state, exposed for the host's diagnostics and for the release-all proof.
    [[nodiscard]] bool key_held(platform::Key key) const noexcept;

    [[nodiscard]] bool button_held(platform::MouseButton button) const noexcept;

    [[nodiscard]] std::size_t held_count() const noexcept;

    // The wire's `mods` bitmask IS `platform::KeyMods` (Shift=1, Ctrl=2, Alt=4, Super=8) — see the
    // `InputEvent` comment in protocol.hpp. Bits above those four are reserved and ignored rather
    // than rejected, so a newer client that starts sending one is not refused.
    [[nodiscard]] static platform::KeyMods mods_from_wire(std::uint32_t mods) noexcept;

    // DOM `MouseEvent.button` order (0 left, 1 middle, 2 right, 3 back, 4 forward) -> the engine's
    // `MouseButton` (Left, Right, Middle, X1, X2). The two orders DIFFER in the middle — the
    // single most likely place for this path to be silently wrong — so the swap lives in one named
    // function with one test rather than inline in a switch. Returns false for an index neither
    // side defines.
    [[nodiscard]] static bool button_from_wire(std::uint32_t code,
                                               platform::MouseButton& out) noexcept;

private:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(platform::Key::Count);
    static constexpr std::size_t kButtonCount =
        static_cast<std::size_t>(platform::MouseButton::Count);

    std::array<bool, kKeyCount> keys_{};
    std::array<bool, kButtonCount> buttons_{};
    // The last pointer position, in stream-frame pixels, and whether we have one at all: the first
    // PointerMove of a session has no predecessor, and inventing a delta from (0,0) would snap a
    // first-person camera by the whole screen on the player's first twitch.
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
    bool have_pointer_ = false;
    std::uint64_t unknown_usages_ = 0;
    std::uint64_t unknown_buttons_ = 0;
};

// What `dispatch_input_message` did with a drained message.
enum class InputDispatch : std::uint8_t {
    NotInput,  // some other message type; the caller's own handling still owns it
    Applied,   // decoded and translated; `out` grew by 0..n events
    Malformed, // an `Input` whose payload did not decode — count it and carry on
};

// The editor host's `Input` handling, as a function.
//
// It lives here rather than inline in the drain loop for one reason: the drain loop needs a GPU, a
// socket and a live client, so a branch inside it is unprovable, and "the host handles `Input`" is
// exactly the claim m18 needs to be able to break on purpose. Pulling it out makes the claim a
// three-line call at the call site and a testable function here.
//
// Returns `NotInput` for anything that is not `MessageType::Input`, so the caller's `if` reads the
// same way the `Capabilities` and `KeyframeRequest` branches beside it do — and, crucially, this
// must be asked BEFORE an editor host casts the type to an `EditorMessage`: `Input` is 0x0101, a
// STREAM-band code, and reinterpreting it as an editor message is a silent no-op that looks
// exactly like a client saying nothing.
[[nodiscard]] InputDispatch dispatch_input_message(stream::MessageType type,
                                                   std::span<const std::byte> payload,
                                                   StreamInputTranslator& translator,
                                                   std::vector<platform::Event>& out);

} // namespace rime::app
