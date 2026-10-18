// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "narrowphase.hpp"

// Exact runtime geometry is deliberate: registering these hulls again re-centres their vertices,
// changing the last bits that trigger the false containment. Like gjk_test.cpp, this private-seam
// exception isolates arithmetic hidden by the public seam. Source: the two first-pair snapshots
// in docs/perf/environment-preview-2026-10-17/ghost-contact-hulls.json. No replay or GPU is needed.
using namespace rime;
using namespace rime::physics;

namespace {
struct CapturedPair {
    ConvexHull a, b;
    core::Vec3 pa, pb;
    core::Quat qa, qb;
};

CapturedPair captured_pair(int pair) {
    CapturedPair p;
    if (pair == 0) {
        p.a.vertices = {
            {-0.275697649f, -0.601077139f, 0.161548257f},
            {0.524547637f, -0.0734103471f, 0.161548257f},
            {0.0558150336f, 0.602120161f, 0.161548257f},
            {-0.429802358f, -0.484583378f, 0.161548257f},
            {-0.0307063516f, 0.865053177f, -0.138451755f},
            {0.587092698f, -0.0253099352f, -0.138451755f},
            {-0.270058811f, -0.59049958f, -0.138451755f},
            {-0.577347696f, -0.35820806f, -0.138451755f},
        };
        p.a.face_normals = {
            {7.36250882e-09f, 0.0f, 1.0f},
            {0.0f, 0.0f, -1.0f},
            {0.550382078f, -0.834694743f, -0.0190849788f},
            {0.794629693f, 0.55137223f, 0.254071534f},
            {-0.775641322f, 0.346612453f, 0.527484834f},
            {-0.602558613f, -0.797099888f, -0.0394302085f},
        };
        p.a.face_indices = {0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 6, 5, 2, 1, 5, 4, 3, 2, 4, 7, 0, 3, 7, 6};
        p.a.face_offsets = {0, 4, 8, 12, 16, 20, 24};
        p.b.vertices = {
            {-1.11780584f, -0.400078356f, 0.149440497f},
            {1.27729368f, -0.400078356f, 0.149440497f},
            {1.08461285f, -0.0914429948f, 0.149440497f},
            {0.595395327f, 0.286631227f, 0.149440497f},
            {-0.481936097f, 0.711084187f, 0.149440497f},
            {1.26903439f, -0.400078356f, -0.150559515f},
            {-1.10301042f, -0.400078356f, -0.150559515f},
            {-0.471946001f, 0.702687323f, -0.150559515f},
            {0.547023773f, 0.301227987f, -0.150559515f},
            {1.09613991f, -0.123136692f, -0.150559515f},
        };
        p.b.face_normals = {
            {0.0f, 0.0f, 1.0f},
            {2.38856979e-09f, 0.0f, -1.0f},
            {0.0f, -1.0f, -9.44253387e-08f},
            {0.366526574f, 0.930304766f, -0.0138333701f},
            {0.610389292f, 0.78982681f, -0.0599884838f},
            {-0.867139518f, 0.496225953f, -0.0427653939f},
            {0.848034143f, 0.529427111f, -0.0233472195f},
        };
        p.b.face_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 6, 5, 1, 0, 8,
                            7, 4, 3, 9, 8, 3, 2, 0, 4, 7, 6, 1, 5, 9, 2};
        p.b.face_offsets = {0, 5, 10, 14, 18, 22, 26, 30};
        p.pa = {29.9181042f, 2.06812119f, -13.9763346f};
        p.qa = {-0.0165744592f, -0.0117769428f, -0.00448407372f, 0.999783218f};
        p.pb = {30.3253403f, 3.36591744f, -13.951601f};
        p.qb = {-0.0176558793f, -0.00938515645f, 0.00620343443f, 0.999780834f};
    }
    if (pair == 1) {
        p.a.vertices = {
            {2.04527187f, -0.149781018f, -0.922629714f},
            {-2.29699349f, -0.149781018f, -0.922629714f},
            {-2.27060366f, 0.150218993f, -0.922629714f},
            {2.01808786f, 0.150218993f, -0.922629714f},
            {2.04295278f, 0.150218993f, -0.649329782f},
            {-0.611436844f, 0.150218993f, 1.15610111f},
            {0.323465884f, 0.150218993f, 1.6500777f},
            {2.06687975f, -0.149781018f, -0.685129046f},
            {0.320930749f, -0.149781018f, 1.64966547f},
            {-0.655793071f, -0.149781018f, 1.13359153f},
        };
        p.a.face_normals = {
            {0.0f, 0.0f, -1.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, -1.0f, -2.32901942e-09f},
            {0.991856575f, 0.0898747519f, -0.0902394056f},
            {-0.779727876f, 0.0685897246f, 0.622350276f},
            {-0.467168003f, 0.00273251091f, 0.884164333f},
            {0.80082351f, -0.00759046152f, 0.598852336f},
        };
        p.a.face_indices = {0, 1, 2, 3, 4, 3, 2, 5, 6, 1, 0, 7, 8, 9, 7,
                            0, 3, 4, 1, 9, 5, 2, 6, 5, 9, 8, 4, 6, 8, 7};
        p.a.face_offsets = {0, 4, 9, 14, 18, 22, 26, 30};
        p.b.vertices = {
            {0.580522239f, -1.50598812f, 0.151358098f},
            {1.13161051f, -0.278614223f, 0.151358098f},
            {-0.875978351f, 1.30748999f, 0.151358098f},
            {-1.41434038f, 1.30748999f, 0.151358098f},
            {-1.46879423f, 1.30748999f, -0.148641914f},
            {-0.889169574f, 1.30748999f, -0.148641914f},
            {1.12930286f, -0.28721267f, -0.148641914f},
            {0.560360134f, -1.55435181f, -0.148641914f},
        };
        p.b.face_normals = {
            {-1.2282487e-08f, 0.0f, 1.0f},
            {-1.16329577e-08f, 0.0f, -1.0f},
            {0.0f, 1.0f, 4.44285604e-08f},
            {0.61969471f, 0.784369886f, -0.0272482745f},
            {0.912253082f, -0.409599692f, 0.00472247414f},
            {-0.806955159f, -0.572161794f, 0.146472648f},
        };
        p.b.face_indices = {0, 1, 2, 3, 4, 5, 6, 7, 3, 2, 5, 4, 6, 5, 2, 1, 7, 6, 1, 0, 0, 3, 4, 7};
        p.b.face_offsets = {0, 4, 8, 12, 16, 20, 24};
        p.pa = {3.46282816f, 0.426654279f, -12.8053885f};
        p.qa = {-0.00270736404f, -0.000463540375f, -0.000841144065f, 0.999995947f};
        p.pb = {3.06828094f, 1.66893756f, -13.9257278f};
        p.qb = {-0.0133385239f, 0.00178589975f, 0.000704072125f, 0.999909282f};
    }
    return p;
}

