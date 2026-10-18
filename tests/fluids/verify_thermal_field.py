#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Offline exact thermal-field journal/rollback and tick-allocation diagnostic (POSIX)."""
import argparse
from collections import Counter
from fractions import Fraction as F
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess

if not __debug__:
    raise SystemExit('Verification requires assertions; do not use Python -O/PYTHONOPTIMIZE')

PROBE = r'''// SPDX-License-Identifier: Apache-2.0
#include <rime/fluids/thermal_field.hpp>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>
static bool watching=false;
static std::size_t allocations=0;
void* operator new(std::size_t n) {
    if(watching) ++allocations;
    if(void* p=std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n,std::align_val_t alignment) {
    if(watching) ++allocations;
    void* p=nullptr;
    if(posix_memalign(&p,static_cast<std::size_t>(alignment),n ? n : 1)!=0) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p,std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p,std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { std::free(p); }
using namespace rime::fluids;
double read() { std::uint64_t u; std::cin>>u; return std::bit_cast<double>(u); }
void emit(double d) { std::cout<<' '<<std::bit_cast<std::uint64_t>(d); }
void receipt(const HeatExchange& r) {
    emit(r.requested_joules); emit(r.applied_joules); emit(r.applied_error_joules); emit(r.roundoff_joules);
}
int main() {
    unsigned cases; std::cin>>cases;
    for(unsigned test=0;test<cases;++test) {
        std::size_t nc,nl,ns,nt;std::cin>>nc>>nl>>ns>>nt;
        ThermalFieldConfig config;config.max_substep_seconds=read();
        std::cin>>config.max_substeps>>config.receipt_budget>>config.contact_trial_budget;
        std::vector<ThermalSample> cells;
        for(std::size_t i=0;i<nc;++i) {
            double p[9];for(auto& d:p)d=read();
            auto c=ThermalSample::create(p[0],{p[1],p[2]},{p[3],p[4],p[5],p[6],p[7],p[8]});
            if(!c) return 2;
            cells.push_back(*c);
        }
        std::vector<ThermalLink> links(nl);
        for(auto& l:links) {std::cin>>l.first>>l.second;l.conductance_w_k=read();}
        std::vector<ThermalPowerSource> sources(ns);
        for(auto& s:sources) {std::cin>>s.cell;s.power_watts=read();}
        std::vector<double> steps(nt);for(auto& h:steps)h=read();
        auto f=ThermalField::create(cells,links,sources,config);if(!f)return 3;
        std::cout<<"I";for(const auto& c:f->cells()) {emit(c.energy_joules());emit(c.state().temperature_kelvin);emit(c.state().liquid_mass_fraction);}std::cout<<'\n';
        for(double h:steps) {
            allocations=0;watching=true;const auto r=f->advance(h);watching=false;
            const auto& s=f->last_step();
            std::cout<<"R "<<static_cast<int>(r.status)<<' '<<f->tick()<<' '<<allocations<<' '
                <<r.attempted_operations<<' '<<r.failed_substep<<' '<<r.failed_descriptor<<' '
                <<static_cast<int>(r.heat_status)<<' '<<static_cast<int>(r.contact_status)<<' '<<s.substeps;
            emit(s.substep_dt_seconds);emit(s.requested_dt_seconds);
            std::cout<<' '<<s.exchanged_contacts<<' '<<s.unchanged_contacts<<' '<<f->journal().size()<<'\n';
            std::cout<<"C";for(const auto& c:f->cells()) {emit(c.energy_joules());emit(c.state().temperature_kelvin);emit(c.state().liquid_mass_fraction);}std::cout<<'\n';
            for(const auto& op:f->journal()) {
                std::cout<<"O "<<static_cast<int>(op.kind)<<' '<<op.descriptor<<' '<<op.substep<<' '
                    <<static_cast<int>(op.first.status)<<' '<<static_cast<int>(op.second.status);
                emit(op.first_before_joules);emit(op.first_after_joules);emit(op.second_before_joules);emit(op.second_after_joules);
                receipt(op.first);receipt(op.second);std::cout<<'\n';
            }
        }
    }
    return std::cin.fail() ? 4 : 0;
}
'''

BINARY = [256., 2., 4., 16., 128., 512.]
WATER = [273.15, 2052., 4179., 333500., 253.15, 313.15]


