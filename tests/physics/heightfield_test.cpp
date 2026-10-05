// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the terrain heightfield shape (M19.1, ADR-0060-m19.1-heightfield), all through the
// PhysicsWorld seam:
//
//   * REGISTRATION validates and the store keeps the hull/compound lifecycle (static-only bodies,
//     reject-if-referenced unregister).
//   * RAYCASTS against surfaces with an analytic answer: a flat plane and a tilted plane are
//     piecewise-planar with EVERY triangle on the one plane, so the heightfield IS the plane and
//     the hit must match to float rounding. A sampled PARABOLOID is not, and the bound it must
//     meet is derived below, not guessed.
//   * THE DIAGONAL: one raised corner makes a non-planar cell whose surface depends on which way
//     the cell is split — the test the brick's falsification breaks.
//   * EDGE CASES: rays along cell edges, along the split diagonal, through grid vertices, grazing
//     a ridge from just above / exactly at / just below it, starting outside the tile, starting
//     under the surface.
//   * CONTACTS: bodies rest ON terrain (flat, sloped, a bowl, a gully), measured by an
//     independent raycast as well as by the narrowphase, and the patch grouping behaves.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

using namespace rime;
using namespace rime::physics;

namespace {

// A terrain built by SAMPLING a height function at the grid points and quantising to u16 exactly as
// a 16-bit height map would: q = round((f - offset) / scale).
struct Terrain {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    float dx = 1.0f;
    float dz = 1.0f;
    float scale = 1.0f;
    float offset = 0.0f;
    std::vector<std::uint16_t> samples;
};

Terrain sample_terrain(std::uint32_t columns,
                       std::uint32_t rows,
                       float dx,
                       float dz,
                       float scale,
                       float offset,
                       const std::function<double(double, double)>& f) {
    Terrain t{columns, rows, dx, dz, scale, offset, {}};
    t.samples.resize(std::size_t{columns} * rows);
    for (std::uint32_t j = 0; j < rows; ++j) {
        for (std::uint32_t i = 0; i < columns; ++i) {
            const double h = f(double(i) * dx, double(j) * dz);
            const double q = std::round((h - offset) / scale);
            t.samples[i + columns * j] = static_cast<std::uint16_t>(std::clamp(q, 0.0, 65535.0));
        }
    }
    return t;
}

HeightfieldDesc desc_of(const Terrain& t) {
    HeightfieldDesc d;
    d.samples = t.samples;
    d.columns = t.columns;
    d.rows = t.rows;
    d.cell_size_x = t.dx;
    d.cell_size_z = t.dz;
    d.height_scale = t.scale;
    d.height_offset = t.offset;
    return d;
}

BodyId add_terrain(PhysicsWorld& w,
                   const Terrain& t,
                   core::Vec3 pos = {0.0f, 0.0f, 0.0f},
                   core::Quat q = core::quat_identity(),
                   float friction = 0.5f) {
    const HeightfieldId id = w.register_heightfield(desc_of(t));
    REQUIRE(id.is_valid());
    BodyDesc b;
    b.motion = MotionType::Static;
    b.shape.type = ShapeType::Heightfield;
    b.shape.heightfield = id;
    b.position = pos;
    b.orientation = q;
    b.friction = friction;
    const BodyId body = w.create_body(b);
    REQUIRE(body.is_valid());
    return body;
}

bool cast(const PhysicsWorld& w, core::Vec3 o, core::Vec3 d, RayHit& hit, float tmax = 1000.0f) {
    Ray r;
    r.origin = o;
    r.direction = d;
    r.max_distance = tmax;
    return w.raycast(r, hit);
}

// Exact ray-plane distance for the plane y = c + a*x + b*z, in double.
double ray_plane_t(core::Vec3 o, core::Vec3 d_raw, double a, double b, double c) {
    const double len = std::sqrt(double(d_raw.x) * d_raw.x + double(d_raw.y) * d_raw.y +
                                 double(d_raw.z) * d_raw.z);
    const double dx = d_raw.x / len, dy = d_raw.y / len, dz = d_raw.z / len;
    // o.y + t*dy = c + a*(o.x + t*dx) + b*(o.z + t*dz)
    return (c + a * o.x + b * o.z - o.y) / (dy - a * dx - b * dz);
}

BodyId add_sphere(PhysicsWorld& w, core::Vec3 pos, float r, float friction = 0.5f) {
    BodyDesc b;
    b.shape.type = ShapeType::Sphere;
    b.shape.radius = r;
    b.position = pos;
    b.friction = friction;
    return w.create_body(b);
}

float max_penetration(const PhysicsWorld& w) {
    std::vector<Manifold> ms;
    w.compute_contacts(ms);
    float worst = 0.0f;
    for (const Manifold& m : ms) {
        for (std::uint8_t k = 0; k < m.count; ++k) {
            worst = std::max(worst, m.points[k].penetration);
        }
    }
    return worst;
}

} // namespace

// ── Registration ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("heightfield registration validates, and the store keeps the shape lifecycle") {
    PhysicsWorld w;
    const std::vector<std::uint16_t> four{0, 1, 2, 3};
    HeightfieldDesc d;
    d.samples = four;
    d.columns = 2;
    d.rows = 2;

    HeightfieldDesc bad = d;
    bad.columns = 1;
    CHECK_FALSE(w.register_heightfield(bad).is_valid()); // a line is not a surface
    bad = d;
    bad.rows = 3; // span holds 4, not 6
    CHECK_FALSE(w.register_heightfield(bad).is_valid());
    bad = d;
    bad.cell_size_x = 0.0f;
    CHECK_FALSE(w.register_heightfield(bad).is_valid());
    bad = d;
    bad.height_scale = -1.0f;
    CHECK_FALSE(w.register_heightfield(bad).is_valid());
    bad = d;
    bad.thickness = -0.5f;
    CHECK_FALSE(w.register_heightfield(bad).is_valid());

    const HeightfieldId id = w.register_heightfield(d);
    REQUIRE(id.is_valid());
    HeightfieldInfo info;
    REQUIRE(w.heightfield_info(id, info));
    CHECK(info.columns == 2);
    CHECK(info.rows == 2);
    CHECK(info.triangle_count == 2);
    CHECK(info.local_bounds.min.y == doctest::Approx(0.0f - 1.0f)); // min sample 0, thickness 1
    CHECK(info.local_bounds.max.y == doctest::Approx(3.0f));

    // Terrain is static-only.
    BodyDesc b;
    b.shape.type = ShapeType::Heightfield;
    b.shape.heightfield = id;
    b.motion = MotionType::Dynamic;
    CHECK_FALSE(w.create_body(b).is_valid());
    b.motion = MotionType::Kinematic;
    CHECK_FALSE(w.create_body(b).is_valid());
    b.motion = MotionType::Static;
    const BodyId body = w.create_body(b);
    REQUIRE(body.is_valid());

    // Reject-if-referenced, then free, then the stale id reads dead.
    CHECK_FALSE(w.unregister_heightfield(id));
    w.destroy_body(body);
    CHECK(w.unregister_heightfield(id));
    CHECK_FALSE(w.heightfield_info(id, info));
    CHECK_FALSE(w.unregister_heightfield(id));
    b.shape.heightfield = id;
    CHECK_FALSE(w.create_body(b).is_valid());
}

