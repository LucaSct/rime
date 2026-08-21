// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay/character.hpp"

#include <algorithm>
#include <cmath>

#include "rime/physics/query.hpp"
#include "rime/physics/shape.hpp"

// COLLIDE-AND-SLIDE, and the numerics that decide whether it feels solid or feels haunted.
//
// The algorithm is short enough to state in four lines: cast the capsule along the motion you want;
// stop a hair short of what you hit; remove the component of the leftover motion that points into
// that surface; go round again. Everything else in this file is the handling of the cases where
// that description is not quite true — the surface you are already inside of, the step lip whose
// normal lies about being a wall, the corner where two surfaces leave no legal direction at all.
// Those cases are the entire difference between a controller and a demo.
//
// ── SKIN IS A CONTACT OFFSET, NOT AN EPSILON ─────────────────────────────────────────────────
// The capsule is kept `skin` metres away from every surface. That is not distrust of the cast; it
// is what makes the NEXT cast well-posed. A capsule resting exactly on a plane is a degenerate GJK
// configuration — the witness points coincide, the direction between them is whatever cancellation
// left behind, and the reported normal is noise. Standing off by 2 cm means every query the
// controller ever issues is made from a configuration where the geometry is unambiguous.
//
// HOW that offset is realised is itself load-bearing and cost a rewrite to get right: the movement
// queries sweep a capsule INFLATED by skin rather than subtracting skin from a reported distance.
// See the Caster block below for the measurement behind that, and for what the subtracted form
// does to a character on a slope.
//
// The size is measured, not chosen. The cast's own tolerance stack is kTouchTolerance = 5e-5 plus
// GJK's worst residual ~2.3e-4 (src/scene_query.hpp, ROADMAP 2026-08-21), so 2 cm dominates the
// error floor by roughly two orders of magnitude, and still dominates — by 2x — the 9.9e-3 worst
// overshoot measured before the shallow-penetration stall fix.
//
// ── SCALE-RELATIVE EPSILONS ──────────────────────────────────────────────────────────────────
// #131's lesson: an absolute length threshold is wrong at some scale, always. Every length
// comparison here is relative to a config length — `min_move` is a thousandth of the radius, the
// step-up progress gate is `skin`, the snap placement is `skin`. The controller adds NO new
// absolute length constant.
//
// The DIMENSIONLESS comparisons are the deliberate exception and are called out where they occur:
// `max_slope_cos` and the crease's sine floor are ratios of unit vectors. Scaling those with a
// length would be a category error, and the next reader to "fix" them should find this paragraph
// first.
namespace rime::gameplay {
namespace {

// World up. The controller is up-axis-aware by construction (gravity, slope, step, snap all refer
// to it); making it a runtime parameter would be a different, larger design.
constexpr core::Vec3 kUp{0.0f, 1.0f, 0.0f};

// How nearly parallel two contact planes may be before their crease direction is meaningless.
// DIMENSIONLESS: |cross(a, b)| of two unit vectors is the sine of the angle between them, so this
// is "closer than about a twentieth of a degree". Below it the cross product is float noise and
// the honest response is to stop rather than to slide along a direction we invented.
constexpr float kCreaseSinFloor2 = 1e-6f; // (1e-3)^2

// The controller's slide loop keeps at most this many contact planes. Three is not a budget, it is
// the dimension: three independent planes pin a point in space, so a fourth simultaneous contact
// cannot be satisfied by any direction and the only correct answer is to stop.
constexpr int kMaxPlanes = 3;

// At most this many depenetration pushes per tick before the tick gives up and says so. Two,
// because the second one exists to handle "pushed out of A, now slightly inside B" — the wedge —
// and a third would be an unbounded search wearing a small number's clothes.
constexpr int kMaxRecoveryPushes = 2;

// A sweep stops this fraction of the contact offset SHORT of where the inflated probe would touch,
// so the probe is never left exactly tangent to a surface.
//
// This is not the contact offset — the inflated probe is (see Caster). It is the smaller,
// separate requirement that the NEXT query be well posed. A probe resting exactly on a plane
// reports `initial_overlap` for a sweep in ANY direction, including one straight along the
// surface; the whole tick then measures nothing and the character cannot move. Worse, the flag is
// raised by whatever the probe is touching — the FLOOR — which masks the wall it was walking into.
// Measured: a 30° climb frozen after 0.6 m with 290 exhausted slide budgets in 300 ticks, the
// velocity perfectly correct at 5.196 m/s the entire time.
//
// Backing off along the sweep is the classic over-correction on a graze (it divides by a cosine
// nobody applied) — but it can no longer pin anything, because a grazing move never reaches this
// code: a tangential sweep leaves the destination reachable, and the confirm-first branch in the
// slide loop takes it. The back-off only runs when the character is genuinely approaching a
// surface, which is exactly the geometry it is correct for.
constexpr float kBackOffFraction = 0.5f;

// How firmly a contact must OPPOSE the motion to be capable of blocking it. DIMENSIONLESS — a
// cosine between two unit vectors — and deliberately the same value and the same argument as the
// shape cast's own kNormalOpposesSweep (src/scene_query.hpp): a surface you ran INTO must have a
// normal pointing back along the way you came. A plane with dot(n, dir) == 0 is PARALLEL to the
// motion; travelling along it does not approach it, so it cannot be what stopped you, whatever a
// query says.
//
// This is the controller's last line against a query that is wrong in the same way twice. Measured:
// walking up a 30° ramp, at one particular spot 19 m from the ramp's centre, both the cast and the
// penetration measurement reported the ramp as blocking a purely TANGENTIAL move. The clip then
// removed nothing (the normal was perpendicular to the velocity), so the next tick posed the
// identical query, got the identical answer, and the character stood still for 147 ticks with a
// flawless 5.196 m/s velocity. Nothing else in this file can break that loop, because nothing else
// changes between the ticks — but the geometry can: a perpendicular normal is not an obstruction.
constexpr float kBlockingOpposition = 1e-3f;

[[nodiscard]] core::Vec3 horizontal(core::Vec3 v) noexcept {
    return core::Vec3{v.x, 0.0f, v.z};
}

[[nodiscard]] bool finite(float x) noexcept {
    return std::isfinite(x);
}

// The smallest motion worth casting for, as a fraction of the capsule's own size. Below this the
// remaining displacement is inside the query's noise and another iteration would only convert
// noise into jitter.
[[nodiscard]] float min_move(const CharacterConfig& c) noexcept {
    return 1e-3f * c.radius;
}

[[nodiscard]] bool walkable(core::Vec3 normal, const CharacterConfig& c) noexcept {
    // Dimensionless by construction: both operands are unit vectors, so this is cos(slope).
    return core::dot(normal, kUp) >= c.max_slope_cos;
}

// Move `v` toward `target` by at most `max_delta`, in a straight line through velocity space.
// Straight-line rather than per-axis, so a diagonal input accelerates at the same rate as a
// cardinal one (per-axis clamping is the same bug as clamping the input square instead of the
// input disc, one derivative up).
[[nodiscard]] core::Vec3 approach(core::Vec3 v, core::Vec3 target, float max_delta) noexcept {
    const core::Vec3 delta = target - v;
    const float len = core::length(delta);
    if (len <= max_delta || len <= 0.0f) {
        return target;
    }
    return v + delta * (max_delta / len);
}

// ── HOW THE SKIN IS APPLIED: an INFLATED PROBE, not a subtracted distance ────────────────────
//
// The obvious way to keep a contact offset is to cast the real capsule and stop `skin` short:
// `advance = hit.distance - skin`. It is wrong, and wrong in a way that only shows up on a slope,
// which is where a character spends its life.
//
// `hit.distance` is measured ALONG THE SWEEP. Subtracting a fixed `skin` from it therefore leaves
// a clearance of `skin` measured along the sweep too, and the PERPENDICULAR clearance that
// actually matters is that times the cosine of the approach angle. Slide down a 60° face and the
// approach is nearly tangential: the perpendicular clearance collapses toward zero, the next
// tick's cast reports a contact at a distance BELOW skin, `distance - skin` clamps to zero, and
// the character is pinned in mid-air with a perfectly healthy velocity. Measured before this
// change: frozen at tick 6, 281 exhausted slide budgets in 300 ticks, velocity climbing past
// 20 m/s against a position that never moved.
//
// So the skin is applied where it is geometrically meaningful — to the SHAPE. Every movement query
// sweeps a capsule of radius + skin. A hit distance then already IS the safe advance, the
// clearance it leaves is perpendicular by construction at any approach angle, and not one line of
// the algorithm has to divide by a cosine. This is what "skin is a contact offset, not an epsilon"
// means operationally: an offset belongs to the geometry, an epsilon belongs to a comparison.
//
// The real capsule keeps one job — the depenetration pre-pass, which must ask about GENUINE
// overlap and would otherwise report every resting contact as a penetration to escape from.
struct Caster {
    const physics::PhysicsWorld* world;
    physics::ShapeDesc sweep;   // the SWEEP probe: radius + skin
    physics::ShapeDesc confirm; // the CONFIRM probe: radius + skin/2 — see below
    physics::QueryFilter filter;
    StepStats* stats;

