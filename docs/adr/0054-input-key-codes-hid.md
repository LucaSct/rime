# ADR-0054 — `InputEvent.code` carries USB HID usage IDs

- **Status:** accepted
- **Date:** 2026-09-29
- **Decided by:** Luca ("`InputEvent.code` for `KeyDown`/`KeyUp` is a USB HID usage ID,
  Keyboard/Keypad page (0x07)")
- **Relates to:** [ADR-0030](0030-streaming-v1.md) §5 (the input path and the latency echo),
  [ADR-0038](0038-platform-proof-m15.md) (the host a game composes, which grew the play-mode
  input hook this needs), [ADR-0052](0052-browser-decode-path.md) (the browser page that produces
  these events), [ADR-0023](0023-app-fixed-tick-loop.md) §5 (`post_input` /
  `frame_input`, the route the decoded events take into the engine)

## Context

`stream::InputEvent` has been on the wire since S0.4 with a `code` field documented as "a key code
(Key*)", and until m18 Track H **nothing on the server ever read it**. That is not an oversight
that cost nothing: it meant the field's meaning was never forced to be decided, and by the time two
producers existed they had each picked something different.

- `samples/04-remote-view` sent `static_cast<std::uint32_t>(platform::Key)` — the enum's
  declaration ordinal.
- `tools/rime-gateway/web/keymap.js` sent the same ordinal, transcribed **by hand** from
  `keyboard.hpp`, and said so in its own comment: "INFERRED, not measured … If a future engine-side
  injector picks a different numbering, this table moves."

Both are unsound for the same reason, and the header says it out loud:

> Physical keys (US-QWERTY position names). **Values are arbitrary and stable; do not rely on them
> numerically.** — `engine/platform/include/rime/platform/keyboard.hpp`

`Key`'s value freedom is load-bearing. The enum is grouped by function (letters, number row,
navigation, keypad), so the natural way to add a key — `F13` after `F12`, another keypad key in the
keypad block — **renumbers everything after it**. An engine and a browser page that disagreed by
one would deliver `Key::B` for every `A`.

So m18 had to decide the numbering before the host could consume the message at all.

## The three options, and what each costs

### 1. Freeze the `platform::Key` ordinals — REJECTED

Declare the enum's current values to be the wire format and stop moving them.

**Cost:** it takes back exactly the freedom `keyboard.hpp` reserves, permanently, in exchange for
nothing — the ordinals are not a standard anybody else knows, so the browser would still be
hand-transcribing a table, just a table it was now forbidden to be wrong about. Every future key
would have to be appended to the end of the enum rather than filed with its neighbours, which makes
the enum less readable forever so that one wire field can be lazier. And the constraint would be
invisible: nothing about `Key::F12,` in a header tells the next contributor that inserting a line
below it breaks a deployed browser.

### 2. Send `KeyboardEvent.code` strings — REJECTED

Put `"KeyA"`, `"ArrowRight"` on the wire and look them up server-side.

**Cost:** it breaks the fixed 37-byte payload. `InputEvent` is a fixed-size record — that is what
lets `decode` reject a truncated event in one length check and what keeps the browser's encoder a
straight-line `DataView` walk. A variable-length string means a length prefix, a bounded-length
policy, a UTF-8 validation decision, and a hash or a 200-entry `string_view` comparison per event
on the server. It also hands an internet-facing decoder an attacker-chosen length. The browser is
the only client whose key identity is *natively* a string; a native client would have to invent one.

### 3. USB HID usage IDs, Keyboard/Keypad page 0x07 — **CHOSEN**

`KeyA` = 0x04, `Enter` = 0x28, `Space` = 0x2C, `ArrowRight` = 0x4F, `ControlLeft` = 0xE0.

**Why it is the right answer and not merely a workable one.** The HID page identifies keys **by
position**, which is precisely what `platform::Key` means and precisely what `KeyboardEvent.code`
means — all three already agree about what a key *is*, so the mapping is a transcription and not a
translation. It is a published standard (HID Usage Tables §10) that has not moved since USB 1.1, so
neither end ships a version of it and neither end can renumber it. And every OS keymap in this
repository is already derived from it one step removed: Linux evdev scancodes *are* HID usages plus
8, and the Win32 and macOS tables are the same physical keyboard under other names.