// ── Raycasts against analytic surfaces ───────────────────────────────────────────────────────

TEST_CASE("heightfield raycast: a flat plane is hit exactly, on cells, edges and vertices") {
    // Every sample 0 with offset 2: the surface is y = 2 exactly (2 is representable, and so is
    // every t below — the origins sit on dyadic coordinates).
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(9, 7, 1.0f, 1.0f, 0.5f, 2.0f, [](double, double) { return 2.0; });
    const BodyId terrain = add_terrain(w, t);
    for (const float x : {0.0f, 0.25f, 1.0f, 3.5f, 4.0f, 7.75f, 8.0f}) {
        for (const float z : {0.0f, 0.5f, 2.0f, 5.25f, 6.0f}) {
            RayHit hit;
            REQUIRE_MESSAGE(cast(w, {x, 10.0f, z}, {0.0f, -1.0f, 0.0f}, hit), x, ",", z);
            CHECK(hit.body == terrain);
            CHECK(hit.distance == 8.0f);
            CHECK(hit.point.y == 2.0f);
            CHECK(hit.normal.y == 1.0f);
        }
    }
}

TEST_CASE("heightfield raycast: a tilted plane is hit where the analytic plane is") {
    // q = 3i + 5j with scale 0.25 and offset 1 on a unit grid: y = 1 + 0.75 x + 1.25 z, and every
    // triangle lies on that one plane — so the heightfield is EXACTLY the plane, whatever the
    // diagonal, and a ray must hit where the analytic plane says, to float rounding.
    PhysicsWorld w;
    const double a = 0.75, b = 1.25, c = 1.0;
    const Terrain t = sample_terrain(
        12, 10, 1.0f, 1.0f, 0.25f, 1.0f, [&](double x, double z) { return c + a * x + b * z; });
    add_terrain(w, t);
    const core::Vec3 expected_n = core::normalize(core::Vec3{-0.75f, 1.0f, -1.25f});

    // Each ray is aimed at a TARGET point on the plane and starts 12 m back along its direction —
    // so it provably lands inside the tile, and a miss is a bug, not a geometry slip.
    struct Case {
        float tx, tz; // target (x, z); y is the plane's
        core::Vec3 d;
        const char* what;
    };

    const Case aims[] = {
        {3.3f, 2.7f, {0.0f, -1.0f, 0.0f}, "vertical, cell interior"},
        {7.2f, 5.3f, {1.0f, -1.5f, 0.7f}, "oblique, walks many cells"},
        {2.8f, 1.9f, {-0.6f, -2.0f, -0.9f}, "oblique, negative x/z steps"},
        {6.4f, 3.0f, {1.0f, -1.0f, 0.0f}, "along a grid line z = 3 (a cell edge)"},
        {5.0f, 6.5f, {0.0f, -1.0f, 1.0f}, "along the grid line x = 5"},
        {7.0f, 7.0f, {1.0f, -1.2f, 1.0f}, "along the split diagonal, through vertices"},
        {6.0f, 2.0f, {1.0f, -1.5f, -1.0f}, "along the anti-diagonal, vertex to vertex"},
        {4.0f, 4.0f, {0.0f, -1.0f, 0.0f}, "vertical, exactly on a grid vertex"},
        {6.5f, 5.5f, {1.0f, -2.0f, 1.0f}, "exactly THROUGH grid vertices, obliquely"},
        {0.5f, 3.5f, {1.0f, -0.4f, 0.2f}, "enters the tile through its side"},
    };

    struct Ray3 {
        core::Vec3 o;
        core::Vec3 d;
        const char* what;
    };

    std::vector<Ray3> rays;
    for (const Case& k : aims) {
        const core::Vec3 target{k.tx, float(c + a * k.tx + b * k.tz), k.tz};
        rays.push_back({target - core::normalize(k.d) * 12.0f, k.d, k.what});
    }
    for (const Ray3& k : rays) {
        const std::string what = k.what;
        CAPTURE(what);
        RayHit hit;
        CHECK(cast(w, k.o, k.d, hit));
        if (!cast(w, k.o, k.d, hit)) {
            continue;
        }
        const double te = ray_plane_t(k.o, k.d, a, b, c);
        CHECK(std::fabs(hit.distance - te) <= 1e-5 + 2e-6 * te);
        CHECK(hit.point.y == doctest::Approx(c + a * hit.point.x + b * hit.point.z).epsilon(1e-5));
        CHECK(core::dot(hit.normal, expected_n) > 0.99999f);
    }
}