    [[nodiscard]] bool
    cast(core::Vec3 from, core::Vec3 dir, float distance, physics::ShapeHit& out) const {
        physics::ShapeCast c;
        c.shape = sweep;
        c.origin = from;
        c.orientation = core::quat_identity();
        c.direction = dir;
        c.max_distance = distance;
        ++stats->casts;
        return world->shape_cast(c, out, filter);
    }

    // Is this pose genuinely unreachable — and if so, along which axis? EPA measures both, which
    // is why this is the query the controller trusts over the cast (see PHANTOM CONTACTS in
    // step_character).
    //
    // It measures the REAL capsule, not the inflated one, and the difference is what keeps the two
    // questions apart. The sweep asks "where would I first touch, keeping my offset"; this asks
    // "is this pose actually impossible". Asking the fattened shape the second question conflates
    // them: every pose the controller deliberately produces has the inflated probe touching
    // something, so "resting on the floor" and "blocked by a wall" become one reading, and the
    // character stops dead a few centimetres before every step it meant to climb. (Measured: the
    // 0.25 m step halted at z = -0.45 with the riser at z = -1, never once entering the step-up
    // ladder.)
    //
    // The real capsule also has the LARGEST clearance of any shape here at a resting pose — one
    // and a half contact offsets, ~3 cm — which matters because GJK's overlap predicate has a
    // measured false-positive band at millimetre-to-centimetre clearances against large distant
    // geometry. Asking the question with the most margin available is free, and it is the
    // difference between a controller that walks and one that stutters.
    [[nodiscard]] bool obstructed(core::Vec3 at, physics::PenetrationHit& out) const {
        return world->penetration(confirm, at, core::quat_identity(), out, filter);
    }

