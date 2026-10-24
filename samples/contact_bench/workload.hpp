// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "contact_bounds.hpp"
#include "narrowphase.hpp"

// A diagnostic format, not an asset or gameplay protocol. Decimal uint32 float BITS avoid both
// lossy text round trips and ABI/padding/endianness dependencies. Geometry is stored in its exact
// runtime frame: registration would re-centre it and change the rounding under investigation.
// Loading is cold; only replayed collision calls belong in timing measurements.
namespace contact_workload {
namespace physics = rime::physics;
namespace core = rime::core;
inline constexpr std::uint32_t kNoHull = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kMaxRows = 500000;

struct Shape {
    physics::ShapeDesc desc{};
    std::uint32_t hull = kNoHull;
};

struct Pair {
    std::string world;
    std::uint64_t step = 0;
    physics::BodyId a{}, b{};
    Shape sa{}, sb{};
    core::Vec3 pa{}, pb{}, va{}, vb{};
    core::Quat qa{}, qb{};
    float dt = 0.0f;
    bool ccd = false;
    bool exact = false;
    physics::Manifold expected{};

    [[nodiscard]] auto key() const {
        return std::tie(world, step, a.index, a.generation, b.index, b.generation);
    }
};

struct Ledger {
    std::uint64_t steps = 0, selected_steps = 0, queries = 0, selected_queries = 0;
    std::uint64_t rows = 0, drops = 0;
};

struct Workload {
    std::uint32_t stride = 0;
    std::vector<physics::ConvexHull> hulls;
    std::vector<Pair> pairs;
    Ledger ledger{};
};

inline void write_float(std::ostream& out, float f) {
    out << ' ' << std::bit_cast<std::uint32_t>(f);
}

inline void write_vec(std::ostream& out, core::Vec3 v) {
    write_float(out, v.x);
    write_float(out, v.y);
    write_float(out, v.z);
}

inline void write_quat(std::ostream& out, core::Quat q) {
    write_float(out, q.x);
    write_float(out, q.y);
    write_float(out, q.z);
    write_float(out, q.w);
}

inline void write_shape(std::ostream& out, const Shape& s) {
    out << ' ' << static_cast<unsigned>(s.desc.type) << ' ' << s.hull;
    write_float(out, s.desc.radius);
    write_vec(out, s.desc.half_extents);
    write_float(out, s.desc.half_height);
}

inline void write_hull(std::ostream& out, std::uint32_t id, const physics::ConvexHull& h) {
    out << "H " << id << ' ' << h.vertices.size();
    for (auto v : h.vertices)
        write_vec(out, v);
    out << ' ' << h.face_normals.size();
    for (auto v : h.face_normals)
        write_vec(out, v);
    out << ' ' << h.face_offsets.size();
    for (auto v : h.face_offsets)
        out << ' ' << v;
    out << ' ' << h.face_indices.size();
    for (auto v : h.face_indices)
        out << ' ' << v;
    out << '\n';
}

inline void write_pair(std::ostream& out, const Pair& p) {
    out << "P " << p.world << ' ' << p.step << ' ' << p.a.index << ' ' << p.a.generation << ' '
        << p.b.index << ' ' << p.b.generation;
    write_shape(out, p.sa);
    write_shape(out, p.sb);
    write_vec(out, p.pa);
    write_quat(out, p.qa);
    write_vec(out, p.pb);
    write_quat(out, p.qb);
    write_vec(out, p.va);
    write_vec(out, p.vb);
    write_float(out, p.dt);
    out << ' ' << p.ccd << ' ' << p.exact;
    if (p.exact) {
        out << ' ' << static_cast<unsigned>(p.expected.count);
        write_vec(out, p.expected.normal);
        for (unsigned i = 0; i < p.expected.count; ++i) {
            const auto& c = p.expected.points[i];
            write_vec(out, c.position);
            write_float(out, c.penetration);
            out << ' ' << c.feature_id;
        }
    }
    out << '\n';
}

inline void write_footer(std::ostream& out, const Ledger& l) {
    out << "F " << l.steps << ' ' << l.selected_steps << ' ' << l.queries << ' '
        << l.selected_queries << ' ' << l.rows << ' ' << l.drops << '\n';
}

// The line and per-array bounds keep a corrupt fixture from requesting arbitrary allocations.
// Every parsed float must be finite; a missing footer, partial row, extra token, duplicate pair
// identity, cap drop or inconsistent ledger refuses the WHOLE corpus rather than a passing prefix.
struct Reader {
    std::istringstream in;
    bool good = true;

