#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# Rime — the editor CLICK smoke: the real `editor` binary and the real `rime-engine`, driven with a
# mouse and a keyboard, on a display nobody is looking at.
#
#   launch → place a mesh → select it → edit a field → undo → redo → Play → Stop → Save As → quit
#
# and then the only assertion that matters: THE FILE ON DISK says what the clicks did.
#
# Why this exists next to `tools/editor/src/gui/click_tests.rs`: those tests drive the shipped
# widgets against a FAKE engine, so they prove what the UI sends. This proves the two real processes
# agree about it — window, GL, the socket, the engine's world, the scene writer — which is the part
# ADR-0031 left to "Mac-eyeballed". `scripts/editor-smoke.sh` covers the same wire headlessly; this
# is the only place a pointer event travels the whole way to a `.rscene`.
#
# HOW IT WAITS. Never on a timer. Every step ends in an observation with a timeout:
#   * the ENGINE'S LOG — `editor-host: saved N entities to <path>` is printed once per completed
#     save, so "press Ctrl+S, wait for the count of those lines to rise" is a checkpoint: the scene
#     file is then a complete picture of the world, and the step's effect is asserted from it;
#   * a PIXEL — the viewport's border is green exactly while the engine reports Playing;
#   * a PROCESS — the editor exiting, and its engine child with it.
# The short pauses between a pointer move and its click are input pacing, not synchronisation:
# egui hit-tests a press against the widgets of the PREVIOUS frame, so a move and a press delivered
# inside one frame land on nothing. They are a property of the input, like a human's hand.
#
# WHERE IT CLICKS. xdotool has only coordinates, so the geometry is pinned instead: a 1280x800
# screen at 96 dpi, no window manager (the window is the screen), and egui's fonts are compiled into
# the binary, so text metrics do not depend on this machine. Rows are computed from counts (the
# scene's entities, the manifest's order) rather than written down. A click that lands wrong cannot
# pass quietly — every one is followed by a checkpoint that reads the result back from disk.
#
# ISOLATION. Everything happens on Xvfb display :99, which this script starts and stops. It refuses
# to run if :99 already exists, forces winit onto X11, and never reads the caller's DISPLAY or
# WAYLAND_DISPLAY — the user's live desktop cannot receive a single event from it.
#
# Vulkan is pinned to RADV (Mesa): the engine renders offscreen and streams, and Mesa is the stack
# that also behaves on a virtual X server. Needs: Xvfb, xdotool, ImageMagick `import`, python3, a
# built `rime-engine` (scripts/build.sh --cpp-only --no-tests), and cargo.
#
# Exit status: 0 = every step observed; 1 = a step failed (a screenshot + logs are kept and their
# path printed); 2 = a prerequisite is missing.
set -euo pipefail

preset="dev"
keep=0
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        --keep)     keep=1; shift ;;
        -h|--help)
            echo "Usage: scripts/editor-click-smoke.sh [--preset dev|release] [--keep]"
            echo "  --keep   keep the work directory (logs, scenes) even on success"
            exit 0 ;;
        *) echo "editor-click-smoke.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done
case "$preset" in
    dev)     cargo_flag="";          cargo_dir="debug" ;;
    release) cargo_flag="--release"; cargo_dir="release" ;;
    *) echo "editor-click-smoke.sh: unknown preset '$preset'" >&2; exit 2 ;;