float axis_gap(const CapturedPair& p) {
    const auto a = hull_world_aabb(p.a, p.pa, p.qa);
    const auto b = hull_world_aabb(p.b, p.pb, p.qb);
    return std::max({a.min.x - b.max.x,
                     b.min.x - a.max.x,
                     a.min.y - b.max.y,
                     b.min.y - a.max.y,
                     a.min.z - b.max.z,
                     b.min.z - a.max.z});
}

} // namespace

TEST_CASE("captured collapse hulls respect a positive separating plane") {
    for (int pair = 0; pair < 2; ++pair) {
        for (float scale : {0.125f, 1.0f, 8.0f}) {
            for (bool swapped : {false, true}) {
                CAPTURE(pair);
                CAPTURE(scale);
                CAPTURE(swapped);
                auto p = captured_pair(pair);
                for (auto& v : p.a.vertices) {
                    v = v * scale;
                }
                for (auto& v : p.b.vertices) {
                    v = v * scale;
                }
                p.pa = p.pa * scale;
                p.pb = p.pb * scale;
                if (swapped) {
                    std::swap(p.a, p.b);
                    std::swap(p.pa, p.pb);
                    std::swap(p.qa, p.qb);
                }
                const float gap = axis_gap(p);
                REQUIRE(gap > 0.005f * scale);
                const auto a = narrowphase_detail::make_hull_poly_view(p.a, p.pa, p.qa);
                const auto b = narrowphase_detail::make_hull_poly_view(p.b, p.pb, p.qb);
                const auto g = gjk(narrowphase_detail::PolySupport{&a},
                                   narrowphase_detail::PolySupport{&b},
                                   p.pa - p.pb);
                CHECK_FALSE(g.overlapping);
                CHECK(g.distance >= gap - 1e-5f * scale);
                CHECK(g.lower_bound > 0.0f);
                CHECK(g.lower_bound <= g.distance);
                Manifold m;
                CHECK_FALSE(collide_hull_hull(p.a, p.pa, p.qa, p.b, p.pb, p.qb, m));
                CHECK(m.count == 0);

                // Sensitivity control: the same two convex solids centred together must touch.
                // This catches a repair that suppresses hull contacts instead of fixing GJK.
                Manifold control;
                CHECK(collide_hull_hull(p.a, p.pa, p.qa, p.b, p.pa, p.qb, control));
                CHECK(control.count > 0);
            }
        }
    }
}

