#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Generate an external city pass profiler; --compile builds it, never runs it."""
import argparse
import hashlib
import json
import pathlib
import shlex
import shutil
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
SOURCES = {'main': 'samples/99-the-block/main.cpp',
           'graph': 'engine/render/src/render_graph.cpp',
           'passes': 'engine/render/src/passes.cpp'}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def exactly(text, old, new):
    count = text.count(old)
    if count != 1:
        raise ValueError(f'expected exactly one replacement ({count}): {old[:100]!r}')
    return text.replace(old, new, 1)

def region(text, start, end, changes):
    a = text.index(start)
    b = text.index(end, a)
    part = text[a:b]
    for old, new in changes:
        part = exactly(part, old, new)
    return text[:a] + part + text[b:]

def instrument_graph(text, header):
    text = exactly(text, '#include "rime/render/render_graph.hpp"',
                   '#include "rime/render/render_graph.hpp"\n#include ' + json.dumps(str(header)))
    text = region(text, 'void RenderGraph::add_pass_common(', '\nvoid RenderGraph::add_raster_pass(', [
        ('    Pass p;\n    p.name.assign(name);',
         '    // Prefix the identity before the existing uniqueness and fold logic.\n'
         '    const std::string diagnostic_name = std::string(city_profile::config.prefix) + std::string(name);\n'
         '    name = diagnostic_name;\n    Pass p;\n    p.name.assign(name);')])
    text = region(text, 'void RenderGraph::execute(', '\nRenderGraph::TimingPlan', [
        ('    compile();\n    assign_physicals();',
         '    auto stage_start = city_profile::Clock::now();\n'
         '    compile();\n    city_profile::stage("compile", stage_start);\n'
         '    stage_start = city_profile::Clock::now();\n'
         '    assign_physicals();\n    city_profile::stage("assign_physicals", stage_start);\n'
         '    if (city_profile::active() && !city_profile::config.frames.empty()) {\n'
         '        auto& row = city_profile::config.frames.back();\n'
         '        row.executed_passes = order_.size();\n'
         '        row.overflow = order_.size() > max_timed_passes();\n'
         '    }'),
        ('        Pass& pass = passes_[pi];\n        cmd.begin_debug_label(pass.name);',
         '        Pass& pass = passes_[pi];\n'
         '        city_profile::PassTimer diagnostic_timer(pass.name);\n'
         '        cmd.begin_debug_label(pass.name);'),
        ('        cmd.end_debug_label();\n    }',
         '        cmd.end_debug_label();\n    }\n'
         '    if (city_profile::active() && !city_profile::config.frames.empty())\n'
         '        city_profile::config.frames.back().timed_passes = timing_plan().timed;')])
    return text

def instrument_passes(text, header):
    text = exactly(text, '#include "rime/render/passes.hpp"',
                   '#include "rime/render/passes.hpp"\n#include ' + json.dumps(str(header)))
    return region(text, 'void record_draws(', '\n} // namespace', [
        ('    cmd.bind_uniform_buffer(0, data.frame_ubo, data.frame_ubo_offset, data.frame_ubo_size);',
         '    std::uint64_t diagnostic_actual = 0;\n'
         '    cmd.bind_uniform_buffer(0, data.frame_ubo, data.frame_ubo_offset, data.frame_ubo_size);'),
        ('        cmd.draw_indexed(item.index_count, 1, item.first_index);',
         '        cmd.draw_indexed(item.index_count, 1, item.first_index);\n'
         '        ++diagnostic_actual;'),
        ('        ++diagnostic_actual;\n    }\n}',
         '        ++diagnostic_actual;\n    }\n'
         '    city_profile::draws(data.draws.size(), diagnostic_actual);\n}')])

