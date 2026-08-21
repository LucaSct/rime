// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>

#include "gjk.hpp" // PRIVATE header — see the note below before copying this pattern
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/shape.hpp"
#include "support.hpp" // ditto

// THIS IS THE ONE PHYSICS TEST THAT REACHES BELOW THE SEAM, and the exception is argued rather
// than assumed, because every other test in this suite drives PhysicsWorld only and should
// continue to.
//
// The defect it gates lives in GJK, a private header. Its symptom at the public seam — a contact
// normal that comes back along a box's EDGE instead of its FACE — is invisible on x86-64, because
// the layers above work around it: `shape_cast` steps by the lower bound rather than the reported
// distance, and measures its normal at a retracted position. Both were added in m12.1 for other
// reasons and happen to mask this. The symptom therefore appeared ONLY on arm64, and only through
// one assertion, and was diagnosed only by probing GJK directly.
//
// So a seam-level regression test for this cannot exist on x86-64: it would pass identically with
// the bug present and absent, which was verified rather than assumed — a 21,000-case sweep through
// `shape_cast` produces byte-identical results either way. A test that cannot fail is not a test,
// and leaving the fix gated only by one arm64 assertion is how it comes back. Hence this file.
//
// THE DEFECT. Two of GJK's epsilons were ABSOLUTE while the float error they guard is proportional
// to the size of the shapes:
//
//   * the convergence bound compared against `kRelEps * dist2`, a purely relative budget, while
//     the error in the dot product it evaluates scales with the SUPPORT magnitude. Below roughly
//     (float eps / kRelEps) * |w| the test could never fire, so GJK could not stop; it kept taking
//     supports along a direction whose transverse sign was pure noise — flipping between two
//     opposite corners of the same face — and accreted NEAR-DUPLICATE vertices;
//   * the triangle degeneracy check compared |va+vb+vc| against a fixed 1e-9, while those three
//     are differences of products whose ULP grows with |edge|^2 * |point|^2. For vertices a metre
//     out, one ULP is ~1.9e-6 — a thousand times the epsilon — so a COLLINEAR triangle read as a
//     real face, and the barycentric division below it turned pure noise into weights.
//
// Together: GJK returned the CENTROID of a sliver whose three vertices were collinear along the
// box's y=z diagonal. Measured on main before the fix — a 0.3 m sphere 1.9e-5 m from a 1 m box
// reported a distance of 0.47 for a true gap of 1.9e-5, with a normal perpendicular to the true
// one. That is where m12.1's diagonal contact normal came from, and why it looked like an edge
// direction: it was one.
using namespace rime;
using namespace rime::physics;

namespace {

// A sphere at distance `gap` from the +x face of a box of half-extent (0.25, H, H) at the origin.
// The true closest direction is exactly -x, which is what makes this configuration a good probe:
// the answer is known analytically, and the approach is FACE-ON, which is the degenerate case for
// a support function that can only return corners.
constexpr float kSphereRadius = 0.3f;

// The pose kept ADDRESSABLE rather than run and thrown away: the plane-bound invariant below has to
// re-evaluate the two support functions itself, from outside GJK, so it needs the shapes and not
// just the result. `oy`/`oz` slide the sphere across the face — one plane measured eight times is
// not a grid, and a bound is only interesting once the planes genuinely differ.
struct Probe {
    ShapeDesc bx;
    ShapeDesc sp;
    core::Vec3 centre;

    [[nodiscard]] ShapeSupport caster() const {
        return ShapeSupport{&sp, centre, core::quat_identity(), nullptr};
    }

    [[nodiscard]] ShapeSupport wall() const {
        return ShapeSupport{&bx, core::Vec3{}, core::quat_identity(), nullptr};
    }

    [[nodiscard]] GjkResult run() const { return gjk(caster(), wall(), centre); }
};

Probe make_probe(float half_extent, float gap, float oy = 0.0f, float oz = 0.0f) {
    Probe p;
    p.bx.type = ShapeType::Box;
    p.bx.half_extents = {0.25f, half_extent, half_extent};
    p.sp.type = ShapeType::Sphere;
    p.sp.radius = kSphereRadius;
    p.centre = {-(0.25f + kSphereRadius + gap), oy, oz};
    return p;
}

GjkResult probe(float half_extent, float gap) {
    return make_probe(half_extent, gap).run();
}

// ── The 2026-08-21 shallow-penetration family ────────────────────────────────────────────────
//
// A second fixture rather than a parameter on the first, because the defect it gates is a
// different one and the shapes that provoke it are different: a THIN, WIDE slab (the m12.1 shape
// cast's wall) against a caster large enough that the contact sits well inside the face. `signed_
// gap` is positive for a gap and NEGATIVE for a penetration, which is the whole point — the two
// sides of contact are the two halves of the property being asserted.
struct Slab {
    ShapeDesc bx;
    ShapeDesc sp;
    core::Vec3 centre;

