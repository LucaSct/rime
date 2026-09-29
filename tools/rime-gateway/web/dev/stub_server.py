#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.

"""A stand-in gateway for check.sh — Python stdlib only, matching the page's own "no npm, no
build step" rule (page-spec.md). It serves `web/` at `/` and fakes just enough of contract.md's
`/api` surface for a headless browser to render the catalogue view or the sign-in view, nothing
that needs a real store, TURN server or engine process.

Usage: stub_server.py --port <port> [--require-auth]

`--require-auth` makes `GET /api/catalogue` answer `401` (the shape a real gateway gives an
anonymous request when `AccessPolicy::require_account` is set, api.rs) instead of `200` with two
games — check.sh runs this script once each way, because the DOM assertion differs per case and a
single running process cannot honestly answer both to the same unauthenticated request.
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB_ROOT = Path(__file__).resolve().parent.parent

CATALOGUE = {
    "games": [
        {"id": "hello", "title": "Hello Game", "surfaces": ["play"]},
        {"id": "block", "title": "The Block", "surfaces": ["edit", "play"]},
    ]
}


def make_handler(require_auth: bool):
    class Handler(BaseHTTPRequestHandler):
        server_version = "RimeStub/0"

        def log_message(self, fmt, *args):  # noqa: A003 - stdlib signature
            sys.stderr.write("stub: " + (fmt % args) + "\n")

        def _json(self, status: int, body: dict, headers: dict | None = None) -> None:
            payload = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(payload)

        def _static(self) -> None:
            path = self.path.split("?", 1)[0]
            if path == "/":
                path = "/index.html"
            target = (WEB_ROOT / path.lstrip("/")).resolve()
            if WEB_ROOT not in target.parents and target != WEB_ROOT:
                self.send_error(403)
                return
            if not target.is_file():
                self.send_error(404)
                return
            content_type = {
                ".html": "text/html; charset=utf-8",
                ".js": "text/javascript; charset=utf-8",
                ".css": "text/css; charset=utf-8",
            }.get(target.suffix, "application/octet-stream")
            data = target.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:  # noqa: N802 - stdlib method name
            if self.path == "/api/catalogue":
                if require_auth:
                    self._json(401, {"error": "this host requires an account"})
                else:
                    self._json(200, CATALOGUE)
                return
            if self.path.startswith("/api/sessions/") and self.path.endswith("/ice"):
                self._json(200, {"ice_servers": []})
                return
            self._static()

        def do_POST(self) -> None:  # noqa: N802 - stdlib method name
            length = int(self.headers.get("Content-Length", "0"))
            self.rfile.read(length)  # drain the body; the stub does not inspect it
            if self.path == "/api/sessions":
                self._json(
                    201,
                    {"session": "0" * 32, "surface": "play", "game": "hello"},
                    headers={"Location": "/api/sessions/" + "0" * 32},
                )
                return
            if self.path == "/api/auth/login/options":
                # A 401-free placeholder: enough shape for the sign-in view to render without
                # throwing, never asked to produce a real challenge in this harness.
                self._json(
                    200,
                    {
                        "challenge": "stub",
                        "options": {
                            "publicKey": {
                                "challenge": "c3R1Yg",
                                "rpId": "localhost",
                                "allowCredentials": [],
                                "userVerification": "required",
                            }
                        },
                    },
                )
                return
            self._json(404, {"error": "no such route"})

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--require-auth", action="store_true")
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(args.require_auth))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
