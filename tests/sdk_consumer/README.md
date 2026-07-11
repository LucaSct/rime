# `sdk_consumer` — the out-of-tree SDK proof (M6.8)

A minimal downstream application that builds against an **installed** Rime, not the source tree.
It exists to prove the SDK story end to end: `find_package(rime CONFIG)`, link `rime::app` +
`rime::assets`, compile against `<prefix>/include`, and run headless — create an `Application`,
tick its fixed-step loop, and load a cooked mesh.

This is a **separate CMake project**: the engine's top-level `CMakeLists.txt` never
`add_subdirectory()`-es it, so it can only see Rime through the install. Don't try to build it
in-tree.

Run the whole thing with:

```bash
scripts/sdk-smoke.sh          # build engine → install → configure+build+run this → hygiene guards
```

See [`docs/design/sdk.md`](../../docs/design/sdk.md) for the SDK design and the v1 dependency stance
(consumers configure with the same Conan toolchain the engine was built with).
