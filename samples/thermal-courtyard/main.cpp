// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/app/editor_host_app.hpp"
#include "rime/render/text/hud.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/scene/derive_transforms.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/vfx_scene/effects.hpp"
#include "scene.hpp"
using namespace rime;

namespace {
constexpr rhi::Extent2D extent{1280, 720};

bool load(ecs::World& w, const std::string& path) {
    courtyard::register_components(w);
    const auto report = scene::load_scene_file(w, path);
    if (!report.ok)
        std::fprintf(stderr, "courtyard: %s\n", report.error.c_str());
    return report.ok && report.skipped_components == 0;
}

int selftest(const std::string& path) {
    ecs::World w;
    if (!load(w, path))
        return 1;
    const auto authored = scene::save_scene_to_string(w);
    fluids_scene::ThermalPreview thermal;
    std::array<ecs::Entity, 3> stations{};
    std::array<bool, 3> found{};
    w.query<courtyard::Station>().for_each([&](ecs::Entity e, const courtyard::Station& s) {
        if (s.kind < 3) {
            stations[s.kind] = e;
            found[s.kind] = true;
        }
    });
    if (!std::all_of(found.begin(), found.end(), [](bool v) { return v; }))
        return 2;
    for (auto e : stations) {
        const auto* cell = w.get<fluids_scene::ThermalCell>(e);
        if (!cell || !cell->enabled)
            return 2;
    }
    for (std::uint64_t t = 1; t <= 2160; ++t)
        if (!thermal.tick(w, t, 1. / 60.))
            return 3;
    const auto state = [&](ecs::Entity e) {
        const auto rows = thermal.entities();
        const auto it = std::find(rows.begin(), rows.end(), e);
        return thermal.field()->cells()[static_cast<std::size_t>(it - rows.begin())].state();
    };
    const auto cold = state(stations[0]), hot = state(stations[1]), control = state(stations[2]);
    if (cold.liquid_mass_fraction != 0 || cold.temperature_kelvin >= 273.15 ||
        hot.liquid_mass_fraction != 1 || hot.temperature_kelvin <= 273.15 ||
        control.temperature_kelvin != 273.15 || control.liquid_mass_fraction != 1 ||
        authored != scene::save_scene_to_string(w))
        return 4;
    const auto rows = fluids_render::phase_surfaces(w, thermal);
    if (rows.count != 3)
        return 5;
    thermal.reset();
    if (fluids_render::phase_surfaces(w, thermal).count != 0)
        return 6;
    if (!thermal.tick(w, 1, 1. / 60.))
        return 7;
    std::printf("courtyard selftest OK: 3 stations, 2160 ticks; frozen %.3f K, melted %.3f K, "
                "control %.3f K; authored scene unchanged, reset/restart verified\n",
                cold.temperature_kelvin,
                hot.temperature_kelvin,
                control.temperature_kelvin);
    return 0;
}

std::vector<std::uint8_t>
read_pixels(rhi::Device& device, rhi::TextureHandle texture, rhi::Extent2D actual) {
    rhi::BufferDesc desc{};
    desc.size = static_cast<std::uint64_t>(actual.width) * actual.height * 4;
    desc.usage = rhi::BufferUsage::TransferDst;
    desc.memory = rhi::MemoryUsage::GpuToCpu;
    auto buffer = device.create_buffer(desc);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, buffer);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> pixels(desc.size);
    device.read_buffer(buffer, pixels.data(), pixels.size(), 0);
    device.destroy(buffer);
    return pixels;
}

bool capture(const std::string& path, std::span<const std::uint8_t> pixels, rhi::Extent2D actual) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    bool ok = std::fprintf(file, "P6\n%u %u\n255\n", actual.width, actual.height) > 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        ok = std::fwrite(pixels.data() + i, 1, 3, file) == 3 && ok;
    return std::fclose(file) == 0 && ok;
}

