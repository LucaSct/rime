"""Real UDP process proof for ADR-0104 §6. POSIX only until there is a child helper."""

import json
import pathlib
import socket
import subprocess
import sys
import time


EXE = sys.argv[1]
BOUND = 4.0 * 64 / 60  # 64 replay ticks at hello-game's 4 m/s maximum speed.
MAPS_CHECKED = False


def assert_no_libvulkan(client):
    global MAPS_CHECKED
    if not sys.platform.startswith("linux"):
        if not MAPS_CHECKED:
            print("libvulkan maps check skipped: /proc/PID/maps is Linux-only", flush=True)
            MAPS_CHECKED = True
        return
    # Admission happens after the client constructs Application; inspect that live process,
    # since a finished process has no maps and would make an empty check pass by accident.
    maps = pathlib.Path(f"/proc/{client.pid}/maps").read_text()
    assert "libvulkan" not in maps.lower(), f"client {client.pid} mapped libvulkan"
    if not MAPS_CHECKED:
        print("no-gpu client libvulkan maps: absent (Linux)", flush=True)
        MAPS_CHECKED = True


def events(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(event for event in events(output) if event["event"] == "finished")


def server_start():
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", "127.0.0.1:0", "--ticks", "300",
         "--linger-ms", "1000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    ready = json.loads(server.stdout.readline())
    assert ready["event"] == "ready", ready
    return server, ready["bind"].rsplit(":", 1)[1]


def client_args(port, *extra, tape=True):
    return [EXE, "play", "--connect", "127.0.0.1:" + port,
            "--no-gpu", *(["--input-tape"] if tape else []), *extra]


def run_leg(*extra, inject=False, tape=True):
    server, port = server_start()
    prior = ""
    client = None
    try:
        if inject:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind(("127.0.0.1", 0))
                client_port = sock.getsockname()[1]
            args = client_args(port, *extra, "--client-port", str(client_port), tape=tape)
        else:
            args = client_args(port, *extra, tape=tape)
        client = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        prior = server.stdout.readline()
        assert json.loads(prior)["event"] == "client_connected", prior
        assert_no_libvulkan(client)
        if inject:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                for _ in range(4):
                    sock.sendto(b"bad", ("127.0.0.1", client_port))
                    sock.sendto(b"X" * 1400, ("127.0.0.1", client_port))
                    time.sleep(0.02)
        client_out, client_err = client.communicate(timeout=18)
        server_out, server_err = server.communicate(timeout=10)
        s, c = finished(prior + server_out), finished(client_out)
        assert server.returncode == client.returncode == 0, (server.returncode, client.returncode,
                                                        server_out, server_err, client_out,
                                                        client_err)
        assert s["digest"] == c["digest"] and int(c["digest"], 16) != 0, (s, c)
        assert c["checkpoints"] > 0, c
        return c
    finally:
        if client is not None and client.poll() is None:
            client.kill()
            client.communicate()
        if server.poll() is None:
            server.kill()
            server.communicate()


on = run_leg("--net-impair", "delay_ms=100,seed=3")
off = run_leg("--no-prediction", "--net-impair", "delay_ms=100,seed=3")
zero = run_leg(tape=False)
assert on["digest"] != zero["digest"], (on, zero)
assert on["response_ticks"] <= 1, on
assert off["response_ticks"] >= 6, off
print("response on/off:", on["response_ticks"], off["response_ticks"], flush=True)

snaps = []
for seed in (7, 8, 9):
    result = run_leg("--net-impair", f"delay_ms=50,loss=0.05,jitter_ms=20,seed={seed}",
                     "--snap-bound", str(BOUND))
    assert result["corrections"] > 0 and result["replayed"] > 0, result
    assert 0 < result["max_replay"] <= 64, result
    assert result["stale_checkpoints"] > 0, result
    assert result["snaps_over_bound"] == 0 and result["max_snap"] <= BOUND, result
    snaps.append(result["max_snap"])
    print("seed", seed, "corrections/replayed/stale:", result["corrections"],
          result["replayed"], result["stale_checkpoints"], "max_snap:", result["max_snap"],
          flush=True)
print("snap bound:", BOUND, "metres = 4 m/s * 64 ticks / 60 Hz", flush=True)

reseed = run_leg("--net-impair", "delay_ms=0,seed=11,blackout_ms=3000@tick=100")
assert reseed["reseeds"] > 0, reseed
print("reseeds:", reseed["reseeds"], flush=True)

# Each refusal owns its own process and event; a nonzero status alone could be a crash.
server, port = server_start()
try:
    mismatch = subprocess.run(client_args(port, "--profile-version-override", "2"),
                              capture_output=True, text=True, timeout=8)
    assert mismatch.returncode == 1, (mismatch.returncode, mismatch.stdout, mismatch.stderr)
    assert any(e["event"] == "connect_failed" and e["reason"] == "schema"
               for e in events(mismatch.stdout)), mismatch.stdout
    print("negative schema: exit 1, connect_failed schema", flush=True)
finally:
    server.kill()
    server.communicate()

server, port = server_start()
try:
    mismatch = subprocess.run(client_args(port, "--fixed-dt-override", "0.03333333333333333"),
                              capture_output=True, text=True, timeout=8)
    assert mismatch.returncode == 1, (mismatch.returncode, mismatch.stdout, mismatch.stderr)
    assert any(e["event"] == "connect_failed" and e["reason"] == "schema"
               for e in events(mismatch.stdout)), mismatch.stdout
    print("negative timestep: exit 1, connect_failed schema", flush=True)
finally:
    server.kill()
    server.communicate()

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.bind(("127.0.0.1", 0))
    closed_port = sock.getsockname()[1]
start = time.monotonic()
unreachable = subprocess.run(client_args(str(closed_port)), capture_output=True, text=True,
                             timeout=6)
elapsed = time.monotonic() - start
assert unreachable.returncode == 1 and elapsed < 6, (elapsed, unreachable)
assert any(e["event"] == "connect_failed" for e in events(unreachable.stdout)), unreachable.stdout
print("negative unreachable:", round(elapsed, 2), "s, exit 1, connect_failed", flush=True)

server, port = server_start()
try:
    first = subprocess.Popen(client_args(port), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True)
    admitted = json.loads(server.stdout.readline())
    assert admitted["event"] == "client_connected", admitted
    second = subprocess.run(client_args(port), capture_output=True, text=True, timeout=8)
    assert second.returncode == 1, (second.returncode, second.stdout, second.stderr)
    assert any(e["event"] == "connect_failed" and e["reason"] == "full"
               for e in events(second.stdout)), second.stdout
    first_out, first_err = first.communicate(timeout=18)
    server_out, server_err = server.communicate(timeout=10)
    assert first.returncode == server.returncode == 0, (first_out, first_err, server_out, server_err)
    assert finished(first_out)["digest"] == finished(server_out)["digest"]
    print("negative full: exit 1, connect_failed full; first unaffected", flush=True)
finally:
    if server.poll() is None:
        server.kill()
        server.communicate()

bad = run_leg(inject=True)
assert bad["malformed_datagrams"] >= 4 and bad["oversized_datagrams"] >= 4, bad
print("negative raw datagrams:", bad["malformed_datagrams"], bad["oversized_datagrams"],
      "digests equal", flush=True)