TEST_CASE("heightfield raycast: a sampled paraboloid is hit within the derived error bound") {
    // f(x, z) = k((x-cx)² + (z-cz)²), sampled at the grid points and quantised to u16. Two error
    // sources separate the heightfield from f, and each is bounded exactly:
    //
    //  (1) INTERPOLATION. Over one triangle the heightfield is the linear interpolant L of f at the
    //      triangle's corners. For f = k|p|² that interpolant has a closed form: f − L vanishes at
    //      the three corners, and since f − L is k|p|² minus a linear function it is −k times
    //      (|p − c|² − R²) with c, R the triangle's CIRCUMcentre and circumradius. So
    //          L(p) − f(p) = k (R² − |p − c|²),   0 <= L − f <= k R²,
    //      attained at the circumcentre. Our triangles are right triangles with legs dx, dz, whose
    //      circumcentre is the hypotenuse midpoint — the CELL CENTRE (on the split diagonal) — and
    //      R² = (dx² + dz²) / 4.
    //  (2) QUANTISATION. Each corner height was rounded to the nearest step, an error of at most
    //      scale/2; a linear interpolant of values each off by <= e is off by <= e everywhere.
    //
    // So at every hit: −scale/2 <= y_hit − f(x_hit, z_hit) <= k (dx² + dz²)/4 + scale/2 (plus float
    // slack), and a vertical ray through a cell centre sits at the TOP of that bound — which the
    // test also asserts, so the bound is shown to be tight and not merely true.
    const double k = 0.05, cx = 16.0, cz = 12.0;
    const float dx = 1.0f, dz = 0.75f, scale = 0.01f;
    const auto f = [&](double x, double z) {
        return k * ((x - cx) * (x - cx) + (z - cz) * (z - cz));
    };
    PhysicsWorld w;
    const Terrain t = sample_terrain(33, 33, dx, dz, scale, 0.0f, f);
    add_terrain(w, t);
    const double interp_bound = k * (double(dx) * dx + double(dz) * dz) / 4.0;
    const double quant = scale / 2.0;
    const double slack = 1e-4; // float rounding of heights ~ 0..40 m
    const double upper = interp_bound + quant + slack;
    const double lower = -quant - slack;

    int n = 0;
    double worst_hi = -1.0;
    for (int ix = 0; ix < 29; ++ix) {
        for (int iz = 0; iz < 29; ++iz) {
            // Scattered vertical and oblique rays.
            const core::Vec3 o{0.7f + 1.07f * float(ix), 25.0f, 0.4f + 0.81f * float(iz)};
            const core::Vec3 dirs[] = {
                {0.0f, -1.0f, 0.0f}, {0.31f, -1.0f, -0.17f}, {-0.4f, -0.8f, 0.5f}};
            for (const core::Vec3 d : dirs) {
                RayHit hit;
                if (!cast(w, o, d, hit)) {
                    continue; // oblique rays near the rim may leave the tile before landing
                }
                const double e = double(hit.point.y) - f(hit.point.x, hit.point.z);
                CHECK(e <= upper);
                CHECK(e >= lower);
                worst_hi = std::max(worst_hi, e);
                ++n;
            }
        }
    }
    CHECK(n > 1500); // ~80% of the 2523 rays land inside the tile

    // Tightness: straight down through cell centres (the circumcentres), where L − f = k R².
    for (std::uint32_t ci = 2; ci < 30; ci += 3) {
        for (std::uint32_t cj = 2; cj < 30; cj += 3) {
            const float x = (float(ci) + 0.5f) * dx;
            const float z = (float(cj) + 0.5f) * dz;
            RayHit hit;
            REQUIRE(cast(w, {x, 60.0f, z}, {0.0f, -1.0f, 0.0f}, hit));
            const double e = double(hit.point.y) - f(x, z);
            CHECK(e <= upper);
            CHECK(e >= interp_bound - quant - slack);
        }
    }
}

TEST_CASE("heightfield raycast: the cell diagonal is (i,j)→(i+1,j+1), as the format records") {
    // One cell, one raised corner: h00 = h10 = h01 = 0, h11 = 1. The four corners are not
    // coplanar, so the surface depends on the split. With the v00–v11 diagonal:
    //   triangle A (x >= z): y = z        triangle B (x < z): y = x
    // (the planes through (v00, v10, v11) and (v00, v11, v01)). With the OTHER diagonal (v10–v01)
    // the triangle (v00, v10, v01) would be flat at 0 wherever x + z < 1. Probing inside that
    // region tells the two apart — this is the test the brick's falsification breaks.
    PhysicsWorld w;
    const std::vector<std::uint16_t> s{0, 0, 0, 1};
    HeightfieldDesc d;
    d.samples = s;
    d.columns = 2;
    d.rows = 2;
    const HeightfieldId id = w.register_heightfield(d);
    BodyDesc b;
    b.motion = MotionType::Static;
    b.shape.type = ShapeType::Heightfield;
    b.shape.heightfield = id;
    REQUIRE(w.create_body(b).is_valid());

    struct Probe {
        float x, z, y;
    };

    const Probe probes[] = {
        {0.8f, 0.1f, 0.1f},    // A, below the anti-diagonal: other split says 0
        {0.1f, 0.8f, 0.1f},    // B, below the anti-diagonal: other split says 0
        {0.5f, 0.5f, 0.5f},    // on the diagonal: other split says 0
        {0.3f, 0.2f, 0.2f},    // A
        {0.9f, 0.6f, 0.6f},    // A, above the anti-diagonal
        {0.25f, 0.75f, 0.25f}, // B, ON the anti-diagonal: other split says 0
    };
    for (const Probe& p : probes) {
        CAPTURE(p.x);
        CAPTURE(p.z);
        RayHit hit;
        REQUIRE(cast(w, {p.x, 5.0f, p.z}, {0.0f, -1.0f, 0.0f}, hit));
        CHECK(hit.point.y == doctest::Approx(p.y).epsilon(1e-6));
    }
    // And the normal of triangle A is the plane y = z's: (0, 1, −1)/√2.
    RayHit hit;
    REQUIRE(cast(w, {0.8f, 5.0f, 0.1f}, {0.0f, -1.0f, 0.0f}, hit));
    CHECK(hit.normal.z == doctest::Approx(-std::sqrt(0.5f)));
    CHECK(hit.normal.x == doctest::Approx(0.0f));
}

TEST_CASE("heightfield raycast: the surface is watertight at every edge and vertex") {
    // A bumpy, non-planar terrain (pseudo-random samples at an awkward scale, so neighbouring
    // triangles' planes evaluate their shared edges with genuinely different rounding). Every
    // downward ray that starts above the tile and lands within its footprint MUST hit — the
    // surface is closed — including rays aimed exactly at grid vertices, at edge midpoints, and
    // along the diagonal and grid lines, which is where a walk that tests triangles independently
    // leaks through the seams.
    PhysicsWorld w;
    Terrain t{17, 13, 0.37f, 0.53f, 0.0137f, -3.1f, {}};
    std::uint32_t state = 12345u;
    for (std::uint32_t k = 0; k < t.columns * t.rows; ++k) {
        state = state * 1664525u + 1013904223u;
        // 0..15 steps: a ~0.2 m bumpy range, gentler than every ray below is steep, so a ray
        // starting metres above cannot pass UNDER a bump without first crossing down into it.
        t.samples.push_back(static_cast<std::uint16_t>(state >> 28));
    }
    add_terrain(w, t);
    int misses = 0;
    int casts = 0;
    // Interior vertices only: at the tile's own rim a ray can meet the footprint in a single
    // boundary point, and whether rounding lands it a hair inside or outside is not a seam.
    for (std::uint32_t j = 1; j + 1 < t.rows; ++j) {
        for (std::uint32_t i = 1; i + 1 < t.columns; ++i) {
            const float x = t.dx * float(i);
            const float z = t.dz * float(j);
            // Vertex, the two edge midpoints, the diagonal midpoint — straight down, and obliquely
            // from four directions aimed at the same point.
            const core::Vec3 aims[] = {{x, 0.0f, z},
                                       {x + 0.5f * t.dx, 0.0f, z},
                                       {x, 0.0f, z + 0.5f * t.dz},
                                       {x + 0.5f * t.dx, 0.0f, z + 0.5f * t.dz}};
            const core::Vec3 dirs[] = {{t.dx, -1.0f, t.dz},
                                       {-t.dx, -1.0f, t.dz},
                                       {t.dx, -2.0f, 0.0f},
                                       {0.0f, -2.0f, -t.dz}};
            for (const core::Vec3 aim : aims) {
                // Straight down first: the surface height at the aim point.
                RayHit down;
                ++casts;
                if (!cast(w, {aim.x, 100.0f, aim.z}, {0.0f, -1.0f, 0.0f}, down)) {
                    ++misses;
                    continue;
                }
                // Then obliquely, aimed at exactly that surface point from 5 m back: the ray is
                // above the surface until it gets there, so it must hit at (or before) it.
                for (const core::Vec3 d : dirs) {
                    const core::Vec3 dn = core::normalize(d);
                    const core::Vec3 o = down.point - dn * 5.0f;
                    RayHit hit;
                    ++casts;
                    if (!cast(w, o, d, hit, 5.01f)) {
                        ++misses;
                    }
                }
            }
        }
    }
    CHECK(casts == 15 * 11 * 4 * 5);
    CHECK(misses == 0);
}