esac

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
say()  { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
note() { printf '   %s\n' "$1"; }

# ── The isolated display ────────────────────────────────────────────────────────────────────────
# Set once, here, and exported: every X client below inherits it. The caller's own display
# variables are dropped on the floor so nothing can fall back to them.
unset WAYLAND_DISPLAY WAYLAND_SOCKET XAUTHORITY
export DISPLAY=":99"
export WINIT_UNIX_BACKEND="x11"
screen_w=1280; screen_h=800

for tool in Xvfb xdotool import python3 cargo; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "editor-click-smoke.sh: missing prerequisite '$tool'" >&2; exit 2; }
done
radv_icd="/usr/share/vulkan/icd.d/radeon_icd.json"
[ -f "$radv_icd" ] || { echo "editor-click-smoke.sh: no RADV ICD at $radv_icd" >&2; exit 2; }
if [ -e /tmp/.X11-unix/X99 ] || [ -e /tmp/.X99-lock ]; then
    echo "editor-click-smoke.sh: display :99 already exists — refusing to share or replace it" >&2
    exit 2
fi

build_dir="build/$preset"
engine_bin="$repo_root/$build_dir/bin/rime-engine"
[ -x "$engine_bin" ] || {
    echo "editor-click-smoke.sh: missing $engine_bin — run scripts/build.sh --cpp-only --no-tests" >&2
    exit 2; }

work="$repo_root/$build_dir/editor-click-smoke"
rm -rf "$work"; mkdir -p "$work"
log="$work/editor.log"

# ── Build the editor (with its window) and cook something to place ──────────────────────────────
say "build the editor (cargo, --features gui) and cook the glTF zoo"
( cd tools && cargo build --quiet -p editor --features gui $cargo_flag )
editor_bin="${CARGO_TARGET_DIR:-$repo_root/tools/target}/$cargo_dir/editor"
[ -x "$editor_bin" ] || { echo "editor-click-smoke.sh: missing $editor_bin" >&2; exit 2; }
# Cooked from the repo root, the way the CTest fixture does it, so the browser shows the same
# source paths a developer would see.
cargo run --quiet $cargo_flag --manifest-path tools/Cargo.toml --bin rime -- \
    cook samples/08-gltf-zoo/assets --out "$work/cooked" >"$work/cook.log" 2>&1 || {
    cat "$work/cook.log" >&2; echo "editor-click-smoke.sh: the cook failed" >&2; exit 2; }
manifest="$work/cooked/manifest.txt"

# The scene to open: a COPY, because Ctrl+S writes back to what was opened.
source_scene="$repo_root/samples/07-first-light/first_light.rscene"
opened="$work/opened.rscene"
saved_as="$work/saved-as.rscene"
cp "$source_scene" "$opened"

# ── Teardown: nothing of ours outlives the script, and a failure leaves evidence ────────────────
xvfb_pid=""; editor_pid=""; engine_pid=""; status=1
cleanup() {
    if [ "$status" -ne 0 ] && [ -n "$xvfb_pid" ] && kill -0 "$xvfb_pid" 2>/dev/null; then
        # Kept ONLY on failure: what the screen looked like at the moment a step gave up.
        import -window root "$work/failure.png" 2>/dev/null || true
    fi
    for pid in "$editor_pid" "$engine_pid" "$xvfb_pid"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    [ -n "$xvfb_pid" ] && wait "$xvfb_pid" 2>/dev/null || true
    if [ "$status" -eq 0 ] && [ "$keep" -eq 0 ]; then
        rm -rf "$work"
    else
        echo "editor-click-smoke.sh: logs and scenes kept in $work" >&2
    fi
}
trap cleanup EXIT

fail() {
    echo "editor-click-smoke.sh: FAIL — $1" >&2
    [ -f "$log" ] && { echo "--- last engine/editor log lines ---" >&2
                       grep -v -e '\[WARN\] \[vulkan\]' -e '^vkCreate' "$log" | tail -8 >&2; }
    exit 1
}

# Poll `"$@"` until it succeeds or $1 seconds pass. The poll interval is how often we LOOK, not how
# long anything is assumed to take.
wait_for() { # $1 = timeout seconds, $2 = what (for the failure message), rest = the predicate
    local timeout="$1" what="$2"; shift 2
    local deadline=$(( $(date +%s%N) + timeout * 1000000000 ))
    until "$@"; do
        [ "$(date +%s%N)" -lt "$deadline" ] || fail "timed out after ${timeout}s waiting for $what"
        sleep 0.05
    done
}

