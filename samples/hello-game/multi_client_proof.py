"""Gate 1 agreement: three mirrors match authority; idle/one-client controls catch false work.
Gate 2 attribution: tapes move assigned bodies; a digest-green swap catches misrouting.
Gate 3 isolation: a 3 s blackout stays within healthy envelopes; resync catches a wedged seat.
Gate 4 spawn routing: only the thrower adopts; no-throw control catches spurious outcomes.
Gate 5 churn: generation and cleanup advance; no-leave control catches false reuse.
Gate 6 negatives: full and codec-2 refusals; no-probe control catches false refusal counts.
Each gate reports measured numbers; a control that passes its challenged gate fails this proof.
"""

import json
import os
import select
import subprocess
import sys
import time


EXE = sys.argv[1]
PROBE = EXE.rsplit("/", 1)[0] + "/hello_game_net_proof"
TAPES = ("--input-tape-throw", "--input-tape-push", "--input-tape")
IMPAIR = ("delay_ms=30,seed=101", "delay_ms=60,seed=202", "delay_ms=100,loss=0.05,seed=303")
ISOLATION_TAPES = ("--input-tape-isolation",) * 3
# Player radius is 0.35 m (game.hpp). At 1.10 m centre distance, the 0.40 m
# margin also clears the crate's 0.5 m half-extent even at its closest corner.
ISOLATION_REACH = 2 * 0.35 + 0.40


def events(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith("{")]


def finished(output):
    return next(e for e in events(output) if e["event"] == "finished")


def server_event(server, name, seen):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        ready, _, _ = select.select([server.stdout], [], [], max(0, deadline - time.monotonic()))
        assert ready, f"server did not emit {name}: {seen}"
        line = server.stdout.readline()
        assert line, f"server exited before {name}: {seen}"
        item = json.loads(line)
        seen.append(item)
        if item["event"] == name:
            return item
    raise AssertionError(f"server did not emit {name}: {seen}")


def launch(ticks, cap=3, swap=False, diagnostic=False):
    args = [EXE, "dedicated", "--listen", "127.0.0.1:0", "--max-clients", str(cap),
            "--ticks", str(ticks), "--linger-ms", "1200", "--input-slack", "3"]
    if swap:
        args += ["--seat-debug-break", "swap"]
    env = os.environ.copy()
    if diagnostic:
        env["RIME_COLLISION_DIAGNOSTIC"] = str(ticks)
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                               text=True, bufsize=1)
    seen = []
    port = server_event(process, "ready", seen)["bind"].rsplit(":", 1)[1]
    return process, port, seen


def client(port, tape=None, impairment=None, frames=None):
    args = [EXE, "play", "--connect", f"127.0.0.1:{port}", "--no-gpu"]
    if tape:
        args.append(tape)
    if impairment:
        args += ["--net-impair", impairment]
    if frames:
        args += ["--frames", str(frames)]
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def collect(process, timeout=25):
    out, err = process.communicate(timeout=timeout)
    assert process.returncode == 0, (process.args, process.returncode, out, err)
    result = finished(out)
    diagnostic = [e for e in events(out) if e["event"] == "collision_diagnostic"]
    if diagnostic:
        result["collision_diagnostic"] = diagnostic[-1]
    return result


def joined(server, seen, slot, gen=0):
    item = server_event(server, "client_connected", seen)
    assert (item["seat"], item["gen"]) == (slot, gen), item


def cleanup(processes):
    for process in processes:
        if process.poll() is None:
            process.kill()
            process.communicate()


