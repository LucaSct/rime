#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Prepare an external city contact recorder. No production files are edited; never runs the game."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def exactly(text, old, new):
    count = text.count(old)
    if count != 1:
        raise ValueError(f'expected one source anchor, got {count}: {old[:100]!r}')
    return text.replace(old, new, 1)


def instrument(source, header):
    source = exactly(source, '#include "narrowphase.hpp"',
                     '#include "narrowphase.hpp"\n#include ' + json.dumps(str(header)))
    anchor = 'void PhysicsWorld::Impl::build_contacts(std::vector<Manifold>& out, float dt) const {\n'
    source = exactly(source, anchor, anchor +
                     '    ::contact_capture::Step capture_step(this, zone(StepZone::Step), dt);\n')
    anchor = '        if (!touching) {\n            // The exact shapes do not overlap.'
    hook = '''        ::contact_capture::record(capture_step, pr.a, pr.b,
            shape[da], position[da], orientation[da], linear_velocity[da],
            shape[db], position[db], orientation[db], linear_velocity[db],
            ccd[da] != 0 || ccd[db] != 0, ha, hb, touching, m);
'''
    source = exactly(source, anchor, hook + anchor)
    end = "}\n\nvoid PhysicsWorld::compute_pairs(std::vector<Pair>& out) const {"
    source = exactly(source,end,
        "    if (::contact_capture::hashes_enabled())\n"
        "        ::contact_capture::hash_step(p.zone(StepZone::Step), world_hash());\n" + end)
    # The actual runtime cache is shared with replay. The unfiltered reference is selected at
    # compile time below, so the hash proof exercises the final production source, not a copy.
    if source.count('plain_bounds.begin(') != 1 or 'RIME_PHYSICS_UNFILTERED_CONTACTS' not in source:
        raise ValueError('expected the production plain-contact cache and reference guard')

    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--compile', action='store_true')
    parser.add_argument('--candidate',choices=['none','child'],default='none')
    parser.add_argument('--oracle',type=Path,help='required passing frozen corpus for a candidate')
    args = parser.parse_args()
    root, build, out = (p.resolve() for p in (args.root, args.build_dir, args.out_dir))
    out.mkdir(parents=True, exist_ok=True)
    oracle_result=None
    if args.candidate != 'none':
        if args.oracle is None: parser.error('--candidate requires --oracle')
        verification=subprocess.run([str(build/'bin/contact_bench'),'--input',str(args.oracle.resolve()),
            '--candidate','cached','--verify-only'],check=True,text=True,capture_output=True)
        oracle_result=json.loads(verification.stdout)

    # Reuse the existing three-building charge, physics hash ledger and content witnesses.
    # Its preparation records exact build/link inputs and rejects a different source checkout.
    city = out / 'city'
    cmd = [sys.executable, str(root / 'docs/perf/play-city-profile-2026-10-17/prepare.py'),
           '--root', str(root), '--build-dir', str(build), '--out-dir', str(city)]
    if args.compile:
        cmd.append('--compile')
    subprocess.run(cmd, check=True)
    source = root / 'engine/physics/src/world.cpp'
    generated = out / 'capture-world.cpp'
    generated.write_text(instrument(source.read_text(), root / 'samples/contact_bench/capture.hpp'))
    entries = json.loads((build / 'compile_commands.json').read_text())
    matches = [e for e in entries if Path(e['file']).resolve() == source]
    if len(matches) != 1:
        raise ValueError('expected one world.cpp compile command for this checkout')
    entry = matches[0]
    compile_cmd = entry.get('arguments') or shlex.split(entry['command'])
    if compile_cmd.count(str(source)) != 1 or compile_cmd.count('-o') != 1:
        raise ValueError('unexpected world.cpp command')
    obj, binary = out / 'capture-world.o', out / 'capture_city'
    compile_cmd[compile_cmd.index(str(source))] = str(generated)
    compile_cmd[compile_cmd.index('-o') + 1] = str(obj)
    compile_cmd += ['-I' + str(root / 'samples/contact_bench')]
    if args.candidate == 'none':
        compile_cmd.append('-DRIME_PHYSICS_UNFILTERED_CONTACTS')
    info = json.loads((city / 'provenance.json').read_text())
    link = info['link_command'].copy()
    archives = [i for i, token in enumerate(link) if token.endswith('librime_physics.a')]
    if not archives or link.count(str(city / 'profile_play')) != 1:
        raise ValueError('unexpected city link command')
    link.insert(archives[0], str(obj))
    link[link.index(str(city / 'profile_play'))] = str(binary)
    link = [('-Wl,--dependency-file=' + str(out / 'capture_city.link.d'))
            if t.startswith('-Wl,--dependency-file=') else t for t in link]
    files = [source, generated, root / 'samples/contact_bench/capture.hpp',
             root / 'samples/contact_bench/workload.hpp', Path(__file__).resolve()]
    files.extend(sorted((root / 'engine/physics/src').glob('*.hpp')))
    files.extend(sorted((root / 'engine/physics/include').rglob('*.hpp')))
    files.extend([root/'docs/perf/play-city-profile-2026-10-17/profile.hpp',
                  root/'docs/perf/play-city-profile-2026-10-17/prepare.py'])
    info.update({'candidate': args.candidate, 'oracle_sha256':sha(args.oracle) if args.oracle else None, 'oracle_result':oracle_result, 'capture_source_sha256': {str(p): sha(p) for p in files},
                 'capture_compile': compile_cmd, 'capture_link': link,
                 'capture_binary_sha256': None, 'capture_scope': 'plain exact calls only; warm cache excluded'})
    if args.compile:
        for name, command in [('capture-compile', compile_cmd), ('capture-link', link)]:
            with (out / (name + '.log')).open('w') as log:
                subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        info['capture_binary_sha256'] = sha(binary)
    (out / 'provenance.json').write_text(json.dumps(info, indent=2) + '\n')
    print(binary if args.compile else f'Prepared {out}; use --compile before running')


if __name__ == '__main__':
    main()