    [[nodiscard]] GjkResult run() const {
        const ShapeSupport caster{&sp, centre, core::quat_identity(), nullptr};
        const ShapeSupport wall{&bx, core::Vec3{}, core::quat_identity(), nullptr};
        return gjk(caster, wall, centre);
    }
};

constexpr float kSlabThickness = 0.1f; // half-extent along x: the wall the cast sweeps at

Slab make_slab(float half_extent, float radius, float signed_gap, float oy, float oz) {
    Slab s;
    s.bx.type = ShapeType::Box;
    s.bx.half_extents = {kSlabThickness, half_extent, half_extent};
    s.sp.type = ShapeType::Sphere;
    s.sp.radius = radius;
    // Surfaces meet when the centre is at -(thickness + radius); pull back by the gap.
    s.centre = {-(kSlabThickness + radius + signed_gap), oy, oz};
    return s;
}

} // namespace

TEST_CASE("gjk: a face-on approach resolves the FACE, at every scale and separation") {
    // The grid is the test. A single configuration is what let this through in the first place:
    // the failures are knife-edge on particular float values, so the property has to be asserted
    // across a sweep rather than at a point.
    //
    // Gaps run from 1 m down to 1e-4 m. Below that the separation approaches one ULP of the
    // vertex coordinates themselves (for a 30 m box, 1 ULP is 5e-6 m), where no float algorithm
    // can resolve a direction — that floor is documented in docs/ROADMAP.md rather than asserted
    // against here.
    for (const float half : {1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 30.0f, 50.0f, 100.0f}) {
        for (int i = 0; i < 60; ++i) {
            const float gap = std::pow(10.0f, -4.0f * (static_cast<float>(i) / 59.0f));
            CAPTURE(half);
            CAPTURE(gap);

            const GjkResult g = probe(half, gap);
            REQUIRE_FALSE(g.overlapping);

            // The distance is the analytically known gap.
            CHECK(g.distance == doctest::Approx(gap).epsilon(0.02));

            // And `closest` — the separating axis, and the direction every caller derives a normal
            // from — points along -x, the face normal. Before the fix this came back as the box's
            // y=z EDGE direction, with the axial component near zero.
            const float len = core::length(g.closest);
            REQUIRE(len > 0.0f);
            CHECK(std::fabs(g.closest.x) / len > 0.99f);
        }
    }
}

TEST_CASE("gjk: the lower bound never over-claims, which is what makes a sweep safe") {
    // `distance` is an UPPER bound — GJK terminates on a simplex, and the closest point of a subset
    // is never nearer than that of the whole set. `lower_bound` is the support-plane bound and must
    // never exceed the truth, because m12.1's shape cast advances by it: a bound that over-claimed
    // would step past a surface and put a character capsule inside a wall.
    for (const float half : {1.0f, 10.0f, 100.0f}) {
        for (int i = 0; i < 40; ++i) {
            const float gap = std::pow(10.0f, -3.0f * (static_cast<float>(i) / 39.0f));
            CAPTURE(half);
            CAPTURE(gap);
            const GjkResult g = probe(half, gap);
            REQUIRE_FALSE(g.overlapping);

            CHECK(g.lower_bound >= 0.0f);
            CHECK(g.lower_bound <= g.distance);
            // The bound may be WEAK (it is only as good as the search direction that produced it —
            // see the ROADMAP note on its quality) but it must never claim more than the truth.
            CHECK(g.lower_bound <= gap * 1.02f);
        }
    }
}

