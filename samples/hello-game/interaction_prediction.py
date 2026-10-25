"""Two-process interaction proof for ADR-0106 §5 (m20.6c). POSIX only until there is a child helper.

The failure this proof exists to catch is invisible to a final digest: PHYSICS STATE THE CHECKPOINT
DOES NOT CAPTURE. The client predicts a crate in a small physics world of its own, rebuilt from the
server's checkpoint and replayed. If that world keeps anything the checkpoint lacks (a warm-start
contact cache, a sleeping flag, a restore from an older state), the replay drifts from the server by
millimetres to centimetres -- and both processes still finish with EQUAL DIGESTS, because the end
state is rest and the digest hashes what the server replicates. So beside the digest gates, every
leg reads the client's per-checkpoint correction TRACE (`--interaction-trace`: one simulated
correction per checkpoint, tagged with the server tick it restored) and gates the shape of it:

  residual       every correction BEFORE the first kick, at a checkpoint where the predicted PLAYER
                 agreed with the server (<= 1 mm), is <= 1 cm. With nothing disturbing the crate the
                 prediction must agree with the server to far less than anything visible. Catches
                 hidden physics state. Checkpoints where the player itself was corrected (5 % loss
                 loses a command) are an input disagreement the physics amplifies; they are counted,
                 printed and bounded by the input bound, never dropped. The `no-rebuild` and `stale-restore` controls MUST fail it.
  disturbance    the first kick produces a correction the gate can SEE: > 10 cm under 108 ms delay
                 (2 m/s of kick velocity x the ~10 ticks the prediction runs ahead), > half of one
                 tick of kick velocity on loopback (where the prediction runs ~1 tick ahead). A gate
                 that cannot see a real disturbance would pass for the wrong reason.
  convergence    the next TWO checkpoints are back under 1 cm, and so is every later one (this
                 covers the second kick's sliding re-join): the replay now includes the server's
                 velocity, so a correction is paid once, not repeatedly.
  raw bound      no simulation jump exceeds `interaction_raw_bound(4, 12, 6, 64, 1/60)` = 6.4 m,
                 which the SERVER reports from the code's own function, so the proof and the engine
                 share one piece of arithmetic.
  presented      with no snap, one presented step is <= snap_distance * (1 - exp(-dt / tau)): the
                 smoother spreads a correction over frames instead of popping it.
  response       the first predicted touch moves the crate on screen within 1 tick. With
                 `--no-prediction`, and with the `present-mirror` control (prediction runs, the
                 mirror is shown), it takes the round trip. The control is what makes "<= 1" mean
                 prediction rather than a scene that cannot show latency.
  membership     exactly as scripted: 3 joins (the first before the client connects), 2 leaves,
                 epoch advances = joins + leaves, no join while touching, one join while the crate
                 still slides, one kick as a member and one as a non-member; the client sees every
                 roster change the server made after it connected.

THE TAPE (see game.cpp `push_tape`): start inside the group, retreat until the crate leaves, walk back
until it joins, push it, retreat, rest, retreat until it leaves, walk back; the server kicks it once
at rest while a member and once while it is not, timed so the player's approach drags the re-join
inside the ~24 ticks the kicked crate keeps sliding. Kick times are SERVER ticks, but the tape runs
in CLIENT ticks, which start `offset` server ticks later (measured: 4 loopback; 36-42 plus lost
commands under 108 ms delay). A leg whose kicks did not land as scripted (kick #1 as a member, kick
#2 as a non-member, the re-join sliding) lost the race against that offset; it is RERUN (max 3), and
a discarded attempt must still hold the invariants (equal digests, no join in contact, epochs =
roster changes).

DELAY. 108 ms rather than 100: exactly 6 ticks makes a tick of release jitter (see
throw_prediction.py). ONE_WAY = ceil(108 * 60 / 1000) = 7 ticks, ROUND_TRIP = 14. `loss=0.05` is 5 %
(the flag takes a fraction), `jitter_ms=20` as in the m20.4 proof.
"""

import json
import math
import subprocess
import sys
import time


EXE = sys.argv[1]
TICK_HZ = 60
DT = 1.0 / TICK_HZ
DELAY_MS = 108
ONE_WAY = math.ceil(DELAY_MS * TICK_HZ / 1000)   # 7
ROUND_TRIP = 2 * ONE_WAY                         # 14
SEEDS = (7, 8, 9)

