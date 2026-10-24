# Play-control protocol leaf — commit 0c69102 (base 7be38d2)

## Done
- EditorMessage::PlayControlState=0x020B, PlayControl=0x021E (C++ enum + Rust EditorMessage to_code/from_code). Old tags unchanged.
- C++: engine/editorhost/include/rime/editorhost/play_control.hpp, src/play_control.cpp: PlayControlAction, PlayControlMsg, PlayControlStateMsg (+ max_reason_bytes=1024), serialize_/parse_play_control, serialize_/parse_play_control_state. Added to editorhost + tests/editorhost CMake.
- Classifier: PlayControl -> true, PlayControlState -> false (only the two switch cases touched). The command switch's `default:` already drops both; no change made, so Root implements semantics.
- Rust: tools/rime-protocol/src/play_control.rs; exports PlayControl, PlayControlAction, PlayControlState from lib.rs; new Error::BadPlayControl. encode()->Vec<u8> for PlayControl; PlayControlState::encode()->Result<Vec<u8>> (invalid state refused), decode both -> Result.
- Wire exactly as specified (9-byte command; 25-byte + reason state).
- Validation: caps 1|3, mode<=1 and Player needs bit1, phase/status ranges, Ready/Refused => Edit+uncaptured, Active => non-Edit + session>0, captured => mode1/Playing/Active/session>0, reason<=1024, strict UTF-8. Parse failure leaves C++ out unchanged. Serializers return empty (C++) / Err (Rust) for invalid input.
- Tests: identical explicit byte vectors in play_control_test.cpp and play_control.rs (command, Active state, Refused "no"); round trips for all actions and Ready/Active/Paused/Refused; stale session ids preserved; 15 inconsistent states; every truncated prefix; trailing byte; 1024 cap and forged 1025; invalid UTF-8; tag asserts + classifier.

## Checks actually run
- cargo test -p rime-protocol: 6+25 passed (lib + conformance); cargo fmt -p rime-protocol.
- clang-format (~/.rime-tools) on 3 new C++ files.
- clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Werror (+ -Wno-c2y-extensions -Wno-#warnings) on play_control.cpp and the test: clean. Hand-assembled include paths, not compile_commands (no build tree).
- Standalone g++ build of codec + test with a STUB message_affects_frame: 7 cases / 242 assertions passed. Temp files deleted.

## NOT run
- No CMake configure, no full ctest; the real classifier assertion and editor_host.cpp compile are unverified here. Disk 16 GB free at start.

## Deviations / limitations
- UTF-8: existing C++ editor strings are unvalidated length-prefixed bytes, and Rust elsewhere uses lossy decode (editor.rs:407) or maps errors to Truncated (:1223). The new messages are strict on both sides, so this deviates from existing convention. Both sides use the same strict set (no overlongs/surrogates/>U+10FFFF).
- No .bin fixture and no conformance.rs entry; vectors are duplicated in the two test files instead.
- Refused reason may be empty (as decided).
- Adding Rust EditorMessage variants breaks any exhaustive match in tools/editor (Root-owned); I did not check it.
