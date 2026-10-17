#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarize external diagnostics, including gzip-compressed retained CSVs.

Sum matching passes within each frame BEFORE taking quantiles. Adding pass
medians would combine observations from different frames and invent a frame.
These blocking captures attribute work; they never certify a native FPS gate.
"""
import argparse
from collections import defaultdict
import csv
import gzip
import json
import math
from pathlib import Path
import statistics


def rows(directory, name):
    path = directory / name
    compressed = not path.exists()
    if compressed:
        path = directory / (name + '.gz')
    with (gzip.open(path, 'rt') if compressed else path.open()) as stream:
        return list(csv.DictReader(stream))


def distribution(values):
    ordered = sorted(values)
    return {'p50': statistics.median(ordered),
            'p99': ordered[math.ceil(.99 * len(ordered)) - 1],
            'max': ordered[-1]}


def group(name):
    view, _, label = name.partition('/')
    if label.startswith('csm-cascade'):
        label = 'csm'
    elif label.startswith('spot-shadow'):
        label = 'spot'
    elif 'terrain' in label:
        label = 'terrain'
    return view + '/' + label.split('#')[0]


def analyze(directory):
    frames = rows(directory, 'frames.csv')
    if not frames:
        raise ValueError(f'{directory}: no frames')
    ids = [int(row['frame']) for row in frames]
    if len(set(ids)) != len(ids):
        raise ValueError(f'{directory}: duplicate frames')
    result = {'frames': len(frames), 'perf_gate_pass': None, 'passes': {},
              'run_options': json.loads((directory / 'summary.json').read_text())}
    for filename, kind in [('cpu-passes.csv', 'cpu'), ('gpu-passes.csv', 'gpu')]:
        totals = defaultdict(lambda: defaultdict(float))
        draws = defaultdict(lambda: defaultdict(int))
        for row in rows(directory, filename):
            key, frame = group(row['name']), int(row['frame'])
            if frame not in ids:
                raise ValueError(f'{directory}: pass outside measured frames')
            totals[key][frame] += float(row['ms'])
            draws[key][frame] += int(row.get('mesh_indexed_draws', 0))
            if key.endswith('/csm'):
                totals['all/csm'][frame] += float(row['ms'])
                draws['all/csm'][frame] += int(row.get('mesh_indexed_draws', 0))
        result['passes'][kind] = {
            key: {'ms': distribution([values.get(frame, 0) for frame in ids]),
                  'indexed_draws': distribution([draws[key].get(frame, 0) for frame in ids])
                  if kind == 'cpu' else None}
            for key, values in totals.items()}
    # Retain the original column labels: frame_cpu_ms is elapsed wall time around
    # blocking app.step, including GPU wait. It is NOT total CPU execution time.
    metrics = [key for key in frames[0] if key.endswith('_ms') or
               key.startswith(('server_', 'client_')) or key == 'committed_ops']
    result['frame_metrics'] = {
        key: distribution([float(row[key]) for row in frames]) for key in metrics}
    result['first_frame'] = frames[0]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', nargs='+', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = {run.name: analyze(run) for run in args.runs}
    args.out.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
