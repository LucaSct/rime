# scripts/ — setup, build, and lint helpers

Cross-platform convenience scripts so contributors on Windows, Linux, and macOS get a
consistent experience. Scripts wrap the real tools (CMake, Cargo, clang-format); they
never hide what they do — read them to learn the actual commands.

## Available now

| Script | Purpose |
| --- | --- |
| `setup.sh` / `setup.ps1` | Check the toolchain; auto-install the user-local pieces (Conan in an isolated venv, Rust via rustup) and guide on the rest (CMake ≥ 3.24, Ninja, a C++ compiler, the **Vulkan SDK** — needed from M3). |
| `build.sh` / `build.ps1` | Configure + build the engine (Conan + CMake) and the tools (Cargo) and run their tests, in one step. |
| `check-license-headers.sh` | Verify every C++/Rust source carries the SPDX header (CLAUDE.md). Run by CI's license gate. |

```sh
scripts/setup.sh                 # once, to get the toolchains in place
scripts/build.sh                 # build everything (dev) + run all tests
scripts/build.sh --preset release --no-tests
scripts/build.sh --cpp-only --clean
scripts/build.sh --cpp-only --sanitizer address   # ASan+UBSan build (GCC/Clang; see CI)
```

> `--sanitizer address|thread` sets the `RIME_SANITIZER` CMake option (`address` = ASan+UBSan,
> `thread` = TSan). It instruments the C++ engine only and needs GCC/Clang. CI runs two Linux
> sanitizer jobs on top of the normal matrix; `thread` guards the lock-free deque + job system.

On Windows, use the `.ps1` equivalents (e.g. `pwsh scripts/build.ps1 -Preset release`).

## Planned (later bricks)

| Script | Purpose |
| --- | --- |
| `format.sh` / `format.ps1` | run `clang-format` over C++ and `cargo fmt` over Rust |
| `lint.sh` / `lint.ps1` | `clippy` + clang-tidy + license-header check |

> A dedicated `test.sh` isn't needed: `build.sh` runs `ctest` and `cargo test` already
> (pass `--no-tests` to skip). The format/lint helpers arrive alongside the CI gates.

> Conventions: scripts are thin and readable; they echo the underlying commands they run
> so they double as documentation. The `.sh` scripts target Linux/macOS; the `.ps1`
> scripts mirror them on Windows and are exercised by CI (Milestone 0.5).
