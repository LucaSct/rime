# ADR-0001: C++ core + Rust tooling

- Status: Accepted
- Date: 2026-06-16

## Context

The single most foundational choice for an engine is its language(s). It sets the
performance ceiling, decides which middleware we can use, and shapes who can contribute.

Our constraints (from [VISION.md](../../VISION.md)): AAA performance and features come
first; we want to match the reference engines (Frostbite, Unreal, O3DE — all C++) and
their middleware ecosystem (Jolt, PhysX, FMOD, Wwise — all ship C++ SDKs); and we want
modern, safe, pleasant tooling that attracts contributors.

We weighed three options: C++ everywhere; Rust everywhere; or C++ for the runtime with
Rust for tooling.

## Decision

**C++20/23 for the engine runtime, renderer, and all hot paths. Rust for the editor,
asset pipeline, and supporting tools/services.**

The two worlds meet only at *stable, explicit boundaries*: a C ABI, shared file
formats, or a command protocol. Rust tools never reach into C++ internals, and vice
versa.

## Consequences

**Good**
- Maximum performance ceiling on the hot path; direct access to the entire AAA C++
  middleware ecosystem; learning the engine maps 1:1 to industry skills.
- The largest, most error-prone *offline* tools (importers, cookers, editor) get Rust's
  memory safety and excellent tooling (cargo, crates), where their correctness matters
  most and their latency matters least.

**Costs we accept**
- **Two toolchains** (CMake/clang/MSVC + cargo) and CI for both.
- An **FFI boundary** to design and maintain; data crossing it must be in stable
  formats. We treat the boundary as a real interface, versioned and documented.
- C++ brings manual memory-safety responsibility on the runtime side; we mitigate with
  strict allocators, RAII, no raw `new`/`delete`, sanitizers, and the `.clang-format` /
  review discipline in [CLAUDE.md](../../CLAUDE.md).

## Alternatives considered

- **C++ everywhere.** Simplest single toolchain and what most AAA engines do, but gives
  up Rust's safety/tooling exactly where we write the most fiddly offline code.
- **Rust everywhere** (à la Bevy). Memory-safe and ergonomic, with a clean ECS story,
  but a thinner AAA-middleware ecosystem, borrow-checker friction for some engine data
  structures (graphs, intrusive structures), and a smaller pool of contributors who
  have shipped AAA runtimes. We keep Rust where it shines and don't force it onto the
  hottest paths.
