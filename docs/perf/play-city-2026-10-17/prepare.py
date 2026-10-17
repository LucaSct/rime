#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Generate an external 1981-frame native play capture; compile only on request."""
import argparse
import hashlib
import json
import pathlib
import shlex
import shutil
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
REL_MAIN = pathlib.Path('samples/99-the-block/main.cpp')
CITY = 'session.server.city = session.client.city = environment_enabled && scene_path.empty();'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_state(root):
    def run(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    return {'head': run('rev-parse', 'HEAD'), 'dirty': bool(run('status', '--porcelain'))}


def exactly(text, old, new):
    count = text.count(old)
    if count != 1:
        raise ValueError(f'expected one occurrence ({count} found): {old!r}')
    return text.replace(old, new, 1)


def instrument(source, csv_path, header_path, disable_city):
    start = source.index('int run_play(')
    end = source.index('\n} // namespace', start)
    play = source[start:end]
    def change(old, new):
        nonlocal play
        play = exactly(play, old, new)
    change('    block_play::PlayInput live;', '    window_diag::Capture capture;\n    if (!demo.app.windowed() || !demo.viewmodel || !demo.play_world) return 3;\n    block_play::PlayInput live;')
    change('demo.app.on_render([&](app::FrameContext& ctx) {', 'demo.app.on_render([&](app::FrameContext& ctx) {\n        capture.begin(ctx);')
    change('const gameplay::FrameIntent intent = gameplay::map_frame_input(', 'gameplay::FrameIntent intent = gameplay::map_frame_input(')
    change('        live.frame(intent.character, ctx.input, bindings.fire, gameplay::kActionFire);',
           '        demo.session.client.view.yaw = capture.yaw(play_sunset::kViewYaw);\n'
           '        demo.session.client.view.pitch = play_sunset::kViewPitch;\n'
           '        intent.character.yaw = demo.session.client.view.yaw;\n'
           '        intent.character.pitch = demo.session.client.view.pitch;\n'
           '        live.frame(intent.character, ctx.input, bindings.fire, gameplay::kActionFire);')
    change('            demo.viewmodel->apply_controls(demo.session.client.devices, ctx.input);',
           '            demo.viewmodel->apply_controls(demo.session.client.devices, ctx.input);\n'
           '            demo.viewmodel->set_aiming(capture.aiming());')
    change('        demo.render(ctx);',
           '        demo.render(ctx);\n'
           '        capture.record(ctx, demo.app.tick_count(), demo.app.window()->framebuffer_size(),\n'
           '                       demo.viewmodel->aim_fraction(), demo.session.client.view.yaw,\n'
           '                       demo.session.client.view.pitch,\n'
           '                       demo.visuals->last_scope_ldr.is_valid(),\n'
           '                       demo.play_world->stats().resident, ctx.graph->pass_count(),\n'
           '                       demo.session.shots_fired, demo.charge_fired);\n'
           '        if (capture.done()) demo.app.request_quit();')
    change('    if (demo.app.windowed()) {\n        (void)demo.app.run();',
           '    const auto diagnostic_owner = std::this_thread::get_id();\n'
           '    core::set_zone_sink([&](std::string_view name, double elapsed) {\n'
           '        if (std::this_thread::get_id() == diagnostic_owner)\n'
           '            capture.zone(demo.app.frame_index(), name, elapsed);\n'
           '    });\n'
           '    if (demo.app.windowed()) {\n        (void)demo.app.run();')
    change('    std::printf("99-the-block: %llu frames, %llu ticks%s\\n",',
           '    core::set_zone_sink({});\n'
           '    std::printf("99-the-block: %llu frames, %llu ticks%s\\n",')
    change('    demo.app.finish_gpu(); // first-person resources retire before Visuals and their registries',
           '    if (!capture.write(' + json.dumps(str(csv_path)) + ')) return 4;\n'
           '    demo.app.finish_gpu(); // first-person resources retire before Visuals and their registries')
    source = source[:start] + play + source[end:]
    source = exactly(source, 'namespace {', '#include ' + json.dumps(str(header_path)) + '\n\nnamespace {')
    if disable_city:
        # Explicit control-only exception: the city assignment lives in Demo::start, outside run_play.
        source = exactly(source, CITY, 'session.server.city = session.client.city = false;')
    return source


def commands(root, build, out):
    entries = json.loads((build / 'compile_commands.json').read_text())
    matches = [e for e in entries if pathlib.Path(e['file']).as_posix().endswith('/' + REL_MAIN.as_posix())]
    if len(matches) != 1:
        raise ValueError(f'expected one sample compile entry; found {len(matches)}')
    entry = matches[0]
    original_root = pathlib.Path(entry['file']).resolve().parents[2]
    cmd = shlex.split(entry['command']) if 'command' in entry else list(entry['arguments'])
    old_input = entry['file']
    if cmd.count(old_input) != 1 or cmd.count('-o') != 1 or cmd.count('-c') != 1:
        raise ValueError('unexpected sample compile command')
    cmd[cmd.index(old_input)] = str(out / 'capture_wrapper.cpp')
    cmd[cmd.index('-o') + 1] = str(out / 'capture_wrapper.o')
    for i, arg in enumerate(cmd):
        if arg.startswith('-I' + str(original_root) + '/'):
            cmd[i] = '-I' + str(root) + arg[len('-I' + str(original_root)):]
    cmd.insert(1, '-I' + str(root / 'samples/99-the-block'))
    raw = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'bin/the_block'], text=True)
    lines = [line for line in raw.splitlines() if line.strip()]
    if not lines:
        raise ValueError('no ninja link command')
    link = shlex.split(lines[-1])
    if link[:2] != [':', '&&'] or link[-2:] != ['&&', ':']:
        raise ValueError('unexpected ninja link command wrapper')
    link = link[2:-2]
    main_objects = [i for i, token in enumerate(link) if token.endswith('/CMakeFiles/the_block.dir/main.cpp.o')]
    if len(main_objects) != 1 or link.count('bin/the_block') != 1:
        raise ValueError('unexpected the_block link command')
    link[main_objects[0]] = str(out / 'capture_wrapper.o')
    link[link.index('bin/the_block')] = str(out / 'capture_play')
    for i, token in enumerate(link):
        if token.startswith('-Wl,--dependency-file='):
            link[i] = '-Wl,--dependency-file=' + str(out / 'capture_play.link.d')
    return cmd, link, original_root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=pathlib.Path, required=True, help='source checkout')
    ap.add_argument('--build-dir', type=pathlib.Path, required=True, help='existing Ninja build with compile_commands.json')
    ap.add_argument('--out-dir', type=pathlib.Path, required=True, help='generated files and CSV destination')
    ap.add_argument('--disable-city', action='store_true', help='eight-building control: replace the generated city assignment')
    ap.add_argument('--compile', action='store_true', help='compile and link the capture executable')
    args = ap.parse_args()
    root, build, out = (p.expanduser().resolve() for p in (args.root, args.build_dir, args.out_dir))
    if not (root / REL_MAIN).is_file() or not (build / 'compile_commands.json').is_file():
        ap.error('source main.cpp or build/compile_commands.json missing')
    out.mkdir(parents=True, exist_ok=True)
    # A failed or interrupted replacement capture must not inherit an older CSV or binary.
    for name in ('capture_play', 'capture.frames.csv', 'capture.summary.json', 'capture.intervals.csv'):
        (out / name).unlink(missing_ok=True)
    source_path = root / REL_MAIN
    source = source_path.read_text()
    (out / 'source-main.cpp').write_text(source)
    header = out / 'capture.hpp'
    shutil.copyfile(HERE / 'capture.hpp', header)
    csv_path = out / 'capture.frames.csv'
    generated = out / 'instrumented-main.cpp'
    generated.write_text(instrument(source, csv_path, header, args.disable_city))
    wrapper = out / 'capture_wrapper.cpp'
    wrapper.write_text('#define main rime_embedded_sample_main\n#include ' + json.dumps(str(generated)) +
                       '\n#undef main\nint main() { return run_play(RIME_BLOCK_COOKED_DIR, {}); }\n')
    compile_cmd, link_cmd, build_source = commands(root, build, out)
    (out / 'compile-command.txt').write_text(shlex.join(compile_cmd) + '\n')
    (out / 'link-command.txt').write_text(shlex.join(link_cmd) + '\n')
    info = {'source_root': str(root), 'source_git': git_state(root), 'build_dir': str(build),
            'build_source_root': str(build_source), 'build_source_git': git_state(build_source),
            'source_sha256': sha(source_path), 'instrumented_sha256': sha(generated),
            'wrapper_sha256': sha(wrapper), 'capture_header_sha256': sha(header),
            'disable_city': args.disable_city, 'compile_command': compile_cmd, 'link_command': link_cmd,
            'binary_sha256': None, 'csv_path': str(csv_path)}
    info['linked_inputs_sha256'] = {}
    for token in link_cmd:
        path = pathlib.Path(token)
        if path.suffix in ('.a', '.o', '.so') and path != out / 'capture_wrapper.o':
            path = path if path.is_absolute() else build / path
            info['linked_inputs_sha256'][str(path)] = sha(path)
    cache = build / 'CMakeCache.txt'
    info['cmake_build_type'] = next((s.split('=', 1)[1] for s in cache.read_text().splitlines()
                                     if s.startswith('CMAKE_BUILD_TYPE:STRING=')), None) if cache.exists() else None
    def save():
        (out / 'provenance.json').write_text(json.dumps(info, indent=2) + '\n')
    save()
    if args.compile:
        for label, cmd in [('compile', compile_cmd), ('link', link_cmd)]:
            result = subprocess.run(cmd, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / f'{label}.log').write_text(result.stdout)
            if result.returncode:
                raise SystemExit(f'{label} failed ({result.returncode}); see {out / (label + ".log")}')
        info['binary_sha256'] = sha(out / 'capture_play')
        save()
    print(out / 'capture_play')


if __name__ == '__main__':
    main()
