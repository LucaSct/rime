#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.

# The gate for tools/rime-gateway/web/ (page-spec.md). Judged by EXIT STATUS, never by grepping
# its own output (CLAUDE.md's rule) — every check below sets `ok=0` on failure and the script's
# own exit code is 1 if any of them did.
#
# What it proves and what it does not: a headless Chromium `--dump-dom` against
# dev/stub_server.py's fakes shows the two entry views render without a real gateway, a real
# store or a browser UI behind them; dev/protocol_test.mjs shows the wire encoders match the Rust
# layouts by hand-derived byte vectors; `node --check` shows every module parses. None of this
# drives a real WebAuthn ceremony or a real WebRTC connection — see page-report.md for what could
# not be verified this way and why.

set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB="$DIR/.."
ok=0

# Scratch space for this run only (the .mjs syntax-check copies below, and chromium's stderr log
# on a dump-dom failure) — cleaned up on exit so a run never leaves files behind for the next one
# to trip over or for a commit to accidentally pick up.
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

pick_port() {
  python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

run_dump_dom() {
  local port="$1" require_auth="$2" needle="$3" label="$4"
  local args=(--port "$port")
  if [ "$require_auth" = "1" ]; then
    args+=(--require-auth)
  fi
  python3 "$DIR/stub_server.py" "${args[@]}" &
  local pid=$!
  # Give the stub a moment to bind before Chromium connects; check.sh has no other signal for
  # "the socket is listening" without adding a dependency to poll it with.
  sleep 0.5

  local dom
  dom=$(chromium --headless=new --no-sandbox --disable-gpu --virtual-time-budget=5000 \
    --dump-dom "http://127.0.0.1:${port}/" 2>"$SCRATCH/chromium-${port}.log")
  local status=$?

  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null

  if [ "$status" -ne 0 ]; then
    echo "FAIL - $label: chromium exited $status:"
    sed 's/^/    /' "$SCRATCH/chromium-${port}.log"
    ok=1
    return
  fi
  if echo "$dom" | grep -q -- "$needle"; then
    echo "ok - $label"
  else
    echo "FAIL - $label: DOM did not contain expected marker ($needle)"
    ok=1
  fi
}

echo "== node --check on every module =="
# `node --check foo.js` is not the strict check it looks like: Node's module-type detection for a
# bare `.js` path takes a lenient pass that does NOT reliably surface every syntax error (measured
# — a stray `const z = (;` at the end of a real file here passed `node --check` clean). The same
# content checked through a `.mjs` path uses the real ESM parser and catches it. So every module is
# copied (not symlinked — Node resolves a symlink's real path and reapplies `.js`'s lenient
# detection, measured) into a scratch `.mjs` file before checking.
while IFS= read -r -d '' file; do
  name="$(basename "$file")"
  copy="$SCRATCH/${name%.js}.mjs"
  cp "$file" "$copy"
  if node --check "$copy"; then
    echo "ok - node --check $name"
  else
    echo "FAIL - node --check $name"
    ok=1
  fi
done < <(find "$WEB" -maxdepth 1 -name '*.js' -print0)

echo "== protocol_test.mjs =="
if node "$DIR/protocol_test.mjs"; then
  echo "ok - protocol_test.mjs"
else
  echo "FAIL - protocol_test.mjs"
  ok=1
fi

echo "== dump-dom: catalogue requires an account -> the sign-in form renders =="
PORT1=$(pick_port)
run_dump_dom "$PORT1" 1 'aria-label="Sign in"' "401 catalogue shows the sign-in form"

echo "== dump-dom: catalogue is open -> both games are listed =="
PORT2=$(pick_port)
run_dump_dom "$PORT2" 0 "Hello Game" "200 catalogue lists the first game"
PORT3=$(pick_port)
run_dump_dom "$PORT3" 0 "The Block" "200 catalogue lists the second game"

exit "$ok"