    explicit Reader(const std::string& line) : in(line) {}

    std::uint64_t u64() {
        std::string token;
        if (!(in >> token) || token.empty() || token.size() > 20) {
            good = false;
            return 0;
        }
        std::uint64_t n = 0;
        for (char c : token) {
            if (c < '0' || c > '9' || n > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10) {
                good = false;
                return 0;
            }
            n = n * 10 + static_cast<unsigned>(c - '0');
        }
        return n;
    }

    std::uint32_t u32() {
        const auto n = u64();
        if (n > UINT32_MAX)
            good = false;
        return static_cast<std::uint32_t>(n);
    }

    float scalar() {
        const float f = std::bit_cast<float>(u32());
        if (!std::isfinite(f))
            good = false;
        return f;
    }

    core::Vec3 vec() {
        const float x = scalar(), y = scalar(), z = scalar();
        return {x, y, z};
    }

    core::Quat quat() {
        const float x = scalar(), y = scalar(), z = scalar(), w = scalar();
        if (!(x * x + y * y + z * z + w * w > 0.0f))
            good = false;
        return {x, y, z, w};
    }

    std::uint32_t count(std::uint32_t min, std::uint32_t max) {
        const auto n = u32();
        if (n < min || n > max) {
            good = false;
            return 0;
        }
        return n;
    }

    bool boolean() {
        const auto n = u32();
        if (n > 1)
            good = false;
        return n != 0;
    }

