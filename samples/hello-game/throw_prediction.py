"""Two-process Throw proof for ADR-0105 §5 (m20.5c2). POSIX only until there is a child helper.

The failure this proof exists to catch is invisible to a final digest: TWO IDENTITIES SURVIVING A
CONFIRMATION (a ghost projectile beside its adopted replica) or AN EFFECT PLAYED TWICE (a second
Launch when a late verdict binds the replica). Both processes can finish with equal digests while the
player saw two projectiles. So beside the digest gates, every leg gates the client's presentation
counters, which are sampled on every tick and every applied checkpoint, not at the end:

  live max      `live_presentations_max <= 1`: never two live entities for one key, on any tick.
                Catches the ghost. The `no-adopt` control MUST fail it.
  exactly once  `presentation_launch == 1` and `presentation_end` as the leg implies. Catches a
                replayed or re-confirmed effect. The `no-ledger` control MUST fail it.
  resumed       a key coming back after leaving view. Allowed only where a late verdict binds the
                replica (late-alive); anywhere else it is a guess resurrected by replay.

Legs, and what each proves:

  baseline      digests equal, nonzero, differ from a no-throw control; one accepted launch, one
                adoption, Launch 1, End 1, acks sent == applied >= 1, and the flight ran its full
                life, i.e. the tape's throw MISSES (see GEOMETRY), so the score is unchanged.
  response      under delay the predicted projectile shows <= 1 tick after the press; with
                --no-prediction, and with the authority-presentation control (prediction running,
                authority presented), >= the round trip. The control is what makes "<= 1" mean
                prediction rather than a scene that cannot show latency.
  refuse        the server refuses a launch the client allowed: Rejected, the Launch cancelled.
  lose-one      one datagram with the launch lost: redundancy recovers it, launch accepted once.
  lose-all      every copy lost until it retires synthesized: no launch, no End, no resurrection.
  duplicate     checkpoints doubled and the ack held: repeats are ignored, one projectile.
  late-alive    the verdict arrives after the guess expired, the replica still flying: it binds,
                no second Launch.
  late-ended    the verdict arrives after the server's flight ended: it stays absent, End once.

GEOMETRY. The tape moves only along x (two ticks right, one left) and presses Throw on tick 12, so
the player's z stays at kPlayerStart.z = 0 and the throw flies along +-x at z = 0. The target is at
z = -2.55 with radius 0.08: no segment of the flight comes within 2.47 m of it. A miss runs the
45-tick life, which the server reports as LIFE - 1 = 44 ticks between its launch and end reports
(the launch tick itself takes the first step); a hit would end it sooner and award the one point.

DELAY. m20.4's proof uses delay_ms=100, which is exactly 6.000 ticks at 60 Hz. The client's
ImpairedLink releases a delayed packet only at its once-per-tick pump, so a 100 ms packet leaves on
tick +6 or +7 depending on sub-millisecond work: one tick of arrival jitter, which the server's
one-command input buffer cannot absorb (measured: 4 of 6 runs under parallel load lost the launch
command, ~86 of 160 ticks synthesized). 108 ms = 6.48 ticks sits mid-tick, so every packet leaves
on +7: ONE_WAY = ceil(108 * 60 / 1000) = 7 ticks, ROUND_TRIP = 14.

TICK BUDGET per leg, in server ticks (the server starts ticking before the client connects):
  admission 6 one-way trips (measured startup: 34-36 synthesized ticks at 7-tick trips, ~5 trips)
  + the press tick (12) + the longer of the life and the fault's hold + verdict, ack and End trips
  (3 one-way) + one more LIFE of margin, so a slow verdict fails a gate rather than the run's end
  cutting it off. Loopback counts a one-way trip as one tick (one pump).
"""

import json
import math
import subprocess
import sys
import time