def bits(d):
    return struct.unpack('Q', struct.pack('d', d))[0]


def number(s):
    return struct.unpack('d', struct.pack('Q', int(s)))[0]


def cell(m, t, fraction=1., config=BINARY):
    return [m, t, fraction] + config


def cases():
    out = []
    rng = random.Random(930093)
    for _ in range(30):
        n = rng.randrange(2, 9)
        cells = [cell(10. ** rng.uniform(-1, 1), rng.uniform(260., 310.)) for _ in range(n)]
        links = [(i, i + 1, rng.uniform(.1, 5.)) for i in range(n - 1)]
        sources = [(i, rng.uniform(-1., 1.)) for i in range(n) if rng.random() < .3]
        rng.shuffle(links);rng.shuffle(sources)
        out.append((cells, links, sources, [.025, 32, 4096, 65536], [.11] * 4 + [0., -1., .025]))
    out += [
        ([cell(1., 288.), cell(1., 511.)], [], [(0, 1.), (1, 4.)], [.25, 256, 4096, 65536], [.25, 1., 0., .125]),
        ([cell(1., 300.), cell(1., 280.), cell(1e100, 288.)], [(0, 1, 2.), (1, 2, 1.)], [(0, 4.)], [1., 256, 4096, 65536], [.25, 0.]),
        ([cell(1., 288.), cell(1., 272.)], [(0, 1, 2.)], [(0, 4.)], [.25, 4, 3, 65536], [.25, .5]),
        ([cell(1., 288.), cell(1., 272.)], [(0, 1, 2.)], [(0, 4.)], [.25, 4, 8, 255], [.25, .5]),
        ([cell(1., 288.)], [], [(0, 1e-300)], [1., 256, 4096, 65536], [1e-300, 0.]),
        ([cell(1., 288.)], [], [(0, 0.)], [1., 256, 4096, 65536], [.1, .3]),
    ]
    for heating in [False, True]:
        out.append(([cell(100., 273.15 + (10. if heating else -10.), float(heating), WATER),
                     cell(.01, 273.15, float(not heating), WATER), cell(.01, 273.15, float(not heating), WATER)],
                    [(0, 1, 100.), (1, 2, 100.)], [(0, 100. if heating else -100.)],
                    [.1, 256, 4096, 65536], [.1] * 160))
    return out


def encode(inputs):
    lines = [str(len(inputs))]
    for cells, links, sources, config, steps in inputs:
        lines += [' '.join(map(str, [len(cells), len(links), len(sources), len(steps)])),
                  ' '.join(map(str, [bits(config[0])] + config[1:]))]
        lines += [' '.join(str(bits(v)) for v in c) for c in cells]
        lines += [f'{a} {b} {bits(g)}' for a, b, g in links]
        lines += [f'{i} {bits(p)}' for i, p in sources]
        lines += [' '.join(str(bits(h)) for h in steps)]
    return '\n'.join(lines) + '\n'