# The game's constants, each with the line that owns it (samples/hello-game/game.{hpp,cpp}).
V_PUSH_MAX = 4.0        # HelloGame::kPlayerSpeed
J_MAX = 12.0            # HelloGame::kCrateKickImpulse (N*s), reported back by the server
CRATE_MASS = 6.0        # kCrateRigidBody.mass (kg)
REPLAY_TICKS = 64       # the interaction profile's replay budget
KICK_DV = J_MAX / CRATE_MASS                      # 2 m/s
RAW_BOUND = (V_PUSH_MAX + KICK_DV) * REPLAY_TICKS * DT   # 6.4 m, ADR-0106 §4
SNAP_DISTANCE, TAU = 1.0, 0.1                     # SmootherParams defaults (the leaf)
PRESENTED_STEP = SNAP_DISTANCE * (1.0 - math.exp(-DT / TAU))   # 0.1535 m per step, no snap
# Three buffered commands, as throw_prediction.py: the hosted macOS runner starves a one-command
# server buffer, and this proof is about the crate, not the jitter buffer.
INPUT_SLACK = 3
RESIDUAL = 0.01         # ADR-0106: an undisturbed correction stays under 1 cm
PLAYER_AGREES = 0.001   # the predicted player's own correction at a checkpoint: <= 1 mm = agreed
VISIBLE_IMPAIRED = 0.10 # a kick must show > 10 cm when the prediction runs ~10+ ticks ahead
VISIBLE_LOOPBACK = KICK_DV * DT / 2.0             # half of one tick of kick velocity, 16.7 mm

# Tape geometry: client tick of the stand in which kick #1 lands, and of the walk-back in which
# kick #2 lands (game.cpp: 'KICK #1 lands here' 276-315, 'KICK #2 lands' 426-520; the unkicked
# join is at ~501, and the sliding window is ~[475, 500] in loopback server ticks).
TAPE_KICK1, TAPE_KICK2, TAPE_LENGTH = 306, 485, 600
OFFSET_LOOPBACK, OFFSET_IMPAIRED = 4, 39
IMPAIR_BASE = f"delay_ms={DELAY_MS},loss=0.05,jitter_ms=20"
MAX_ATTEMPTS = 3


def budget(one_way, offset):
    """Server ticks: the offset, the whole tape, 3 one-way trips of drain, and 100 of margin."""
    return offset + TAPE_LENGTH + 3 * one_way + 100