TEST_CASE("heightfield raycast: grazing, one-sidedness, and rays that never reach the tile") {
    // A ridge along x = 2: heights 0, 0, 1, 0, 0 across columns (q = 0 or 100, scale 0.01).
    PhysicsWorld w;
    const Terrain t = sample_terrain(
        5, 4, 1.0f, 1.0f, 0.01f, 0.0f, [](double x, double) { return x == 2.0 ? 1.0 : 0.0; });
    add_terrain(w, t);
    RayHit hit;

    SUBCASE("a horizontal ray across the ridge: above misses, exactly-at touches, below hits") {
        CHECK_FALSE(cast(w, {-1.0f, 1.001f, 1.5f}, {1.0f, 0.0f, 0.0f}, hit));
        REQUIRE(cast(w, {-1.0f, 1.0f, 1.5f}, {1.0f, 0.0f, 0.0f}, hit));
        CHECK(hit.point.x == doctest::Approx(2.0f).epsilon(1e-6)); // touches the ridge line
        REQUIRE(cast(w, {-1.0f, 0.999f, 1.5f}, {1.0f, 0.0f, 0.0f}, hit));
        CHECK(hit.point.x == doctest::Approx(1.999f).epsilon(1e-4)); // on the rising flank
    }
    SUBCASE("a ray lying in the flat part of the surface touches at its first point") {
        // y = 0 exactly, over the flat strip x in [0, 1]: the ray is IN the surface.
        REQUIRE(cast(w, {0.0f, 0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}, hit));
        CHECK(hit.distance == 0.0f);
    }
    SUBCASE("one-sided: a ray starting beneath the surface never hits it on the way out") {
        CHECK_FALSE(cast(w, {0.5f, -0.5f, 1.5f}, {0.0f, 1.0f, 0.0f}, hit));
        CHECK_FALSE(cast(w, {2.0f, 0.5f, 1.5f}, {0.3f, 1.0f, 0.0f}, hit)); // inside the ridge
    }
    SUBCASE("rays outside the tile's bounds miss cleanly") {
        CHECK_FALSE(cast(w, {-1.0f, 5.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, hit));      // beside, x < 0
        CHECK_FALSE(cast(w, {2.0f, 5.0f, 3.5f}, {0.0f, -1.0f, 0.0f}, hit));       // beyond z = 3
        CHECK_FALSE(cast(w, {-1.0f, 5.0f, 1.0f}, {-1.0f, -1.0f, 0.0f}, hit));     // pointing away
        CHECK_FALSE(cast(w, {0.0f, 2.0f, 0.0f}, {1.0f, 0.1f, 1.0f}, hit));        // climbing above
        CHECK_FALSE(cast(w, {2.0f, 5.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, hit, 3.9f)); // too short
        REQUIRE(cast(w, {2.0f, 5.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, hit, 4.1f));
        CHECK(hit.distance == doctest::Approx(4.0f));
    }
}

TEST_CASE("heightfield raycast: a posed (moved + yawed) tile is hit in world space") {
    // The tilted plane again, placed at (100, -3, 50) and yawed 90° about +Y. A world point p maps
    // to local l = R⁻¹(p − pos); rather than re-derive the rotated plane, cast the local-frame ray
    // and the world-frame ray and require the same distance and the same hit point.
    const double a = 0.75, b = 1.25, c = 1.0;
    const Terrain t = sample_terrain(
        12, 10, 1.0f, 1.0f, 0.25f, 1.0f, [&](double x, double z) { return c + a * x + b * z; });
    PhysicsWorld local;
    add_terrain(local, t);
    PhysicsWorld posed;
    const core::Vec3 pos{100.0f, -3.0f, 50.0f};
    const core::Quat q = core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5707963f);
    add_terrain(posed, t, pos, q);

    const core::Vec3 lo{4.3f, 15.0f, 2.2f};
    const core::Vec3 ld{0.3f, -1.0f, 0.4f};
    RayHit hl;
    RayHit hp;
    REQUIRE(cast(local, lo, ld, hl));
    REQUIRE(cast(posed, pos + core::rotate(q, lo), core::rotate(q, ld), hp));
    CHECK(hp.distance == doctest::Approx(hl.distance).epsilon(1e-5));
    const core::Vec3 back = core::rotate(core::conjugate(q), hp.point - pos);
    CHECK(back.x == doctest::Approx(hl.point.x).epsilon(1e-4));
    CHECK(back.y == doctest::Approx(hl.point.y).epsilon(1e-4));
    CHECK(back.z == doctest::Approx(hl.point.z).epsilon(1e-4));
    CHECK(core::dot(core::rotate(core::conjugate(q), hp.normal), hl.normal) > 0.99999f);
}

// ── Contacts: bodies rest on terrain ─────────────────────────────────────────────────────────

TEST_CASE("heightfield contact: a sphere resting on flat terrain stays put") {
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(17, 17, 1.0f, 1.0f, 0.5f, 2.0f, [](double, double) { return 2.0; });
    add_terrain(w, t);
    // Placed exactly on a grid VERTEX, where six triangles meet — the case where every one of them
    // finds the same contact and the dedup must collapse them to one.
    const core::Vec3 start{8.0f, 2.0f + 0.5f, 8.0f};
    const BodyId s = add_sphere(w, start, 0.5f);
    for (int i = 0; i < 120; ++i) {
        w.step(1.0f / 60.0f);
    }
    BodyState st;
    REQUIRE(w.get_body_state(s, st));
    CHECK(core::length(st.position - start) < 5e-3f);
    CHECK(std::fabs(st.position.x - start.x) < 1e-5f); // no sideways push from the seams
    CHECK(std::fabs(st.position.z - start.z) < 1e-5f);
    CHECK(w.stats().heightfield_patch_merges == 0);
    std::vector<Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1); // one patch...
    CHECK(ms[0].count == 1); // ...holding one point, not six copies of it
}

TEST_CASE("heightfield contact: a sphere dropped on a sampled bowl rolls to the bottom and rests") {
    // The paraboloid from the raycast test, as a bowl. Dropped high on its slope, the ball rolls
    // down, oscillates, and — with some damping so the test finishes — settles at the bottom.
    const double k = 0.05, cx = 16.0, cz = 12.0;
    PhysicsWorld w;
    const Terrain t = sample_terrain(33, 33, 1.0f, 0.75f, 0.01f, 0.0f, [&](double x, double z) {
        return k * ((x - cx) * (x - cx) + (z - cz) * (z - cz));
    });
    add_terrain(w, t, {0.0f, 0.0f, 0.0f}, core::quat_identity(), 0.8f);
    BodyDesc b;
    b.shape.type = ShapeType::Sphere;
    b.shape.radius = 0.4f;
    b.position = {22.0f, 5.0f, 9.0f}; // on the slope, above f(22, 9) = 2.25
    b.friction = 0.8f;
    b.linear_damping = 0.3f;
    b.angular_damping = 0.6f;
    const BodyId s = w.create_body(b);
    for (int i = 0; i < 60 * 40; ++i) {
        w.step(1.0f / 60.0f);
    }
    BodyState st;
    REQUIRE(w.get_body_state(s, st));
    CHECK(core::length(st.linear_velocity) < 0.05f);
    CHECK(std::fabs(st.position.x - float(cx)) < 0.5f); // near the bottom
    CHECK(std::fabs(st.position.z - float(cz)) < 0.5f);

    // Resting ON the surface: independently, a ray straight down from the centre meets the ground
    // one radius below (within the solver's 5 mm penetration slop and a little), and the
    // narrowphase's own measured penetration is under a centimetre.
    RayHit hit;
    Ray r;
    r.origin = st.position;
    r.direction = {0.0f, -1.0f, 0.0f};
    QueryFilter statics_only;
    statics_only.dynamics = false;
    REQUIRE(w.raycast(r, hit, statics_only));
    CHECK(hit.distance == doctest::Approx(0.4f).epsilon(0.03));
    CHECK(max_penetration(w) < 0.01f);
}

TEST_CASE("heightfield contact: a sphere in a gully touches both walls — two patches") {
    // A V-shaped gully along Z: y = |x − 4| * 0.5. A ball resting in it touches both slopes, whose
    // normals differ by ~53° — one manifold cannot hold both, so the terrain path emits two patches
    // with two distinct patch ids (and two contact events).
    PhysicsWorld w;
    const Terrain t = sample_terrain(
        9, 9, 1.0f, 1.0f, 0.5f, 0.0f, [](double x, double) { return std::fabs(x - 4.0) * 0.5; });
    add_terrain(w, t);
    const BodyId s = add_sphere(w, {4.0f, 1.5f, 4.3f}, 0.5f);
    for (int i = 0; i < 240; ++i) {
        w.step(1.0f / 60.0f);
    }
    BodyState st;
    REQUIRE(w.get_body_state(s, st));
    // Resting where a 0.5 m ball fits a slope-0.5 V: centre height r·√(1 + 0.25)/1 above the
    // vertex. Centred to within a few mm: the geometry is symmetric, but the sequential-impulse
    // solve of the two walls is not (one patch is solved first), and friction then holds the small
    // offset.
    CHECK(std::fabs(st.position.x - 4.0f) < 1e-2f);
    CHECK(st.position.y == doctest::Approx(0.5 * std::sqrt(1.25)).epsilon(0.02));
    std::vector<Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 2);
    CHECK(ms[0].patch != ms[1].patch);
    CHECK(ms[0].normal.x * ms[1].normal.x < 0.0f); // the two walls lean opposite ways
}