def verify(inputs, text):
    rows = iter(text.splitlines());counts = Counter();operations = 0;product_losses = 0;partitions = 0
    for cells, links, sources, config, steps in inputs:
        links = sorted((min(a,b), max(a,b), g) for a,b,g in links);sources = sorted(sources)
        initial = next(rows).split();assert initial[0] == 'I'
        current_wire = initial[1:]
        current = [number(v) for v in initial[1:]]
        tick = 0;published = (0, 0., 0., 0, 0, [])
        for dt in steps:
            r = next(rows).split();c = next(rows).split();assert r[0] == 'R' and c[0] == 'C'
            status, next_tick, allocations, attempted, fail_step, fail_desc, hs, cs, n = map(int, r[1:10])
            h, requested_dt = map(number, r[10:12]);exchanged, unchanged, size = map(int, r[12:15])
            assert allocations == 0, 'advance allocated memory'
            after = [number(v) for v in c[1:]];assert len(after) == len(current)
            journal = [next(rows).split() for _ in range(size)]
            assert all(op[0] == 'O' and len(op) == 18 for op in journal)
            now = (n, h, requested_dt, exchanged, unchanged, journal)
            counts[str(status)] += 1
            if status != 0 or dt == 0.:
                assert c[1:] == current_wire and next_tick == tick and now == published, 'partial publication on refusal/no-op'
                continue
            assert next_tick == tick + 1 and requested_dt == dt
            assert 0 < h <= config[0] and 0 < n <= config[1]
            assert size == attempted == n * (len(sources) + len(links))
            assert exchanged + unchanged == n * len(links)
            energies = [F(current[i*3]) for i in range(len(cells))]
            update_residual = F(0);rounded_source = F(0);product_residual = F(0)
            for k, op in enumerate(journal):
                kind, descriptor, substep, s1, s2 = map(int, op[1:6])
                per = len(sources) + len(links);expected_source = k % per < len(sources)
                assert kind == int(not expected_source) and substep == k // per
                assert descriptor == (k % per if expected_source else k % per - len(sources))
                values = [number(v) for v in op[6:]];assert all(math.isfinite(v) for v in values)
                b1,a1,b2,a2,q1,hi1,lo1,res1,q2,hi2,lo2,res2 = map(F, values)
                i = sources[descriptor][0] if kind == 0 else links[descriptor][0]
                assert energies[i] == b1 and a1 - b1 == hi1 + lo1
                assert q1 == a1 - b1 + res1 and s1 == 0
                energies[i] = a1;update_residual += res1
                if kind == 0:
                    power = sources[descriptor][1]
                    assert values[4] == power * h and s2 == 1
                    assert b2 == a2 == q2 == hi2 == lo2 == res2 == 0
                    rounded_source += q1
                    loss = F(power) * F(h) - q1
                    product_losses += bool(loss);product_residual += loss
                else:
                    j = links[descriptor][1]
                    assert energies[j] == b2 and a2 - b2 == hi2 + lo2 and s2 == 0
                    assert q2 == a2 - b2 + res2 and q1 == -q2
                    energies[j] = a2;update_residual += res2
                operations += 1
            assert energies == [F(after[i*3]) for i in range(len(cells))], 'journal omitted a stored update'
            delta = sum(energies) - sum(F(current[i*3]) for i in range(len(cells)))
            assert delta + update_residual == rounded_source
            partition_residual = sum(F(p) * (F(dt) - n * F(h)) for _,p in sources)
            partitions += bool(partition_residual)
            ideal = sum(F(p) * F(dt) for _,p in sources)
            assert delta + update_residual + product_residual + partition_residual == ideal
            if not any(p for _,p in sources):
                low, high = min(current[1::3]), max(current[1::3])
                assert all(low <= t <= high for t in after[1::3])
            for i,cdef in enumerate(cells):
                assert cdef[-2] <= after[i*3+1] <= cdef[-1] and 0 <= after[i*3+2] <= 1
            current = after;current_wire = c[1:];tick = next_tick;published = now
    assert next(rows, None) is None
    assert counts['0'] > 300 and counts['6'] and counts['7'] and counts['4'] and counts['5']
    assert product_losses and partitions, 'Need controls exposing source multiplication and duration rounding'
    return {'fields': len(inputs), 'calls': sum(counts.values()), 'status_counts': dict(counts),
            'verified_operations': operations, 'source_product_rounding_witnesses': product_losses,
            'duration_partition_witnesses': partitions, 'allocations_during_advances': 0}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', required=True, type=Path);p.add_argument('--work-dir', required=True, type=Path)
    p.add_argument('--compiler', default='c++');args = p.parse_args()
    root = Path(__file__).resolve().parents[2];args.work_dir.mkdir(parents=True, exist_ok=True)
    report = args.work_dir / 'field-report.json';report.unlink(missing_ok=True)
    source = args.work_dir / 'field-probe.cpp';binary = args.work_dir / 'field-probe';source.write_text(PROBE)
    subprocess.run([args.compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'engine/fluids/include'), str(source), str(args.library.resolve()),
                    '-o', str(binary)], check=True)
    inputs = cases();data = encode(inputs)
    result = subprocess.run([str(binary)], input=data, text=True, capture_output=True, check=True)
    (args.work_dir/'input.txt').write_text(data);(args.work_dir/'output.txt').write_text(result.stdout)
    summary = verify(inputs, result.stdout)
    summary.update(input_sha256=hashlib.sha256(data.encode()).hexdigest(),
                   output_sha256=hashlib.sha256(result.stdout.encode()).hexdigest(), compiler=args.compiler,
                   library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest())
    report.write_text(json.dumps(summary, indent=2)+'\n');print(report.read_text(), end='')


if __name__ == '__main__':
    main()
