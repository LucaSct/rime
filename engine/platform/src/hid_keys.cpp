// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/platform/hid_keys.hpp"

#include <array>
#include <cstddef>

namespace rime::platform {
namespace {

// ONE table, both directions. The two lookup arrays below are *derived* from this list at compile
// time, so a key can never map forward to a usage that maps back to a different key — the class of
// bug a hand-written pair of switch statements invites and that the round-trip test would then have
// to catch after the fact. Order here is the HID usage order (HID Usage Tables §10, "Keyboard/
// Keypad Page (0x07)"), which is also roughly the order a US keyboard is read.
struct Mapping {
    std::uint8_t usage;
    Key key;
};

constexpr std::array<Mapping, 105> kMappings{{
    // 0x04..0x1D — letters, in alphabetical order (the page's own order, not the keyboard's).
    {0x04, Key::A},         {0x05, Key::B},         {0x06, Key::C},
    {0x07, Key::D},         {0x08, Key::E},         {0x09, Key::F},
    {0x0A, Key::G},         {0x0B, Key::H},         {0x0C, Key::I},
    {0x0D, Key::J},         {0x0E, Key::K},         {0x0F, Key::L},
    {0x10, Key::M},         {0x11, Key::N},         {0x12, Key::O},
    {0x13, Key::P},         {0x14, Key::Q},         {0x15, Key::R},
    {0x16, Key::S},         {0x17, Key::T},         {0x18, Key::U},
    {0x19, Key::V},         {0x1A, Key::W},         {0x1B, Key::X},
    {0x1C, Key::Y},         {0x1D, Key::Z},
    // 0x1E..0x27 — the number row. Note the page starts at 1 and puts 0 LAST, which is the one
    // place this table is not a straight offset from the digit.
    {0x1E, Key::Num1},      {0x1F, Key::Num2},      {0x20, Key::Num3},
    {0x21, Key::Num4},      {0x22, Key::Num5},      {0x23, Key::Num6},
    {0x24, Key::Num7},      {0x25, Key::Num8},      {0x26, Key::Num9},
    {0x27, Key::Num0},
    // 0x28..0x38 — whitespace, editing and the US punctuation positions.
    {0x28, Key::Enter},     {0x29, Key::Escape},    {0x2A, Key::Backspace},
    {0x2B, Key::Tab},       {0x2C, Key::Space},     {0x2D, Key::Minus},
    {0x2E, Key::Equal},     {0x2F, Key::LeftBracket},
    {0x30, Key::RightBracket},
    // 0x31 is "\ and |"; 0x32 is the non-US "# and ~" key that sits in the same place on an ISO
    // board. `Key` has one Backslash, and a keyboard never has both, so only 0x31 is mapped —
    // 0x32 decodes to Unknown and is counted. (Mapping both would break the round trip.)
    {0x31, Key::Backslash}, {0x33, Key::Semicolon}, {0x34, Key::Apostrophe},
    {0x35, Key::Grave},     {0x36, Key::Comma},     {0x37, Key::Period},
    {0x38, Key::Slash},
    // 0x39..0x45 — CapsLock then F1..F12.
    {0x39, Key::CapsLock},  {0x3A, Key::F1},        {0x3B, Key::F2},
    {0x3C, Key::F3},        {0x3D, Key::F4},        {0x3E, Key::F5},
    {0x3F, Key::F6},        {0x40, Key::F7},        {0x41, Key::F8},
    {0x42, Key::F9},        {0x43, Key::F10},       {0x44, Key::F11},
    {0x45, Key::F12},
    // 0x46..0x52 — the system/navigation cluster above and beside the arrows.
    {0x46, Key::PrintScreen},
    {0x47, Key::ScrollLock},
    {0x48, Key::Pause},     {0x49, Key::Insert},    {0x4A, Key::Home},
    {0x4B, Key::PageUp},    {0x4C, Key::Delete},    {0x4D, Key::End},
    {0x4E, Key::PageDown},  {0x4F, Key::Right},     {0x50, Key::Left},
    {0x51, Key::Down},      {0x52, Key::Up},
    // 0x53..0x63, 0x67 — the keypad. Like the number row it runs 1..9 then 0.
    {0x53, Key::NumLock},   {0x54, Key::KPDivide},  {0x55, Key::KPMultiply},
    {0x56, Key::KPSubtract},
    {0x57, Key::KPAdd},     {0x58, Key::KPEnter},   {0x59, Key::KP1},
    {0x5A, Key::KP2},       {0x5B, Key::KP3},       {0x5C, Key::KP4},
    {0x5D, Key::KP5},       {0x5E, Key::KP6},       {0x5F, Key::KP7},
    {0x60, Key::KP8},       {0x61, Key::KP9},       {0x62, Key::KP0},
    {0x63, Key::KPDecimal}, {0x65, Key::Menu},      {0x67, Key::KPEqual},
    // 0xE0..0xE7 — the eight modifiers, the one block of the page every OS agrees on exactly.
    {0xE0, Key::LeftCtrl},  {0xE1, Key::LeftShift}, {0xE2, Key::LeftAlt},
    {0xE3, Key::LeftSuper}, {0xE4, Key::RightCtrl}, {0xE5, Key::RightShift},
    {0xE6, Key::RightAlt},  {0xE7, Key::RightSuper},
}};

// The reverse direction, indexed by usage. 0xE7 is the highest usage this page defines for us, so a
// 232-entry array covers it densely — one load, no search, and the "is it in range" question and
// the "is it mapped" question have the same answer (`Key::Unknown`).
constexpr std::size_t kUsageTableSize = 0xE8;

constexpr std::array<Key, kUsageTableSize> kUsageToKey = [] {
    std::array<Key, kUsageTableSize> table{}; // value-initialised: every slot is Key::Unknown (0)
    for (const Mapping& m : kMappings) {
        table[m.usage] = m.key;
    }
    return table;
}();

constexpr std::array<std::uint8_t, static_cast<std::size_t>(Key::Count)> kKeyToUsage = [] {
    std::array<std::uint8_t, static_cast<std::size_t>(Key::Count)> table{}; // 0 = "no usage"
    for (const Mapping& m : kMappings) {
        table[static_cast<std::size_t>(m.key)] = m.usage;
    }
    return table;
}();

// A compile-time guard that the table really does cover every named key. Without it, adding a key
// to `keyboard.hpp` would silently make it un-sendable from the browser — the page would send a
// usage the engine maps, but nothing would ever produce it, and no test that only walks the table
// could notice. Key::Unknown (0) is the one deliberate hole.
constexpr bool every_key_has_a_usage() {
    for (std::size_t i = 1; i < kKeyToUsage.size(); ++i) {
        if (kKeyToUsage[i] == 0) {
            return false;
        }
    }
    return true;
}

static_assert(every_key_has_a_usage(),
              "every platform::Key but Unknown needs a HID usage in kMappings — a new key in "
              "keyboard.hpp must be given one here (see docs/adr/0054-input-key-codes-hid.md)");

} // namespace

std::uint32_t key_to_hid_usage(Key key) noexcept {
    const auto index = static_cast<std::size_t>(key);
    if (index >= kKeyToUsage.size()) {
        return 0; // Key::Count, or a value cast in from outside the enum
    }
    return kKeyToUsage[index];
}

Key hid_usage_to_key(std::uint32_t usage) noexcept {
    if (usage >= kUsageTableSize) {
        return Key::Unknown; // above the page's modifier block, or another usage page entirely
    }
    return kUsageToKey[usage];
}

} // namespace rime::platform