def run(tapes=TAPES, blackout=False, swap=False, ticks=440, negatives=False,
        diagnostic=False):
    start = time.monotonic()
    server, port, seen = launch(ticks, swap=swap, diagnostic=diagnostic)
    clients = []
    try:
        for seat, tape in enumerate(tapes):
            impairment = IMPAIR[seat]
            if blackout and seat == 1:
                impairment += ",blackout_ms=3000@tick=90"
            clients.append(client(port, tape, impairment))
            joined(server, seen, seat)
        if negatives:
            extra = client(port)
            clients.append(extra)
            out, err = extra.communicate(timeout=8)
            refusal = [e for e in events(out) if e["event"] == "connect_failed"]
            assert extra.returncode == 1 and any(e["reason"] == "full" for e in refusal), (out, err)
            # The refused session closes before the codec probe takes its transport slot.
            time.sleep(0.25)
            probe = subprocess.run([PROBE, "--codec2-to", port, "--no-gpu"], capture_output=True,
                                   text=True, timeout=8)
            assert probe.returncode == 0 and any(
                e.get("version_refused") for e in events(probe.stdout)), probe
        outputs = [collect(p) for p in clients[:3]]
        authority = collect(server, timeout=15)
        return authority, outputs, time.monotonic() - start
    finally:
        cleanup([*clients, server])


def agreement(authority, outputs):
    digest = authority["digest"]
    return digest != "0x0000000000000000" and all(c["digest"] == digest for c in outputs)


def poses(output):
    return {p["seat"]: p for p in output["players"]}


def displacement(output, seat):
    p = poses(output)[seat]
    return p["x"] - (-1.8 + 1.2 * seat), p["z"]


def signature(output, tapes):
    # Different tapes have different axes. These bounds are checked against one-client controls
    # below; displacement need not match exactly because seats start at different positions and
    # remote commands are sampled on separate real clocks.
    result = []
    for seat, tape in enumerate(tapes):
        dx, dz = displacement(output, seat)
        if tape == "--input-tape-push":
            result.append(abs(dx) < 0.25 and abs(dz) > 0.25)
        else:
            result.append(dx > 0.25 and abs(dz) < 0.25)
    return all(result), [tuple(round(v, 3) for v in displacement(output, i)) for i in range(3)]


def matches_single_tape(output, tapes, controls):
    # Same tape means same dominant-axis direction and a comparable travel distance. The
    # 25% envelope covers different seat origins and input arrival times, while still rejecting
    # an unmoved or wrong-axis body. The push tape's sign can flip around the shared crate.
    for seat, tape in enumerate(tapes):
        control_x, control_z = controls[TAPES.index(tape)]
        dx, dz = displacement(output, seat)
        control_major = abs(control_z) if tape == "--input-tape-push" else abs(control_x)
        actual_major = abs(dz) if tape == "--input-tape-push" else abs(dx)
        if not (0.75 * control_major <= actual_major <= 1.25 * control_major):
            return False
    return True


def print_gate(name, passed, **numbers):
    print(("PASS" if passed else "FAIL"), name,
          " ".join(f"{key}={value}" for key, value in numbers.items()), flush=True)
    assert passed, (name, numbers)


def churn_attempt():
    start = time.monotonic()
    server, port, seen = launch(500)
    clients = []
    try:
        clients.append(client(port, "--input-tape", IMPAIR[0]))
        joined(server, seen, 0)
        # The bounded thrower sends the same graceful disconnect as a closed window. The game
        # currently requests its live projectile's despawn at departure, so a normal churn run
        # expects zero late Ended reports; the unit test exercises the stale-report branch.
        # A bounded client spends only 30 ms pumping its graceful close; a 60 ms artificial
        # one-way delay would discard that packet when the process exits.
        clients.append(client(port, "--input-tape-throw", "delay_ms=0,seed=202", frames=27))
        joined(server, seen, 1)
        clients.append(client(port, "--input-tape-push", IMPAIR[2]))
        joined(server, seen, 2)
        old = collect(clients[1], 8)
        left = server_event(server, "client_left", seen)
        assert (left["seat"], left["gen"], left["reason"]) == (1, 0, "graceful"), left
        clients.append(client(port, "--input-tape", "delay_ms=60,seed=404"))
        joined(server, seen, 1, 1)
        current = [collect(clients[i]) for i in (0, 2, 3)]
        authority = collect(server, 15)
        return authority, old, current, time.monotonic() - start
    finally:
        cleanup([*clients, server])


def one_client_control(tape, impairment):
    server, port, seen = launch(440)
    sole = client(port, tape, impairment)
    try:
        joined(server, seen, 0)
        output = collect(sole)
        authority = collect(server, 15)
        assert agreement(authority, [output]), (authority["digest"], output["digest"])
        return authority, output
    finally:
        cleanup([sole, server])


