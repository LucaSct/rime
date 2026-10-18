// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <sstream>

#include "../../samples/contact_bench/workload.hpp"

namespace cw = contact_workload;
namespace physics = rime::physics;
namespace core = rime::core;

namespace {
cw::Workload fixture() {
    cw::Workload w;
    w.stride = 1;
    // A runtime tetrahedron dictionary row also exercises topology validation, independently of
    // the primitive contacts below. All arrays are bounded and their CSR relationship is checked.
    physics::ConvexHull h;
    const std::vector<core::Vec3> vertices = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
    const std::vector<std::uint32_t> counts = {3, 3, 3, 3};
    const std::vector<std::uint32_t> indices = {0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2};
    REQUIRE(physics::build_convex_hull(vertices, counts, indices, h));
    w.hulls.push_back(h);
    for (int i = 0; i < 4; ++i) {
        cw::Pair p;
        p.world = i % 2 == 0 ? "physics.server.step" : "physics.client.step";
        p.step = static_cast<std::uint64_t>(i + 1);
        p.a = {1, 2};
        p.b = {3, 4};
        p.sa.desc.type = physics::ShapeType::Sphere;
        p.sb.desc.type = physics::ShapeType::Sphere;
        if (i >= 2) {
            p.sb.desc.type = physics::ShapeType::ConvexHull;
            p.sb.hull = 0;
        }
        p.pb = {i % 2 == 0 ? 0.75f : 4.0f, 0, 0};
        p.dt = 1.0f / 60;
        p.ccd = true;
        p.va = {300, 0, 0};
        p.exact = cw::exact(w, p, p.expected);
        w.pairs.push_back(p);
    }
    w.ledger = {4, 4, 4, 4, 4, 0};
    return w;
}

std::string encode(const cw::Workload& w) {
    std::ostringstream out;
    out << "RIME_CONTACTS 1 " << w.stride << '\n';
    for (std::size_t i = 0; i < w.hulls.size(); ++i)
        cw::write_hull(out, static_cast<std::uint32_t>(i), w.hulls[i]);
    for (const auto& p : w.pairs)
        cw::write_pair(out, p);
    cw::write_footer(out, w.ledger);
    return out.str();
}

bool decode(const std::string& text, cw::Workload& out) {
    std::istringstream in(text);
    std::string error;
    const bool ok = cw::load(in, out, error);
    CHECK(ok == error.empty());
    return ok;
}
} // namespace

TEST_CASE("frozen contact format preserves exact inputs and contact fields") {
    auto w = fixture();
    // Signed zero is observable by the bit oracle, even though float == would hide it.
    w.pairs[0].va.y = -0.0f;
    cw::Workload loaded;
    REQUIRE(decode(encode(w), loaded));
    REQUIRE(loaded.pairs.size() == 4);
    CHECK(loaded.hulls.size() == 1);
    CHECK(loaded.ledger.queries == 4);
    int contacts = 0, misses = 0;
    for (const auto& p : loaded.pairs) {
        physics::Manifold actual;
        const bool hit = cw::exact(loaded, p, actual);
        CHECK(hit == p.exact);
        if (hit) {
            ++contacts;
            CHECK(cw::same_manifold(actual, p.expected));
        } else
            ++misses;
    }
    CHECK(contacts > 0);
    CHECK(misses > 0);
    auto it = std::find_if(
        loaded.pairs.begin(), loaded.pairs.end(), [](const auto& p) { return p.step == 1; });
    REQUIRE(it != loaded.pairs.end());
    CHECK(cw::same_float(it->va.y, -0.0f));
    CHECK_FALSE(cw::same_float(it->va.y, 0.0f));
    auto changed = it->expected;
    changed.points[0].feature_id ^= 1;
    CHECK_FALSE(cw::same_manifold(changed, it->expected));
    changed = it->expected;
    changed.points[0].normal_impulse = 1;
    CHECK_FALSE(cw::same_manifold(changed, it->expected));
}