    bool end() {
        in >> std::ws;
        return good && in.eof();
    }
};

inline Shape read_shape(Reader& r, std::size_t hulls) {
    Shape s;
    const auto type = r.u32();
    if (type > static_cast<unsigned>(physics::ShapeType::ConvexHull))
        r.good = false;
    s.desc.type = static_cast<physics::ShapeType>(type);
    s.hull = r.u32();
    if (s.hull != kNoHull && (s.hull >= hulls || s.desc.type != physics::ShapeType::ConvexHull))
        r.good = false;
    s.desc.radius = r.scalar();
    s.desc.half_extents = r.vec();
    s.desc.half_height = r.scalar();
    if (s.desc.radius < 0 || s.desc.half_height < 0 || s.desc.half_extents.x < 0 ||
        s.desc.half_extents.y < 0 || s.desc.half_extents.z < 0)
        r.good = false;
    return s;
}

inline bool load(std::istream& input, Workload& result, std::string& error) {
    Workload w;
    std::string line;
    std::uint64_t line_number = 0;
    bool header = false, footer = false;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > 1024 * 1024 || footer)
            break;
        Reader r(line);
        std::string tag;
        r.in >> tag;
        if (!header) {
            if (tag != "RIME_CONTACTS" || r.u32() != 1)
                break;
            w.stride = r.count(1, 1000000);
            header = true;
        } else if (tag == "H") {
            const auto id = r.u32();
            if (id != w.hulls.size() || id >= 10000)
                break;
            physics::ConvexHull h;
            const auto nv = r.count(4, 4096);
            for (unsigned i = 0; i < nv; ++i)
                h.vertices.push_back(r.vec());
            const auto nf = r.count(4, 4096);
            for (unsigned i = 0; i < nf; ++i)
                h.face_normals.push_back(r.vec());
            const auto no = r.count(nf + 1, nf + 1);
            for (unsigned i = 0; i < no; ++i)
                h.face_offsets.push_back(r.u32());
            const auto ni = r.count(12, 65536);
            for (unsigned i = 0; i < ni; ++i) {
                const auto index = r.u32();
                if (index >= nv)
                    r.good = false;
                h.face_indices.push_back(index);
            }
            if (h.face_offsets.empty() || h.face_offsets.front() != 0 ||
                h.face_offsets.back() != ni)
                r.good = false;
            for (std::size_t i = 1; i < h.face_offsets.size(); ++i) {
                const auto a = h.face_offsets[i - 1], b = h.face_offsets[i];
                if (b < a || b - a < 3 || b - a > physics::hull_detail::kMaxHullFaceVertices)
                    r.good = false;
            }
            w.hulls.push_back(std::move(h));
        } else if (tag == "P") {
            if (w.pairs.size() >= kMaxRows)
                break;
            Pair p;
            r.in >> p.world;
            if (p.world.empty() || p.world.size() > 96)
                r.good = false;
            p.step = r.u64();
            p.a.index = r.u32();
            p.a.generation = r.u32();
            p.b.index = r.u32();
            p.b.generation = r.u32();
            if (p.a.index >= p.b.index || p.step == 0 || p.step % w.stride != 0)
                r.good = false;
            p.sa = read_shape(r, w.hulls.size());
            p.sb = read_shape(r, w.hulls.size());
            p.pa = r.vec();
            p.qa = r.quat();
            p.pb = r.vec();
            p.qb = r.quat();
            p.va = r.vec();
            p.vb = r.vec();
            p.dt = r.scalar();
            if (p.dt < 0)
                r.good = false;
            p.ccd = r.boolean();
            p.exact = r.boolean();
            p.expected.a = p.a;
            p.expected.b = p.b;
            if (p.exact) {
                p.expected.count = static_cast<std::uint8_t>(r.count(1, 4));
                p.expected.normal = r.vec();
                for (unsigned i = 0; i < p.expected.count; ++i) {
                    auto& c = p.expected.points[i];
                    c.position = r.vec();
                    c.penetration = r.scalar();
                    c.feature_id = r.u32();
                    if (c.penetration < 0)
                        r.good = false;
                }
            }
            w.pairs.push_back(std::move(p));
        } else if (tag == "F") {
            auto& l = w.ledger;
            l.steps = r.u64();
            l.selected_steps = r.u64();
            l.queries = r.u64();
            l.selected_queries = r.u64();
            l.rows = r.u64();
            l.drops = r.u64();
            if (l.steps < l.selected_steps || l.selected_steps == 0 ||
                l.queries < l.selected_queries || l.rows != w.pairs.size() || l.rows == 0 ||
                l.drops != 0 || l.selected_queries != l.rows)
                r.good = false;
            footer = true;
        } else
            break;
        if (!r.end())
            break;
        if (footer) {
            // Even a syntactically valid footer cannot launder trailing junk or duplicate rows.
            if (input.peek() != std::char_traits<char>::eof())
                break;
            std::sort(w.pairs.begin(), w.pairs.end(), [](const Pair& a, const Pair& b) {
                return a.key() < b.key();
            });
            if (std::adjacent_find(
                    w.pairs.begin(), w.pairs.end(), [](const Pair& a, const Pair& b) {
                        return a.key() == b.key();
                    }) != w.pairs.end())
                break;
            result = std::move(w);
            error.clear();
            return true;
        }
    }
    error = "invalid/incomplete contact workload at line " + std::to_string(line_number);
    return false;
}

inline const physics::ConvexHull* hull(const Workload& w, const Shape& s) {
    return s.hull == kNoHull ? nullptr : &w.hulls[s.hull];
}