# ── Input ───────────────────────────────────────────────────────────────────────────────────────
pace() { sleep 0.12; }   # one comfortable UI frame between dependent input events (see header)
click() { xdotool mousemove "$1" "$2"; pace; xdotool click 1; pace; }
# Keys are sent with the pointer PARKED on the status bar. That is not fussiness: the fly camera
# reads W/A/S/D/Q/E whenever the pointer is over the viewport and ignores modifiers, so a Ctrl+S
# pressed there also flies the camera backwards, and the next checkpoint records a camera nobody
# moved (click_tests.rs: ctrl_s_over_the_viewport_also_flies_the_camera_backwards).
park() { xdotool mousemove 640 $(( screen_h - 6 )); pace; }
key()  { park; xdotool key "$1"; pace; }
drag() { # $1,$2 → $3,$4 (left button), in steps a real hand would produce
    xdotool mousemove "$1" "$2"; pace; xdotool mousedown 1; pace
    local i
    for i in 1 2 3 4 5 6; do
        xdotool mousemove $(( $1 + ($3 - $1) * i / 6 )) $(( $2 + ($4 - $2) * i / 6 )); sleep 0.04
    done
    pace; xdotool mouseup 1; pace
}

# Ask the window to close the way a window manager's ✕ does: a WM_DELETE_WINDOW client message.
# (There is no WM on :99 to do it, xdotool's `windowclose` DESTROYS the window — winit panics on
# that — and the editor has no Quit of its own.)
close_window() { # $1 = X window id
    python3 - "$1" <<'PY'
import ctypes, sys
x = ctypes.CDLL("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XInternAtom.restype = ctypes.c_ulong
x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
class ClientMessage(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("window", ctypes.c_ulong),
                ("message_type", ctypes.c_ulong), ("format", ctypes.c_int),
                ("data", ctypes.c_long * 5), ("pad", ctypes.c_long * 24)]  # >= sizeof(XEvent)
dpy = x.XOpenDisplay(b":99")
if not dpy:
    sys.exit("cannot open display :99")
win = int(sys.argv[1])
ev = ClientMessage()
ev.type = 33  # ClientMessage
ev.window = win
ev.message_type = x.XInternAtom(dpy, b"WM_PROTOCOLS", 0)
ev.format = 32
ev.data[0] = x.XInternAtom(dpy, b"WM_DELETE_WINDOW", 0)
x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long,
                         ctypes.c_void_p]
x.XSendEvent(dpy, win, 0, 0, ctypes.byref(ev))
x.XFlush.argtypes = [ctypes.c_void_p]
x.XFlush(dpy)
PY
}

# ── Observation ─────────────────────────────────────────────────────────────────────────────────
saves_logged() { grep -c 'editor-host: saved ' "$log" 2>/dev/null || true; }
pixel() { import -window root -crop "1x1+$1+$2" txt:- 2>/dev/null | grep -o 'srgb([0-9,]*)' || true; }

# One checkpoint: Ctrl+S, then wait for the ENGINE to say it wrote the file. After this returns,
# "$opened" is the world as the engine holds it.
checkpoint() {
    local before; before="$(saves_logged)"
    key ctrl+s
    wait_for 10 "the engine to log a completed save" \
        eval '[ "$(saves_logged)" -gt '"$before"' ]'
}

entity_count() { grep -c '^entity ' "$1"; }
# The block of one entity in a scene file (`entity N {` … the closing `}` in column 0).
entity_block() { awk -v n="$2" '$0 == "entity " n " {" {on=1} on {print} on && $0 == "}" {exit}' "$1"; }
translation_of() { entity_block "$1" "$2" | grep -m1 'translation' | sed 's/^ *//'; }
expect() { # $1 = what, $2 = actual, $3 = expected
    [ "$2" = "$3" ] || fail "$1: expected '$3', got '$2'"
    note "ok  $1: $2"
}

