// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// rime-blockgen — the vision demo's procedural assembly, emitting a `.rscene` (m13.2c, ADR-0035
// §1).
//
//   rime-blockgen --out block.rscene [--stats] [--cooks]
//
// This is the "procedural assembly script" ADR-0035 named, and it is C++ rather than Rust tooling
// for a reason the format forces: `.rscene` keys every component record by its C++ reflection
// `type_hash`, so a Rust emitter would have to reproduce those hashes across a language boundary.
// A hash that drifts is a clean, loud load error on this side and an invisible mis-author on the
// other. See engine/blockkit/include/rime/blockkit/block.hpp.
//
// NOTHING GENERATED IS COMMITTED. The scene is written into the build directory by a CTest fixture,
// exactly as the `rime fracture` cook fixtures already are — the generator is the source of truth,
// and its determinism is asserted (tests/blockkit) rather than assumed. A checked-in `.rscene`
// would be a second source of truth that drifts silently the first time the prefab changes.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "rime/blockkit/block.hpp"
#include "rime/ecs/world.hpp"
#include "rime/scene/scene_format.hpp"

using namespace rime;

namespace {

void print_cooks() {
    // The cook table, one line per `rime fracture` invocation. Printed rather than executed: the
    // cooks run as CTest fixtures at build time, and tests/blockkit re-reads the produced `.rdest`
    // files to confirm the CMake list still matches this table — so a drifted cook is a red test
    // instead of a quietly smaller block.
    for (const blockkit::CookSpec& c : blockkit::cook_specs()) {
        std::printf("fracture --size %g %g %g --parts %u --seed %llu --name %s  # asset 0x%llx\n",
                    static_cast<double>(c.size_x),
                    static_cast<double>(c.size_y),
                    static_cast<double>(c.size_z),
                    c.parts,
                    static_cast<unsigned long long>(c.seed),
                    c.name,
                    static_cast<unsigned long long>(c.asset));
    }
}

void print_stats(const blockkit::BlockStats& s) {
    std::printf("block: %zu buildings, %zu slabs + %zu crates = %zu destructibles, %zu parts\n",
                s.buildings,
                s.slabs,
                s.crates,
                s.destructibles(),
                s.parts);
    std::printf("       %zu props, %zu point + %zu spot = %zu local lights + %zu sun\n",
                s.props,
                s.point_lights,
                s.spot_lights,
                s.local_lights(),
                s.dir_lights);
    std::printf("       %zu entities total\n", s.entities);
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path out;
    bool stats = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (a == "--stats") {
            stats = true;
        } else if (a == "--cooks") {
            print_cooks();
            return EXIT_SUCCESS;
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: rime-blockgen --out <file.rscene> [--stats] [--cooks]\n");
            return EXIT_SUCCESS;
        } else {
            std::fprintf(stderr,
                         "rime-blockgen: unknown argument '%.*s'\n",
                         static_cast<int>(a.size()),
                         a.data());
            return EXIT_FAILURE;
        }
    }

    if (out.empty()) {
        std::fprintf(stderr, "rime-blockgen: --out <file.rscene> is required\n");
        return EXIT_FAILURE;
    }

    ecs::World world;
    const blockkit::BlockParams params;
    const blockkit::BlockStats produced = blockkit::assemble(world, params);

    // Two independent routes to the same counts: `assemble` reports what it spawned, `predict`
    // derives it from the parameters alone. They agree only if the prefab wired up what it meant
    // to, so disagreeing here is a build failure rather than a note.
    const blockkit::BlockStats expected = blockkit::predict(params);
    if (produced.entities != expected.entities || produced.parts != expected.parts ||
        produced.destructibles() != expected.destructibles()) {
        std::fprintf(stderr,
                     "rime-blockgen: assembled %zu entities / %zu parts, predicted %zu / %zu\n",
                     produced.entities,
                     produced.parts,
                     expected.entities,
                     expected.parts);
        return EXIT_FAILURE;
    }

    if (!out.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
    }
    if (!scene::save_scene_file(world, out)) {
        std::fprintf(stderr, "rime-blockgen: could not write '%s'\n", out.string().c_str());
        return EXIT_FAILURE;
    }

    if (stats) {
        print_stats(produced);
    }
    std::printf("rime-blockgen: wrote %s\n", out.string().c_str());
    return EXIT_SUCCESS;
}