TEST_CASE("gjk: the plane behind `lower_bound` re-verifies from raw support points") {
    // `plane_dir` (2026-08-21) is the direction of the support plane that PRODUCED `lower_bound`,
    // and it exists because the bound alone is a radial statement. m12.1's shape cast could only
    // spend it radially, which shrinks an oblique sweep's gap by (1 - cos θ) per iteration and left
    // an 85° graze metres short of the wall. Dividing the bound by the sweep's closing rate fixes
    // that — but ONLY against the plane the bound came from. Against any other direction the
    // quotient is a category error, and m12.1 had already measured what a mismatched direction does
    // to a step: a 30× leap, straight through a wall and out the far side. `lower_bound` is a
    // running maximum over planes from DIFFERENT iterations, so without this field the pairing is
    // genuinely unrecoverable by the caller.
    //
    // What is asserted is the plane's own statement, re-derived FROM RAW SUPPORT POINTS: if the
    // plane is real, the extreme point of the Minkowski difference along -plane_dir cannot lie
    // nearer the origin, measured along plane_dir, than `lower_bound`. One support evaluation per
    // shape — no simplex, no iteration history — so a bound produced by a corrupted iteration
    // cannot certify itself out of its own leftovers.
    struct Pose {
        float half;
        float gap;
        float oy;
        float oz;
    };

    // Face-on at three scales and three separations, plus three off-centre poses whose true
    // closest direction is still the face normal but whose supports are far from the axis.
    const Pose poses[] = {{1.0f, 1.0f, 0.0f, 0.0f},
                          {1.0f, 1e-2f, 0.0f, 0.0f},
                          {1.0f, 1e-4f, 0.0f, 0.0f},
                          {20.0f, 1.0f, 0.0f, 0.0f},
                          {20.0f, 1e-2f, 0.0f, 0.0f},
                          {20.0f, 1e-4f, 0.0f, 0.0f},
                          {100.0f, 1.0f, 0.0f, 0.0f},
                          {100.0f, 1e-3f, 0.0f, 0.0f},
                          {1.0f, 0.5f, 0.4f, 0.2f},
                          {20.0f, 0.5f, 8.0f, -3.0f},
                          {100.0f, 0.2f, 40.0f, 20.0f}};

    int with_a_plane = 0;
    for (const Pose& pose : poses) {
        CAPTURE(pose.half);
        CAPTURE(pose.gap);
        CAPTURE(pose.oy);
        CAPTURE(pose.oz);

        const Probe p = make_probe(pose.half, pose.gap, pose.oy, pose.oz);
        const GjkResult g = p.run();
        REQUIRE_FALSE(g.overlapping);
        CAPTURE(g.lower_bound);

        const float len = core::length(g.plane_dir);
        if (g.lower_bound <= 0.0f) {
            // No plane ever beat zero, so there is no direction to report — and reporting one
            // anyway would invite a caller to divide by its closing rate and step on nothing. The
            // shape cast keys its "proven miss" branch on `lower_bound > 0` for exactly this
            // reason. A zero bound is REACHABLE here rather than hypothetical: against a large
            // target at a tiny gap it is what GJK's early exits actually produce.
            CHECK(len == 0.0f);
            continue;
        }
        ++with_a_plane;

        // Unit, because the caller divides by `dot(dir, plane_dir)` and a direction of length 0.9
        // would silently inflate every step by 11%.
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-4));

        // The re-verification. `support_A(-n) - support_B(+n)` is the support of the Minkowski
        // difference along -n by definition, i.e. its most origin-ward point in that direction.
        const core::Vec3 wa = p.caster()(g.plane_dir * -1.0f);
        const core::Vec3 wb = p.wall()(g.plane_dir);
        const float reached = core::dot(wa - wb, g.plane_dir);

        // The tolerance scales with the COORDINATE MAGNITUDE of the supports rather than with the
        // gap, because that is what the dot product's float error scales with — a 100 m wall's
        // support point carries ~1e-5 of absolute noise no matter how narrow the gap it is
        // bounding. (The absolute-epsilon mistake this whole file is about, avoided rather than
        // repeated.) Worst slack measured across these poses is 1.5e-8, against bounds from 1.7e-4
        // to 1.9e-2 — six orders of margin, so this fails on a broken pairing and on nothing else.
        const float scale = core::length(wa) + core::length(wb);
        CHECK(reached >= g.lower_bound - 1e-4f * scale);
    }

    // Without this the block would pass vacuously on a build where every bound collapsed to zero —
    // which is precisely the failure the shape cast's rescue path exists to survive, and precisely
    // the state in which a silent green here would be a lie.
    CHECK(with_a_plane >= 8);
}