    // WHERE THE GROUND IS UNDER `from`, AND WHICH WAY IT FACES — by RAYCAST, and the choice of
    // instrument is the point.
    //
    // "What am I standing on" is the most consequential question this controller asks: its answer
    // decides `walkable`, which decides grounded, which decides gravity, slope projection and
    // whether a step-up is accepted. A shape query answers it badly at exactly the pose the
    // character occupies — resting contact, where witness points coincide and the normal is
    // whatever cancellation left behind — and worse, `penetration()` reports the DEEPEST overlap,
    // which near a step lip is the RISER rather than the floor. Measured: a character standing
    // squarely on flat ground beside a 0.35 m step was un-grounded, because the query it asked
    // about the floor answered about the wall, and it then floated up the lip for 60 ticks.
    //
    // A ray against a box is a slab test (src/scene_query.hpp) — analytic, exact, and it returns
    // the face's own normal. Against a hull it is the face-plane generalization. No tolerance, no
    // near-tangential regime, no deepest-overlap ambiguity.
    //
    // Named cost (README): a ray is infinitely thin, so a capsule straddling a gap narrower than
    // itself is judged by what is under its AXIS. v1 accepts that; the alternative is the shape
    // query whose failure mode is worse and less predictable.
    //
    // `out_drop` is how far to lower the capsule so it rests `clearance` clear of that plane
    // measured PERPENDICULAR to it — derived from the plane's own normal, so the cosine a slope
    // introduces is applied rather than left implicit. It may be negative (the capsule is already
    // below its resting height), which callers clamp.
    [[nodiscard]] bool ground_below(core::Vec3 from,
                                    const CharacterConfig& cfg,
                                    float reach,
                                    float& out_drop,
                                    core::Vec3& out_normal) const {
        const float clearance = (1.0f + kBackOffFraction) * cfg.skin;
        physics::Ray ray;
        ray.origin = from;
        ray.direction = kUp * -1.0f;
        ray.max_distance = cfg.half_height + cfg.radius + clearance + reach;
        physics::RayHit hit;
        ++stats->casts;
        if (!world->raycast(ray, hit, filter)) {
            return false;
        }
        const float facing = core::dot(hit.normal, kUp);
        if (facing <= 0.0f) {
            return false; // a downward-facing surface is a ceiling, not ground
        }
        // The bottom cap's centre sits `half_height` below the origin and must end up
        // (radius + clearance) from the plane ALONG ITS NORMAL, which costs 1/facing vertically.
        out_drop = hit.distance - (cfg.half_height + (cfg.radius + clearance) / facing);
        out_normal = hit.normal;
        return true;
    }
};

// ── Step-up: why a slope test at a step lip is not enough ────────────────────────────────────
//
// A stair riser is a vertical wall, and the slope test correctly calls it unwalkable — so without
// this, a player walks into every staircase and stops. But the deeper reason step-up exists is
// that at the LIP of a step a capsule's rounded bottom touches an EDGE, and the normal an edge
// contact reports is tilted somewhere between the riser's and the tread's. That normal is not
// wrong, it is just not a description of anything the slope test can classify: sometimes it reads
// walkable and the character climbs a wall, sometimes unwalkable and the character stops dead at a
// 5 cm lip. So the controller does not ask the normal what to do at a lip. It asks the WORLD, with
// three casts that reproduce the motion a leg makes:
//
//   1. up   — is there headroom to lift into? (a low ceiling means no step here)
//   2. along — from up there, does the blocked motion now actually get somewhere?
//   3. down — and is there a WALKABLE surface to put the foot on?
//
// All three must answer yes. Cast 3 is what refuses a too-tall step (nothing to land on within the
// lift) and what refuses "stepping up" onto a steep wedge (there is a surface, but you would slide
// straight back off it).
//
// On success `position` and `remaining` are updated to the climbed pose and the leftover motion.
[[nodiscard]] bool try_step_up(const Caster& caster,
                               const CharacterConfig& cfg,
                               core::Vec3& position,
                               core::Vec3& remaining,
                               bool& grounded,
                               core::Vec3& ground_normal) {
    const core::Vec3 lateral = horizontal(remaining);
    const float lateral_len = core::length(lateral);
    if (lateral_len <= min_move(cfg)) {
        return false; // nothing lateral left to carry over the step
    }
    const core::Vec3 lateral_dir = lateral * (1.0f / lateral_len);

    // Every distance below comes straight off the INFLATED probe (see Caster), so it is already a
    // safe advance and no `- skin` correction appears anywhere in this ladder.

    // 1. LIFT. Stop at any ceiling, and refuse the whole manoeuvre if the lift would be nothing —
    //    a step-up with no headroom is a teleport into a ceiling.
    float lift = cfg.step_height;
    physics::ShapeHit up_hit;
    if (caster.cast(position, kUp, cfg.step_height, up_hit)) {
        if (up_hit.initial_overlap) {
            // Already touching something — which is EXPECTED here and must not abort the attempt:
            // the only way to reach this code is by running into a riser, so of course the probe
            // is in contact with it. A sweep cannot measure headroom from inside a contact, so ask
            // the measurement instead: is the fully lifted pose clear? (Refusing on this flag was
            // what made every step-up fail while reporting a tidy `step_rejected` count — the
            // 0.25 m step stalled forever four rejections per tick.)
            //
            // v1 deferral, named in the README: an obstructed lift refuses outright rather than
            // searching for the tallest lift that fits, so a step under a low ceiling is not
            // climbed.
            physics::PenetrationHit overhead;
            if (caster.obstructed(position + kUp * cfg.step_height, overhead)) {
                return false;
            }
        } else {
            lift = std::min(std::max(up_hit.distance - kBackOffFraction * cfg.skin, 0.0f),
                            cfg.step_height);
        }
    }
    if (lift <= cfg.skin) {
        return false;
    }
    const core::Vec3 lifted = position + kUp * lift;

    // 2. ADVANCE from the lifted pose, and probe a REACH rather than the leftover budget.
    //
    // The distance that decides "is this a step or a wall" is geometric — roughly a capsule radius,
    // the distance at which the footprint is over the tread rather than the riser. The leftover
    // displacement is not that distance and must not be used as it: a character already slowed by
    // the wall it is standing against has a per-tick budget of millimetres, which is BELOW the
    // progress gate, so every attempt is refused and the character stays slow. That is a feedback
    // loop, and it wedges permanently — measured on a 0.25 m step under a 0.3 m budget: stalled at
    // z = -0.69 forever, four step-up rejections every tick, never once accepted.
    //
    // So the probe reaches `max(leftover, radius)`, and — unlike every other advance in this file
    // — the step COMMITS the whole of what it gains. A step-up is a discrete manoeuvre, not a
    // slide: the capsule has to end up with its footprint over the TREAD, and that means moving
    // about a radius forward. Committing only the tick's leftover leaves it hanging over the
    // step's front EDGE instead, where the contact normal is a blend of riser and tread and the
    // walkability test becomes a coin flip. (Measured: a 0.25 m step whose landing normal read
    // (0, 0.495, 0.869) — refused — and a 0.40 m step whose edge normal read (0, 0.723, 0.691),
    // just walkable, so the too-tall step was ACCEPTED and climbed 0.3 m at a time.)
    const float reach = std::max(lateral_len, cfg.radius);
    float gained = reach;
    physics::ShapeHit fwd_hit;
    if (caster.cast(lifted, lateral_dir, reach, fwd_hit)) {
        if (fwd_hit.initial_overlap) {
            return false;
        }
        gained = std::min(std::max(fwd_hit.distance - kBackOffFraction * cfg.skin, 0.0f), reach);
    }
    // THE GATE, and it is what makes step_height a real bound: from the lifted pose the capsule
    // must be able to travel a FULL RADIUS forward — which is exactly the statement "the obstacle
    // has been cleared", since a capsule whose centre has advanced a radius past the riser is
    // standing over what is beyond it. A step too tall for the lift blocks that move immediately
    // (the 0.40 m riser above gained 0.131 m of 0.4 m), so it is refused on a MEASURED clearance
    // rather than on the reading of an edge normal.
    //
    // Named cost (README): a climbable step with a wall less than a radius beyond it is refused.
    if (gained < cfg.radius) {
        return false;
    }
    const core::Vec3 advanced = lifted + lateral_dir * gained;

    // 3. SET THE FOOT DOWN, no further than we lifted. A miss means there is nothing under the new
    //    position; an unwalkable landing means we would slide straight back off; a landing further
    //    down than we lifted is not a step at all but a fall we would be performing instantly.
    //    All three refuse.
    //
    // Asked with a RAY (Caster::ground_below), because a step lip is precisely where a shape
    // query's normal is least trustworthy and "is this walkable" is the question the whole ladder
    // turns on.
    float drop = 0.0f;
    core::Vec3 floor_normal{0.0f, 1.0f, 0.0f};
    if (!caster.ground_below(advanced, cfg, lift, drop, floor_normal)) {
        return false;
    }
    if (!walkable(floor_normal, cfg) || drop > lift) {
        return false;
    }
    drop = std::max(drop, 0.0f); // never LIFT on the way down; that is the recovery pass's job

    position = advanced - kUp * drop;
    // The tick's motion is spent on the step (`gained` is at least a radius, comfortably more than
    // one tick of walking), so there is nothing left to slide with. Clamping rather than
    // subtracting keeps the budget from going negative and the loop from walking backwards.
    remaining = remaining - lateral_dir * std::min(gained, lateral_len);
    grounded = true;
    ground_normal = floor_normal;
    return true;
}

} // namespace

bool validate(CharacterConfig& config) noexcept {
    const CharacterConfig defaults{};
    bool changed = false;

    // Non-finite by REPLACEMENT, before any clamping. A NaN compares false against every bound, so
    // a clamp alone would pass it straight through — the trap that makes "we validate our inputs"
    // untrue in a lot of code that looks like it does (replication::sanitize learned this first).
    const auto fix = [&](float& field, float fallback) {
        if (!finite(field)) {
            field = fallback;
            changed = true;
        }
    };
    fix(config.radius, defaults.radius);
    fix(config.half_height, defaults.half_height);
    fix(config.max_speed, defaults.max_speed);
    fix(config.accel, defaults.accel);
    fix(config.air_accel, defaults.air_accel);
    fix(config.gravity, defaults.gravity);
    fix(config.jump_speed, defaults.jump_speed);
    fix(config.max_slope_cos, defaults.max_slope_cos);
    fix(config.step_height, defaults.step_height);
    fix(config.snap_distance, defaults.snap_distance);
    fix(config.skin, defaults.skin);
    fix(config.max_depenetration_per_tick, defaults.max_depenetration_per_tick);

    const auto clamp_to = [&](float& field, float lo, float hi) {
        const float v = std::clamp(field, lo, hi);
        if (v != field) {
            field = v;
            changed = true;
        }
    };

    // Radius first: it is the scale every other length is judged against, including skin's ceiling.
    clamp_to(config.radius, 1e-3f, 1e3f);
    clamp_to(config.half_height, 0.0f, 1e3f); // 0 is legal — that capsule is a sphere
    clamp_to(config.max_speed, 0.0f, 1e3f);
    clamp_to(config.accel, 0.0f, 1e5f);
    clamp_to(config.air_accel, 0.0f, 1e5f);
    clamp_to(config.gravity, 0.0f, 1e4f);
    clamp_to(config.jump_speed, 0.0f, 1e3f);
    // Dimensionless: a cosine. Outside [0, 1] it would mean "overhangs are walkable" or "nothing
    // is", neither of which the rest of the controller is written to survive.
    clamp_to(config.max_slope_cos, 0.0f, 1.0f);
    clamp_to(config.step_height, 0.0f, 10.0f * config.radius);
    clamp_to(config.snap_distance, 0.0f, 10.0f * config.radius);
    clamp_to(config.max_depenetration_per_tick, 0.0f, 10.0f * config.radius);

    // Skin's window, and the reason for each end. Below 1e-3 m it is inside the cast's own
    // tolerance stack, so a reported stop could be a position the capsule is already past. Above a
    // quarter of the radius it is visible: the capsule floats off every wall it touches.
    clamp_to(config.skin, 1e-3f, 0.25f * config.radius);

    const int iters = std::clamp(config.max_slide_iterations, 1, 8);
    if (iters != config.max_slide_iterations) {
        config.max_slide_iterations = iters;
        changed = true;
    }
    return changed;
}

physics::ShapeDesc character_shape(const CharacterConfig& config) noexcept {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Capsule;
    s.radius = config.radius;
    s.half_height = config.half_height;
    return s;
}

CharacterState step_character(const CharacterState& state,
                              const CharacterInput& input,
                              const CharacterConfig& config,
                              const physics::PhysicsWorld& world,
                              physics::BodyId self,
                              float dt,
                              StepStats* stats) noexcept {
    StepStats discard;
    StepStats& st = stats != nullptr ? *stats : discard;

    CharacterState s = state;
    if (!finite(dt) || dt <= 0.0f) {
        return s; // a tick of no duration advances nothing; better than propagating a NaN
    }

    CharacterConfig cfg = config;
    (void)validate(cfg); // clamped locally: the caller's copy is theirs, and a bad one is not fatal

    // ── (a) Input shaping ─────────────────────────────────────────────────────────────────────
    // Re-clamped HERE, not merely trusted from the wire. replication::sanitize already does this
    // on arrival, but a replay tape is also an input path, and prediction replays inputs from a
    // client's own buffer. The mover is the last place that can refuse a hostile or corrupt
    // number, so it does, cheaply, every tick.
    float move_x = finite(input.move_x) ? input.move_x : 0.0f;
    float move_y = finite(input.move_y) ? input.move_y : 0.0f;
    const float yaw = finite(input.yaw) ? input.yaw : 0.0f;
    const float move_len2 = move_x * move_x + move_y * move_y;
    if (move_len2 > 1.0f) {
        // The unit DISC, not the unit square: clamping per axis would leave a diagonal at length
        // √2 and make strafe-running measurably faster, which over a network is a client-chosen
        // speed multiplier rather than a quirk.
        const float inv = 1.0f / std::sqrt(move_len2);
        move_x *= inv;
        move_y *= inv;
    }

    // The yaw basis. Engine convention (render/components.hpp): an entity's forward is its rotation
    // applied to -Z, and yaw turns about world +Y right-handed. So at yaw = 0 forward is -Z and
    // right is +X, and the two axes rotate as below. The trig lives here rather than on the tape so
    // that a recorded input stays device-shaped (an angle, not a basis) — see the determinism note
    // in the header for why std::sin/std::cos are acceptable inside the same-binary claim.
    const float sy = std::sin(yaw);
    const float cy = std::cos(yaw);
    const core::Vec3 right{cy, 0.0f, -sy};
    const core::Vec3 forward{-sy, 0.0f, -cy};
    const core::Vec3 wish = right * move_x + forward * move_y;

    // ── (b) Velocity: semi-implicit, and deliberately simple ──────────────────────────────────
    // Velocity is updated FIRST and then carried through the slide analytically, never re-derived
    // as (moved distance / dt). That is what lets a test assert a slide direction to a few ULP:
    // the same plane clip is applied to the displacement and the velocity, so the two stay exactly
    // consistent instead of approximately so.
    const bool was_grounded = s.grounded;
    const bool jumped = was_grounded && (input.pressed & kActionJump) != 0u;

    if (was_grounded) {
        // ON THE GROUND, EVERYTHING HAPPENS IN THE GROUND PLANE — the wish direction, the target,
        // and the acceleration. Doing it in the horizontal plane and projecting the RESULT looks
        // equivalent and is not: the projection shrinks the horizontal component by cos²(slope)
        // every tick while the acceleration only pushes it back up by accel*dt, so the two settle
        // at a fixed point well below the intended speed. Measured on a 30° ramp: 2.5 m/s where
        // 4.5 was wanted, which reads as "the controller mysteriously walks uphill at half speed".
        //
        // The target's magnitude is max_speed * cos(slope). That is a design ruling, not an
        // accident of the algebra: the speed budget is partly spent on climbing, so a steeper hill
        // is ascended more slowly, with no separate rule and no clamp.
        const core::Vec3 n = s.ground_normal;
        const core::Vec3 in_plane = s.velocity - n * core::dot(s.velocity, n);
        const core::Vec3 wish_plane = wish - n * core::dot(wish, n);
        const float wish_len = core::length(wish_plane);
        const core::Vec3 target = wish_len > 0.0f
                                      ? wish_plane * (cfg.max_speed * core::dot(n, kUp) / wish_len)
                                      : core::Vec3{};
        core::Vec3 v = approach(in_plane, target, cfg.accel * dt);
        // Re-project: `approach` interpolates through velocity space and its result can leave the
        // plane by a rounding's worth even when both ends lie in it.
        v = v - n * core::dot(v, n);
        if (jumped) {
            // A jump SETS the vertical speed rather than adding an impulse, so jump height is a
            // config number a designer can read off, independent of what the ground was doing.
            v.y = cfg.jump_speed;
            s.grounded = false;
        }
        s.velocity = v;
    } else {
        const core::Vec3 flat = horizontal(s.velocity);
        const core::Vec3 v = approach(flat, wish * cfg.max_speed, cfg.air_accel * dt);
        s.velocity = core::Vec3{v.x, s.velocity.y - cfg.gravity * dt, v.z};
    }

    // The two shapes, and the division of labour between them (see the Caster note above): the
    // REAL capsule answers "am I genuinely inside something", the INFLATED probe answers every
    // movement question, so the contact offset is carried by the geometry rather than by an
    // arithmetic correction applied to a distance measured in the wrong direction.
    const physics::ShapeDesc shape = character_shape(cfg);
    physics::ShapeDesc sweep_shape = shape;
    sweep_shape.radius += cfg.skin;

    // Two filters, and the difference between them is the v1 SOLIDITY RULING (see README):
    //
    //   MOVEMENT casts see statics only. The character's solid world is the level. Walking into a
    //   crate therefore means arriving overlapping it, and PhysicsSync::push_in hands the solver
    //   the velocity that move implies, so the solver pushes the crate at walking speed — which is
    //   also how the debris-hurts-the-player contact events fire (ADR-0035 §3). The named costs are
    //   in the README; they are accepted for v1, not overlooked.
    //
    //   RECOVERY sees everything. "A crate was pushed INTO me between ticks" is precisely the case
    //   depenetration exists for, and it is a dynamic body by definition.
    //
    // `exclude` is set on both. On the movement filter it is currently redundant — a kinematic
    // character lives in the dynamics tree, which that filter already skips — and it is set anyway,
    // because the redundancy is a property of today's ruling and the correctness is not.
    physics::QueryFilter move_filter;
    move_filter.statics = true;
    move_filter.dynamics = false;
    move_filter.exclude = self;

    physics::QueryFilter recovery_filter;
    recovery_filter.statics = true;
    recovery_filter.dynamics = true;
    recovery_filter.exclude = self;

    const Caster caster{&world, sweep_shape, shape, move_filter, &st};

    // ── PHANTOM CONTACTS, and why a contact has to be CONFIRMED ───────────────────────────────
    //
    // A shape cast is a PREDICATE with a tolerance; `penetration()` is a MEASUREMENT. When they
    // disagree, the measurement wins — and they do disagree, reproducibly, in exactly the
    // configuration a character controller lives in.
    //
    // Measured at m12.2: a capsule sliding TANGENTIALLY along a large rotated face (a 120 m ramp
    // at 30°, the capsule 1.7 cm clear of it, sweeping parallel to the surface) intermittently
    // draws a "hit" whose normal is EXACTLY the reversed sweep direction — dot(n, dir) = -1.0000
    // — at a distance near the end of the budget, while `penetration()` at the same pose reports
    // the capsule comfortably clear. That normal is not geometry. It is `measure_normal`'s
    // documented fallback (src/scene_query.hpp): when the retracted probe cannot produce a
    // well-conditioned direction, the cast returns a deterministic retreat instead of guessing,
    // which is the right thing for the cast to do and the wrong thing for a caller to believe.
    //
    // Believing it is expensive. The clip below removes the velocity component along the contact
    // normal, and a normal that exactly opposes the motion removes ALL of it — so one phantom
    // contact per few ticks halves a character's walking speed, and a run of them freezes it. The
    // 30° climb measured 12.4 m instead of 26 m before this check existed.
    //
    // The same disagreement poisons the NORMAL, which is worse, because the normal decides whether
    // a surface is ground. Measured on a 60° face: reported normals of (0.12, 0.29, 0.95),
    // (-0.41, -0.71, -0.57), (-0.53, 0.85, 0.08) at successive ticks — in a scene with no Z
    // geometry at all — against a true face normal of (-0.87, 0.50, 0). One of those reads as
    // walkable, and the character strolls up a 60° cliff.
    //
    // So the controller takes BOTH decisions from one measurement instead: a contact blocks only
    // if the capsule genuinely cannot stand where it was going, and its normal is EPA's
    // penetration axis at that pose — measured where the shapes really do overlap, which is
    // precisely where EPA is well conditioned, rather than at a touch, which is precisely where a
    // distance query is not. One query, on the contact path only.
    //
    // `PenetrationHit::normal` is already stated as "push the QUERY shape to separate it", which
    // for a surface IS its outward normal facing the capsule — the same convention the slide loop
    // wants, with no flip.
    // "Obstructed" means the real capsule cannot keep (most of) its contact offset here — the
    // condition the movement phase actually needs. See Caster::obstructed for why the probe it
    // measures is half an offset thinner than the one the casts sweep.
    const auto obstructed_at = [&](core::Vec3 at, physics::PenetrationHit& out) {
        return caster.obstructed(at, out);
    };

    // ── (c) Depenetration pre-pass ────────────────────────────────────────────────────────────
    // Starting a tick inside geometry is not a hypothetical: a crate the solver shoved into the
    // capsule, or destruction spawning debris on top of the player, both produce it in an ordinary
    // frame. A controller that meets it with a shape cast gets `initial_overlap` back — a flag that
    // says "depenetrate", carries no measurement, and cannot be moved on. So recovery runs FIRST,
    // out of the penetration query, before anything tries to move.
    //
    // The push is capped per tick. Depenetration is a position write with no velocity behind it, so
    // an uncapped one is a teleport; capping it turns "shoved 2 m into a wall" into a visible slide
    // out over several ticks.
    bool overlapping = false;
    for (int i = 0; i <= kMaxRecoveryPushes; ++i) {
        physics::PenetrationHit pen;
        if (!world.penetration(shape, s.position, core::quat_identity(), pen, recovery_filter)) {
            overlapping = false;
            break;
        }
        overlapping = true;
        if (i == kMaxRecoveryPushes) {
            break; // measured, and out of pushes — the give-up, reported below
        }
        // depth + skin, so the push clears the surface rather than landing exactly on it — the same
        // stand-off every other position in this file keeps.
        const float push = std::min(pen.depth + cfg.skin, cfg.max_depenetration_per_tick);
        s.position = s.position + pen.normal * push;
        ++st.depenetrations;
    }
    if (overlapping) {
        // Counted rather than hidden. A `stuck` that appears for a few ticks is a deep overlap
        // being ground out at the capped rate and is fine; a `stuck` that never returns to zero is
        // the freeze this counter exists to make visible instead of mysterious.
        ++st.stuck;
    }

    bool touched_ground = false;

    // ── (d) The slide loop ────────────────────────────────────────────────────────────────────
    core::Vec3 planes[kMaxPlanes];
    int plane_count = 0;
    core::Vec3 remaining = s.velocity * dt;
    const float min_d = min_move(cfg);
    int iteration = 0;

    if (!overlapping) {
        for (; iteration < cfg.max_slide_iterations; ++iteration) {
            const float len = core::length(remaining);
            if (len <= min_d) {
                remaining = core::Vec3{};
                break;
            }
            const core::Vec3 dir = remaining * (1.0f / len);

            physics::ShapeHit hit;
            ++st.slide_iterations;
            const bool cast_hit = caster.cast(s.position, dir, len, hit);
            const core::Vec3 destination = s.position + dir * len;

            // A CLEAN SWEEP WINS OUTRIGHT, and it is the one statement here strong enough to.
            // The probe that just swept the entire budget and touched nothing strictly CONTAINS
            // the real capsule, so "the inflated shape fits the whole way" implies "the capsule
            // fits the whole way" as a matter of geometry, not of tolerance. No endpoint
            // measurement of the smaller shape can contradict it; if one did, it would be the
            // false reading. Taking the move here is also what removes the last deadlock: the old
            // code refused to move on that disagreement, and since refusing changes none of the
            // query's inputs it produced the identical disagreement forever. Measured: a 30° climb
            // that walked 1.4 m and then stood still for 280 ticks with a perfect 5.196 m/s
            // velocity and every counter at zero but one.
            if (!cast_hit) {
                s.position = destination;
                remaining = core::Vec3{};
                break;
            }

            // Something is in the way — but is it? CONFIRM BEFORE BELIEVING (see PHANTOM CONTACTS
            // above). The question that decides everything is "can the capsule stand where it was
            // trying to go", and `penetration()` answers it by measurement; the cast contributes
            // only HOW FAR along the way the first touch is, which is the one thing it is good at.
            physics::PenetrationHit block;
            if (!obstructed_at(destination, block)) {
                ++st.phantom_contacts;
                s.position = destination;
                remaining = core::Vec3{};
                break;
            }
            if (core::dot(block.normal, dir) >= -kBlockingOpposition) {
                // A surface parallel to the motion cannot be what stops it — see
                // kBlockingOpposition. This is the floor under a walking character, and it is the
                // shape every doubled-up query failure takes.
                ++st.phantom_contacts;
                s.position = destination;
                remaining = core::Vec3{};
                break;
            }
            if (hit.initial_overlap) {
                // The probe is already touching something. That is the RESTING case, not the
                // "inside a wall" case — the real capsule is still clear (the recovery pass above
                // measured that), it is simply within its contact offset of a surface. So: do not
                // advance, do not panic, take the normal from the measurement, and let the clip
                // below slide along the surface. Only a real overlap of the real capsule is stuck,
                // and that is what the recovery pass reports.
                physics::PenetrationHit here;
                if (caster.obstructed(s.position, here)) {
                    block = here; // measure the resting contact where the capsule actually is
                }
            }

            // The advance comes off the INFLATED probe, so the contact offset it leaves is
            // perpendicular at any approach angle, minus the small back-off that keeps the probe
            // off exact tangency (kBackOffFraction — and note this code is only reached when the
            // destination is genuinely blocked, i.e. when the character is approaching a surface
            // rather than grazing it).
            const float back_off = kBackOffFraction * cfg.skin;
            const float advance = cast_hit && !hit.initial_overlap
                                      ? std::min(std::max(hit.distance - back_off, 0.0f), len)
                                      : 0.0f;
            s.position = s.position + dir * advance;
            remaining = remaining - dir * advance;

            const core::Vec3 n = block.normal;
            if (walkable(n, cfg)) {
                s.grounded = true;
                s.ground_normal = n;
                touched_ground = true;
            } else if (was_grounded && !jumped) {
                // A steep normal while walking is either a wall or a step lip, and the casts in
                // try_step_up are what tell those apart. A rejected attempt falls through to the
                // ordinary clip below, which is the correct response to a real wall.
                if (try_step_up(caster, cfg, s.position, remaining, s.grounded, s.ground_normal)) {
                    ++st.steps_climbed;
                    touched_ground = true;
                    continue;
                }
                ++st.step_rejected;
            }

            // THE CLIP, applied identically to the leftover displacement and to the velocity. Both
            // lose exactly their component along the surface normal, which is what "slide along the
            // wall" means and why the velocity stays analytically exact.
            remaining = remaining - n * core::dot(remaining, n);
            s.velocity = s.velocity - n * core::dot(s.velocity, n);

            // IS THIS A SURFACE WE ARE ALREADY TOUCHING? Almost always, yes: sliding along a wall
            // means running into the same wall on every iteration, with a normal that differs from
            // last iteration's only by float noise. Recording it again is not a harmless
            // duplicate — it fills the plane list with three copies of one surface, and then the
            // crease machinery below tries to slide along the intersection of a plane with itself.
            // That cross product is zero, the degenerate-crease guard fires, and the character is
            // "corner locked" against a single flat ramp. Measured before this check: 280 corner
            // locks in 300 ticks on one 60° face, with the character frozen from tick 6 onward.
            //
            // The comparison is a DOT PRODUCT OF UNIT NORMALS — dimensionless, an angle, so it is
            // legitimately an absolute constant (see the epsilon note at the top of this file).
            // cos(2.5°): loose enough to absorb query noise on one flat face, far tighter than any
            // corner a character can be in.
            constexpr float kSamePlaneCos = 0.999f;
            bool duplicate = false;
            for (int p = 0; p < plane_count; ++p) {
                if (core::dot(n, planes[p]) > kSamePlaneCos) {
                    duplicate = true;
                    break;
                }
            }

            // THE CREASE. Clipping by the new plane can push the motion back INTO a plane we are
            // already touching — the inside of a corner. Sliding along either plane alone is then
            // wrong; the only direction that satisfies both is their line of intersection, so the
            // motion is projected onto it.
            bool locked = false;
            for (int p = 0; p < plane_count && !locked; ++p) {
                if (core::dot(n, planes[p]) > kSamePlaneCos) {
                    continue; // the same surface: a plane forms no crease with itself
                }
                if (core::dot(remaining, planes[p]) >= 0.0f) {
                    continue;
                }
                const core::Vec3 axis = core::cross(planes[p], n);
                const float axis_len2 = core::length_squared(axis);
                if (axis_len2 <= kCreaseSinFloor2) {
                    // Two near-parallel planes have no meaningful crease — the cross product is
                    // noise. Stopping is the honest answer; inventing a direction is not.
                    locked = true;
                    break;
                }
                const core::Vec3 crease = axis * (1.0f / std::sqrt(axis_len2));
                remaining = crease * core::dot(remaining, crease);
                s.velocity = crease * core::dot(s.velocity, crease);
            }
            if (duplicate) {
                continue; // already constrained by this surface; the clip above was the whole job
            }
            // A fourth DISTINCT simultaneous plane, or a degenerate crease: no direction satisfies
            // every constraint. Stop dead, and say so in a counter rather than jittering in place.
            if (locked || plane_count == kMaxPlanes) {
                s.velocity = core::Vec3{};
                remaining = core::Vec3{};
                ++st.corner_locked;
                break;
            }
            planes[plane_count] = n;
            ++plane_count;
        }
        if (iteration >= cfg.max_slide_iterations && core::length(remaining) > min_d) {
            // Out of iterations with motion still owed. Not a correctness failure — the capsule is
            // outside everything it cast against — but it IS a frame where the player asked for
            // more than they got, and a rising count is the signal that the geometry is nastier
            // than four iterations.
            ++st.slide_exhausted;
        }
    }

    // ── (f) Ground snap / step-down ───────────────────────────────────────────────────────────
    // Without this, walking down a staircase is a series of little falls: the capsule leaves each
    // tread, gravity gets a tick, it lands on the next. The snap says instead that a character who
    // was on the ground and is not jumping stays on the ground as long as the ground is within
    // reach — the same reach it could have stepped UP, which is why snap_distance defaults to
    // step_height.
    //
    // Only a WALKABLE surface grounds you. A steep one is not "ground you are standing on but
    // sliding down"; it is not ground, and letting gravity plus the plane clip do their work makes
    // sliding down a steep face emergent rather than a special case. Ceilings need no case at all:
    // upward motion into a downward-facing plane is clipped by the loop above like any other wall.
    //
    // It runs on EVERY grounded tick, including one where the character is moving upward. That is
    // not an optimisation missed — the snap is the only thing that re-verifies groundedness, and
    // gating it on a downward velocity meant a rising character was never re-checked. A step lip's
    // contact normal tilts upward, so a clip against it converts forward speed into upward speed;
    // with no gravity while grounded and no snap while rising, the character simply flew, gaining
    // 4.09 m of altitude on a 0.35 m step it was meant to be refused by. Verifying unconditionally
    // closes that: either the ground is still under you, or you are airborne and gravity resumes.
    if (was_grounded && !jumped && !overlapping) {
        // Asked with a RAY, for the reasons in Caster::ground_below: the supporting surface's
        // identity and normal are what everything downstream turns on, and a shape query answers
        // that question about the nearest WALL when the character is standing next to one.
        float drop = 0.0f;
        core::Vec3 floor_normal{0.0f, 1.0f, 0.0f};
        if (caster.ground_below(s.position, cfg, cfg.snap_distance, drop, floor_normal) &&
            walkable(floor_normal, cfg) && drop <= cfg.snap_distance) {
            s.position = s.position - kUp * std::max(drop, 0.0f);
            s.grounded = true;
            s.ground_normal = floor_normal;
            ++st.snaps;
        } else if (!touched_ground) {
            // Nothing walkable within reach, and the slide loop did not land on anything either:
            // the character has walked off an edge. (The `touched_ground` guard matters — the loop
            // may have landed on a surface this cast cannot see straight down from.)
            s.grounded = false;
        }
    }

    // ── (g) The velocity ends the tick IN THE GROUND PLANE ────────────────────────────────────
    // One re-projection, against the ground normal as it finally stands, and it does three jobs
    // that would otherwise each need their own rule:
    //
    //   * it removes the upward speed a clip against a steep lip injected (see the snap note) —
    //     which is what makes step-up load-bearing rather than optional, because a lip can no
    //     longer be RIDDEN, only climbed;
    //   * it removes the downward speed a standing character would otherwise accumulate, so the
    //     first step off a ledge is not launched by a fall that never happened;
    //   * it leaves an honest slope walk untouched, because that velocity is already in the plane.
    //
    // Doing it here rather than before the snap matters: `ground_normal` may have just changed
    // (stepped onto a ramp, snapped onto a lower floor), and projecting against the plane you were
    // on rather than the one you are on is how a controller acquires a one-tick hitch at every
    // surface transition.
    if (s.grounded && !jumped) {
        s.velocity = s.velocity - s.ground_normal * core::dot(s.velocity, s.ground_normal);
    }

    return s;
}

} // namespace rime::gameplay
