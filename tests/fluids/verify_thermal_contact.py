#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Offline exact ledger/bracket diagnostic against the actual fluids archive (POSIX)."""
import argparse
from collections import Counter
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess

if not __debug__:
    raise SystemExit("Verification requires assertions: do not use Python -O/PYTHONOPTIMIZE")

PROBE = r'''
#include "rime/fluids/thermal_contact.hpp"
#include <bit>
#include <cstdint>
#include <iostream>
using namespace rime::fluids;
double read() { std::uint64_t u; std::cin >> u; return std::bit_cast<double>(u); }
void emit(double d) { std::cout << ' ' << std::bit_cast<std::uint64_t>(d); }
void state(const ThermalSample& s) {
    emit(s.energy_joules()); emit(s.state().temperature_kelvin); emit(s.state().liquid_mass_fraction);
}
void receipt(const HeatExchange& r) {
    std::cout << ' ' << static_cast<int>(r.status);
    emit(r.requested_joules); emit(r.applied_joules); emit(r.applied_error_joules); emit(r.roundoff_joules);
}
int main() {
    unsigned count; std::cin >> count;
    for (unsigned i=0; i<count; ++i) {
        double p[20]; for (auto& d:p) d=read();
        PhaseChangeConfig ca{p[8],p[9],p[10],p[11],p[12],p[13]};
        PhaseChangeConfig cb{p[14],p[15],p[16],p[17],p[18],p[19]};
        auto a=ThermalSample::create(p[0],{p[1],p[2]},ca);
        auto b=ThermalSample::create(p[3],{p[4],p[5]},cb);
        if (!a || !b) { std::cout << "create-refused\n"; continue; }
        const auto before_a=*a, before_b=*b;
        const auto r=exchange_heat(*a,*b,p[6],p[7]);
        std::cout << static_cast<int>(r.status) << ' ' << r.iterations;
        state(before_a); state(before_b); state(*a); state(*b);
        receipt(r.first); receipt(r.second); emit(r.lower_joules); emit(r.upper_joules);
        auto upper_a=before_a, upper_b=before_b;
        const bool first_hot=before_a.state().temperature_kelvin>before_b.state().temperature_kelvin;
        const auto ar=upper_a.apply_heat(first_hot ? -r.upper_joules : r.upper_joules);
        const auto br=upper_b.apply_heat(first_hot ? r.upper_joules : -r.upper_joules);
        std::cout << ' ' << static_cast<int>(ar.status) << ' ' << static_cast<int>(br.status);
        emit(upper_a.state().temperature_kelvin); emit(upper_b.state().temperature_kelvin);
        std::cout << '\n';
    }
    return std::cin.fail() ? 2 : 0;
}
'''


def bits(d):
    return struct.unpack('Q', struct.pack('d', d))[0]


def double(u):
    return struct.unpack('d', struct.pack('Q', int(u)))[0]


def cases():
    config = [273.15, 2052., 4179., 333500., 253.15, 313.15]
    rng = random.Random(910092)
    result = []
    for i in range(1200):
        samples = []
        for _ in range(2):
            mass = 10. ** rng.uniform(-6., 6.)
            phase = rng.randrange(3)
            temperature = 273.15 + (rng.uniform(-19., -.1) if phase == 0 else
                                    rng.uniform(.1, 39.) if phase == 2 else 0.)
            fraction = 0. if phase == 0 else 1. if phase == 2 else rng.random()
            samples += [mass, temperature, fraction]
        g = min(samples[0], samples[3]) * 2052. * 10. ** rng.uniform(-5., 9.)
        if i % 40 == 0:
            g = 0.
        dt = 0. if i % 41 == 0 else 1.
        result.append(samples + [g, dt] + config + config)
    for g, dt in [(1e-300, 1.), (1e-300, 1e-300), (1e308, 1e308), (1e100, 1.),
                  (-1., 1.), (1., -1.), (float('inf'), 1.), (1., float('nan'))]:
        result.append([1., 283.15, 1., 1., 263.15, 0., g, dt] + config + config)
    result.append([1e100, 283.15, 1., 1., 263.15, 0., 1., 1.] + config + config)
    narrow = config.copy()
    narrow[-1] = 274.15
    result.append([1., 303.15, 1., 1., 273.15, 1., 1e6, 1.] + config + narrow)
    other = [280., 2000., 4000., 300000., 250., 320.]
    result.append([1., 280., .5, 1., 273.15, .5, 10., 1.] + other + config)
    # Binary-exact closed-form and unequal-mass cases complement the unit tests.
    binary = [256., 2., 4., 16., 128., 512.]
    for mass in [1e-6, 1., 3., 1e6]:
        result.append([1., 288., 1., mass, 272., 1., 2., 1.] + binary + binary)
    for exponent in [-1000, -1020, -1040, -1060, -1070]:
        mass = math.ldexp(1., exponent)
        for g in [2. * mass, 1.3 * mass]:
            result.append([mass, 288., 1., mass, 272., 1., g, 1.] + binary + binary)
    return result


