"""Two UDP processes prove the opt-in round loop and its unchanged first-win control."""

import json
import re
import select
import subprocess
import sys


EXE = sys.argv[1]


def events(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(event for event in events(output) if event["event"] == "finished")


# Measure the game rule's first win rather than pinning a guessed route duration. A round
# started from the final marker can take longer, so the bounded live leg allows three full
# first-win routes plus both 180-tick displays and one second of network margin.
probe = subprocess.run(
    [EXE, "dedicated", "--offline", "--ticks", "2000", "--autopilot"],
    capture_output=True, text=True, timeout=15,
)
assert probe.returncode == 0, (probe.stdout, probe.stderr)
match = re.search(r"dedicated — (\d+) ticks, .* finished", probe.stdout)
assert match, probe.stdout
first_win = int(match.group(1))
ticks = 3 * first_win + 2 * 180 + 60


def run(endless):
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", "127.0.0.1:0", "--ticks", str(ticks),
         "--linger-ms", "1000", *(["--endless"] if endless else [])],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    client = None
    try:
        ready, _, _ = select.select([server.stdout], [], [], 12)
        assert ready, "server did not announce its listener"
        announcement = json.loads(server.stdout.readline())
        assert announcement["event"] == "ready", announcement
        port = announcement["bind"].rsplit(":", 1)[1]
        client = subprocess.Popen(
            [EXE, "play", "--connect", f"127.0.0.1:{port}", "--no-gpu", "--autopilot"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        client_out, client_err = client.communicate(timeout=ticks / 60 + 15)
        server_out, server_err = server.communicate(timeout=10)
        assert server.returncode == 0, (server.returncode, server_out, server_err)
        assert client.returncode == 0, (client.returncode, client_out, client_err)
        authority = finished(server_out)
        mirror = finished(client_out)
        assert authority["replication_orphans"] == 0, authority
        assert authority["digest"] == mirror["digest"], (authority, mirror)
        if endless:
            assert authority["rounds_completed"] >= 2, authority
            assert authority["ticks"] == ticks, authority
            # hello-game counts live sensor entities in the mirrored world after each
            # observed score reset; each event records a distinct completed round.
            sightings = [event for event in events(client_out)
                         if event["event"] == "round_pickups_visible"]
            assert [event["round"] for event in sightings[:2]] == [1, 2], sightings
            assert all(event["pickups"] == 5 and event["score"] == 0
                       for event in sightings), sightings
        else:
            assert authority["rounds_completed"] == 0, authority
            assert authority["ticks"] < ticks, authority
        return authority, mirror
    finally:
        for process in (client, server):
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()


control, _ = run(False)
endless, client = run(True)
print("first win", first_win, "ticks; control exit", control["ticks"],
      "; endless", endless["rounds_completed"], "rounds in", endless["ticks"],
      "ticks; mirrored five pickups after each restart; digest", client["digest"], flush=True)