int demo(const std::string& path,
         bool headless,
         unsigned ticks,
         unsigned frames,
         bool controls_test,
         const std::string& image) {
    app::AppConfig cfg;
    cfg.gpu = true;
    cfg.render_extent = extent;
    cfg.worker_threads = 2;
    cfg.windowed = !headless;
    cfg.window_size = {extent.width, extent.height};
    cfg.window_title = "Rime - Frost Yard / thermal courtyard";
    app::Application app(cfg);
    if (!app.device())
        return 1;
    render::MeshRegistry meshes(*app.device());
    render::MaterialRegistry materials;
    if (!load(app.world(), path))
        return 2;
    courtyard::prepare(app.world(), meshes, materials);
    render::SceneRenderer renderer(*app.device(), meshes, materials);
    renderer.set_frames_in_flight(app.frames_in_flight());
    renderer.set_ambient(.07f, .095f, .14f);
    render::LightingSettings lighting{};
    lighting.shadows_enabled = true;
    renderer.set_lighting(lighting);
    render::text::HudRenderer hud(*app.device(), render::kLdrFormat);
    hud.set_frames_in_flight(app.frames_in_flight());
    fluids_scene::ThermalPreview thermal;
    vfx_scene::Effects effects;
    std::uint64_t tick = 0;
    bool paused = false;
    // The sample's automatic pause is a DEMO duration, not an energy clamp. At 36 s both
    // samples have completed their latent plateau and stay inside the documented domain.
    const auto advance = [&] {
        if (paused || tick >= 2160)
            return;
        ++tick;
        (void)thermal.tick(app.world(), tick, 1. / 60.);
        (void)effects.tick(app.world(), tick, 1.f / 60.f);
    };
    render::RGTexture last;
    rhi::Extent2D last_extent = extent;
    std::vector<render::GpuParticle> particles;
    particles.reserve(vfx_scene::Effects::max_particles);
    if (!headless)
        app.on_fixed_tick([&](ecs::World&, double) { advance(); });
    app.on_render([&](app::FrameContext& ctx) {
        last_extent = ctx.extent;
        for (const auto& event : ctx.input)
            if (event.type == platform::EventType::KeyDown && !event.key.repeat) {
                if (event.key.key == platform::Key::Space || event.key.key == platform::Key::P)
                    paused = !paused;
                if (event.key.key == platform::Key::R) {
                    thermal.reset();
                    effects.reset();
                    tick = 0;
                    paused = false;
                }
                if (event.key.key == platform::Key::Escape)
                    app.request_quit();
            }
        scene::derive_world_transforms(ctx.world, ctx.jobs);
        thermal.reconcile(ctx.world);
        effects.reconcile(ctx.world);
        const auto surfaces = fluids_render::phase_surfaces(ctx.world, thermal);
        (void)renderer.set_surface_appearances(surfaces.span());
        particles.clear();
        for (const auto& p : effects.field().particles())
            particles.push_back({{p.position.x, p.position.y, p.position.z, p.size},
                                 {.35f, .35f, .35f, .3f * std::max(0.f, 1 - p.age / p.lifetime)}});
        render::FxSettings fx;
        fx.enabled = !particles.empty();
        fx.max_particles = vfx_scene::Effects::max_particles;
        renderer.set_smoke_particles(particles, fx);
        std::array<render::GpuPointLight, 16> lights{};
        std::size_t n = 0;
        for (const auto& light : effects.lights())
            lights[n++] = {{light.position.x, light.position.y, light.position.z, light.radius},
                           {light.color.x * light.intensity,
                            light.color.y * light.intensity,
                            light.color.z * light.intensity,
                            0}};
        renderer.set_transient_point_lights({lights.data(), n});
        // Preserve the saved overview's horizontal framing on a narrow/tiled window. A copied
        // camera override avoids baking viewport aspect changes into authored camera settings.
        render::ExtractedCamera fitted;
        const render::ExtractedCamera* camera_override = nullptr;
        if (ctx.extent.width && ctx.extent.height &&
            static_cast<std::uint64_t>(ctx.extent.width) * extent.height <
                static_cast<std::uint64_t>(ctx.extent.height) * extent.width) {
            fitted = render::extract_scene(ctx.world).camera;
            const float aspect = static_cast<float>(ctx.extent.width) / ctx.extent.height;
            fitted.fov_y =
                2 * std::atan(std::tan(fitted.fov_y * .5f) *
                              (static_cast<float>(extent.width) / extent.height) / aspect);
            camera_override = &fitted;
        }
        last = renderer.render(*ctx.graph, ctx.world, ctx.extent, true, camera_override).ldr;
        if (!last.is_valid()) {
            app.request_quit();
            return;
        }
        hud.begin(ctx.extent);
        hud.panel(20, 18, 690, 112);
        hud.text(38, 32, "FROST YARD / CITY THERMAL LAB", {.78f, .91f, 1, 1}, 23);
        hud.text(38, 69, "SPACE pause / resume    R restart    ESC close", {}, 14);
        hud.text(38,
                 96,
                 "Fixed-mass phase display. Smoke / fire are separate visual effects.",
                 {.65f, .72f, .8f, 1},
                 12);
        const float w = static_cast<float>(ctx.extent.width);
        hud.panel(20, static_cast<float>(ctx.extent.height) - 137, w - 40, 117);
        const bool complete = tick >= 2160;
        hud.text(
            38,
            static_cast<float>(ctx.extent.height) - 123,
            fmt::format("{}   {:.1f} / 36.0 s",
                        complete ? "COMPLETE - press R to replay" : (paused ? "PAUSED" : "RUNNING"),
                        double(tick) / 60),
            {.9f, .92f, .95f, 1},
            15);
        std::array<std::string, 3> values{};
        if (const auto* field = thermal.field()) {
            const auto ids = thermal.entities();
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (const auto* station = ctx.world.get<courtyard::Station>(ids[i]);
                    station && station->kind < 3) {
                    const auto state = field->cells()[i].state();
                    values[station->kind] = fmt::format("{:.1f} C   {:.0f}% ice",
                                                        state.temperature_kelvin - 273.15,
                                                        (1 - state.liquid_mass_fraction) * 100);
                }
        }
        constexpr std::array names{
            "LEFT / COOLING  -100 kW", "CENTER / MELTING  +100 kW", "RIGHT / CONTROL  0 W"};
        for (unsigned i = 0; i < 3; ++i) {
            const float x = 38 + float(i) * (w - 76) / 3;
            hud.text(x,
                     static_cast<float>(ctx.extent.height) - 91,
                     names[i],
                     i == 1 ? render::text::Color{1, .69f, .32f, 1}
                            : render::text::Color{.58f, .81f, .95f, 1},
                     14);
            hud.text(x, static_cast<float>(ctx.extent.height) - 61, values[i], {}, 18);
        }
        if (thermal.status() == fluids_scene::PreviewStatus::StepRefused)
            hud.text(38,
                     145,
                     "THERMAL STEP REFUSED - showing last committed state",
                     {1, .4f, .3f, 1},
                     16);
        else if (thermal.status() == fluids_scene::PreviewStatus::InvalidDescription ||
                 thermal.status() == fluids_scene::PreviewStatus::CapacityExceeded)
            hud.text(38,
                     145,
                     "INVALID THERMAL GRAPH - preview cleared; correct settings",
                     {1, .4f, .3f, 1},
                     16);
        hud.declare(*ctx.graph, last);
        ctx.present = last;
    });
    scene::derive_world_transforms(app.world(), app.jobs());
    if (headless) {
        for (unsigned i = 0; i < ticks; ++i)
            advance();
        if (controls_test) {
            const auto press = [&](platform::Key key) {
                platform::Event event{};
                event.type = platform::EventType::KeyDown;
                event.key.key = key;
                app.post_input(event);
                app.step(0);
            };
            const auto before = tick;
            press(platform::Key::Space);
            advance();
            if (!paused || tick != before)
                return 6;
            press(platform::Key::Space);
            advance();
            if (paused || tick != before + 1)
                return 7;
            press(platform::Key::R);
            if (paused || tick != 0 || !thermal.field() || thermal.field()->tick() != 0)
                return 8;
            advance();
            if (tick != 1 || thermal.field()->tick() != 1)
                return 9;
            std::puts("courtyard input selftest OK: real Application events pause, resume and "
                      "restart the demo");
        }
        for (int i = 0; i < 3; ++i)
            app.step(0);
    } else if (app.windowed()) {
        if (frames)
            app.run_frames(static_cast<int>(frames));
        else
            (void)app.run();
    } else {
        std::fprintf(stderr,
                     "courtyard: no display; use --headless --ticks N --capture file.ppm\n");
        return 3;
    }
    app.finish_gpu();
    if (!last.is_valid())
        return 4;
    if (!image.empty() &&
        !capture(image,
                 read_pixels(*app.device(), app.graph()->physical(last), last_extent),
                 last_extent))
        return 4;
    std::printf("courtyard: %llu thermal ticks, %llu refused, %zu particles, %zu phase surfaces\n",
                static_cast<unsigned long long>(tick),
                static_cast<unsigned long long>(thermal.stats().refused_ticks),
                effects.field().particles().size(),
                fluids_render::phase_surfaces(app.world(), thermal).count);
    return thermal.stats().refused_ticks ? 5 : 0;
}
} // namespace