inline bool exact(const Workload& w, const Pair& p, physics::Manifold& m) {
    m = {};
    m.a = p.a;
    m.b = p.b;
    return physics::collide_shapes(
               p.sa.desc, p.pa, p.qa, p.sb.desc, p.pb, p.qb, m, hull(w, p.sa), hull(w, p.sb)) &&
           m.count != 0;
}

// Experimental policies only: tight is a sensitivity control; child reuses the existing
// compound midphase's 1 mm margin, fixed BEFORE the capture is examined. Neither policy is
// enabled in PhysicsWorld. The oracle must reject even one discarded reference contact.
enum class Candidate { None, Tight, Child };

inline bool bounds_may_touch(const physics::Aabb& a, const physics::Aabb& b, Candidate candidate) {
    return physics::overlaps(
        candidate == Candidate::Child ? physics::contact_bounds_detail::padded(a) : a, b);
}

inline bool reject_bounds(const Workload& w, const Pair& p, Candidate candidate) {
    if (candidate == Candidate::None)
        return false;
    const auto bounds = [&](const Shape& s, core::Vec3 pos, core::Quat q) {
        const auto* h = hull(w, s);
        return h ? physics::hull_world_aabb(*h, pos, q) : physics::compute_aabb(s.desc, pos, q);
    };
    if ((p.sa.desc.type == physics::ShapeType::ConvexHull && p.sa.hull == kNoHull) ||
        (p.sb.desc.type == physics::ShapeType::ConvexHull && p.sb.hull == kNoHull))
        return false;
    return !bounds_may_touch(bounds(p.sa, p.pa, p.qa), bounds(p.sb, p.pb, p.qb), candidate);
}

struct Evaluation {
    physics::Manifold manifold{};
    bool exact = false, contact = false, speculative = false, skipped = false;
};

inline Evaluation evaluate_decision(const Workload& w, const Pair& p, bool skip) {
    Evaluation e;
    e.manifold.a = p.a;
    e.manifold.b = p.b;
    e.skipped = skip;
    e.exact = !e.skipped && exact(w, p, e.manifold);
    e.contact = e.exact;
    // Rejecting the CURRENT overlap test never rejects CCD: an approaching pair may still
    // need a negative-depth speculative contact. Always preserve that separate dispatch.
    if (!e.exact && p.ccd && p.dt > 0.0f) {
        e.contact = physics::collide_speculative(p.sa.desc,
                                                 p.pa,
                                                 p.qa,
                                                 p.va,
                                                 p.sb.desc,
                                                 p.pb,
                                                 p.qb,
                                                 p.vb,
                                                 p.dt,
                                                 e.manifold,
                                                 hull(w, p.sa),
                                                 hull(w, p.sb));
        e.speculative = e.contact;
    }
    return e;
}

inline Evaluation evaluate(const Workload& w, const Pair& p, Candidate candidate) {
    return evaluate_decision(w, p, reject_bounds(w, p, candidate));
}

inline bool same_float(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

inline bool same_vec(core::Vec3 a, core::Vec3 b) {
    return same_float(a.x, b.x) && same_float(a.y, b.y) && same_float(a.z, b.z);
}

inline bool same_manifold(const physics::Manifold& a, const physics::Manifold& b) {
    if (a.a != b.a || a.b != b.b || a.count != b.count || a.child_a != b.child_a ||
        a.child_b != b.child_b || a.patch != b.patch || !same_vec(a.normal, b.normal))
        return false;
    for (unsigned i = 0; i < a.count; ++i) {
        const auto& x = a.points[i];
        const auto& y = b.points[i];
        if (!same_vec(x.position, y.position) || !same_float(x.penetration, y.penetration) ||
            x.feature_id != y.feature_id || !same_float(x.normal_impulse, y.normal_impulse) ||
            !same_float(x.tangent_impulse, y.tangent_impulse))
            return false;
    }
    return true;
}
} // namespace contact_workload
