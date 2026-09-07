// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// RIME_FORCE_INLINE — `inline` is a hint about linkage, not about inlining, and on one function in
// this engine the compiler's judgement measurably costs a fifth of the physics solve.
//
// THE MEASUREMENT, because a force-inline with no number attached is how a codebase fills up with
// them. `core::rotate` (quat.hpp) is three lines and marked `inline`, and GCC 15 at -O2 still
// emitted it out of line — one `rime::core::rotate [clone .isra.0]` symbol in the release binary,
// reached from the solver's velocity iteration (solver.hpp: three calls per contact point per
// iteration) and from the narrowphase's support functions. Forcing it inline, measured interleaved
// on the m17.5 tape, three runs an arm, bit-identical trajectory (`parts.alive_end` 1227 in all
// six):
//
//     physics.server.solve    p50  2.757 / 2.757 / 2.790  ->  2.232 / 2.260 / 2.184   -19.6%
//     physics.server.step     p50  5.513 / 5.552 / 5.558  ->  5.078 / 5.027 / 5.037    -8.9%
//     physics.server.contacts p50  2.482 / 2.428 / 2.375  ->  2.463 / 2.478 / 2.484    none
//
// No overlap between the arms on the solve. The narrowphase does not move, which is the shape you
// would predict: its calls are per pair, the solver's are per contact point per iteration.
//
// USE IT SPARINGLY AND ONLY WITH A NUMBER. Forcing inline costs code size (this one cost 70 KB of
// 40 MB) and can hurt by evicting a hot loop from the instruction cache, so it is not a decoration
// to sprinkle on small functions — it is a fix for a specific call site the profile named.
//
// THE MACRO CARRIES `inline` ITSELF, so a caller writes `RIME_FORCE_INLINE T f()` and never spells
// `inline` beside it. That is not a style preference — it is the portability bug this header
// shipped with for one CI round. MSVC's `__forceinline` already IMPLIES inline, so
// `RIME_FORCE_INLINE inline`, which the first version of this very comment instructed, expands to
// `__forceinline inline` and MSVC raises C4141 "'inline': used more than once" — fatal under /WX.
// GCC and Clang accept the doubled spelling silently, so nothing on this machine could see it, and
// MSVC is the one compiler not in its matrix. Spelling `inline` at a call site reintroduces it;
// the macro is the only place that word belongs. Equally, REMOVING it from the macro would make
// every user a non-inline function defined in a header — an ODR violation the linker finds, not
// the compiler — which is why all three branches carry it.
#if defined(_MSC_VER)
#define RIME_FORCE_INLINE __forceinline // implies inline; spelling it again is C4141
#elif defined(__GNUC__) || defined(__clang__)
#define RIME_FORCE_INLINE inline __attribute__((always_inline))
#else
#define RIME_FORCE_INLINE inline
#endif
