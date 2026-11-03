"""Two-process proof for ADR-0107 (m20.7c): the adaptive server input buffer absorbs a client frame
hitch WITHOUT losing the commands, and drains the surplus afterwards. Portable Python: every OS.

The failure this proof exists to catch is the one the owner felt in play and that reddened hosted
macOS and Windows runners: a client frame hitch of k ticks (a stall, then a catch-up burst) makes
k commands arrive after the fixed buffer has already retired their slots with stand-ins, so the
server throws the real commands away as late. With ADR-0103's fixed buffer that loss is permanent
whatever the slack. The adaptive buffer must instead STALL (run a neutral stand-in, retire nothing),
apply the late commands when they arrive, and then have the client dilate its own tick to drain the
extra buffering. Every leg runs a real hitch (`play --debug-hitch 240:200`, 200 ms = 12 ticks at
60 Hz) while the player keeps moving (`--input-tape`: +x, +x, -x, repeating), and gates:

  digest         client digest == server digest and nonzero, every leg. Catches a divergence the
                 wiring could introduce (a stall applied to the wrong tick, a command applied twice).
  lossless       (adaptive legs) `late_missed <= 3 * gaps_retired`: a late command whose slot ran a
                 stand-in is only legitimate when its slot was retired as a GAP (every redundant copy
                 lost), and each retired gap can have at most its 3 redundant copies arrive late. On
                 loopback both are 0. Catches the fixed buffer's behaviour sneaking back in.
  accounting     (adaptive legs) every command the client sent is accounted for: `commands_sent -
                 (accepted + gaps_retired)` lies in [0, TAIL]. A command is either accepted into the
                 server's buffer (applied now, or still buffered when the session ended) or its slot
                 was retired as a gap; a command that was received but discarded as late is neither,
                 so it shows as a positive residual. TAIL is only what the client sends after the
                 server's session closed. (The spec's `applied + gaps_retired + final_margin` is
                 printed as well; see the note at `adaptive_gates` for why it cannot be the gate.)
  absorbed       (adaptive legs) `stalls >= 1` and the server's `max_margin >= hitch_ticks - 2`: the
                 hitch really reached the server as a gap in time and the buffer really grew by
                 about the hitch, so "no loss" is not a hitch that never happened.
  drained        (adaptive legs) the client's pacer drained: `pacer_drains >= 1` and
                 `pacer_drained_buffer_ticks >= hitch_ticks - 3 - jitter_ticks`. Catches a buffer
                 that only grows (permanent input delay).
  settled        (adaptive legs, TIMING) the server's `final_margin <= drain_above + 2`. Printed as
                 REPORT, not enforced, when RIME_REALTIME_GATES=report (CI's hosted VMs). `jitter_ticks` (0 loopback, ceil(20 ms / 16.7 ms) = 2 impaired): the pacer
                 drains to the window's MINIMUM margin, and arrival jitter puts that minimum up to
                 jitter_ticks below the steady margin -- headroom the pacer keeps on purpose.
  control        (fixed leg, MUST fail the lossless claim) the same hitch with `--input-buffer fixed`
                 and `--input-slack 1` loses at least `hitch_ticks - 3` commands (`late_missed`) and
                 never receives a pacing report (`pacing_reports == 0`). A gate that cannot see the
                 loss it exists to prevent proves nothing; this is the sensitivity control.

Precondition reruns (max 3) happen only for a leg whose hitch did not happen (`hitch_ticks == 0`),
never for a failed gate.

TIMING. 108 ms one-way delay rather than 100 (exactly 6 ticks makes a tick of release jitter, see
throw_prediction.py): ONE_WAY = ceil(108 * 60 / 1000) = 7 ticks, ROUND_TRIP = 14. `loss=0.05` is 5 %
(the flag takes a fraction), `jitter_ms=20` as in the m20.4 proof.
"""

import json
import math
import os
import subprocess
import sys
import time


EXE = sys.argv[1]
TICK_HZ = 60
DT = 1.0 / TICK_HZ
DELAY_MS = 108
ONE_WAY = math.ceil(DELAY_MS * TICK_HZ / 1000)   # 7
JITTER_MS = 20
# Arrival jitter in whole ticks: how far below the steady margin a window's minimum can read.
JITTER_TICKS = {False: 0, True: math.ceil(JITTER_MS * TICK_HZ / 1000)}   # 0 / 2
ROUND_TRIP = 2 * ONE_WAY                         # 14
IMPAIR = f"delay_ms={DELAY_MS},loss=0.05,jitter_ms={JITTER_MS},seed=7"
MAX_ATTEMPTS = 3