TEST_CASE("heightfield contact: a box rests level on flat terrain across many cells") {
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(17, 17, 0.5f, 0.5f, 0.5f, 1.0f, [](double, double) { return 1.0; });
    add_terrain(w, t);
    BodyDesc b;
    b.shape.type = ShapeType::Box;
    b.shape.half_extents = {1.3f, 0.4f, 0.9f}; // spans ~6 x 4 cells, corners mid-cell
    b.position = {4.1f, 1.0f + 0.4f + 0.05f, 3.9f};
    const BodyId box = w.create_body(b);
    for (int i = 0; i < 180; ++i) {
        w.step(1.0f / 60.0f);
    }
    BodyState st;
    REQUIRE(w.get_body_state(box, st));
    CHECK(st.position.y == doctest::Approx(1.4f).epsilon(0.005));
    CHECK(std::fabs(st.position.x - 4.1f) < 2e-3f);
    CHECK(std::fabs(st.position.z - 3.9f) < 2e-3f);
    CHECK(std::fabs(st.orientation.w) > 0.99999f); // still level
    // Flat ground is ONE patch, reduced to four points like a box on a box floor.
    std::vector<Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    CHECK(ms[0].count == 4);
    CHECK(max_penetration(w) < 0.01f);
}

TEST_CASE("heightfield contact: a box on a slope steeper than zero but under the friction angle "
          "holds") {
    // y = 0.3 z (≈16.7°), μ = 0.9 on both: tan θ = 0.3 < 0.9, so static friction holds the box.
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(13, 13, 1.0f, 1.0f, 0.1f, 0.0f, [](double, double z) { return 0.3 * z; });
    add_terrain(w, t, {0.0f, 0.0f, 0.0f}, core::quat_identity(), 0.9f);
    const float theta = std::atan(0.3f);
    BodyDesc b;
    b.shape.type = ShapeType::Box;
    b.shape.half_extents = {0.5f, 0.25f, 0.5f};
    b.orientation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -theta); // lies on the slope
    const core::Vec3 up = core::rotate(b.orientation, core::Vec3{0.0f, 1.0f, 0.0f});
    b.position = core::Vec3{6.0f, 0.3f * 6.0f, 6.0f} + up * 0.26f;
    b.friction = 0.9f;
    const BodyId box = w.create_body(b);
    for (int i = 0; i < 180; ++i) {
        w.step(1.0f / 60.0f);
    }
    BodyState st;
    REQUIRE(w.get_body_state(box, st));
    CHECK(core::length(st.position - b.position) < 0.02f);
    CHECK(max_penetration(w) < 0.01f);
}