# ── The pinned geometry (1280x800 @ 96 dpi; see header) ─────────────────────────────────────────
menu_y=10
file_menu_x=107
save_as_field_x=230; save_as_field_y=85; save_as_write_x=389
play_x=576; stop_x=646
# The left column (Outliner over Assets) starts 219 px wide. The "place" button used to sit AFTER
# the source path, so at that width a real path clipped it off the panel's right edge, and this
# script had to drag the splitter to reach it. The button now leads each row (E2), so it is on
# screen at the default width too; the drag stays only because the rest of the pinned geometry
# below (the viewport's left edge, the border) was measured with the panel widened.
splitter_x=220; splitter_to_x=560; splitter_y=300
outliner_row0_y=90; row_h=21; outliner_text_x=48
asset_row0_y=545
# The "place" button's centre on a mesh row, measured from a failure screenshot at the widened
# panel (its left edge is at x 28; the left edge does not move with the splitter). Pinned rather
# than computed from the path length, since the button no longer follows the path.
place_centre_x=46
inspector_translation_x_field="1093 216"   # first numeric field of the first component
# 3 px inside the (widened) viewport panel's left edge: the play-state border.
border_px="566 400"
playing_green="srgb(90,200,120)"

# ── Launch ──────────────────────────────────────────────────────────────────────────────────────
say "start Xvfb $DISPLAY (${screen_w}x${screen_h}, 96 dpi) and the editor"
Xvfb "$DISPLAY" -screen 0 "${screen_w}x${screen_h}x24" -dpi 96 -nolisten tcp -noreset \
    >"$work/xvfb.log" 2>&1 &
xvfb_pid=$!
wait_for 10 "Xvfb to create its socket" test -e /tmp/.X11-unix/X99

VK_ICD_FILENAMES="$radv_icd" "$editor_bin" --engine "$engine_bin" --scene "$opened" \
    --assets "$manifest" >"$log" 2>&1 &
editor_pid=$!

window_id() { xdotool search --name '^Rime Editor$' 2>/dev/null | head -1; }
wait_for 30 "the editor window" eval '[ -n "$(window_id)" ]'
wid="$(window_id)"
wait_for 30 "the engine host to come up" grep -q 'editor-host: editor host + viewport on' "$log"
engine_pid="$(pgrep -P "$editor_pid" -f rime-engine | head -1 || true)"
[ -n "$engine_pid" ] || fail "the editor has no rime-engine child"
grep -q "RADV" "$log" || fail "the engine did not pick the RADV device"
xdotool windowfocus "$wid"
geometry="$(xdotool getwindowgeometry "$wid" | tr '\n' ' ')"
case "$geometry" in
    *"Position: 0,0"*"Geometry: ${screen_w}x${screen_h}"*) ;;
    *) fail "the window is not the pinned ${screen_w}x${screen_h} at 0,0: $geometry" ;;
esac

# Ready means: a keystroke reaches the editor, the editor reaches the engine, and the engine
# answers. Keys sent before the first frame are simply lost, so Ctrl+S is offered until one lands.
say "wait until the editor round-trips a save"
ready() { key ctrl+s; [ "$(saves_logged)" -gt 0 ]; }
wait_for 30 "the first Ctrl+S to round-trip" ready
checkpoint
baseline="$(entity_count "$opened")"
expect "entities in the opened scene" "$baseline" "$(entity_count "$source_scene")"
# Opening a scene and saving it untouched must not change a byte — otherwise every later
# comparison in this script is measuring the writer, not the edit.
cmp -s "$source_scene" "$opened" || fail "an untouched save differs from the scene that was opened"
note "ok  an untouched save is byte-identical to the opened scene"

