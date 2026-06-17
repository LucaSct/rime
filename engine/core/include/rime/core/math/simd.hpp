// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// The SIMD seam for the math library.
//
// M1's math is written as clean, readable SCALAR code over a SIMD-friendly memory layout
// (16-byte-aligned Vec4/Mat4, contiguous storage). This header is the single place a future
// SSE/AVX/NEON backend will plug in: it detects the target instruction set and, today, selects
// the scalar path. We deliberately do NOT hand-write intrinsics yet -- there is no renderer or
// ECS to benchmark against, and CLAUDE.md's "measure before optimizing" says to wait until
// there is (the real math hot loops arrive with ECS transforms in M4 and PBR in M5). When that
// day comes, the Vec/Mat operators gain intrinsic specializations behind these macros with no
// change to their public signatures. See docs/adr/0004 and docs/math/vectors-matrices.md.

#if defined(__AVX2__)
#define RIME_SIMD_ISA "AVX2"
#elif defined(__SSE2__) || (defined(_M_X64)) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define RIME_SIMD_ISA "SSE2"
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#define RIME_SIMD_ISA "NEON"
#else
#define RIME_SIMD_ISA "none"
#endif

// The active math backend. Scalar today; an intrinsic backend will flip this and add the
// specializations. Code/tests can check it without caring about the specific ISA above.
#define RIME_SIMD_SCALAR 1