TEST_CASE("heightfield contact: a capsule lies flat and a compound stands on its feet") {
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(17, 17, 1.0f, 1.0f, 0.5f, 0.0f, [](double, double) { return 0.0; });
    add_terrain(w, t);

    BodyDesc cap;
    cap.shape.type = ShapeType::Capsule;
    cap.shape.radius = 0.3f;
    cap.shape.half_height = 1.0f;
    cap.orientation = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707963f); // lying along X
    cap.position = {4.3f, 0.35f, 4.4f};
    const BodyId c = w.create_body(cap);

    const CompoundChildDesc feet[2] = {
        {ShapeDesc{ShapeType::Box, 0.5f, {0.2f, 0.2f, 0.2f}}, {-1.0f, 0.0f, 0.0f}},
        {ShapeDesc{ShapeType::Sphere, 0.2f}, {1.0f, 0.0f, 0.0f}},
    };
    const CompoundId cid = w.register_compound(CompoundDesc{feet});
    REQUIRE(cid.is_valid());
    BodyDesc dumbbell;
    dumbbell.shape.type = ShapeType::Compound;
    dumbbell.shape.compound = cid;
    dumbbell.position = {10.4f, 0.25f, 10.6f};
    const BodyId d = w.create_body(dumbbell);

    for (int i = 0; i < 240; ++i) {
        w.step(1.0f / 60.0f);
    }
    BodyState cs;
    BodyState ds;
    REQUIRE(w.get_body_state(c, cs));
    REQUIRE(w.get_body_state(d, ds));
    CHECK(cs.position.y == doctest::Approx(0.3f).epsilon(0.03));
    CHECK(ds.position.y == doctest::Approx(0.2f).epsilon(0.05));
    CHECK(max_penetration(w) < 0.01f);
    // The capsule lying flat is held by at least TWO points — its end spheres, plus the shaft's
    // closest approaches to the grid edges it lies across — so it does not seesaw on one.
    std::vector<Manifold> ms;
    w.compute_contacts(ms);
    bool saw_capsule = false;
    bool saw_foot_box = false;
    bool saw_foot_sphere = false;
    for (const Manifold& m : ms) {
        const BodyId other = m.a == c || m.b == c ? c : d;
        if (other == c) {
            saw_capsule = true;
            CHECK(m.count >= 2);
        } else {
            const std::uint16_t child = m.a == d ? m.child_a : m.child_b;
            saw_foot_box = saw_foot_box || child == 0;
            saw_foot_sphere = saw_foot_sphere || child == 1;
        }
    }
    CHECK(saw_capsule);
    CHECK(saw_foot_box);
    CHECK(saw_foot_sphere);
}

// ── Queries, counters, determinism ───────────────────────────────────────────────────────────

TEST_CASE("heightfield: overlap_sphere sees terrain, and no query skips the tile any more") {
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(9, 9, 1.0f, 1.0f, 0.5f, 0.0f, [](double, double) { return 1.0; });
    const BodyId terrain = add_terrain(w, t);
    std::vector<BodyId> hits;
    w.overlap_sphere({4.0f, 1.4f, 4.0f}, 0.5f, hits);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == terrain);
    w.overlap_sphere({4.0f, 1.6f, 4.0f}, 0.5f, hits);
    CHECK(hits.empty());

    // M19.1 asserted here that shape_cast and penetration SKIPPED terrain and counted it. m19.2
    // closes both, so the assertion inverts: the queries answer, and the skip counter stays at
    // zero. That counter is now reserved for one thing only — a candidate set truncated by the
    // per-query triangle budget — which is why the flat terrain below never trips it.
    CHECK(w.heightfield_query_skips() == 0);

    ShapeCast sc;
    sc.shape.type = ShapeType::Sphere;
    sc.shape.radius = 0.5f;
    sc.origin = {4.0f, 5.0f, 4.0f};
    sc.direction = {0.0f, -1.0f, 0.0f};
    sc.max_distance = 10.0f;
    ShapeHit sh;
    REQUIRE(w.shape_cast(sc, sh));
    CHECK(sh.body == terrain);
    CHECK_FALSE(sh.initial_overlap);
    // The sphere's surface reaches the plane at y = 1 when its centre is at y = 1.5, so it has
    // travelled 5 - 1.5 = 3.5 m. This is the whole point of a shape cast over a ray cast: the
    // reported distance is where the SHAPE touches, not where its centre line crosses.
    CHECK(sh.distance == doctest::Approx(3.5f).epsilon(1e-4));
    CHECK(sh.normal.y == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(sh.point.y == doctest::Approx(1.0f).epsilon(1e-3));

    // A sphere already buried in the ground: the cast has no time of impact to report, so it
    // reports the body at distance 0 with `initial_overlap` set — the flag that tells a caller to
    // run `penetration` instead. (Returning false would lose WHICH body it is inside, which is
    // the one thing the caller needs next; the convex path has always answered this way.)
    sc.origin = {4.25f, 0.9f, 4.75f};
    ShapeHit sh2;
    REQUIRE(w.shape_cast(sc, sh2));
    CHECK(sh2.initial_overlap);
    CHECK(sh2.distance == 0.0f);
    CHECK(sh2.body == terrain);
    ShapeDesc probe;
    probe.type = ShapeType::Sphere;
    probe.radius = 0.5f;
    PenetrationHit ph;
    REQUIRE(w.penetration(probe, {4.25f, 0.9f, 4.75f}, core::quat_identity(), ph));
    CHECK(ph.body == terrain);
    // Centre 0.1 below a surface at y = 1 with radius 0.5 ⇒ the sphere's lowest point is 0.6 under
    // the surface, and the way out is STRAIGHT UP.
    //
    // The depth is 0.6, not the 0.4 a per-triangle EPA would report. That difference is the point
    // of the rule this query inherits from the contact build: 0.4 is the shortest way out of the
    // zero-thickness TRIANGLE (downward, since only 0.4 of the sphere sticks up through it), and
    // acting on it would drive a character controller into the rock. 0.6 is the way out of the
    // GROUND. See the header comment on penetration_vs_heightfield_local.
    CHECK(ph.depth == doctest::Approx(0.6f).epsilon(1e-3));
    CHECK(ph.normal.y == doctest::Approx(1.0f).epsilon(1e-3));
    // Clear of the ground is not a penetration, and must not be reported as a shallow one.
    CHECK_FALSE(w.penetration(probe, {4.25f, 1.6f, 4.75f}, core::quat_identity(), ph));
    CHECK(w.heightfield_query_skips() == 0);
}