def instrument_main(text, header, out):
    text = exactly(text, 'namespace {', '#include ' + json.dumps(str(header)) + '\n\nnamespace {')
    text = region(text, '    void render(app::FrameContext& ctx) {', '\n    void draw_hud(', [
        ('                    const auto scoped =\n                        optical.render(*ctx.graph, world, {512, 512}, true, &camera);',
         '                    optical.reset_cull_stats();\n'
         '                    city_profile::Prefix optic_prefix("optic/");\n'
         '                    const auto scoped =\n                        optical.render(*ctx.graph, world, {512, 512}, true, &camera);\n'
         '                    if (city_profile::active() && !city_profile::config.frames.empty()) {\n'
         '                        auto& row = city_profile::config.frames.back();\n'
         '                        const auto c = optical.cull_stats();\n'
         '                        row.optic_considered = c.considered(); row.optic_submitted = c.submitted; row.optic_culled = c.culled;\n'
         '                        const auto s = optical.local_shadow_stats();\n'
         '                        row.optic_shadow_rendered = s.rendered; row.optic_shadow_reused = s.reused;\n'
         '                    }'),
        ('        visuals->renderer.reset_cull_stats();\n        visuals->last_ldr = visuals->renderer.render(*ctx.graph, world, ctx.extent, true).ldr;',
         '        visuals->renderer.reset_cull_stats();\n'
         '        city_profile::Prefix main_prefix("main/");\n'
         '        visuals->last_ldr = visuals->renderer.render(*ctx.graph, world, ctx.extent, true).ldr;\n'
         '        if (city_profile::active() && !city_profile::config.frames.empty()) {\n'
         '            auto& row = city_profile::config.frames.back();\n'
         '            const auto c = visuals->renderer.cull_stats();\n'
         '            row.main_considered = c.considered(); row.main_submitted = c.submitted; row.main_culled = c.culled;\n'
         '            const auto s = visuals->renderer.local_shadow_stats();\n'
         '            row.main_shadow_rendered = s.rendered; row.main_shadow_reused = s.reused;\n'
         '        }')])
    text = region(text, '    void tick(const replication::InputCommand& intent) {', '\n    // Parts still standing', [
        ('        // ── PostSim: the debris bridge ──',
         '        const auto diagnostic_post_sim_start = city_profile::Clock::now();\n'
         '        // ── PostSim: the debris bridge ──'),
        ('        tick_solve.tick = tick_index;',
         '        city_profile::config.session_post_sim_ms = city_profile::elapsed_ms(diagnostic_post_sim_start);\n'
         '        tick_solve.tick = tick_index;')])
    text = region(text, '    void step_sim(const replication::InputCommand& intent) {', '\n    void fire_charge() {', [
        ('        session.tick(intent);',
         '        session.tick(intent);\n'
         '        const auto diagnostic_post_session_start = city_profile::Clock::now();'),
        ('            if (city_dresser.ready()) {\n                (void)city_dresser.sync(\n                    session.client.world, session.client.destruction, visuals->leaves);\n            }\n        }',
         '            if (city_dresser.ready()) {\n                (void)city_dresser.sync(\n                    session.client.world, session.client.destruction, visuals->leaves);\n            }\n        }\n'
         '        city_profile::config.post_session_ms = city_profile::elapsed_ms(diagnostic_post_session_start);')])
    text = region(text, '    void fire_charge() {', '\n    // Pose the camera', [
        ('        (void)detonate(session, p.hero_south, hero_charge(p));',
         '        city_profile::config.charge_targets = detonate(session, p.hero_south, hero_charge(p));')])
    text = region(text, 'int run_play(', '\n} // namespace', [
        ('    cfg.windowed = true;', '    cfg.windowed = false;'),
        ('    Demo demo(cfg);',
         '    city_profile::config.owner = std::this_thread::get_id();\n'
         '    Demo demo(cfg);'),
        ('    if (!demo.start(cooked, scene_path)) {\n        return 1;\n    }',
         '    if (!demo.start(cooked, scene_path)) return 1;\n'
         '    if (!demo.viewmodel || !demo.play_world || !demo.play_world->pass() || !demo.visuals) return 2;'),
        ('    block_play::PlayInput live;',
         '    demo.app.on_frame_timings([&](std::uint64_t index, std::span<const render::RenderGraph::PassTiming> timings) {\n'
         '        if (index < city_profile::config.warmup) return;\n'
         '        for (const auto& t : timings) city_profile::config.gpu.push_back({index, t.name, t.gpu_ms});\n'
         '        for (auto& row : city_profile::config.frames) if (row.frame == index) { row.gpu_present = !timings.empty(); break; }\n'
         '    });\n'
         '    block_play::PlayInput live;'),
        ('        auto input = live.tick();',
         '        const auto diagnostic_start = city_profile::Clock::now();\n'
         '        const auto damage_before = demo.session.damage_ops;\n'
         '        const auto committed_before = demo.session.total_ops;\n'
         '        auto input = live.tick();'),
        ('        demo.step_sim(gameplay_net::to_input_command(input));',
         '        demo.step_sim(gameplay_net::to_input_command(input));\n'
         '        if (city_profile::active()) {\n'
         '            auto& row = city_profile::config.frames.back();\n'
         '            ++row.ticks; row.pre_sim_ms += city_profile::elapsed_ms(diagnostic_start);\n'
         '            row.post_session_ms += city_profile::config.post_session_ms;\n'
         '            row.session_post_sim_ms += city_profile::config.session_post_sim_ms;\n'
         '            row.server_ms += demo.session.last_server_ms;\n'
         '            row.client_ms += demo.session.last_client_ms; row.join_ms += demo.session.last_join_ms;\n'
         '            row.damage_ops += demo.session.damage_ops - damage_before;\n'
         '            row.committed_ops += demo.session.total_ops - committed_before;\n'
         '            row.server_debris = demo.session.server.destruction.debris_count();\n'
         '            row.client_debris = demo.session.client.destruction.debris_count();\n'
         '            city_profile::physics(row.server_physics, demo.session.server.physics.stats());\n'
         '            city_profile::physics(row.client_physics, demo.session.client.physics.stats());\n'
         '        }'),
        ('        live.frame(intent.character, ctx.input, bindings.fire, gameplay::kActionFire);',
         '        gameplay::CharacterInput diagnostic_character{};\n'
         '        diagnostic_character.yaw = play_sunset::kViewYaw;\n'
         '        diagnostic_character.pitch = play_sunset::kViewPitch;\n'
         '        demo.session.client.view.yaw = diagnostic_character.yaw;\n'
         '        demo.session.client.view.pitch = diagnostic_character.pitch;\n'
         '        live.frame(diagnostic_character, ctx.input, bindings.fire, gameplay::kActionFire);'),
        ('            demo.viewmodel->apply_controls(demo.session.client.devices, ctx.input);',
         '            demo.viewmodel->apply_controls(demo.session.client.devices, ctx.input);\n'
         '            demo.viewmodel->set_aiming(city_profile::config.ads);'),
        ('        demo.render(ctx);',
         '        auto diagnostic_lighting = demo.visuals->renderer.lighting();\n'
         '        diagnostic_lighting.shadow_caster_culling = city_profile::config.shadow_caster_culling;\n'
         '        demo.visuals->renderer.set_lighting(diagnostic_lighting);\n'
         '        demo.render(ctx);\n'
         '        if (city_profile::active()) city_profile::config.frames.back().graph_passes = ctx.graph->pass_count();'),
        ('    if (demo.app.windowed()) {\n        (void)demo.app.run();\n    } else {\n        demo.app.run_frames(static_cast<int>(g_ticks));\n    }',
         '    for (std::uint64_t i = 0; i < city_profile::config.warmup + diagnostic_frames; ++i) {\n'
         '        city_profile::config.frame = demo.app.frame_index();\n'
         '        city_profile::config.recording = i >= city_profile::config.warmup;\n'
         '        if (city_profile::active()) {\n'
         '            city_profile::FrameRow row{}; row.frame = city_profile::config.frame;\n'
         '            city_profile::config.frames.push_back(row);\n'
         '        }\n'
         '        const auto diagnostic_frame_start = city_profile::Clock::now();\n'
         '        if (city_profile::config.collapse && i == city_profile::config.warmup) {\n'
         '            city_profile::config.frames.back().charge_requested = true;\n'
         '            demo.fire_charge();\n'
         '            auto& row = city_profile::config.frames.back();\n'
         '            row.charge_fired = demo.charge_fired;\n'
         '            row.charge_targets = city_profile::config.charge_targets;\n'
         '        }\n'
         '        (void)demo.app.step(1.0 / 60.0);\n'
         '        if (city_profile::active()) {\n'
         '            city_profile::config.frames.back().frame_cpu_ms = city_profile::elapsed_ms(diagnostic_frame_start);\n'
         '            city_profile::capture_casters(city_profile::config.frame, "main",\n'
         '                demo.visuals->renderer.csm_caster_stats());\n'
         '            if (demo.visuals->scope_renderer && demo.visuals->last_scope_ldr.is_valid())\n'
         '                city_profile::capture_casters(city_profile::config.frame, "optic",\n'
         '                    demo.visuals->scope_renderer->csm_caster_stats());\n'
         '        }\n'
         '    }\n'
         '    city_profile::config.recording = false;'),
        ('    demo.app.finish_gpu(); // first-person resources retire before Visuals and their registries',
         '    // Readback and file I/O happen after the timed loop, before the graph retires.\n'
         '    if (!city_profile::config.ppm.empty() && demo.app.graph()) {\n'
         '        const auto tex = demo.app.graph()->physical(demo.visuals->last_ldr);\n'
         '        if (tex.is_valid()) {\n'
         '            const auto pixels = read_rgba8(*demo.app.device(), tex, kWidth, kHeight);\n'
         '            std::error_code error;\n'
         '            std::filesystem::remove(city_profile::config.ppm, error);\n'
         '            if (!error) {\n'
         '                write_ppm(city_profile::config.ppm.string().c_str(), pixels, kWidth, kHeight);\n'
         '                const auto bytes = std::filesystem::file_size(city_profile::config.ppm, error);\n'
         '                city_profile::config.ppm_written = !error && bytes >=\n'
         '                    static_cast<std::uintmax_t>(kWidth) * kHeight * 3;\n'
         '            }\n'
         '        }\n'
         '    }\n'
         '    demo.app.finish_gpu(); // first-person resources retire before Visuals and their registries\n'
         '    city_profile::write(diagnostic_out);\n'
         '    std::ofstream summary(diagnostic_out / "summary.json");\n'
         '    if (!summary) throw std::runtime_error("summary.json open failed");\n'
         '    bool overflow = false, gpu_present = true, pass_mismatch = false;\n'
         '    bool collapse_effect_seen = false, ticks_present = true, physics_present = true;\n'
         '    std::uint64_t charge_requests = 0;\n'
         '    for (const auto& f : city_profile::config.frames) {\n'
         '        overflow |= f.overflow; gpu_present &= f.gpu_present;\n'
         '        ticks_present &= f.ticks > 0 && f.frame_cpu_ms > 0;\n'
         '        physics_present &= f.server_physics.bodies > 0 && f.client_physics.bodies > 0;\n'
         '        charge_requests += f.charge_requested ? 1u : 0u;\n'
         '        collapse_effect_seen |= f.committed_ops > 0 || f.server_debris > 0;\n'
         '        pass_mismatch |= f.executed_passes == 0 ||\n'
         '            f.timed_passes != std::min<std::uint64_t>(f.executed_passes, render::RenderGraph::max_timed_passes());\n'
         '    }\n'
         '    const bool count_ok = city_profile::config.frames.size() == diagnostic_frames;\n'
         '    summary << "{\\"ads\\":" << (city_profile::config.ads ? "true" : "false")\n'
         '            << ",\\"control\\":" << (city_profile::config.control ? "true" : "false")\n'
         '            << ",\\"warmup\\":" << city_profile::config.warmup\n'
         '            << ",\\"requested_frames\\":" << diagnostic_frames\n'
         '            << ",\\"measured_frames\\":" << city_profile::config.frames.size()\n'
         '            << ",\\"gpu_present\\":" << (gpu_present ? "true" : "false")\n'
         '            << ",\\"overflow\\":" << (overflow ? "true" : "false")\n'
         '            << ",\\"pass_mismatch\\":" << (pass_mismatch ? "true" : "false")\n'
         '            << ",\\"shadow_caster_culling\\":" << (city_profile::config.shadow_caster_culling ? "true" : "false")\n'
         '            << ",\\"ppm_written\\":" << (city_profile::config.ppm_written ? "true" : "false")\n'
         '            << ",\\"collapse_requested\\":" << (city_profile::config.collapse ? "true" : "false")\n'
         '            << ",\\"collapse_effect_seen\\":" << (collapse_effect_seen ? "true" : "false")\n'
         '            << ",\\"charge_targets\\":" << city_profile::config.charge_targets\n'
         '            << ",\\"perf_gate_pass\\":null}" << std::endl;\n'
         '    summary.flush(); if (!summary) throw std::runtime_error("summary.json write failed");\n'
         '    if (!count_ok || !gpu_present || overflow || pass_mismatch || !ticks_present || !physics_present ||\n'
         '        (!city_profile::config.ppm.empty() && !city_profile::config.ppm_written) ||\n'
         '        (city_profile::config.collapse && (charge_requests != 1 || !demo.charge_fired ||\n'
         '         city_profile::config.charge_targets == 0 || !collapse_effect_seen))) return 5;')])
    text = exactly(text, 'session.server.city = session.client.city = environment_enabled && scene_path.empty();',
                   'session.server.city = session.client.city = environment_enabled && scene_path.empty() && !city_profile::config.control;')
    text = exactly(text, 'int run_play(const std::filesystem::path& cooked, std::string_view scene_path) {',
                   'std::uint64_t diagnostic_frames = 120;\nstd::filesystem::path diagnostic_out;\n'
                   'int run_play(const std::filesystem::path& cooked, std::string_view scene_path) {')
    return text