def verify(inputs, output):
    counts = Counter()
    residual_witnesses = []
    rows = output.splitlines()
    assert len(rows) == len(inputs)
    for index, (p, row) in enumerate(zip(inputs, rows)):
        assert row != 'create-refused', (index, 'unexpected construction refusal')
        tokens = row.split()
        status, iterations = map(int, tokens[:2])
        assert len(tokens) == 30, (index, len(tokens))
        values = [double(v) for v in tokens[2:14]]
        before_a, before_b, after_a, after_b = [values[i:i+3] for i in range(0, 12, 3)]
        ar_status, br_status = int(tokens[14]), int(tokens[19])
        ar = [double(v) for v in tokens[15:19]]
        br = [double(v) for v in tokens[20:24]]
        lower, upper = map(double, tokens[24:26])
        assert all(math.isfinite(v) for v in values + ar + br + [lower, upper])
        assert 0 <= iterations <= 128
        counts[str(status)] += 1
        if status == 0:
            assert p[6] > 0. and p[7] > 0. and before_a[1] != before_b[1]
            assert ar_status == br_status == 0
            assert ar[0] == -br[0] and abs(ar[0]) == lower > 0.
            assert upper >= lower
            assert Fraction(upper) - Fraction(lower) <= Fraction(upper) * Fraction(2.**-36)
            for before, after, receipt in [(before_a, after_a, ar), (before_b, after_b, br)]:
                requested, hi, lo, residual = map(Fraction, receipt)
                delta = Fraction(after[0]) - Fraction(before[0])
                assert hi + lo == delta, (index, 'stored delta')
                assert requested == delta + residual, (index, 'single ledger')
            delta_total = (Fraction(after_a[0]) - Fraction(before_a[0]) +
                           Fraction(after_b[0]) - Fraction(before_b[0]))
            assert delta_total + Fraction(ar[3]) + Fraction(br[3]) == 0, (index, 'pair ledger')
            if delta_total:
                residual_witnesses.append(index)
            first_hot = before_a[1] > before_b[1]
            hot_before, cold_before = (before_a, before_b) if first_hot else (before_b, before_a)
            hot_after, cold_after = (after_a, after_b) if first_hot else (after_b, after_a)
            assert hot_before[1] >= hot_after[1] >= cold_after[1] >= cold_before[1]
            assert (after_a[0] < before_a[0]) == first_hot
            assert (after_b[0] > before_b[0]) == first_hot
            initial_gap = hot_before[1] - cold_before[1]
            explicit = (p[6] * p[7]) * initial_gap
            assert lower / explicit <= (hot_after[1] - cold_after[1]) / initial_gap
            assert int(tokens[26]) == int(tokens[27]) == 0, (index, 'upper must be admissible')
            ua, ub = map(double, tokens[28:30])
            upper_gap = ua - ub if first_hot else ub - ua
            assert upper / explicit >= upper_gap / initial_gap, (index, 'upper sign certificate')
            for after, c in [(after_a, p[8:14]), (after_b, p[14:20])]:
                assert c[4] <= after[1] <= c[5]
                assert 0. <= after[2] <= 1.
        else:
            if status == 1:
                assert p[6] == 0. or p[7] == 0. or before_a[1] == before_b[1]
                assert iterations == 0
            assert all(bits(a) == bits(b) for a, b in zip(before_a + before_b, after_a + after_b))
            assert ar == br == [0.] * 4 and lower == upper == 0.
            assert ar_status == br_status == (0 if status == 1 else 1)
    assert counts['0'] > 500 and counts['1'] > 50
    assert counts['2'] and counts['3'] and counts['4'] and counts['6']
    assert residual_witnesses, 'Need a witness disproving exact stored-energy conservation'
    return {'cases': len(inputs), 'status_counts': dict(counts),
            'nonzero_stored_energy_residuals': len(residual_witnesses),
            'first_residual_witness_case': residual_witnesses[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.work_dir.mkdir(parents=True, exist_ok=True)
    report = args.work_dir / 'thermal-contact-report.json'
    report.unlink(missing_ok=True)
    source, binary = args.work_dir / 'contact-probe.cpp', args.work_dir / 'contact-probe'
    source.write_text(PROBE)
    subprocess.run([args.compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'engine/fluids/include'), str(source),
                    str(args.library.resolve()), '-o', str(binary)], check=True)
    inputs = cases()
    data = str(len(inputs)) + '\n' + '\n'.join(' '.join(str(bits(d)) for d in p) for p in inputs) + '\n'
    completed = subprocess.run([str(binary)], input=data, text=True, capture_output=True, check=True)
    (args.work_dir / 'inputs.txt').write_text(data)
    (args.work_dir / 'output.txt').write_text(completed.stdout)
    summary = verify(inputs, completed.stdout)
    summary['input_sha256'] = hashlib.sha256(data.encode()).hexdigest()
    summary['output_sha256'] = hashlib.sha256(completed.stdout.encode()).hexdigest()
    summary['compiler'] = args.compiler
    summary['library_sha256'] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    report.write_text(json.dumps(summary, indent=2) + '\n')
    print(report.read_text(), end='')


if __name__ == '__main__':
    main()
