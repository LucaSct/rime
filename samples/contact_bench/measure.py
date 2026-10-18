#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Measure frozen contacts on one CPU in ABBA order; abort our child if competing work appears."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

HEAVY = {'ctest', 'cmake', 'ninja', 'make', 'cc1plus', 'rustc', 'cargo', 'the_block',
         'clang++', 'clang', 'lto-wrapper', 'networked_destr', 'destructible_wa'}


def snapshot():
    rows = {}
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            text = (p / 'stat').read_text()
            end = text.rindex(')')
            fields = text[end+2:].split()
            if fields[0] == 'Z':
                continue
            rows[int(p.name)] = (text[text.index('(')+1:end], int(fields[11])+int(fields[12]))
        except (OSError, ValueError, IndexError):
            continue
    return time.monotonic(), rows


def competing(previous, current, exclude):
    elapsed = current[0]-previous[0]
    result = []
    for pid, (name, ticks) in current[1].items():
        if pid in exclude:
            continue
        percent = max(0, ticks-previous[1].get(pid, (name, ticks))[1])/os.sysconf('SC_CLK_TCK')/elapsed*100
        if name in HEAVY or (name.startswith('rime_') and 'test' in name) or percent > 50:
            result.append({'pid': pid, 'name': name, 'cpu_percent': round(percent, 1)})
    return result


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    binary, corpus, out = (v.resolve() for v in (args.binary, args.input, args.out))
    out.parent.mkdir(parents=True, exist_ok=True)
    cpu = min(os.sched_getaffinity(0))
    cpuroot = Path('/sys/devices/system/cpu') / f'cpu{cpu}'
    model = next((line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                  if line.startswith('model name')), 'unknown')
    report = {'scope': 'CPU-only frozen narrowphase; whole-corpus time includes dispatch, bounds and checksum',
              'order': ['none','child','child','none'], 'qualified': False, 'cpu': cpu,
              'model': model, 'platform': platform.platform(),
              'governor': read(cpuroot / 'cpufreq/scaling_governor'),
              'l3_shared_cpus': read(cpuroot / 'cache/index3/shared_cpu_list'),
              'binary_sha256': sha(binary), 'input_sha256': sha(corpus),
              'clock_median_drift_limit': 0.05, 'foreign_cpu_limit_percent': 50,
              'runs': [], 'refusals': []}
    def save(): out.write_text(json.dumps(report, indent=2)+'\n')
    previous = snapshot()
    time.sleep(1)
    current = snapshot()
    bad = competing(previous, current, {os.getpid()})
    if bad:
        report['refusals'].append({'preflight_competition': bad}); save()
        raise SystemExit('machine is busy; no benchmark started')
    print(f'Pinned CPU {cpu}; quiet preflight passed', flush=True)
    for index, mode in enumerate(report['order']):
        stdout, stderr = out.parent / f'timing-{index}-{mode}.json', out.parent / f'timing-{index}-{mode}.log'
        start = snapshot()
        clocks = []
        with stdout.open('w') as output, stderr.open('w') as errors:
            child = subprocess.Popen([str(binary),'--input',str(corpus),'--candidate',mode,'--time'],
                stdout=output, stderr=errors, preexec_fn=lambda: os.sched_setaffinity(0,{cpu}))
            previous = start
            while child.poll() is None:
                time.sleep(0.5)
                current = snapshot()
                bad = competing(previous, current, {os.getpid(),child.pid})
                clock = read(cpuroot / 'cpufreq/scaling_cur_freq')
                if clock:
                    clocks.append(int(clock))
                previous = current
                if bad:
                    # This process is OUR benchmark child. Never terminate a competing job.
                    child.terminate(); child.wait()
                    report['refusals'].append({'run':index,'competition':bad}); save()
                    raise SystemExit('competition appeared; stopped only our benchmark')
            if child.returncode != 0:
                report['refusals'].append({'run':index,'exit':child.returncode,'log':str(stderr)}); save()
                raise SystemExit('replay failed; no timing claim')
        result = json.loads(stdout.read_text())
        result['clock_khz'] = clocks
        result['clock_median_khz'] = statistics.median(clocks) if clocks else None
        result['mode'] = mode
        report['runs'].append(result); save()
        print(mode, result['corpus_ms'], flush=True)
    identities = {(r['input_fnv1a64'],r['contact_hash'],r['rows'],r['exact_contacts'],r['speculative_contacts'])
                  for r in report['runs']}
    if len(identities) != 1:
        report['refusals'].append('input/work/contact identities differ')
    clocks = [r['clock_median_khz'] for r in report['runs']]
    if any(c is None for c in clocks) or max(clocks)/min(clocks)-1 > 0.05:
        report['refusals'].append('CPU clock medians missing or drift beyond 5%')
    report['qualified'] = not report['refusals']
    save()
    if not report['qualified']:
        raise SystemExit('recorded, but comparison refused')


if __name__ == '__main__':
    main()