int main(int argc, char** argv) {
    std::string path = "samples/thermal-courtyard/courtyard.rscene", image, write;
    bool headless = false, test = false, host = false, controls_test = false;
    unsigned ticks = 1800, frames = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--editor-host")
            host = true;
        else if (arg == "--scene" && i + 1 < argc)
            path = argv[++i];
        else if (arg == "--capture" && i + 1 < argc)
            image = argv[++i];
        else if (arg == "--write-demo" && i + 1 < argc)
            write = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) {
            try {
                const auto parsed = std::stoul(argv[++i]);
                if (parsed > 100000)
                    return 2;
                frames = static_cast<unsigned>(parsed);
            } catch (...) {
                return 2;
            }
        } else if (arg == "--ticks" && i + 1 < argc) {
            try {
                const auto parsed = std::stoul(argv[++i]);
                if (parsed > 2160)
                    return 2;
                ticks = static_cast<unsigned>(parsed);
            } catch (...) {
                return 2;
            }
        } else if (arg == "--controls-selftest")
            controls_test = true;
        else if (arg == "--headless")
            headless = true;
        else if (arg == "--selftest")
            test = true;
        else if (arg == "--help") {
            std::puts(
                "thermal-courtyard [--scene courtyard.rscene] [--headless --ticks 0..2160 "
                "--capture frame.ppm]\nSPACE pause / resume; R restart; ESC close. --selftest "
                "checks the saved scene without GPU.\n--write-demo file.rscene generates the "
                "authored scene. --editor-host uses the standard editor channel.");
            return 0;
        }
    }
    if (host)
        return app::run_editor_host(
            argc, argv, courtyard::register_components, "thermal-courtyard", courtyard::prepare);
    if (!write.empty()) {
        ecs::World world;
        courtyard::build_scene(world);
        return scene::save_scene_file(world, write) ? 0 : 1;
    }
    if (test)
        return selftest(path);
    if (ticks > 2160) {
        std::fprintf(stderr, "courtyard: ticks must be 0..2160\n");
        return 2;
    }
    if (controls_test) {
        headless = true;
        ticks = 60;
    }
    return demo(path, headless, ticks, frames, controls_test, image);
}