def command_for(entries, rel, generated, obj, root, build_source_root):
    matches = [e for e in entries if pathlib.Path(e['file']).as_posix().endswith('/' + rel)]
    if len(matches) != 1: raise ValueError(f'{rel}: expected one compile entry, got {len(matches)}')
    e = matches[0]
    cmd = shlex.split(e['command']) if 'command' in e else list(e['arguments'])
    file = e['file']
    if cmd.count(file) != 1 or cmd.count('-o') != 1 or cmd.count('-c') != 1:
        raise ValueError(f'{rel}: unexpected compile command')
    cmd[cmd.index(file)] = str(generated)
    cmd[cmd.index('-o') + 1] = str(obj)
    # The build's generated include directory stays in the build tree. Source includes
    # point at the checkout whose files were just instrumented.
    old_prefix = str(build_source_root) + '/'
    for i, token in enumerate(cmd):
        if token.startswith('-I' + old_prefix):
            cmd[i] = '-I' + str(root) + '/' + token[len('-I' + old_prefix):]
    # Relative includes in the original sample command are rooted at its source directory.
    cmd.insert(1, '-I' + str(root / pathlib.Path(rel).parent))
    return cmd

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=pathlib.Path, required=True)
    ap.add_argument('--build-dir', type=pathlib.Path, required=True)
    ap.add_argument('--out-dir', type=pathlib.Path, required=True)
    ap.add_argument('--ads', action='store_true')
    ap.add_argument('--control', action='store_true')
    ap.add_argument('--warmup', type=int, default=60)
    ap.add_argument('--frames', type=int, default=120)
    ap.add_argument('--compile', action='store_true')
    args = ap.parse_args()
    if args.warmup < 0 or args.frames < 1: ap.error('warmup must be nonnegative and frames positive')
    root, build, out = (p.expanduser().resolve() for p in (args.root, args.build_dir, args.out_dir))
    out.mkdir(parents=True, exist_ok=True)
    header = out / 'profile.hpp'; shutil.copyfile(HERE / 'profile.hpp', header)
    source_paths = {key: root / rel for key, rel in SOURCES.items()}
    generated = {key: out / f'instrumented-{key}.cpp' for key in SOURCES}
    generated['main'].write_text(instrument_main(source_paths['main'].read_text(), header, out))
    generated['graph'].write_text(instrument_graph(source_paths['graph'].read_text(), header))
    generated['passes'].write_text(instrument_passes(source_paths['passes'].read_text(), header))
    wrapper = out / 'wrapper.cpp'
    wrapper.write_text('#define main rime_embedded_sample_main\n#include ' + json.dumps(str(generated['main'])) +
                       '\n#undef main\nint main(int argc, char** argv) { city_profile::config.ads = ' + ('true' if args.ads else 'false') +
                       '; city_profile::config.control = ' + ('true' if args.control else 'false') +
                       '; city_profile::config.warmup = ' + str(args.warmup) +
                       '; diagnostic_frames = ' + str(args.frames) +
                       '; diagnostic_out = ' + json.dumps(str(out)) +
                       '; const auto generated_out = diagnostic_out' +
                       '; if (!city_profile::parse_cli(argc, argv, diagnostic_out, diagnostic_frames)) { '
                       'std::fputs("usage: profile_play [--out-dir DIR] [--ads|--hip] [--control|--city] '
                       '[--warmup N] [--frames N] [--collapse] [--shadow-culling-off|--shadow-culling-on] [--ppm PATH]\\n", stderr); return 2; } '
                       'if (generated_out != diagnostic_out) { std::error_code error; '
                       'std::filesystem::copy_file(generated_out / "provenance.json", '
                       'diagnostic_out / "provenance.json", '
                       'std::filesystem::copy_options::overwrite_existing, error); '
                       'if (error) { std::fputs("provenance copy failed\\n", stderr); return 2; } } '
                       'return run_play(RIME_BLOCK_COOKED_DIR, {}); }\n')
    entries = json.loads((build / 'compile_commands.json').read_text())
    sample_entries = [e for e in entries if pathlib.Path(e['file']).as_posix().endswith('/' + SOURCES['main'])]
    if len(sample_entries) != 1: raise ValueError('expected one sample compile entry')
    build_source_root = pathlib.Path(sample_entries[0]['file']).resolve().parents[2]
    for key, rel in SOURCES.items():
        built_source = build_source_root / rel
        if not built_source.is_file() or sha(built_source) != sha(source_paths[key]):
            raise ValueError(f'build/source mismatch for {rel}')
    objects = {'main': out / 'wrapper.o', 'graph': out / 'render_graph.o', 'passes': out / 'passes.o'}
    commands = {key: command_for(entries, rel, wrapper if key == 'main' else generated[key], objects[key], root, build_source_root)
                for key, rel in SOURCES.items()}
    raw = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'bin/the_block'], text=True)
    link = shlex.split([line for line in raw.splitlines() if line.strip()][-1])
    if link[:2] != [':', '&&'] or link[-2:] != ['&&', ':']: raise ValueError('unexpected Ninja link wrapper')
    link = link[2:-2]
    matches = [i for i, token in enumerate(link) if token.endswith('/CMakeFiles/the_block.dir/main.cpp.o')]
    if len(matches) != 1 or link.count('bin/the_block') != 1: raise ValueError('unexpected sample link command')
    link[matches[0]] = str(objects['main'])
    libs = [i for i, token in enumerate(link) if token.endswith('librime_render.a')]
    if len(libs) != 1: raise ValueError('expected one render archive')
    link[libs[0]:libs[0]] = [str(objects['graph']), str(objects['passes'])]
    link[link.index('bin/the_block')] = str(out / 'profile_play')
    link = [('-Wl,--dependency-file=' + str(out / 'profile_play.link.d')) if t.startswith('-Wl,--dependency-file=') else t for t in link]
    info = {'source_root': str(root), 'build_source_root': str(build_source_root), 'build_dir': str(build), 'ads': args.ads, 'control': args.control,
            'warmup': args.warmup, 'frames': args.frames,
            'source_sha256': {k: sha(p) for k, p in source_paths.items()},
            'generated_sha256': {k: sha(p) for k, p in generated.items()},
            'header_sha256': sha(header), 'wrapper_sha256': sha(wrapper),
            'compile_commands': commands, 'link_command': link, 'binary_sha256': None,
            'linked_inputs_sha256': {}}
    for token in link:
        path = pathlib.Path(token)
        if path.suffix in ('.a', '.o', '.so') and path not in objects.values():
            p = path if path.is_absolute() else build / path
            info['linked_inputs_sha256'][str(p)] = sha(p)
    provenance = out / 'provenance.json'
    provenance.write_text(json.dumps(info, indent=2) + '\n')
    if args.compile:
        for key, cmd in commands.items():
            result = subprocess.run(cmd, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / f'compile-{key}.log').write_text(result.stdout)
            if result.returncode: raise SystemExit(f'compile {key} failed; see {out / f"compile-{key}.log"}')
        result = subprocess.run(link, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / 'link.log').write_text(result.stdout)
        if result.returncode: raise SystemExit(f'link failed; see {out / "link.log"}')
        info['binary_sha256'] = sha(out / 'profile_play')
        provenance.write_text(json.dumps(info, indent=2) + '\n')
    print(out / 'profile_play')

if __name__ == '__main__': main()
