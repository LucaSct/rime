#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Validate and summarize an external native play capture CSV."""
import argparse
from collections import Counter
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics

PHASES = ('warmup', 'hip', 'settle_ads', 'ads', 'settle_turn', 'turning', 'finish')
FIELDS = ('dt_ms', 'pump_ms', 'sim_ms', 'acquire_ms', 'declare_ms', 'execute_ms', 'present_ms',
          'accounted_ms', 'residual_ms')
ZONES = ('pump_ms', 'sim_ms', 'acquire_ms', 'declare_ms', 'execute_ms', 'present_ms')
REQUIRED = ('frame', 'interval_phase', 'render_phase', 'dt_ms', 'ticks', 'width', 'height',
            'native_width', 'native_height', 'resize_events_total', 'resize_width',
            'resize_height', 'aim', 'yaw', 'pitch', 'scope_feed', 'terrain',
            'declared_passes', 'shots', 'charge', *ZONES)


def finite(row, field, positive=False):
    value = float(row[field])
    if not math.isfinite(value) or (positive and value <= 0):
        raise ValueError(f'frame {row["frame"]}: invalid {field}: {row[field]}')
    return value


def phase(frame):
    return ('warmup' if frame < 60 else 'hip' if frame < 660 else
            'settle_ads' if frame < 720 else 'ads' if frame < 1320 else
            'settle_turn' if frame < 1380 else 'turning' if frame < 1980 else 'finish')


def stats(rows):
    out = {'n': len(rows)}
    if not rows:
        return out
    for field in FIELDS:
        values = sorted(row[field] for row in rows)
        out[field] = {'mean': statistics.mean(values), 'p50': statistics.median(values),
                      'p95': values[math.ceil(.95 * len(values)) - 1],
                      'p99': values[math.ceil(.99 * len(values)) - 1], 'max': values[-1]}
    out['over16_7'] = sum(row['dt_ms'] > 16.7 for row in rows)
    out['over33_3'] = sum(row['dt_ms'] > 33.3 for row in rows)
    out['extent_histogram'] = dict(Counter(f'{r["width"]}x{r["height"]}' for r in rows))
    out['ticks_histogram'] = dict(Counter(r['ticks'] for r in rows))
    return out