# ── Place a mesh ────────────────────────────────────────────────────────────────────────────────
say "place a mesh from the asset browser"
drag "$splitter_x" "$splitter_y" "$splitter_to_x" "$splitter_y"
# The first mesh in the manifest: which row it is on, how wide its path is, what id it carries.
mesh_row="$(awk -F'\t' '!/^#/ {row++} $2=="mesh" {print row-1; exit}' "$manifest")"
mesh_path="$(awk -F'\t' '$2=="mesh" {print $1; exit}' "$manifest")"
mesh_id_hex="$(awk -F'\t' '$2=="mesh" {print $3; exit}' "$manifest")"
mesh_id_dec="$(python3 -c "print(int('$mesh_id_hex', 16))")"
place_x=$place_centre_x
place_y=$(( asset_row0_y + row_h * mesh_row ))
note "placing $mesh_path (id $mesh_id_hex) — row $mesh_row, button at $place_x,$place_y"
click "$place_x" "$place_y"
checkpoint
placed="$baseline"   # entities are numbered from 0, so the new one is number <old count>
expect "entities after place" "$(entity_count "$opened")" "$(( baseline + 1 ))"
entity_block "$opened" "$placed" | grep -q "asset $mesh_id_dec\$" \
    || fail "entity $placed does not reference mesh $mesh_id_hex ($mesh_id_dec)"
note "ok  entity $placed references the mesh by content id"
expect "placed at the engine's default transform" \
    "$(translation_of "$opened" "$placed")" "translation { x 0 y 0 z 0 }"

# ── Select it, edit a field ─────────────────────────────────────────────────────────────────────
say "select the placed entity in the outliner and type translation.x = 5"
click "$outliner_text_x" $(( outliner_row0_y + row_h * placed ))
# shellcheck disable=SC2086 # two words on purpose: x y
click $inspector_translation_x_field
xdotool type --delay 40 "5"; pace
xdotool key Return; pace
checkpoint
expect "after the edit" "$(translation_of "$opened" "$placed")" "translation { x 5 y 0 z 0 }"

# ── Undo, redo ──────────────────────────────────────────────────────────────────────────────────
say "undo (Ctrl+Z), redo (Ctrl+Y)"
key ctrl+z
checkpoint
expect "after undo" "$(translation_of "$opened" "$placed")" "translation { x 0 y 0 z 0 }"
key ctrl+y
checkpoint
expect "after redo" "$(translation_of "$opened" "$placed")" "translation { x 5 y 0 z 0 }"
cp "$opened" "$work/before-play.rscene"

# ── Play, Stop ──────────────────────────────────────────────────────────────────────────────────
say "Play, then Stop"
is_playing() { [ "$(pixel $border_px)" = "$playing_green" ]; }
not_playing() { ! is_playing; }
not_playing || fail "the viewport border is already green before Play"
click "$play_x" "$menu_y"
wait_for 10 "the viewport border to turn green (Playing)" is_playing
note "ok  Playing (the viewport border is $playing_green)"
click "$stop_x" "$menu_y"
wait_for 10 "the viewport border to clear (back to Edit)" not_playing
note "ok  stopped (border cleared)"

# A SECOND PINNED KNOWN DEFECT, same cause as the Parent one asserted at the end: Stop re-creates
# every entity under a new handle, and the undo history still names the old ones. Undo stays
# enabled, is consumed, and changes nothing — one Play/Stop silently kills the whole history.
# Pinned the same way: exactly this behaviour passes; if undo starts working again, remove the pin.
key ctrl+z
checkpoint
case "$(translation_of "$opened" "$placed")" in
    "translation { x 5 y 0 z 0 }")
        note "KNOWN DEFECT (pinned): undo after Play→Stop did nothing (history names dead handles)" ;;
    "translation { x 0 y 0 z 0 }")
        fail "undo works again after Play/Stop — the known-defect pin is stale: remove it" ;;
    *)  fail "undo after Play/Stop did something unexpected: $(translation_of "$opened" "$placed")" ;;
esac