// ── The 2026-08-21 stall certificate ─────────────────────────────────────────────────────────
//
// THE DEFECT, and it is a different organ from the one above. GJK has three STALL exits — a
// duplicate support point, a step that made no monotone progress, and the iteration cap — and all
// three used to finish by calling the SEPARATED path with whatever `distance` the stalled simplex
// happened to hold. That number is an upper bound taken from an arbitrary simplex. It says nothing
// whatever about which SIDE of contact the shapes are on, and at a stall it is routinely wrong by
// orders of magnitude (this loop's own trace: 14.14 m reported for a true gap of 2.8e-5 m).
//
// Reporting it as a separation is therefore not "a loose answer", it is an answer with the wrong
// sign. Measured on the family below before the fix: a sphere 1.85 mm INSIDE a slab came back as
// "separated, 1.11e-3" — a plausible-looking millimetre gap where the truth was a millimetre of
// penetration. The m12.1 shape cast's only guard against stopping inside a wall is GJK's
// `overlapping` verdict (it bisects against that predicate rather than against any distance), so a
// penetration that never announces itself is a caster that stops up to a centimetre inside the
// geometry — and the over-report bound in tests/physics/shape_cast_test.cpp is held 40x loose
// today for exactly this reason.
//
// THE PROPERTY. A stall may only claim separation with a CERTIFICATE: a support plane whose bound
// clears the scale-relative noise floor. The geometry behind it is not a tolerance and cannot be
// tuned away — when the origin lies inside the Minkowski difference NO support plane can exclude
// it, every bound dot(w, n) being <= 0, so a positive bound is a PROOF of separation and the
// absence of one at a stall means "not proven outside". The two test cases below are the two
// halves of that: the certificate must never be granted to a penetration, and it must not be
// withheld from a genuine gap.
TEST_CASE("gjk: a shallow penetration never reports as a small positive gap") {
    // The grid is the test, for the same reason as the sweep above: the misreads are knife-edge on
    // particular float values, so a single pose proves nothing either way. Pre-fix, 56 of these
    // 1,536 configurations report SEPARATED while the shapes genuinely interpenetrate; the worst
    // announces a 1.11e-3 m gap at a true penetration of 1.85e-3 m.
    //
    // Penetration depths run from 10 cm down to 1 mm. The floor is deliberate: below roughly the
    // support coordinates' own noise (kSupportEps * |w|, which is 2.9e-6 m against a 5 m slab and
    // 2.8e-5 m against a 50 m one) "penetrating" and "touching" are not distinguishable by any
    // float algorithm, and a narrowphase that answers "contact" there is right rather than lucky.
    // 1 mm is two orders above that floor at every scale swept.
    for (const float half : {5.0f, 20.0f, 50.0f}) {
        for (const float r : {0.3f, 1.0f}) {
            for (int i = 0; i < 16; ++i) {
                const float pen = std::pow(10.0f, -1.0f - 2.0f * (static_cast<float>(i) / 15.0f));
                // Off-centre rows are not decoration. The defect is driven by the ratio between
                // the gap and the SUPPORT MAGNITUDE, and sliding the caster across a large face
                // is what makes the support points far-flung while the contact stays face-on with
                // an analytically known answer.
                for (const float fy : {0.0f, 0.2f, 0.4f, 0.6f}) {
                    for (const float fz : {0.0f, 0.15f, 0.3f, 0.5f}) {
                        CAPTURE(half);
                        CAPTURE(r);
                        CAPTURE(pen);
                        CAPTURE(fy);
                        CAPTURE(fz);

                        const GjkResult g = make_slab(half, r, -pen, fy * half, fz * half).run();
                        CAPTURE(g.distance);
                        CAPTURE(g.lower_bound);

                        // The whole property in one line. There is no tolerance to argue about:
                        // the shapes overlap, so the only honest verdict is `overlapping`, and any
                        // separated answer — with any distance — is the defect.
                        CHECK(g.overlapping);
                    }
                }
            }
        }
    }
}