def isolation_leg(blackout):
    # These are server-side minima over every fixed tick. A final pose would miss a brief
    # contact; a client mirror could miss it during the blackout. Discard a failed scenario,
    # never relax the healthy-client correction envelope below.
    for attempt in range(1, 4):
        authority, clients, runtime = run(ISOLATION_TAPES, blackout=blackout, ticks=540,
                                          diagnostic=True)
        assert agreement(authority, clients), ("isolation digest", attempt)
        diagnostic = authority["collision_diagnostic"]
        pair = diagnostic["min_player_distance"]
        crate = diagnostic["min_player_crate_distance"]
        print("PRECONDITION isolation", "blackout" if blackout else "baseline",
              "attempt", attempt, "min_pairwise_m", pair, "min_crate_center_m", crate,
              "required_m", ISOLATION_REACH, flush=True)
        if pair >= ISOLATION_REACH and crate >= ISOLATION_REACH:
            return authority, clients, runtime
    raise AssertionError("isolation players or crate came within the 0.40 m margin in three attempts")


def main():
    # The controls have their own processes, so their digest cannot be inherited from a main leg.
    idle_server, _, _ = launch(440)
    try:
        idle = collect(idle_server, 15)
    finally:
        cleanup([idle_server])
    controls = [one_client_control(tape, IMPAIR[i]) for i, tape in enumerate(TAPES)]
    one_digests = [authority["digest"] for authority, _ in controls]
    one_axes = [tuple(round(v, 3) for v in displacement(output, 0))
                for _, output in controls]
    # At a common starting seat, the solitary tapes reveal which axis each drives. Exact end
    # coordinates are not portable to three seats: each starts elsewhere, and the push tape
    # contacts the shared crate from the opposite side at seat one.
    assert all(abs(dx) > 0.25 and abs(dx) > abs(dz) for dx, dz in (one_axes[0], one_axes[2]))
    assert abs(one_axes[1][1]) > 0.25 and abs(one_axes[1][1]) > abs(one_axes[1][0])
    print("CONTROL idle_digest", idle["digest"], "one_digests", one_digests,
          "one_displacements", one_axes, flush=True)

    for index, tapes in enumerate((TAPES, (TAPES[2], TAPES[1], TAPES[0])), 1):
        authority, clients, runtime = run(tapes, negatives=index == 1)
        print_gate(f"agreement.{index}", agreement(authority, clients) and
                   authority["digest"] not in (idle["digest"], *one_digests),
                   digest=authority["digest"], applied=[s["applied"] for s in authority["seats"]],
                   runtime_s=round(runtime, 2))
        ok, positions = signature(clients[0], tapes)
        print_gate(f"attribution.{index}", ok and matches_single_tape(clients[0], tapes, one_axes)
                   and all(s["applied"] > 0 for s in authority["seats"])
                   and all(c["response_ticks"] == 0 for c in clients)
                   and all(poses(c) == poses(clients[0]) for c in clients),
                   displacement=positions, response=[c["response_ticks"] for c in clients])
        thrower = tapes.index("--input-tape-throw")
        print_gate(f"spawn.{index}", authority["spawn_accepted"] == 1 and
                   clients[thrower]["ledger_adoptions"] == 1 and
                   all(c["ledger_adoptions"] == 0 and c["spawn_unpredicted_outcomes"] == 0
                       for j, c in enumerate(clients) if j != thrower),
                   accepted=authority["spawn_accepted"],
                   adoptions=[c["ledger_adoptions"] for c in clients])
        if index == 1:
            print_gate("negatives", authority["hello_full"] >= 1 and
                       authority["hello_version"] >= 1,
                       full=authority["hello_full"], version=authority["hello_version"])
            print_gate("churn.no_leave_control", authority["seats"][1]["joins"] == 1 and
                       authority["seats"][1]["gen"] == 0,
                       joins=authority["seats"][1]["joins"], gen=authority["seats"][1]["gen"])
        else:
            print_gate("negatives.no_probe_control", authority["hello_full"] == 0 and
                       authority["hello_version"] == 0,
                       full=authority["hello_full"], version=authority["hello_version"])

    # A launch key contains its source incarnation. Keep this sensitivity control movement-only:
    # the fault swaps commands across seats, not transport identities, and spawn refusal would
    # make final digests differ for a separate reason.
    swap_tapes = ("--input-tape-push", "--input-tape", "--input-tape")
    swapped, mirrors, swap_runtime = run(swap_tapes, swap=True)
    swap_attributed, swap_positions = signature(mirrors[0], swap_tapes)
    print_gate("attribution.swap_control", agreement(swapped, mirrors) and not swap_attributed,
               digest=swapped["digest"], displacement=swap_positions,
               runtime_s=round(swap_runtime, 2))
    print_gate("spawn.no_throw_control", swapped["spawn_accepted"] == 0 and
               all(c["ledger_adoptions"] == 0 for c in mirrors),
               accepted=swapped["spawn_accepted"],
               adoptions=[c["ledger_adoptions"] for c in mirrors])

    normal, baseline, base_runtime = isolation_leg(False)
    dark, impaired, dark_runtime = isolation_leg(True)
    # Each healthy client's own no-blackout maximum is its measured envelope. A scheduling
    # allowance covers real-clock jitter; the blackout must not impose seconds of corrections.
    # The allowance is RELATIVE plus a floor, because the correction COUNT scales with the run:
    # two no-blackout runs of the same tape differ by ~20 on counts near 450 (measured 2026-11-02:
    # starbase 444 -> 467, hosted macOS 513 -> 526, every max_snap 0.0), so a fixed +12 failed on
    # scheduling noise alone. 10 % of ~450 is ~45 corrections, under one second at 60 Hz, still
    # far below the seconds of corrections a blackout leaking into healthy seats would impose, and
    # the max_snap bound is unchanged. Owner decision 2026-11-03.
    envelopes = [(baseline[i]["corrections"], baseline[i]["max_snap"]) for i in (0, 2)]
    healthy = all(impaired[i]["corrections"] <= baseline[i]["corrections"] * 1.10 + 12 and
                  impaired[i]["max_snap"] <= baseline[i]["max_snap"] + 0.15 for i in (0, 2))
    print_gate("isolation.no_blackout_control", normal["seats"][1]["resyncs"] == 0,
               resyncs=normal["seats"][1]["resyncs"])
    print_gate("isolation", agreement(dark, impaired) and healthy and dark["resyncs"] >= 1 and
               dark["seats"][1]["resyncs"] >= 1 and dark["seats"][1]["applied"] > 0,
               baseline=envelopes,
               blackout=[(impaired[i]["corrections"], impaired[i]["max_snap"]) for i in (0, 2)],
               min_pairwise_m=(normal["collision_diagnostic"]["min_player_distance"],
                               dark["collision_diagnostic"]["min_player_distance"]),
               resyncs=dark["resyncs"], runtime_s=round(base_runtime + dark_runtime, 2))

    for attempt in range(1, 4):
        churned, departed, current, runtime = churn_attempt()
        # Even a discarded precondition attempt still has to uphold final mirror agreement.
        print_gate(f"churn.agreement_attempt{attempt}", agreement(churned, current),
                   digest=churned["digest"], runtime_s=round(runtime, 2))
        if departed["presentation_launch"] == 0:
            print("PRECONDITION throw did not fire; rerun", attempt, flush=True)
            continue
        print_gate("churn", churned["seats"][1]["joins"] == 2 and
                   churned["seats"][1]["leaves"] >= 1 and
                   current[2]["seat"] == 1 and current[2]["gen"] == 1 and
                   current[2]["ledger_adoptions"] == 0 and
                   current[2]["spawn_unpredicted_outcomes"] == 0 and
                   churned["spawns_ended_after_owner_left"] == 0 and
                   churned["game_despawns"] >= 2,
                   old_launches=departed["presentation_launch"],
                   generation=current[2]["gen"],
                   late_ends=churned["spawns_ended_after_owner_left"],
                   game_despawns=churned["game_despawns"])
        break
    else:
        raise AssertionError("churn throw did not fire in three attempts")


if __name__ == "__main__":
    main()