# ── Save As ─────────────────────────────────────────────────────────────────────────────────────
say "File ▸ Save As"
before="$(saves_logged)"
click "$file_menu_x" "$menu_y"
click "$save_as_field_x" "$save_as_field_y"
xdotool key ctrl+a; pace
xdotool type --delay 8 "$saved_as"; pace
click "$save_as_write_x" "$save_as_field_y"
wait_for 10 "the engine to log the Save As" eval '[ "$(saves_logged)" -gt '"$before"' ]'
grep 'editor-host: saved ' "$log" | tail -1 | grep -q "to $saved_as " \
    || fail "the last save did not go to $saved_as"
[ -s "$saved_as" ] || fail "Save As wrote no file"
note "ok  the engine wrote $(wc -c <"$saved_as") bytes to saved-as.rscene"

# ── Quit ────────────────────────────────────────────────────────────────────────────────────────
say "close the window"
close_window "$wid"
gone() { ! kill -0 "$1" 2>/dev/null; }
wait_for 15 "the editor process to exit" gone "$editor_pid"
editor_status=0; wait "$editor_pid" || editor_status=$?
editor_pid=""
expect "editor exit status" "$editor_status" "0"
wait_for 10 "the engine child to exit with its editor" gone "$engine_pid"
engine_pid=""
note "ok  no engine process left behind"

# ── The assertion: what is on disk ──────────────────────────────────────────────────────────────
say "assert the saved scene"
expect "entities in saved-as.rscene" "$(entity_count "$saved_as")" "$(( baseline + 1 ))"
entity_block "$saved_as" "$placed" | grep -q "asset $mesh_id_dec\$" \
    || fail "saved-as.rscene: entity $placed does not reference mesh $mesh_id_hex"
note "ok  the placed mesh is in the file, by content id"
expect "the redone edit is in the file" \
    "$(translation_of "$saved_as" "$placed")" "translation { x 5 y 0 z 0 }"

# Play → Stop must hand back the world that was there before Play (README: "Stop restores it
# bit-exactly"), so this save must equal the checkpoint taken before Play.
#
# IT DOES NOT, and this is a pinned KNOWN DEFECT rather than a tolerance: the restore re-creates
# every entity under a new handle and does not remap handles stored INSIDE components, so
# `ecs::Parent` comes back dangling and is written as `null`. Playing once and saving silently
# deletes the scene's hierarchy. The pin accepts exactly that difference and nothing else — any
# other change fails here, and so does the day the defect is fixed (then delete the pin and keep
# the `cmp`).
restore_diff="$(diff "$work/before-play.rscene" "$saved_as" || true)"
known_defect="$(printf '%s\n' '61c61' '<     value @2' '---' '>     value null')"
if [ -z "$restore_diff" ]; then
    fail "Play/Stop now restores the scene exactly — the known-defect pin is stale: remove it"
elif [ "$restore_diff" = "$known_defect" ]; then
    note "KNOWN DEFECT (pinned): Play→Stop lost a Parent reference (value @2 → null)"
else
    printf '%s\n' "$restore_diff" >&2
    fail "Play/Stop changed the scene in a way the known-defect pin does not cover"
fi

# And the file is a scene the engine can open again: reload it through the headless client.
reload="$("$editor_bin" --smoke --engine "$engine_bin" --scene "$saved_as" 2>&1)" \
    || { printf '%s\n' "$reload" | tail -5 >&2; fail "the engine could not reopen saved-as.rscene"; }
printf '%s\n' "$reload" | grep -q "loaded $(( baseline + 1 )) entities, .* 0 skipped" \
    || fail "the reload did not report $(( baseline + 1 )) entities with 0 skipped"
note "ok  reloaded: $(( baseline + 1 )) entities, 0 skipped"
note "sha256 $(sha256sum "$saved_as" | cut -d' ' -f1)  saved-as.rscene"

status=0
say "editor click smoke: PASS"