def analyze(path):
    opener = gzip.open if path.suffix == '.gz' else open
    with opener(path, 'rt', newline='') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None or set(REQUIRED) - set(reader.fieldnames):
            raise ValueError('capture CSV missing required columns')
        raw = list(reader)
    if len(raw) != 1981:
        raise ValueError(f'expected 1981 complete frames, found {len(raw)}')
    for i, row in enumerate(raw):
        if None in row or None in row.values():
            raise ValueError(f'frame {i}: truncated or extra CSV columns')
        if int(row['frame']) != i:
            raise ValueError(f'frame IDs must be contiguous 0..1980; row {i} has {row["frame"]}')
        if row['render_phase'] != phase(i) or row['interval_phase'] != phase(i-1 if i else 0):
            raise ValueError(f'frame {i}: unexpected phase alignment')
        for field in ('dt_ms', 'aim', 'yaw', 'pitch', *ZONES):
            finite(row, field, positive=field == 'dt_ms' and i > 0)
        if any(finite(row, field) < 0 for field in ZONES):
            raise ValueError(f'frame {i}: negative zone duration')
        for field in ('ticks', 'width', 'height', 'native_width', 'native_height',
                      'resize_events_total', 'resize_width', 'resize_height', 'scope_feed',
                      'terrain', 'declared_passes', 'shots', 'charge'):
            if int(row[field]) < 0:
                raise ValueError(f'frame {i}: negative {field}')
        if int(row['width']) <= 0 or int(row['height']) <= 0 or int(row['native_width']) <= 0 or int(row['native_height']) <= 0:
            raise ValueError(f'frame {i}: zero extent')
    extents = {(r['width'], r['height'], r['native_width'], r['native_height']) for r in raw}
    if len(extents) != 1:
        raise ValueError(f'extent changed during capture: {sorted(extents)}')
    joined = []
    for i in range(1, len(raw)):
        current, previous = raw[i], raw[i-1]
        row = {'frame': i, 'phase': previous['render_phase'], 'dt_ms': finite(current, 'dt_ms', True),
               'pump_ms': finite(current, 'pump_ms'), 'width': int(previous['width']),
               'height': int(previous['height']), 'ticks': int(previous['ticks']),
               'scope_feed': int(previous['scope_feed'])}
        for field in ZONES[1:]:
            row[field] = finite(previous, field)
        row['accounted_ms'] = sum(row[field] for field in ZONES)
        row['residual_ms'] = row['dt_ms'] - row['accounted_ms']
        joined.append(row)
    grouped = {p: [r for r in joined if r['phase'] == p] for p in PHASES[:-1]}
    for p in ('hip', 'ads', 'turning'):
        if len(grouped[p]) < 600:
            raise ValueError(f'{p} has {len(grouped[p])} intervals; requires at least 600')
    steady = [r for r in joined if r['phase'] in ('hip', 'ads', 'turning')]
    summary = {'status': 'descriptive_only', 'baseline_proven': False, 'perf_gate_pass': None,
               'recorded': len(raw), 'joined': len(joined), 'contiguous_ids': True,
               'extent': {'width': int(raw[0]['width']), 'height': int(raw[0]['height']),
                          'native_width': int(raw[0]['native_width']),
                          'native_height': int(raw[0]['native_height'])},
               'phases': {p: stats(grouped[p]) for p in grouped},
               'shots': max(int(r['shots']) for r in raw),
               'charge': any(int(r['charge']) for r in raw),
               'terrain_range': [min(int(r['terrain']) for r in raw), max(int(r['terrain']) for r in raw)],
               'resize_events_total': int(raw[-1]['resize_events_total']),
               'scope_feed_by_phase': {p: dict(Counter(r['scope_feed'] for r in grouped[p]))
                                       for p in ('hip', 'ads', 'turning')},
               'steady_long': stats([r for r in steady if r['dt_ms'] > 33.3]),
               'steady_short': stats([r for r in steady if r['dt_ms'] < 20]),
               'worst': sorted(joined, key=lambda r: r['dt_ms'], reverse=True)[:10],
               'scope_transitions': []}
    by_frame = {r['frame']: r for r in joined}
    for i in range(1, len(raw)):
        if raw[i]['scope_feed'] != raw[i-1]['scope_feed']:
            summary['scope_transitions'].append({'render_frame': i, 'phase': raw[i]['render_phase'],
                'feed': bool(int(raw[i]['scope_feed'])), 'aim': finite(raw[i], 'aim'),
                'following_interval': by_frame.get(i+1)})
    return summary, joined


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, help='source checkout used by prepare.py')
    ap.add_argument('--build-dir', type=Path, help='build directory used by prepare.py')
    ap.add_argument('--out-dir', type=Path, help='capture output directory')
    ap.add_argument('--csv', type=Path, help='input CSV; defaults to OUT-DIR/capture.frames.csv')
    ap.add_argument('--csv-archive', type=Path, help='validate a retained CSV or CSV.GZ without the original executable; print descriptive statistics')
    args = ap.parse_args()
    if args.csv_archive:
        archive = args.csv_archive.expanduser().resolve()
        summary, _ = analyze(archive)
        summary['archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
        summary['binary_verified'] = False
        print(json.dumps(summary, indent=2))
        return
    if not all((args.root, args.build_dir, args.out_dir)):
        ap.error('--root, --build-dir and --out-dir are required for a live capture')
    root, build, out = (p.expanduser().resolve() for p in (args.root, args.build_dir, args.out_dir))
    csv_path = args.csv.expanduser().resolve() if args.csv else out / 'capture.frames.csv'
    provenance = json.loads((out / 'provenance.json').read_text())
    if provenance['source_root'] != str(root) or provenance['build_dir'] != str(build):
        ap.error('root/build-dir differ from prepare provenance')
    summary, joined = analyze(csv_path)
    binary = out / 'capture_play'
    if not provenance.get('binary_sha256') or hashlib.sha256(binary.read_bytes()).hexdigest() != provenance['binary_sha256']:
        ap.error('capture binary missing or changed since compilation')
    summary['csv_sha256'] = hashlib.sha256(csv_path.read_bytes()).hexdigest()
    summary['provenance'] = provenance
    summary['csv_path'] = str(csv_path)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'capture.summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    with (out / 'capture.intervals.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(joined[0]))
        writer.writeheader()
        writer.writerows(joined)
    print(out / 'capture.summary.json')


if __name__ == '__main__':
    main()
