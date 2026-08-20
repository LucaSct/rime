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

GjkResult probe(float half_extent, float gap) {
    ShapeDesc bx;
    bx.type = ShapeType::Box;
    bx.half_extents = {0.25f, half_extent, half_extent};
    ShapeDesc sp;
    sp.type = ShapeType::Sphere;
    sp.radius = kSphereRadius;

    const float x = -(0.25f + kSphereRadius + gap);
    const ShapeSupport a{&sp, {x, 0.0f, 0.0f}, core::quat_identity(), nullptr};
    const ShapeSupport b{&bx, {0.0f, 0.0f, 0.0f}, core::quat_identity(), nullptr};
    return gjk(a, b, core::Vec3{x, 0.0f, 0.0f});
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