TEST_CASE("nearly flat separated GJK tetrahedra never certify containment") {
    // GJK terminal simplexes from the two captures. Every point lies strictly on one side of
    // y=0 (client) or z=0 (server), so their convex hull cannot contain the origin. The volume
    // is float cancellation residue; permuting vertices must not turn it into a solid.
    const std::array<std::array<core::Vec3, 4>, 2> fixtures = {{
        {{{0.66844368f, -0.0184495449f, -0.0356502533f},
          {-1.70300102f, -0.0486588478f, -0.0796451569f},
          {-1.6259861f, -0.302204609f, 0.230843544f},
          {0.745458603f, -0.271995306f, 0.274838448f}}},
        {{{3.85610962f, -2.7096312f, 0.0789318085f},
          {-2.48509097f, 0.107423246f, 0.00700759888f},
          {1.85716629f, 0.100129247f, 0.0110521317f},
          {-0.486147523f, -2.70233727f, 0.0748872757f}}},
    }};
    for (std::size_t fixture = 0; fixture < fixtures.size(); ++fixture) {
        for (float scale : {0.125f, 1.0f, 8.0f}) {
            std::array<int, 4> order = {0, 1, 2, 3};
            do {
                CAPTURE(fixture);
                CAPTURE(scale);
                CAPTURE(order[0]);
                CAPTURE(order[1]);
                CAPTURE(order[2]);
                CAPTURE(order[3]);
                SupportVertex vertices[4];
                for (int i = 0; i < 4; ++i) {
                    const auto w = fixtures[fixture][static_cast<std::size_t>(order[i])] * scale;
                    vertices[i] = {w, w, {}};
                }
                float lambda[4] = {};
                int count = 4;
                core::Vec3 closest;
                CHECK_FALSE(solve_simplex(vertices, lambda, count, closest));
                CHECK(count >= 1);
                CHECK(count <= 3);
                CHECK(core::length(closest) > 0.005f * scale);
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
}

TEST_CASE("GJK retains overlap for solid and very thin enclosing tetrahedra") {
    for (float half_height : {1.0f, 1e-4f}) {
        for (float scale : {0.125f, 1.0f, 8.0f}) {
            CAPTURE(half_height);
            CAPTURE(scale);
            // The origin is strictly inside: its four barycentrics are (1/8,1/8,1/4,1/2).
            // The thin case must still overlap when the float volume test defers to faces.
            const std::array<core::Vec3, 4> points = {
                core::Vec3{100.0f, -half_height, -100.0f} * scale,
                core::Vec3{-100.0f, -half_height, -100.0f} * scale,
                core::Vec3{0.0f, -half_height, 100.0f} * scale,
                core::Vec3{0.0f, half_height, 0.0f} * scale,
            };
            const auto tetrahedron = [&](core::Vec3 dir) {
                auto best = points[0];
                for (const auto& p : points) {
                    if (core::dot(p, dir) > core::dot(best, dir)) {
                        best = p;
                    }
                }
                return best;
            };
            const auto origin = [](core::Vec3) { return core::Vec3{}; };
            CHECK(gjk(tetrahedron, origin, {1.0f, 1.0f, 1.0f}).overlapping);
        }
    }
}