# The hitch: at client tick HITCH_AT the client sleeps HITCH_MS. 200 ms / (1000/60) = 12 ticks.
HITCH_AT, HITCH_MS = 240, 200
HITCH_TICKS = HITCH_MS * TICK_HZ // 1000         # 12, what the client reports as hitch_ticks

# InputPacerParams defaults (engine/replication/include/rime/replication/input_pacing.hpp).
WINDOW_REPORTS, DRAIN_ABOVE, DRAIN_TICKS = 60, 1, 16
INPUT_SLACK = 1
# Reports are unreliable and 5 % are lost on the impaired leg, so a window of 60 FRESH reports
# takes 60 / 0.95 = 64 ticks there, 60 on loopback.
WINDOW_TICKS = {False: WINDOW_REPORTS, True: math.ceil(WINDOW_REPORTS / 0.95)}
# Server ticks between the server's first tick and the client's first tick (the connect round
# trips): measured 4 on loopback and 36-42 under 108 ms delay (interaction_prediction.py).
OFFSET = {False: 4, True: 39}

# The client runs until the server disconnects it (no --frames), because only then has it received
# the server's final snapshot and the two digests can be equal. So the client keeps sending for
# the server's linger and for however long the server's Disconnect takes to reach it. Commands
# arriving during the linger are accepted (the ring is 64 deep; 600 ms is 36) but never executed,
# so the unexecuted tail is `accepted - applied`, not the `final_margin` recorded at the last step.
# What is NOT accepted is what the client sent that never reached an open session: (a) commands in
# flight when the server closed it (one one-way trip), (b) commands sent while the server's
# Disconnect travelled back (another one-way trip), (c) the server's 5 close pumps of 8 ms
# (ceil(40 / 16.67) = 3 ticks), (d) the jitter on the way back (20 ms = 2 ticks) and (e) 2 ticks of
# phase between the two loops. Loopback: a one-way trip is 1 tick, no jitter: 3 + 2*1 + 2 = 7.
# Impaired: 3 + 2*7 + 2 + 2 = 21. The impaired bound (21) is wider than the hitch (12), so on that
# leg this gate is a consistency check and the lossless gate carries the claim; on loopback 7 < 12,
# so a hitch's worth of vanished commands DOES fail it.
LINGER_MS = 600
CLOSE_TICKS = math.ceil(40 / (1000 / TICK_HZ))   # 3
TAIL = {False: CLOSE_TICKS + 2 * 1 + 2, True: CLOSE_TICKS + 2 * ONE_WAY + 2 + 2}


def frames_for(impaired):
    """Client frames long enough for the drain to finish, worst case:
       HITCH_AT                the hitch happens
     + HITCH_TICKS             the catch-up burst (those ticks skip frames, so they add to ticks)
     + 2 * window              the window holding the stall is discarded (<= 1 window), then one
                               clean window must close before the pacer may drain
     + round trip              the last report reaches the client
     + (HITCH_TICKS - DRAIN_ABOVE) * DRAIN_TICKS   the episode: surplus x 16 dilated ticks
     + 1 window                one more window, so the buffer is observed back under the target
     + 40                      margin."""
    window = WINDOW_TICKS[impaired]
    rtt = ROUND_TRIP + 2 if impaired else 2
    return (HITCH_AT + HITCH_TICKS + 2 * window + rtt +
            (HITCH_TICKS - DRAIN_ABOVE) * DRAIN_TICKS + window + 40)


def server_ticks_for(impaired, frames):
    """Server ticks so the server outlives the client: its offset, every client tick (frames plus
    the hitch's skipped frames), the dilation (each dilated tick is 1/16 longer: one extra server
    tick per 16, i.e. one per surplus tick), 3 one-way trips of drain, and 100 of margin."""
    one_way = ONE_WAY if impaired else 1
    return (OFFSET[impaired] + frames + HITCH_TICKS + (HITCH_TICKS - DRAIN_ABOVE) +
            3 * one_way + 100)


