"""Two seated clients through real UDP, with attribution beyond digest agreement (POSIX)."""

import json
import select
import subprocess
import sys


EXE = sys.argv[1]
LISTEN = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1:0"
# 150, not 200: the push tape walks +z for ticks 11-90 and back -z from tick 101, so it crosses
# z = 0 again at client tick ~180. At 200 server ticks the late-joining push seat executed
# ~175-187 commands, which straddles that crossing, and hosted macOS measured z = 1.6e-7 (exactly
# 80 ticks out, 80 back) against the `abs(z) > 0.3` check. At 150 it executes ~125-137, ending
# 2.9-3.7 m out, far from the crossing on any runner. The throw tape throws on tick 12.
TICKS = 150


def event_lines(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(event for event in event_lines(output) if event["event"] == "finished")


def next_server_event(server, name):
    while True:
        ready, _, _ = select.select([server.stdout], [], [], 12)
        assert ready, f"timed out waiting for {name}"
        line = server.stdout.readline()
        assert line, f"server ended waiting for {name}: {server.stderr.read()}"
        event = json.loads(line)
        if event["event"] == name:
            return event


def client(port, tape, *extra):
    return subprocess.Popen(
        [EXE, "play", "--connect", f"127.0.0.1:{port}", "--no-gpu", tape, *extra],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )


def run(tapes):
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", LISTEN, "--max-clients", "2",
         "--ticks", str(TICKS), "--linger-ms", "1000", "--input-slack", "3"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    clients = []
    try:
        port = next_server_event(server, "ready")["bind"].rsplit(":", 1)[1]
        for tape in tapes:
            clients.append(client(port, tape))
            joined = next_server_event(server, "client_connected")
            assert joined["seat"] == len(clients) - 1, joined
        third = subprocess.run(
            [EXE, "play", "--connect", f"127.0.0.1:{port}", "--no-gpu"],
            capture_output=True, text=True, timeout=8,
        )
        assert third.returncode == 1, (third.returncode, third.stdout, third.stderr)
        assert any(e["event"] == "connect_failed" and e["reason"] == "full"
                   for e in event_lines(third.stdout)), third.stdout
        outputs = []
        for process in clients:
            out, err = process.communicate(timeout=20)
            assert process.returncode == 0, (process.returncode, out, err)
            outputs.append(finished(out))
        server_out, server_err = server.communicate(timeout=10)
        assert server.returncode == 0, (server.returncode, server_out, server_err)
        authority = finished(server_out)
        assert authority["clients_refused"] >= 1, authority
        assert authority["interaction_joins"] >= 2, authority  # m20.9b: <=4 seats predict the crate
        assert authority["interaction_contention_unsupported"] == 0, authority
        assert authority["spawn_accepted"] == 1, authority  # the x tape launches one throw
        assert authority["seats"][0]["applied"] > 0 and authority["seats"][1]["applied"] > 0, authority
        for index, outcome in enumerate(outputs):
            assert outcome["digest"] == authority["digest"], (authority, outcome)
            assert outcome["response_ticks"] == 0, outcome
            assert outcome["seat"] == index, outcome
            assert outcome["crate_member_ticks"] > 0, outcome
            assert {p["seat"] for p in outcome["players"]} == {0, 1}, outcome
            assert outcome["presentation_launch"] == (tapes[index] == "--input-tape-throw"), outcome
        # Both mirrors agree on both bodies. Digest equality alone would also pass if the server
        # drove the wrong body: compare each final displacement with the tape assigned to its
        # seat. X tape walks mainly right; push tape walks only along z in this time window.
        for seat in (0, 1):
            a = next(p for p in outputs[0]["players"] if p["seat"] == seat)
            b = next(p for p in outputs[1]["players"] if p["seat"] == seat)
            assert abs(a["x"] - b["x"]) < 0.001 and abs(a["z"] - b["z"]) < 0.001, (a, b)
            start_x = -1.8 + 1.2 * seat
            if tapes[seat] == "--input-tape-throw":
                assert a["x"] - start_x > 0.3 and abs(a["z"]) < 0.2, (seat, a)
            else:
                assert abs(a["x"] - start_x) < 0.2 and abs(a["z"]) > 0.3, (seat, a)
        print("seats", [c["seat"] for c in outputs], "applied",
              [s["applied"] for s in authority["seats"]], "response",
              [c["response_ticks"] for c in outputs], "digest", authority["digest"],
              "throws", authority["spawn_accepted"], "full", authority["clients_refused"],
              flush=True)
    finally:
        for process in [*clients, server]:
            if process.poll() is None:
                process.kill()
                process.communicate()


def churn():
    """A bounded client run uses the same graceful goodbye as closing its window."""
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", LISTEN, "--max-clients", "2",
         # 600 (10 s), not 320: the first client's graceful goodbye must reach the server before
         # its tick budget ends, and hosted macOS starts processes slowly enough that at 320 the
         # server closed first (`reason: server_closed`). Nothing below depends on the length.
         "--ticks", "600", "--linger-ms", "1000", "--input-slack", "3"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    clients = []
    try:
        port = next_server_event(server, "ready")["bind"].rsplit(":", 1)[1]
        first = client(port, "--input-tape", "--frames", "120")
        clients.append(first)
        assert next_server_event(server, "client_connected")["seat"] == 0
        other = client(port, "--input-tape-push")
        clients.append(other)
        assert next_server_event(server, "client_connected")["seat"] == 1
        early_out, early_err = first.communicate(timeout=8)
        assert first.returncode == 0, (early_out, early_err)
        left = next_server_event(server, "client_left")
        assert (left["seat"], left["gen"], left["reason"]) == (0, 0, "graceful"), left
        replacement = client(port, "--input-tape-throw")
        clients.append(replacement)
        joined = next_server_event(server, "client_connected")
        assert (joined["seat"], joined["gen"]) == (0, 1), joined
        outcomes = []
        for process in (other, replacement):
            out, err = process.communicate(timeout=20)
            assert process.returncode == 0, (process.returncode, out, err)
            outcomes.append(finished(out))
        server_out, server_err = server.communicate(timeout=10)
        assert server.returncode == 0, (server.returncode, server_out, server_err)
        authority = finished(server_out)
        assert authority["seats"][0]["joins"] == 2 and authority["seats"][0]["leaves"] >= 1, authority
        assert authority["seats"][1]["applied"] > 0, authority
        assert outcomes[0]["seat"] == 1 and outcomes[1]["seat"] == 0 and outcomes[1]["gen"] == 1, outcomes
        assert all(outcome["digest"] == authority["digest"] for outcome in outcomes), (authority, outcomes)
        print("churn seat 0 gen 0->1, seat 1 applied", authority["seats"][1]["applied"],
              "digest", authority["digest"], flush=True)
    finally:
        for process in [*clients, server]:
            if process.poll() is None:
                process.kill()
                process.communicate()


run(("--input-tape-throw", "--input-tape-push"))
run(("--input-tape-push", "--input-tape-throw"))
churn()