TEST_CASE("heightfield: a shape sunk exactly on a vertex or diagonal gets NO depenetration") {
    // A LIMITATION, pinned down so it is a known gap with a gate on it rather than a surprise.
    // It is inherited, not introduced: the below-plane branch of the contact build
    // (`sphere_vs_triangle`) only accepts a shape whose centre projects into a triangle's
    // INTERIOR, because a centre outside the triangle "belongs to a neighbour whose face region
    // does contain it" — and on a grid VERTEX, or on a cell's DIAGONAL, no triangle's interior
    // contains it, so every candidate declines and the deepest-contact reduction has nothing to
    // reduce. Writing the test above walked into it twice in a row (x = 4, z = 4 is a vertex of a
    // unit grid; x = 4.5, z = 4.5 is on the min→max diagonal), which is how it was found.
    //
    // It affects DEPENETRATION ONLY. A body resting ON terrain is handled by the above-plane
    // path, which does accept edge and vertex features — the watertightness and resting proofs
    // cover exactly those points and pass. So the reachable symptom is narrow: a body already
    // sunk below the surface, with its centre on a grid line, is told it is not penetrating and
    // stays there. The fix belongs to the contact rule, not to this query (accepting a boundary
    // feature means not double-counting it across the triangles that share it, which is what
    // `push_unique`'s dedup exists for), so it is named in ADR-0061 and left for the brick that
    // owns the controller.
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(9, 9, 1.0f, 1.0f, 0.5f, 0.0f, [](double, double) { return 1.0; });
    add_terrain(w, t);
    ShapeDesc probe;
    probe.type = ShapeType::Sphere;
    probe.radius = 0.5f;
    PenetrationHit ph;

    // Interior of a cell half: answered, and the depth is the way out of the GROUND.
    REQUIRE(w.penetration(probe, {4.25f, 0.9f, 4.75f}, core::quat_identity(), ph));
    CHECK(ph.depth == doctest::Approx(0.6f).epsilon(1e-3));

    // Exactly on a grid vertex, and exactly on a cell diagonal: not answered. These two CHECKs
    // are the gate — if a later brick fixes the contact rule they go red, which is the signal to
    // come back here and promote them.
    CHECK_FALSE(w.penetration(probe, {4.0f, 0.9f, 4.0f}, core::quat_identity(), ph));
    CHECK_FALSE(w.penetration(probe, {4.5f, 0.9f, 4.5f}, core::quat_identity(), ph));

    // And the gap is specifically the BELOW-plane branch: the same two positions, resting just
    // above the surface, are seen perfectly well.
    std::vector<BodyId> hits;
    w.overlap_sphere({4.0f, 1.4f, 4.0f}, 0.5f, hits);
    CHECK(hits.size() == 1);
    w.overlap_sphere({4.5f, 1.4f, 4.5f}, 0.5f, hits);
    CHECK(hits.size() == 1);
}

TEST_CASE("heightfield: a shape cast up a slope stops on the slope, not on the flat") {
    // A ramp rising along +X at 45 degrees. A cast travelling along -Y from above the middle must
    // report the RAMP's normal, which is the measurement a plane could not distinguish: a cast
    // that quietly used a vertical ray plus the cell's height would get the point right and the
    // normal wrong.
    PhysicsWorld w;
    const Terrain t =
        sample_terrain(17, 9, 0.5f, 0.5f, 0.001f, 0.0f, [](double x, double) { return x; });
    const BodyId terrain = add_terrain(w, t);
    ShapeCast sc;
    sc.shape.type = ShapeType::Sphere;
    sc.shape.radius = 0.25f;
    sc.origin = {4.0f, 9.0f, 2.0f};
    sc.direction = {0.0f, -1.0f, 0.0f};
    sc.max_distance = 20.0f;
    ShapeHit sh;
    REQUIRE(w.shape_cast(sc, sh));
    CHECK(sh.body == terrain);
    // 45 degrees: the normal is (-1, 1, 0)/sqrt(2) — pointing up and back down the slope.
    CHECK(sh.normal.y == doctest::Approx(0.70710678f).epsilon(2e-3));
    CHECK(sh.normal.x == doctest::Approx(-0.70710678f).epsilon(2e-3));
    CHECK(std::fabs(sh.normal.z) < 1e-3f);
    // The sphere rests tangent to the slope. Its centre stops where the distance to the plane
    // y = x equals r, i.e. at y = 4 + r*sqrt(2); the contact point is r along -n from there, so
    // y = 4 + r*sqrt(2) - r/sqrt(2) = 4 + r/sqrt(2).
    CHECK(sh.point.y == doctest::Approx(4.0f + 0.25f * 0.70710678f).epsilon(5e-3));
    CHECK(w.heightfield_query_skips() == 0);
}

TEST_CASE("heightfield: a fast body does not tunnel through terrain (speculative CCD)") {
    // THE PROOF THIS BRICK EXISTS FOR. A 1 cm sphere falling at 300 m/s moves 5 m in a 1/60 s
    // step — hundreds of times its own thickness — so the exact narrowphase never samples it
    // overlapping the ground and it passes straight through. M19.1 counted that as
    // heightfield_ccd_skipped and let it happen. With the speculative path wired, the solver is
    // handed a negative-penetration contact and arrests the body AT the surface.
    //
    // Falsifiable by construction: the SAME scene with ccd off must tunnel. Asserting only that
    // the CCD body stops would pass against a bug that made terrain thick enough to catch
    // anything, which is why the control run is part of the test rather than a separate one.
    const auto drop = [](bool ccd_on, float& final_y, std::uint32_t& skips) {
        PhysicsWorld w;
        const Terrain t =
            sample_terrain(33, 33, 1.0f, 1.0f, 0.001f, 0.0f, [](double, double) { return 0.0; });
        add_terrain(w, t);
        BodyDesc b;
        b.shape.type = ShapeType::Sphere;
        b.shape.radius = 0.01f;
        b.position = {16.0f, 6.0f, 16.0f};
        b.mass = 1.0f;
        b.ccd = ccd_on;
        b.linear_velocity = {0.0f, -300.0f, 0.0f};
        w.set_gravity({0.0f, 0.0f, 0.0f}); // velocity alone, so the step count is the whole story
        const BodyId body = w.create_body(b);
        for (int i = 0; i < 6; ++i) {
            w.step(1.0f / 60.0f);
        }
        BodyState st;
        REQUIRE(w.get_body_state(body, st));
        final_y = st.position.y;
        skips = w.stats().heightfield_ccd_skipped;
    };

    float y_ccd = 0.0f;
    float y_plain = 0.0f;
    std::uint32_t skips_ccd = 0;
    std::uint32_t skips_plain = 0;
    drop(true, y_ccd, skips_ccd);
    drop(false, y_plain, skips_plain);

    // The control tunnels: with no speculative contact it ends far below the ground.
    CHECK(y_plain < -10.0f);
    // The CCD body is arrested at the surface — above it, and nowhere near where it would have
    // been without the brick.
    CHECK(y_ccd > -0.5f);
    CHECK(y_ccd > y_plain + 10.0f);
    // And the "could not look" counter stayed silent: the body was caught, not skipped.
    CHECK(skips_ccd == 0);
}