EXE = sys.argv[1]
TICK_HZ = 60
LIFE = 45                       # hello-game's projectile life, ticks
PRESS_TICK = 12                 # diagnostic_throw_input presses Throw on tick 12
FULL_FLIGHT = LIFE - 1          # launch-to-end report ticks for a miss
DELAY_MS = 108
ONE_WAY = math.ceil(DELAY_MS * TICK_HZ / 1000)   # 7
ROUND_TRIP = 2 * ONE_WAY                         # 14
IMPAIR = ["--net-impair", f"delay_ms={DELAY_MS},seed=3"]
# late-alive: hold the verdict a third of the life -- the replica is ~16 ticks old when bound, 29
# ticks short of its end. The client's unresolved bound is a third of that hold, so the guess
# expires ~10 ticks before the verdict can possibly land. late-ended: hold one hold past the life,
# so the server's flight (44 ticks) has ended before anything is said. duplicate: the same window
# as late-alive, short of the life, so every repeated verdict is an Accepted one.
HOLD_ALIVE = LIFE // 3                     # 15
UNRESOLVED_BOUND = HOLD_ALIVE // 3         # 5
HOLD_ENDED = LIFE + HOLD_ALIVE             # 60
DUPLICATE_WINDOW = HOLD_ALIVE              # 15


def budget(one_way, hold=0):
    return 6 * one_way + PRESS_TICK + max(LIFE, hold) + 3 * one_way + LIFE