def events(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(event for event in events(output) if event["event"] == "finished")


def run_leg_once(mode, impaired):
    frames = frames_for(impaired)
    ticks = server_ticks_for(impaired, frames)
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", "127.0.0.1:0", "--ticks", str(ticks), "--linger-ms",
         str(LINGER_MS), "--input-slack", str(INPUT_SLACK), "--input-buffer", mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    client = None
    try:
        ready = json.loads(server.stdout.readline())
        assert ready["event"] == "ready", ready
        port = ready["bind"].rsplit(":", 1)[1]
        client = subprocess.Popen(
            [EXE, "play", "--connect", "127.0.0.1:" + port, "--no-gpu", "--input-tape",
             "--debug-hitch", f"{HITCH_AT}:{HITCH_MS}",
             *(["--net-impair", IMPAIR] if impaired else [])],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        client_out, client_err = client.communicate(timeout=90)
        server_out, server_err = server.communicate(timeout=60)
        assert server.returncode == client.returncode == 0, (
            server.returncode, client.returncode, server_err[-500:], client_out[-500:],
            client_err[-500:])
        s, c = finished(server_out), finished(client_out)
        assert s["ticks"] == ticks, s
        return s, c, frames, ticks
    finally:
        for process in (client, server):
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()


def run_leg(mode, impaired):
    for attempt in range(1, MAX_ATTEMPTS + 1):
        s, c, frames, ticks = run_leg_once(mode, impaired)
        if c["hitch_ticks"] != 0:
            return s, c, frames, ticks
        print(f"rerun: the hitch did not happen (attempt {attempt}, hitch_ticks 0)", flush=True)
    return s, c, frames, ticks


def gate(name, ok, detail):
    print(("PASS " if ok else "FAIL ") + name + ": " + detail, flush=True)
    if not ok:
        sys.exit(1)


# Owner decision 2026-11-02: a gate that measures WALL-CLOCK settling is enforced only on a runner
# whose timing we trust. GitHub's hosted macOS/Windows VMs are shared and bursty, and on them the
# server's end-of-run margin settled at 4-9 ticks against a bound of 3 while every logic gate
# (accounting, absorption, the pacer's own drain) passed in the same run. `RIME_REALTIME_GATES=report`
# (set by CI on hosted jobs only) prints the measurement as REPORT instead of failing, so the
# number stays visible; the self-hosted runner and local runs leave it unset and enforce it.
REPORT_ONLY_TIMING = os.environ.get("RIME_REALTIME_GATES") == "report"


def timing_gate(name, ok, detail):
    if REPORT_ONLY_TIMING and not ok:
        print("REPORT " + name + " (not enforced on this runner): " + detail, flush=True)
        return
    gate(name, ok, detail)


def digest_gate(label, s, c):
    gate(f"{label} digest", s["digest"] == c["digest"] and int(c["digest"], 16) != 0,
         f"server {s['digest']} == client {c['digest']}, nonzero")


def adaptive_gates(label, s, c, impaired):
    gate(f"{label} mode", s["input_buffer"] == "adaptive" and c["pacing_reports"] > 0 and
         c["hitch_ticks"] == HITCH_TICKS,
         f"server input_buffer {s['input_buffer']}, client pacing_reports {c['pacing_reports']} "
         f"(sent {s['pacing_sent']}, refused {s['pacing_send_refused']}), hitch_ticks "
         f"{c['hitch_ticks']} = {HITCH_MS} ms / {1000 / TICK_HZ:.2f} ms rounded down = {HITCH_TICKS}")
    digest_gate(label, s, c)
    gate(f"{label} lossless", s["late_missed"] <= 3 * s["gaps_retired"],
         f"late_missed {s['late_missed']} <= 3 x gaps_retired {s['gaps_retired']} = "
         f"{3 * s['gaps_retired']} (late_unclassified {s['late_unclassified']}, late {s['late']})")
    if not impaired:
        gate(f"{label} nothing missed", s["late_missed"] == 0 and s["gaps_retired"] == 0,
             f"loopback loses nothing: late_missed {s['late_missed']} == 0, gaps_retired "
             f"{s['gaps_retired']} == 0")
    unaccounted = c["commands_sent"] - (s["accepted"] + s["gaps_retired"])
    spec_form = c["commands_sent"] - (s["applied"] + s["gaps_retired"] + s["final_margin"])
    accounting = (f"commands_sent {c['commands_sent']} - (accepted {s['accepted']} + gaps_retired "
                  f"{s['gaps_retired']}) = {unaccounted}, in [0, {TAIL[impaired]}] (the client's "
                  f"tail after the session closed); executed {s['applied']}, still buffered at the "
                  f"end {s['accepted'] - s['applied']}; spec form sent - (applied + gaps + "
                  f"final_margin {s['final_margin']}) = {spec_form}")
    # The two ends of this interval are different kinds of claim, so they are gated differently.
    # The LOWER end is logic: the server can never account for more commands than the client sent,
    # so a negative residual is a bookkeeping bug on any machine. The UPPER end (TAIL) is a
    # wall-clock budget: it counts what the client sent between the server closing and the client
    # noticing, so it grows with how slowly the two processes are scheduled at shutdown. Measured
    # 2026-11-02 on a hosted macOS VM: 27 against TAIL 21, with every other logic gate passing in the
    # same run. Owner decision 2026-11-03: the upper end is a timing gate (REPORT under
    # RIME_REALTIME_GATES=report, enforced on self-hosted and local runs), the lower end is not.
    gate(f"{label} accounting", 0 <= unaccounted, accounting)
    timing_gate(f"{label} accounting tail", unaccounted <= TAIL[impaired], accounting)
    gate(f"{label} hitch absorbed", s["stalls"] >= 1 and s["max_margin"] >= HITCH_TICKS - 2,
         f"stalls {s['stalls']} >= 1, max_margin {s['max_margin']} >= hitch_ticks "
         f"{HITCH_TICKS} - 2 = {HITCH_TICKS - 2}")
    drain_floor = HITCH_TICKS - 3 - JITTER_TICKS[impaired]
    # Two halves on purpose. "drained" is LOGIC: the pacer saw the grown buffer and dilated it
    # back down by about the hitch. "settled" is TIMING: where the server's margin happened to
    # sit when the session ended, which a descheduled VM moves -- hence timing_gate.
    gate(f"{label} drained",
         c["pacer_drains"] >= 1 and c["pacer_drained_buffer_ticks"] >= drain_floor,
         f"pacer_drains {c['pacer_drains']} >= 1, drained_buffer_ticks "
         f"{c['pacer_drained_buffer_ticks']} >= {HITCH_TICKS} - 3 - jitter "
         f"{JITTER_TICKS[impaired]} = {drain_floor}, dilated_ticks {c['pacer_dilated_ticks']} "
         f"(windows {c['pacer_windows']}, with stall {c['pacer_windows_with_stall']})")
    timing_gate(f"{label} settled", s["final_margin"] <= DRAIN_ABOVE + 2,
                f"server final_margin {s['final_margin']} <= drain_above {DRAIN_ABOVE} + 2 = "
                f"{DRAIN_ABOVE + 2} (client last_margin {c['last_margin']})")

start = time.monotonic()
print(f"one-way {ONE_WAY} ticks ({DELAY_MS} ms), round trip {ROUND_TRIP}; hitch {HITCH_MS} ms at "
      f"client tick {HITCH_AT} = {HITCH_TICKS} ticks; frames loopback {frames_for(False)}, "
      f"impaired {frames_for(True)}", flush=True)

s, c, frames, ticks = run_leg("adaptive", impaired=False)
print(f"leg 1 adaptive loopback: frames {frames}, server ticks {ticks}, client ticks {c['ticks']}",
      flush=True)
adaptive_gates("adaptive loopback", s, c, False)

s, c, frames, ticks = run_leg("adaptive", impaired=True)
print(f"leg 2 adaptive impaired: frames {frames}, server ticks {ticks}, client ticks {c['ticks']}",
      flush=True)
adaptive_gates("adaptive impaired", s, c, True)

s, c, frames, ticks = run_leg("fixed", impaired=False)
print(f"leg 3 fixed loopback (control): frames {frames}, server ticks {ticks}, client ticks "
      f"{c['ticks']}", flush=True)
digest_gate("control", s, c)
gate("control loses the hitch's commands",
     s["input_buffer"] == "fixed" and c["hitch_ticks"] == HITCH_TICKS and
     s["late_missed"] >= HITCH_TICKS - 3 and c["pacing_reports"] == 0,
     f"late_missed {s['late_missed']} >= hitch_ticks {HITCH_TICKS} - 3 = {HITCH_TICKS - 3}, "
     f"pacing_reports {c['pacing_reports']} == 0, stalls {s['stalls']} (late {s['late']}, "
     f"synthesized {s['synthesized']})")

print(f"adaptive input proof: {time.monotonic() - start:.1f} s", flush=True)