TEST_CASE(
    "heightfield: the convex queries are rotation-covariant and tie-break deterministically") {
    // The same bumpy tile placed at two different orientations must give the same ANSWER in each
    // tile's own frame — the queries transform into the tile's frame and rotate the result back,
    // which is an isometry, so a distance measured through it must not move. This is what catches
    // a transform applied in the wrong order or a normal rotated by the conjugate.
    // Two placements of one tile: identity at the origin, and yawed 90 degrees. A cast fired along
    // each tile's own local -Y from the same local point must report the same distance.
    const auto cast_local = [](const core::Quat& q) {
        PhysicsWorld w;
        const Terrain t = sample_terrain(
            13, 13, 0.5f, 0.5f, 0.001f, 0.0f, [](double x, double z) { return 0.2 * (x + z); });
        const core::Vec3 tile_pos{10.0f, 3.0f, -4.0f};
        add_terrain(w, t, tile_pos, q);

        // Local (3, +4 above the surface, 3) cast straight down in LOCAL space.
        const core::Vec3 local_origin{3.0f, 0.2f * 6.0f + 4.0f, 3.0f};
        ShapeCast sc;
        sc.shape.type = ShapeType::Sphere;
        sc.shape.radius = 0.2f;
        sc.origin = tile_pos + core::rotate(q, local_origin);
        sc.direction = core::rotate(q, core::Vec3{0.0f, -1.0f, 0.0f});
        sc.max_distance = 20.0f;
        ShapeHit sh;
        REQUIRE(w.shape_cast(sc, sh));
        // The normal, brought back into the tile's local frame, must be the same vector whatever
        // the tile's orientation.
        const core::Vec3 n_local = core::rotate(core::conjugate(q), sh.normal);
        return std::pair<float, core::Vec3>{sh.distance, n_local};
    };

    const auto a = cast_local(core::quat_identity());
    const auto b = cast_local(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5707963f));
    const auto c = cast_local(core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 0.7f));
    CHECK(b.first == doctest::Approx(a.first).epsilon(1e-4));
    CHECK(c.first == doctest::Approx(a.first).epsilon(1e-4));
    CHECK(b.second.y == doctest::Approx(a.second.y).epsilon(1e-3));
    CHECK(c.second.y == doctest::Approx(a.second.y).epsilon(1e-3));
    CHECK(b.second.x == doctest::Approx(a.second.x).epsilon(1e-3));
    CHECK(c.second.x == doctest::Approx(a.second.x).epsilon(1e-3));
}

TEST_CASE("heightfield: a terrain scene steps bit-identically across worker counts") {
    const auto run = [](unsigned workers) {
        std::unique_ptr<core::JobSystem> js;
        PhysicsWorld w;
        if (workers > 0) {
            js = std::make_unique<core::JobSystem>(workers);
            w.set_job_system(js.get());
        }
        const Terrain t = sample_terrain(33, 33, 1.0f, 1.0f, 0.01f, 0.0f, [](double x, double z) {
            return 0.05 * ((x - 16) * (x - 16) + (z - 16) * (z - 16)) + 0.3 * std::sin(x);
        });
        add_terrain(w, t);
        for (int i = 0; i < 24; ++i) {
            BodyDesc b;
            if (i % 2 == 0) {
                b.shape.type = ShapeType::Sphere;
                b.shape.radius = 0.35f;
            } else {
                b.shape.type = ShapeType::Box;
                b.shape.half_extents = {0.3f, 0.2f, 0.4f};
            }
            b.position = {
                6.0f + float(i % 6) * 3.7f, 12.0f + float(i) * 0.3f, 7.0f + float(i / 6) * 4.1f};
            REQUIRE(w.create_body(b).is_valid());
        }
        for (int s = 0; s < 240; ++s) {
            w.step(1.0f / 60.0f);
        }
        return w.world_hash();
    };
    const std::uint64_t h0 = run(0);
    CHECK(run(0) == h0);
    CHECK(run(2) == h0);
    CHECK(run(8) == h0);
}

TEST_CASE("heightfield: depenetration reports the DEEPEST overlapping triangle, not just one") {
    // An ASYMMETRIC V-valley: a steep left wall (slope 0.9) meeting a gentle right floor
    // (slope 0.15) at x = 4. A sphere resting in the crease overlaps triangles belonging to both,
    // at genuinely different depths — which a flat tile cannot show, because there every candidate
    // is equally deep and "deepest wins" and "first one found wins" are indistinguishable. That is
    // exactly the hole this test fills: the rule was written, and until this configuration existed
    // nothing in the suite could tell it from its opposite (MEASURED — inverting the comparison to
    // "shallowest" left every other heightfield test green).
    PhysicsWorld w;
    const Terrain t = sample_terrain(17, 9, 0.5f, 0.5f, 0.001f, 0.0f, [](double x, double) {
        return x < 4.0 ? 0.9 * (4.0 - x) : 0.15 * (x - 4.0);
    });
    add_terrain(w, t);
    ShapeDesc probe;
    probe.type = ShapeType::Sphere;
    probe.radius = 0.4f;
    PenetrationHit ph;
    REQUIRE(w.penetration(probe, {4.0f, 0.1f, 2.3f}, core::quat_identity(), ph));
    // The STEEP wall is the deepest violation, so that is the axis to resolve along, and resolving
    // it is what moves the sphere out of the worst overlap rather than merely out of some overlap.
    CHECK(ph.depth == doctest::Approx(0.32567f).epsilon(1e-3));
    CHECK(ph.normal.x == doctest::Approx(0.669f).epsilon(5e-3));
    CHECK(ph.normal.y == doctest::Approx(0.743f).epsilon(5e-3));
    CHECK(std::fabs(ph.normal.z) < 1e-3f);
    // For contrast, the numbers a "shallowest wins" reduction reports at this very position
    // (measured by inverting the comparison): depth 0.0838 along (0, 0.316, 0.949) — a Z-facing
    // triangle on the valley's flank, a third of the depth and a different axis entirely. Pushing
    // the sphere that way leaves it inside the steep wall.
    CHECK(ph.depth > 0.2f);
}