def events(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(event for event in events(output) if event["event"] == "finished")


MAX_ATTEMPTS = 3


def run_leg(server_extra=(), client_extra=(), throw=True, one_way=1, hold=0):
    """One leg, rerun while its PRECONDITION failed: the press must have executed on the server.

    Two real-time processes under CPU load (a parallel ctest, a slow runner) can stall the client long
    enough that the server's one-command input buffer runs dry on the press tick and executes a
    synthesized stand-in -- which never launches (ADR-0103 §2, ADR-0105 §5's lose-all case). That is
    the engine behaving correctly, but the leg was meant to test something else, so it is rerun. The
    tell is the client's own `spawn_retired_without_launch`; `lose-all` expects that, and reruns instead
    when its fault never fired.
    A discarded attempt must STILL hold the invariant every leg gates -- at most one live presentation
    per key and at most one Launch -- so a rerun can never hide a ghost. Reruns are printed.
    """
    expects_lost_press = "lose-all" in server_extra
    for attempt in range(1, MAX_ATTEMPTS + 1):
        s, c = run_leg_once(server_extra, client_extra, throw, one_way, hold)
        if not throw:
            return s, c
        # lose-all's own precondition is that its fault FIRED: on a slow runner (hosted macOS) the
        # press can be synthesized before any copy arrives, leaving the fault nothing to drop
        # ("dropped 0") -- the same starvation as below, seen from the other leg.
        if expects_lost_press and s.get("spawn_fault_lose_all", 0) >= 1:
            return s, c
        if not expects_lost_press and c.get("spawn_retired_without_launch", 0) == 0:
            return s, c
        assert c["live_presentations_max"] <= 1 and c["presentation_launch"] <= 1, (
            "a discarded attempt broke the no-ghost invariant", c)
        print(f"rerun: press executed as a synthesized stand-in (attempt {attempt}, "
              f"{s.get('synthesized', '?')} synthesized ticks)", flush=True)
    return s, c


def run_leg_once(server_extra=(), client_extra=(), throw=True, one_way=1, hold=0):
    ticks = budget(one_way, hold)
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", "127.0.0.1:0", "--ticks", str(ticks), "--linger-ms",
         "1000", *server_extra],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    client = None
    try:
        ready = json.loads(server.stdout.readline())
        assert ready["event"] == "ready", ready
        port = ready["bind"].rsplit(":", 1)[1]
        client = subprocess.Popen(
            [EXE, "play", "--connect", "127.0.0.1:" + port, "--no-gpu",
             "--input-tape-throw" if throw else "--input-tape", *client_extra],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        client_out, client_err = client.communicate(timeout=30)
        server_out, server_err = server.communicate(timeout=15)
        assert server.returncode == client.returncode == 0, (
            server.returncode, client.returncode, server_err, client_out, client_err)
        s, c = finished(server_out), finished(client_out)
        assert s["ticks"] == ticks, s
        assert s["digest"] == c["digest"] and int(c["digest"], 16) != 0, (s, c)
        return s, c
    finally:
        for process in (client, server):
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()


def gate(name, ok, detail):
    print(("PASS " if ok else "FAIL ") + name + ": " + detail, flush=True)
    if not ok:
        sys.exit(1)


def presentation(c, launch=1, end=1, resumed=0):
    """The exactly-once and single-identity gates, as one boolean plus what they read."""
    ok = (c["live_presentations_max"] <= 1 and c["presentation_launch"] == launch and
          c["presentation_end"] == end and c["presentation_resumed"] == resumed and
          c["ledger_duplicate_adoptions"] == 0 and c["spawn_contradictory_outcomes"] == 0)
    return ok, (f"live_max {c['live_presentations_max']}, Launch {c['presentation_launch']}, "
                f"End {c['presentation_end']}, resumed {c['presentation_resumed']}")


def accepted_once(s):
    return (s["spawn_accepted"] == 1 and s["spawn_rejected"] == 0 and
            s["spawn_last_flight_ticks"] == FULL_FLIGHT)


start = time.monotonic()
print(f"one-way {ONE_WAY} ticks ({DELAY_MS} ms), round trip {ROUND_TRIP}; holds alive "
      f"{HOLD_ALIVE} / ended {HOLD_ENDED}, unresolved bound {UNRESOLVED_BOUND}, duplicate window "
      f"{DUPLICATE_WINDOW}", flush=True)

# ── Baseline, and the control it must differ from ───────────────────────────────────────────
s, c = run_leg()
_, zero = run_leg(throw=False)
ok, read = presentation(c)
gate("baseline digest", s["digest"] != zero["digest"],
     f"{s['digest']} == client, != no-throw {zero['digest']}")
gate("baseline verdict", accepted_once(s) and c["spawn_adopted"] == 1,
     f"accepted {s['spawn_accepted']}, adopted {c['spawn_adopted']}, flight "
     f"{s['spawn_last_flight_ticks']} == {FULL_FLIGHT} (full life: the tape's throw misses, score 0)")
gate("baseline acks", c["spawn_acks_sent"] == s["spawn_acks_applied"] >= 1,
     f"sent {c['spawn_acks_sent']} == applied {s['spawn_acks_applied']}")
gate("baseline presentation", ok, read)

# ── Launch response, with its sensitivity control ───────────────────────────────────────────
s, on = run_leg(client_extra=IMPAIR, one_way=ONE_WAY)
ok, read = presentation(on)
gate("response predicted", on["spawn_response_ticks"] <= 1 and accepted_once(s) and ok,
     f"{on['spawn_response_ticks']} <= 1 tick after the press; {read}")
s, off = run_leg(client_extra=["--no-prediction", *IMPAIR], one_way=ONE_WAY)
ok, read = presentation(off)
gate("response without prediction",
     off["spawn_response_ticks"] >= ROUND_TRIP and accepted_once(s) and ok,
     f"{off['spawn_response_ticks']} >= {ROUND_TRIP}; {read}")
s, authority = run_leg(
    client_extra=["--spawn-debug-break", "authority-presentation", *IMPAIR], one_way=ONE_WAY)
ok, read = presentation(authority)
gate("response control (authority presented, prediction running)",
     authority["spawn_response_ticks"] >= ROUND_TRIP and accepted_once(s) and ok,
     f"{authority['spawn_response_ticks']} >= {ROUND_TRIP}; {read}")

# ── The six semantic faults, injected after session decoding ───────────────────────────────
s, c = run_leg(server_extra=["--spawn-fault", "refuse"])
ok, read = presentation(c, end=0)
gate("refuse", s["spawn_fault_refuse"] == 1 and s["spawn_rejected"] == 1 and
     s["spawn_accepted"] == 0 and c["spawn_rejected_launches"] == 1 and
     c["presentation_cancelled"] == 1 and c["spawn_adopted"] == 0 and ok,
     f"fault {s['spawn_fault_refuse']}, rejected {s['spawn_rejected']}, client rejected_launches "
     f"{c['spawn_rejected_launches']}, cancelled {c['presentation_cancelled']}; {read}")

s, c = run_leg(server_extra=["--spawn-fault", "lose-one"])
ok, read = presentation(c)
gate("lose-one", s["spawn_fault_lose_one"] == 1 and s["launch_recovered_by_redundancy"] == 1 and
     accepted_once(s) and c["spawn_adopted"] == 1 and c["spawn_retired_without_launch"] == 0 and ok,
     f"dropped {s['spawn_fault_lose_one']}, recovered {s['launch_recovered_by_redundancy']}, "
     f"accepted {s['spawn_accepted']}, adopted {c['spawn_adopted']}; {read}")

s, c = run_leg(server_extra=["--spawn-fault", "lose-all"])
ok, read = presentation(c, end=0)
gate("lose-all", s["spawn_fault_lose_all"] >= 1 and s["spawn_accepted"] == 0 and
     s["spawn_rejected"] == 0 and c["spawn_retired_without_launch"] == 1 and
     c["presentation_cancelled"] == 0 and c["spawn_adopted"] == 0 and ok,
     f"dropped {s['spawn_fault_lose_all']}, server launches 0, retired_without_launch "
     f"{c['spawn_retired_without_launch']}, respawn_suppressed {c['spawn_respawn_suppressed']}; "
     f"{read}")

s, c = run_leg(server_extra=["--spawn-fault", "duplicate", "--spawn-fault-ticks",
                             str(DUPLICATE_WINDOW)], hold=DUPLICATE_WINDOW)
ok, read = presentation(c)
gate("duplicate", s["spawn_fault_duplicate"] == DUPLICATE_WINDOW and accepted_once(s) and
     c["spawn_duplicate_outcomes_ignored"] >= 1 and c["spawn_adopted"] == 1 and ok,
     f"doubled {s['spawn_fault_duplicate']}, duplicate_outcomes_ignored "
     f"{c['spawn_duplicate_outcomes_ignored']}, stale {c['spawn_stale_checkpoints']}, one "
     f"projectile (accepted {s['spawn_accepted']}); {read}")

bound = ["--spawn-unresolved-bound", str(UNRESOLVED_BOUND)]
s, c = run_leg(server_extra=["--spawn-fault", "late-alive", "--spawn-fault-ticks", str(HOLD_ALIVE)],
               client_extra=bound, hold=HOLD_ALIVE)
ok, read = presentation(c, resumed=1)
gate("late-alive", s["spawn_fault_late_alive"] == HOLD_ALIVE and accepted_once(s) and
     c["spawn_provisional_expired"] == 1 and c["spawn_confirmed_after_expiry_alive"] == 1 and ok,
     f"held {s['spawn_fault_late_alive']}, expired {c['spawn_provisional_expired']}, "
     f"confirmed_after_expiry_alive {c['spawn_confirmed_after_expiry_alive']}; {read}")

s, c = run_leg(server_extra=["--spawn-fault", "late-ended", "--spawn-fault-ticks", str(HOLD_ENDED)],
               client_extra=bound, hold=HOLD_ENDED)
ok, read = presentation(c)
gate("late-ended", s["spawn_fault_late_ended"] == HOLD_ENDED and accepted_once(s) and
     s["spawn_accepted_ended"] == 1 and c["spawn_provisional_expired"] == 1 and
     c["spawn_confirmed_after_expiry_ended"] == 1 and ok,
     f"held {s['spawn_fault_late_ended']}, accepted_ended {s['spawn_accepted_ended']}, "
     f"confirmed_after_expiry_ended {c['spawn_confirmed_after_expiry_ended']}; {read}")

# ── Sensitivity: each control removes one defence, and its gate must see it ─────────────────
s, c = run_leg(client_extra=["--spawn-debug-break", "no-ledger"])
ok, read = presentation(c)
gate("no-ledger control fails exactly-once", not ok and c["presentation_launch"] > 1 and
     accepted_once(s), read)
s, c = run_leg(client_extra=["--spawn-debug-break", "no-adopt"])
ok, read = presentation(c)
gate("no-adopt control fails single identity", not ok and c["live_presentations_max"] >= 2 and
     accepted_once(s), read)

print(f"throw proof: {time.monotonic() - start:.1f} s", flush=True)