TEST_CASE("frozen contact loader refuses incomplete corrupt or biased captures atomically") {
    const auto valid = fixture();
    const auto text = encode(valid);
    auto sentinel = valid;
    sentinel.stride = 777;
    const auto reject = [&](const std::string& bad) {
        auto loaded = sentinel;
        CHECK_FALSE(decode(bad, loaded));
        CHECK(loaded.stride == 777);
        CHECK(loaded.pairs.size() == sentinel.pairs.size());
    };
    SUBCASE("truncated input and missing footer") {
        reject(text.substr(0, text.size() / 2));
        reject(text.substr(0, text.rfind("F ")));
    }
    SUBCASE("trailing records") {
        reject(text + "P junk\n");
    }
    SUBCASE("unsupported version") {
        auto bad = text;
        bad.replace(14, 1, "9");
        reject(bad);
    }
    SUBCASE("cap drops and inconsistent ledger") {
        auto bad = valid;
        bad.ledger.drops = 1;
        reject(encode(bad));
        bad = valid;
        bad.ledger.queries = 0;
        reject(encode(bad));
        bad = valid;
        bad.ledger.selected_queries = 3;
        reject(encode(bad));
    }
    SUBCASE("vacuous corpus") {
        auto bad = valid;
        bad.pairs.clear();
        bad.ledger.rows = 0;
        bad.ledger.selected_queries = 0;
        reject(encode(bad));
    }
    SUBCASE("duplicate semantic identity independent of append order") {
        auto bad = valid;
        bad.pairs[1] = bad.pairs[0];
        reject(encode(bad));
    }
    SUBCASE("unknown hull and nonconvex type") {
        auto bad = valid;
        bad.pairs[0].sa.hull = 20;
        reject(encode(bad));
        bad = valid;
        bad.pairs[0].sa.desc.type = physics::ShapeType::Compound;
        reject(encode(bad));
    }
    SUBCASE("nonfinite scalar and invalid topology") {
        auto bad = valid;
        bad.pairs[0].pa.x = std::numeric_limits<float>::infinity();
        reject(encode(bad));
        bad = valid;
        bad.hulls[0].face_indices[0] = 999;
        reject(encode(bad));
        bad = valid;
        bad.hulls[0].face_offsets.back() = 0;
        reject(encode(bad));
    }
    SUBCASE("selected-step policy") {
        auto bad = valid;
        bad.stride = 2;
        reject(encode(bad));
    }
    SUBCASE("overlarge integer and malformed numeric token") {
        auto bad = text;
        const auto f = bad.rfind("F ");
        bad.replace(f + 2, 1, "18446744073709551616");
        reject(bad);
        bad = text;
        bad.replace(f + 2, 1, "-1");
        reject(bad);
    }
}

TEST_CASE("contact candidates keep exact contacts and approaching CCD misses bit identical") {
    const auto w = fixture();
    for (const auto candidate : {cw::Candidate::Tight, cw::Candidate::Child}) {
        unsigned skipped = 0, speculative = 0, exact = 0;
        for (const auto& p : w.pairs) {
            const auto reference = cw::evaluate(w, p, cw::Candidate::None);
            const auto proposed = cw::evaluate(w, p, candidate);
            CHECK(reference.contact == proposed.contact);
            CHECK(reference.exact == proposed.exact);
            CHECK(reference.speculative == proposed.speculative);
            REQUIRE(reference.contact);
            CHECK(cw::same_manifold(reference.manifold, proposed.manifold));
            if (proposed.skipped)
                ++skipped;
            if (proposed.exact)
                ++exact;
            if (proposed.speculative) {
                ++speculative;
                CHECK(proposed.manifold.points[0].penetration < 0);
            }
            // A stationary separated pair at dt=0 must not gain a speculative contact.
            auto inspection = p;
            inspection.dt = 0;
            CHECK(cw::evaluate(w, inspection, candidate).contact == p.exact);
        }
        CHECK(skipped > 0);
        CHECK(exact > 0);
        CHECK(speculative > 0);
    }
}
