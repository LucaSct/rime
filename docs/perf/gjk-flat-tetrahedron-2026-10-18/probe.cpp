// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
// Exact runtime hulls/poses from the October 17 snapshot. This diagnostic separates the GJK
// containment verdict, EPA result, and final manifold so a zero-depth contact has a visible cause.
#include <cstdio>

#include "narrowphase.hpp"
using namespace rime;
using namespace rime::physics;

int main() {
    {
        ConvexHull a, b;
        a.vertices = {{-0.275697649f, -0.601077139f, 0.161548257f},
                      {0.524547637f, -0.0734103471f, 0.161548257f},
                      {0.0558150336f, 0.602120161f, 0.161548257f},
                      {-0.429802358f, -0.484583378f, 0.161548257f},
                      {-0.0307063516f, 0.865053177f, -0.138451755f},
                      {0.587092698f, -0.0253099352f, -0.138451755f},
                      {-0.270058811f, -0.59049958f, -0.138451755f},
                      {-0.577347696f, -0.35820806f, -0.138451755f}};
        a.face_normals = {{7.36250882e-09f, 0.0f, 1.0f},
                          {0.0f, 0.0f, -1.0f},
                          {0.550382078f, -0.834694743f, -0.0190849788f},
                          {0.794629693f, 0.55137223f, 0.254071534f},
                          {-0.775641322f, 0.346612453f, 0.527484834f},
                          {-0.602558613f, -0.797099888f, -0.0394302085f}};
        a.face_indices = {0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 6, 5, 2, 1, 5, 4, 3, 2, 4, 7, 0, 3, 7, 6};
        a.face_offsets = {0, 4, 8, 12, 16, 20, 24};
        b.vertices = {{-1.11780584f, -0.400078356f, 0.149440497f},
                      {1.27729368f, -0.400078356f, 0.149440497f},
                      {1.08461285f, -0.0914429948f, 0.149440497f},
                      {0.595395327f, 0.286631227f, 0.149440497f},
                      {-0.481936097f, 0.711084187f, 0.149440497f},
                      {1.26903439f, -0.400078356f, -0.150559515f},
                      {-1.10301042f, -0.400078356f, -0.150559515f},
                      {-0.471946001f, 0.702687323f, -0.150559515f},
                      {0.547023773f, 0.301227987f, -0.150559515f},
                      {1.09613991f, -0.123136692f, -0.150559515f}};
        b.face_normals = {{0.0f, 0.0f, 1.0f},
                          {2.38856979e-09f, 0.0f, -1.0f},
                          {0.0f, -1.0f, -9.44253387e-08f},
                          {0.366526574f, 0.930304766f, -0.0138333701f},
                          {0.610389292f, 0.78982681f, -0.0599884838f},
                          {-0.867139518f, 0.496225953f, -0.0427653939f},
                          {0.848034143f, 0.529427111f, -0.0233472195f}};
        b.face_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 6, 5, 1, 0, 8,
                          7, 4, 3, 9, 8, 3, 2, 0, 4, 7, 6, 1, 5, 9, 2};
        b.face_offsets = {0, 5, 10, 14, 18, 22, 26, 30};
        core::Vec3 posA = {29.9181042f, 2.06812119f, -13.9763346f};
        core::Quat quatA = {-0.0165744592f, -0.0117769428f, -0.00448407372f, 0.999783218f};
        core::Vec3 posB = {30.3253403f, 3.36591744f, -13.951601f};
        core::Quat quatB = {-0.0176558793f, -0.00938515645f, 0.00620343443f, 0.999780834f};
        auto av = narrowphase_detail::make_hull_poly_view(a, posA, quatA);
        auto bv = narrowphase_detail::make_hull_poly_view(b, posB, quatB);
        narrowphase_detail::PolySupport sa{&av}, sb{&bv};
        auto g = gjk(sa, sb, posA - posB);
        EpaResult e;
        if (g.overlapping)
            e = epa(sa, sb, g.simplex, g.simplex_count);
        Manifold m;
        bool hit = collide_hull_hull(a, posA, quatA, b, posB, quatB, m);
        printf("fixture 0: overlap=%d distance=%g simplex=%d EPA valid=%d depth=%g contact=%d "
               "count=%u\n",
               g.overlapping,
               g.distance,
               g.simplex_count,
               e.valid,
               e.depth,
               hit,
               m.count);
        for (int j = 0; j < g.simplex_count; ++j)
            printf(
                "w%d=(%.9g,%.9g,%.9g)\n", j, g.simplex[j].w.x, g.simplex[j].w.y, g.simplex[j].w.z);
        gjk_detail::Vec3d dw[4];
        for (int j = 0; j < g.simplex_count; ++j)
            dw[j] = gjk_detail::to_double(g.simplex[j].w);
        auto exact = gjk_detail::closest_on_hull_double(dw, g.simplex_count);
        printf("double simplex: contained=%d distance=%.9g\n",
               exact.contained,
               std::sqrt(exact.dist2));
        if (g.simplex_count == 4) {
            auto e1 = g.simplex[1].w - g.simplex[0].w, e2 = g.simplex[2].w - g.simplex[0].w,
                 e3 = g.simplex[3].w - g.simplex[0].w;
            printf("float determinant=%.9g scale=%.9g\n",
                   core::dot(core::cross(e1, e2), e3),
                   std::sqrt(core::dot(e1, e1) * core::dot(e2, e2) * core::dot(e3, e3)));
        }
        auto aa = hull_world_aabb(a, posA, quatA), bb = hull_world_aabb(b, posB, quatB);
        printf("AABB gaps=(%.9g,%.9g,%.9g)\n",
               std::max(aa.min.x - bb.max.x, bb.min.x - aa.max.x),
               std::max(aa.min.y - bb.max.y, bb.min.y - aa.max.y),
               std::max(aa.min.z - bb.max.z, bb.min.z - aa.max.z));
    }
    {
        ConvexHull a, b;
        a.vertices = {{2.04527187f, -0.149781018f, -0.922629714f},
                      {-2.29699349f, -0.149781018f, -0.922629714f},
                      {-2.27060366f, 0.150218993f, -0.922629714f},
                      {2.01808786f, 0.150218993f, -0.922629714f},
                      {2.04295278f, 0.150218993f, -0.649329782f},
                      {-0.611436844f, 0.150218993f, 1.15610111f},
                      {0.323465884f, 0.150218993f, 1.6500777f},
                      {2.06687975f, -0.149781018f, -0.685129046f},
                      {0.320930749f, -0.149781018f, 1.64966547f},
                      {-0.655793071f, -0.149781018f, 1.13359153f}};
        a.face_normals = {{0.0f, 0.0f, -1.0f},
                          {0.0f, 1.0f, 0.0f},
                          {0.0f, -1.0f, -2.32901942e-09f},
                          {0.991856575f, 0.0898747519f, -0.0902394056f},
                          {-0.779727876f, 0.0685897246f, 0.622350276f},
                          {-0.467168003f, 0.00273251091f, 0.884164333f},
                          {0.80082351f, -0.00759046152f, 0.598852336f}};
        a.face_indices = {0, 1, 2, 3, 4, 3, 2, 5, 6, 1, 0, 7, 8, 9, 7,
                          0, 3, 4, 1, 9, 5, 2, 6, 5, 9, 8, 4, 6, 8, 7};
        a.face_offsets = {0, 4, 9, 14, 18, 22, 26, 30};
        b.vertices = {{0.580522239f, -1.50598812f, 0.151358098f},
                      {1.13161051f, -0.278614223f, 0.151358098f},
                      {-0.875978351f, 1.30748999f, 0.151358098f},
                      {-1.41434038f, 1.30748999f, 0.151358098f},
                      {-1.46879423f, 1.30748999f, -0.148641914f},
                      {-0.889169574f, 1.30748999f, -0.148641914f},
                      {1.12930286f, -0.28721267f, -0.148641914f},
                      {0.560360134f, -1.55435181f, -0.148641914f}};
        b.face_normals = {{-1.2282487e-08f, 0.0f, 1.0f},
                          {-1.16329577e-08f, 0.0f, -1.0f},
                          {0.0f, 1.0f, 4.44285604e-08f},
                          {0.61969471f, 0.784369886f, -0.0272482745f},
                          {0.912253082f, -0.409599692f, 0.00472247414f},
                          {-0.806955159f, -0.572161794f, 0.146472648f}};
        b.face_indices = {0, 1, 2, 3, 4, 5, 6, 7, 3, 2, 5, 4, 6, 5, 2, 1, 7, 6, 1, 0, 0, 3, 4, 7};
        b.face_offsets = {0, 4, 8, 12, 16, 20, 24};
        core::Vec3 posA = {3.46282816f, 0.426654279f, -12.8053885f};
        core::Quat quatA = {-0.00270736404f, -0.000463540375f, -0.000841144065f, 0.999995947f};
        core::Vec3 posB = {3.06828094f, 1.66893756f, -13.9257278f};
        core::Quat quatB = {-0.0133385239f, 0.00178589975f, 0.000704072125f, 0.999909282f};
        auto av = narrowphase_detail::make_hull_poly_view(a, posA, quatA);
        auto bv = narrowphase_detail::make_hull_poly_view(b, posB, quatB);
        narrowphase_detail::PolySupport sa{&av}, sb{&bv};
        auto g = gjk(sa, sb, posA - posB);
        EpaResult e;
        if (g.overlapping)
            e = epa(sa, sb, g.simplex, g.simplex_count);
        Manifold m;
        bool hit = collide_hull_hull(a, posA, quatA, b, posB, quatB, m);
        printf("fixture 1: overlap=%d distance=%g simplex=%d EPA valid=%d depth=%g contact=%d "
               "count=%u\n",
               g.overlapping,
               g.distance,
               g.simplex_count,
               e.valid,
               e.depth,
               hit,
               m.count);
        for (int j = 0; j < g.simplex_count; ++j)
            printf(
                "w%d=(%.9g,%.9g,%.9g)\n", j, g.simplex[j].w.x, g.simplex[j].w.y, g.simplex[j].w.z);
        gjk_detail::Vec3d dw[4];
        for (int j = 0; j < g.simplex_count; ++j)
            dw[j] = gjk_detail::to_double(g.simplex[j].w);
        auto exact = gjk_detail::closest_on_hull_double(dw, g.simplex_count);
        printf("double simplex: contained=%d distance=%.9g\n",
               exact.contained,
               std::sqrt(exact.dist2));
        if (g.simplex_count == 4) {
            auto e1 = g.simplex[1].w - g.simplex[0].w, e2 = g.simplex[2].w - g.simplex[0].w,
                 e3 = g.simplex[3].w - g.simplex[0].w;
            printf("float determinant=%.9g scale=%.9g\n",
                   core::dot(core::cross(e1, e2), e3),
                   std::sqrt(core::dot(e1, e1) * core::dot(e2, e2) * core::dot(e3, e3)));
        }
        auto aa = hull_world_aabb(a, posA, quatA), bb = hull_world_aabb(b, posB, quatB);
        printf("AABB gaps=(%.9g,%.9g,%.9g)\n",
               std::max(aa.min.x - bb.max.x, bb.min.x - aa.max.x),
               std::max(aa.min.y - bb.max.y, bb.min.y - aa.max.y),
               std::max(aa.min.z - bb.max.z, bb.min.z - aa.max.z));
    }
}