**What it costs.** Two hand-derived tables, in two languages, that a compiler cannot check against
each other — `engine/platform/src/hid_keys.cpp` and `tools/rime-gateway/web/keymap.js`. The
mitigation is on both sides and is the reason to write it down here: the C++ table is a single list
driving both directions (so the round trip cannot drift) with a `static_assert` that every named
`Key` has a usage (so adding a key to `keyboard.hpp` is a build error until it has one), and the JS
table has its own test over the standard's checkable anchors and the three places its numbering is
counter-intuitive. It also costs one deliberate hole: usage 0x32 (the non-US "# and ~") is
unmapped, because `Key` has a single `Backslash` and no keyboard carries both keys.

## The three numberings decided alongside it

These were undefined in the same way and are settled here rather than left to the next brick.

- **`code` for `PointerDown`/`PointerUp` is a DOM `MouseEvent.button` index**: 0 left, 1 **middle**,
  2 right, 3 back, 4 forward. This is *not* `platform::MouseButton`'s order (Left, Right, Middle,
  X1, X2) — they differ in the middle. The browser is the client that cannot change its own
  numbering, and the engine is the side that can translate, so the engine translates
  (`app::StreamInputTranslator::button_from_wire`, one named function with one test, because a
  silent middle/right swap is the most likely way this path is wrong).
- **`mods` is the `platform::KeyMods` bitmask**: Shift 1, Ctrl 2, Alt 4, Super 8. Those values are
  explicit in `keyboard.hpp` (unlike `Key`'s), so freezing them takes nothing away. Bits above the
  low four are reserved and **ignored** rather than rejected, so a newer client that starts setting
  one is not refused.
- **`x`/`y` are stream-frame pixels** — the pixel space of the image the server is sending (the
  `StreamConfig` extent), not the client's window or CSS pixels. This was already what the native
  producer did (`samples/04-remote-view` scales window pixels into frame pixels before sending) and
  what `protocol.hpp` meant by "client pixels"; the browser page was sending raw pointer-lock
  deltas, a different quantity in a different space, and is the side that was fixed. Coordinates
  **may fall outside** the frame: a pointer-locked client integrates relative motion into an
  unbounded virtual position, because the engine reconstructs its `MouseMove` delta as
  (this − previous) and clamping at an edge would stall a look mid-turn.

## No protocol version bump

`ProtocolConnection`'s handshake version is unchanged, and that is a judgement call worth recording.

A bump is owed when a **shipped** producer's bytes change meaning for a **shipped** consumer. Here
there is no such pair: `code` for a key event was read by nothing in the repository before this
brick. `samples/04-remote-view`'s own server resets its colour on *any* `KeyDown` without looking at
`code`, and 07-first-light, 08-gltf-zoo and 10-destructible-wall do the same. The browser page is
updated in the same change as the engine and is served from the same deployment. Bumping the
version would refuse connections between two builds that would in fact interoperate, and would
spend the engine's one compatibility signal on a field whose old meaning nobody acted on.

The button-index and coordinate-space changes are the same story: `04-remote-view` was the only
producer of a button index, nothing consumed it, and its coordinates already followed the rule now
written down.

## Consequences

- `platform::hid_usage_to_key` / `key_to_hid_usage` are the only sanctioned route between the two
  numberings; a second hand-rolled switch anywhere is a bug.
- Adding a key to `platform::Key` fails the build until `kMappings` gives it a usage. That is
  deliberate: the alternative is a key that is silently unreachable from every remote client.
- An unmapped usage is `Key::Unknown`, **counted**, and dropped — never an error and never a closed
  session. The peer is a browser on the public internet; "a key this build does not know" must cost
  one counter.
- The editor host's `run_editor_host` grows an optional `PlayTick` callback: the engine decodes and
  delivers input, and the *game's* host binary decides what a key means (ADR-0038's composition
  point, now with a third question after "what are my components" and "what do they look like").
- Not proved here: no browser has driven this path end to end. The engine side is proved by unit
  tests down to `platform::Input`, and the page side by its own table test, but the join between
  them is still the first-real-Chrome-run proof ADR-0052 also owes.