def events(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(event for event in events(output) if event["event"] == "finished")


def run_leg_once(client_extra, impaired, kicks, tape):
    offset = OFFSET_IMPAIRED if impaired else OFFSET_LOOPBACK
    ticks = budget(ONE_WAY if impaired else 1, offset)
    k1, k2 = offset + TAPE_KICK1, offset + TAPE_KICK2
    server = subprocess.Popen(
        [EXE, "dedicated", "--listen", "127.0.0.1:0", "--ticks", str(ticks), "--linger-ms",
         "1000", "--input-slack", str(INPUT_SLACK),
         *(["--interaction-fault", f"kick@{k1},kick@{k2}"] if kicks else [])],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    client = None
    try:
        ready = json.loads(server.stdout.readline())
        assert ready["event"] == "ready", ready
        port = ready["bind"].rsplit(":", 1)[1]
        client = subprocess.Popen(
            [EXE, "play", "--connect", "127.0.0.1:" + port, "--no-gpu",
             "--input-tape-push" if tape == "push" else "--input-tape", "--interaction-trace",
             *client_extra],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        client_out, client_err = client.communicate(timeout=60)
        server_out, server_err = server.communicate(timeout=30)
        assert server.returncode == client.returncode == 0, (
            server.returncode, client.returncode, server_err[-500:], client_out[-500:],
            client_err[-500:])
        s, c = finished(server_out), finished(client_out)
        assert s["ticks"] == ticks, s
        assert s["digest"] == c["digest"] and int(c["digest"], 16) != 0, (s, c)
        return s, c, k1, k2
    finally:
        for process in (client, server):
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()


def scripted(s):
    """Did the kicks land as scripted? The PRECONDITION of every push-tape leg."""
    return (s["interaction_kicks"] == 2 and s["interaction_kicks_member"] == 1 and
            s["interaction_kicks_nonmember"] == 1 and s["interaction_joined_sliding"] == 1 and
            s["interaction_joins"] == 3 and s["interaction_leaves"] == 2)


def invariants(s):
    """Held by EVERY attempt, discarded ones included, so a rerun can never hide a defect."""
    assert s["interaction_joined_in_contact"] == 0, ("a join while touching", s)
    assert s["interaction_epoch_advances"] == s["interaction_joins"] + s["interaction_leaves"], (
        "epoch advances != roster changes", s)
    assert s["interaction_capture_refused"] == 0, ("a checkpoint was refused", s)


def run_leg(client_extra=(), impaired=False, kicks=True, tape="push"):
    for attempt in range(1, MAX_ATTEMPTS + 1):
        s, c, k1, k2 = run_leg_once(list(client_extra), impaired, kicks, tape)
        invariants(s)
        if tape != "push" or not kicks or scripted(s):
            return s, c, k1, k2
        print(f"rerun: kicks did not land as scripted (attempt {attempt}: kicks "
              f"{s['interaction_kicks_member']} member / {s['interaction_kicks_nonmember']} "
              f"non-member, sliding joins {s['interaction_joined_sliding']}, joins "
              f"{s['interaction_joins']}, leaves {s['interaction_leaves']}, synthesized "
              f"{s['synthesized']})", flush=True)
    return s, c, k1, k2


def gate(name, ok, detail):
    print(("PASS " if ok else "FAIL ") + name + ": " + detail, flush=True)
    if not ok:
        sys.exit(1)


def explained(jump, player_jump):
    """Is a crate correction accounted for? Where the PLAYER agreed (<= 1 mm), only the plain 1 cm
    bound. Where the player itself was corrected, the crate is bounded by ADR-0106 §4's raw bound
    WITHOUT the kick term: V_PUSH_MAX * REPLAY_TICKS * dt = 4.27 m.

    Why so loose: a lost or late command makes the server's player stand in place for a tick or two
    while the prediction walked (2 ticks x 4 m/s = 133 mm, observed on the player). The crate, being
    pushed, is then released early on the server and coasts, so the two crates differ by the player's
    offset PLUS the velocity difference integrated over the whole replay window (observed: crate
    255 mm and 379 mm against players of 67 mm and 133 mm). That is an INPUT disagreement amplified
    by physics, not hidden physics state; hidden state shows where the player AGREED, which is where
    the 1 cm bound stays strict, and those checkpoints are the overwhelming majority (215 of 230).
    The disagreeing ones are COUNTED and printed with their values, never dropped."""
    if player_jump <= PLAYER_AGREES:
        return jump <= RESIDUAL
    return jump <= V_PUSH_MAX * REPLAY_TICKS * DT


def residual_before(c, k1):
    """(largest correction among checkpoints that restored a server tick before kick #1 while the
    player agreed, how many such checkpoints, how many were excluded as input disagreements, and
    whether every excluded one was explained by the player's own correction)."""
    agreed, excluded, unexplained, worst = [], 0, [], 0.0
    for tick, jump, player in c["crate_trace"]:
        if tick >= k1:
            continue
        if player <= PLAYER_AGREES:
            agreed.append(jump)
        else:
            excluded += 1
            worst = max(worst, jump)
            if not explained(jump, player):
                unexplained.append((tick, round(jump * 1000, 1), round(player * 1000, 1)))
    return (max(agreed) if agreed else 0.0), len(agreed), (excluded, worst), unexplained


def kick_shape(c, k1, floor):
    """(spike, spike tick offset from kick #1, next two corrections, whether everything after the
    spike is back under bound). The spike is the first correction at/after kick #1 above `floor`."""
    trace = [(tick, jump, player) for tick, jump, player in c["crate_trace"] if tick >= k1]
    spike = next(((i, tick, jump) for i, (tick, jump, _) in enumerate(trace) if jump > floor), None)
    if spike is None:
        return None
    i, tick, jump = spike
    after = trace[i + 1:]
    later_ok = all(explained(j, p) for _, j, p in after)
    return jump, tick - k1, [j for _, j, _ in after[:2]], later_ok, [p for _, _, p in after[:2]]


def membership_ok(s, c):
    """Server and client agree on the roster history. The client connects AFTER the first join, so
    it sees one join fewer than the server made (measured: joins 3 / seen 2)."""
    return (scripted(s) and c["crate_leaves_seen"] == s["interaction_leaves"] and
            c["crate_joins_seen"] == s["interaction_joins"] - 1)


def predicted_gates(label, s, c, k1, impaired):
    """The gates of a normal predicted leg (loopback or impaired, any seed)."""
    gate(f"{label} membership", membership_ok(s, c),
         f"joins {s['interaction_joins']} (seen {c['crate_joins_seen']}), leaves "
         f"{s['interaction_leaves']} (seen {c['crate_leaves_seen']}), epoch advances "
         f"{s['interaction_epoch_advances']}, in contact {s['interaction_joined_in_contact']}, "
         f"sliding joins {s['interaction_joined_sliding']}, kicks {s['interaction_kicks_member']} "
         f"member / {s['interaction_kicks_nonmember']} non-member, quiet deferrals "
         f"{s['interaction_leave_deferred_quiet']}")
    gate(f"{label} bound arithmetic",
         abs(s["interaction_raw_bound"] - RAW_BOUND) < 1e-3 and s["interaction_j_max"] == J_MAX,
         f"server j_max {s['interaction_j_max']} = {J_MAX}; raw bound ({V_PUSH_MAX} + {J_MAX}/"
         f"{CRATE_MASS}) * {REPLAY_TICKS} * 1/60 = {RAW_BOUND:.4f}, code says "
         f"{s['interaction_raw_bound']:.4f}")
    residual, n, (excluded, worst), unexplained = residual_before(c, k1)
    ok = not unexplained
    gate(f"{label} undisturbed residual", n >= 100 and residual <= RESIDUAL and ok,
         f"{n} checkpoints before the first kick with the player agreeing, max correction "
         f"{residual * 1000:.3f} mm <= {RESIDUAL * 1000:.0f} mm; {excluded} more where the player "
         f"itself was corrected (lost or late input), each within the "
         f"{V_PUSH_MAX * REPLAY_TICKS * DT:.2f} m input bound (max {worst * 1000:.0f} mm)"
         if ok else f"{len(unexplained)} of {excluded} input-disagreement checkpoints beyond "
         f"the input bound (server tick, crate mm, player mm): {unexplained}; agreed-set max "
         f"{residual * 1000:.3f} mm over {n}")
    floor = VISIBLE_IMPAIRED if impaired else VISIBLE_LOOPBACK
    shape = kick_shape(c, k1, floor)
    gate(f"{label} kick visible and corrected once",
         shape is not None and shape[1] <= 5 and len(shape[2]) == 2 and shape[3] and
         all(explained(a, p) for a, p in zip(shape[2], shape[4])),
         "no correction above the floor" if shape is None else
         f"spike {shape[0] * 1000:.1f} mm > {floor * 1000:.1f} mm at kick+{shape[1]} ticks, next "
         f"two {[round(a * 1000, 2) for a in shape[2]]} mm, and every later correction back under "
         f"{RESIDUAL * 1000:.0f} mm (or, where the player itself was corrected, within the input bound)")
    gate(f"{label} raw jump", c["crate_max_raw_jump"] <= RAW_BOUND,
         f"max raw jump {c['crate_max_raw_jump']:.4f} m <= {RAW_BOUND:.2f} m")
    gate(f"{label} presented jumps", c["crate_snaps"] == 0 and
         c["crate_max_presented_jump"] <= PRESENTED_STEP * 1.001,
         f"snaps {c['crate_snaps']}, max presented step {c['crate_max_presented_jump']:.4f} m <= "
         f"{SNAP_DISTANCE} * (1 - exp(-1/60/{TAU})) = {PRESENTED_STEP:.4f} m")
    gate(f"{label} response", c["crate_push_tick"] > 0 and c["crate_response_ticks"] <= 1,
         f"push at client tick {c['crate_push_tick']}, crate visibly moved "
         f"{c['crate_response_ticks']} tick(s) later")


start = time.monotonic()
print(f"one-way {ONE_WAY} ticks ({DELAY_MS} ms), round trip {ROUND_TRIP}; raw bound "
      f"{RAW_BOUND:.2f} m, presented step {PRESENTED_STEP:.4f} m", flush=True)

# ── Loopback ───────────────────────────────────────────────────────────────────────────────────
s, c, k1, _ = run_leg()
_, zero, _, _ = run_leg(tape="control")
gate("loopback digest", s["digest"] == c["digest"] != zero["digest"],
     f"{s['digest']} == client, != no-push control {zero['digest']}")
predicted_gates("loopback", s, c, k1, impaired=False)
gate("loopback quiet window", s["interaction_leave_deferred_quiet"] >= 1,
     f"leave_deferred_quiet {s['interaction_leave_deferred_quiet']} >= 1: the second leave "
     f"waited out the 30-tick quiet window after kick #1, which the old counters could not see")

# ── 108 ms delay, 5 % loss, 20 ms jitter: three seeds, predicted ─────────────────────────────────
digests = set()
for seed in SEEDS:
    s, c, k1, _ = run_leg(client_extra=["--net-impair", f"{IMPAIR_BASE},seed={seed}"],
                          impaired=True)
    digests.add(s["digest"])
    predicted_gates(f"impaired seed {seed}", s, c, k1, impaired=True)
    if seed == SEEDS[0]:
        baseline_residual = max(residual_before(c, k1)[0], 1e-6)
        baseline_response = c["crate_response_ticks"]

# ── Sensitivity: each control removes one defence, and its gate must see it ───────────────────
impair = ["--net-impair", f"{IMPAIR_BASE},seed={SEEDS[0]}"]
s, off, k1, _ = run_leg(client_extra=[*impair, "--no-prediction"], impaired=True)
gate("response without prediction",
     off["crate_push_tick"] > 0 and off["crate_response_ticks"] >= ROUND_TRIP and
     membership_ok(s, off),
     f"{off['crate_response_ticks']} >= {ROUND_TRIP} (a real touch at client tick "
     f"{off['crate_push_tick']}, not 'never'); predicted: {baseline_response}")
s, mirror, k1, _ = run_leg(client_extra=[*impair, "--interaction-debug-break", "present-mirror"],
                           impaired=True)
gate("present-mirror control fails the response gate",
     mirror["crate_push_tick"] > 0 and mirror["crate_response_ticks"] >= ROUND_TRIP and
     membership_ok(s, mirror),
     f"{mirror['crate_response_ticks']} >= {ROUND_TRIP}: prediction runs, the mirror is shown")
s, norebuild, k1, _ = run_leg(
    client_extra=[*impair, "--interaction-debug-break", "no-rebuild"], impaired=True)
worst, n, _, _ = residual_before(norebuild, k1)
# The no-rebuild control is gated on its RATIO to the clean run, not on the absolute 1 cm bound. Measured
# 12.5-15.3 mm against that 10 mm bound -- only 1.25-1.5x -- so another machine's last bits could put it
# under the bound, and a control that passes its gate by luck is a CI red that proves nothing. Measured
# ~10x the clean run, which is the robust statement: the residual MEASUREMENT sees a leaked warm-start
# cache. The absolute 1 cm gate is therefore a WEAK detector of this failure (said in ADR-0106's
# addendum); the strong one is m20.6b's in-process replay-twice test (285 of 624 bytes differ without
# the rebuild), which is exact rather than statistical.
NO_REBUILD_MIN_RATIO = 4.0
gate("no-rebuild control: the residual measurement sees the leak",
     worst >= NO_REBUILD_MIN_RATIO * baseline_residual and membership_ok(s, norebuild),
     f"max pre-kick correction {worst * 1000:.2f} mm = {worst / baseline_residual:.1f}x the clean "
     f"run's {baseline_residual * 1000:.2f} mm (>= {NO_REBUILD_MIN_RATIO:.0f}x; vs the 1 cm gate: "
     f"{worst / RESIDUAL:.2f}x): the warm-start cache of a discarded timeline leaks into the replay")
s, stale, k1, _ = run_leg(
    client_extra=[*impair, "--interaction-debug-break", "stale-restore"], impaired=True)
worst, n, _, _ = residual_before(stale, k1)
gate("stale-restore control fails the residual gate", worst > RESIDUAL and membership_ok(s, stale),
     f"max pre-kick correction {worst * 1000:.2f} mm > {RESIDUAL * 1000:.0f} mm "
     f"({worst / baseline_residual:.1f}x the clean run's)")

print(f"interaction proof: {time.monotonic() - start:.1f} s", flush=True)