TEST_CASE("gjk: a shallow gap at a large slab still certifies as separated") {
    // The certificate's POSITIVE side, and the half that keeps the fix honest: a rule that answers
    // "overlapping" whenever it is unsure would pass the case above vacuously while making the
    // narrowphase useless. These poses are genuinely apart — by a millimetre to a centimetre — and
    // must be reported so, with a bound a swept query can actually spend.
    //
    // This is also where GJK's in-loop bound COLLAPSES. The bound is dot(w, closest_hat), and at
    // these gaps `closest` is a barycentric blend of support points metres out, so its transverse
    // error is one ULP of THOSE coordinates while the axial part it encodes is a millimetre.
    // Multiply the resulting tilt back by the slab's extent and the bound goes negative. Measured
    // pre-fix: 155 of the 384 poses below come back with `lower_bound` exactly zero, i.e. with no
    // usable plane at all, which is what drops the shape cast to its distance-rescue tier.
    int certified = 0;
    int strong = 0;
    for (const float half : {5.0f, 20.0f, 50.0f}) {
        for (const float r : {0.3f, 1.0f}) {
            for (const float gap : {1e-3f, 2e-3f, 5e-3f, 1e-2f}) {
                for (const float fy : {0.0f, 0.2f, 0.4f, 0.6f}) {
                    for (const float fz : {0.0f, 0.15f, 0.3f, 0.5f}) {
                        CAPTURE(half);
                        CAPTURE(r);
                        CAPTURE(gap);
                        CAPTURE(fy);
                        CAPTURE(fz);

                        const GjkResult g = make_slab(half, r, gap, fy * half, fz * half).run();
                        CAPTURE(g.distance);
                        CAPTURE(g.lower_bound);

                        // Separated, because they are. A millimetre is not a rounding error at
                        // these scales — it is 340x the support noise floor even against the 50 m
                        // slab — so "cannot tell" is not an available answer here.
                        CHECK_FALSE(g.overlapping);

                        // And the DISTANCE is the gap, not a stalled simplex's leftovers. The
                        // pre-certificate stall exits could return distances that were not loose
                        // but unrelated — 14.14 m for a true gap of 2.8e-5 on the shape cast's
                        // own trace, the lie its Lipschitz leash exists to survive. The polish
                        // reports the gap from its converged simplex: worst measured on this
                        // grid is 1.4% off (a 1 mm gap at the 50 m slab); gated at 5%.
                        CHECK(g.distance == doctest::Approx(gap).epsilon(0.05));

                        // And the certificate exists. `lower_bound > 0` is precisely the predicate
                        // the shape cast keys its proven-step branch on (src/scene_query.hpp, THE
                        // ADVANCE), so a zero here is not a cosmetic weakness: it is the loop
                        // falling back to stepping by a number it cannot trust.
                        CHECK(g.lower_bound > 0.0f);
                        if (g.lower_bound > 0.0f) {
                            ++certified;
                        }

                        // It must still never over-claim — the invariant the sweep's safety rests
                        // on, asserted here again because a certificate is a new way to produce
                        // the number and so a new way to get it wrong.
                        CHECK(g.lower_bound <= g.distance);
                        CHECK(g.lower_bound <= gap * 1.02f);

                        if (g.lower_bound >= 0.8f * gap) {
                            ++strong;
                        }
                    }
                }
            }
        }
    }

    // A QUALITY floor on top of the existence check, in the spirit of the `with_a_plane` gate
    // above: a bound that is merely positive can still be so weak that the cast crawls. Measured
    // with the stall polish in place, ALL 384 poses carry a bound within 20% of the true gap —
    // the worst is 0.978x, a 1 mm gap at the 50 m slab — against 177 pre-fix. The gate is set
    // below the measurement rather than at it, because the exact count is a float-rounding detail
    // and only its collapse is a regression.
    CHECK(certified == 384);
    CHECK(strong >= 360);
}

TEST_CASE("gjk: the stall polish certifies a millimetre gap at a large slab") {
    // The named witness for the polish path, kept alongside the grid because a single pose with
    // known numbers is what a reader can check by hand. Every support plane GJK evaluates in the
    // float loop at this pose comes back non-positive — `lower_bound` is exactly zero pre-fix —
    // and the terminal simplex is a CHORD of the curved Minkowski difference whose perpendicular
    // no plane can certify (the geometry is argued at finish_stalled in src/gjk.hpp), so the
    // certificate has to be FOUND by the double-precision restart, not synthesized from the
    // stalled simplex.
    //
    // A 1 m sphere, a 1 mm gap, 2 m up the face of a 5 m slab. Measured with the fix: separated,
    // with a bound of 9.9977e-4 m against a true gap of 1.000e-3 m — the polish recovers the gap
    // to within 0.023%, which is what makes it worth spending on a step, and its plane comes back
    // as the face normal to ~4e-8 transverse.
    const GjkResult g = make_slab(5.0f, 1.0f, 1e-3f, 2.0f, 0.0f).run();
    CAPTURE(g.distance);
    CAPTURE(g.lower_bound);

    REQUIRE_FALSE(g.overlapping);
    CHECK(g.lower_bound >= 0.9e-3f);
    CHECK(g.lower_bound <= 1.02e-3f);
    // And the plane it came from is reported, unit, so the cast can divide by its closing rate.
    CHECK(core::length(g.plane_dir) == doctest::Approx(1.0f).epsilon(1e-4));
}
