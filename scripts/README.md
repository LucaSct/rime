# scripts/ — setup, build, and lint helpers

Cross-platform convenience scripts so contributors on Windows, Linux, and macOS get a
consistent experience. Scripts wrap the real tools (CMake, Cargo, clang-format); they
never hide what they do — read them to learn the actual commands.

## Planned scripts

| Script | Purpose |
| --- | --- |
| `setup.sh` / `setup.ps1` | locate/install the toolchain (compiler, CMake, Rust) and the **Vulkan SDK**; verify versions |
| `build.sh` / `build.ps1` | configure + build the engine (CMake) and tools (Cargo) in one step |
| `format.sh` / `format.ps1` | run `clang-format` over C++ and `cargo fmt` over Rust |
| `lint.sh` / `lint.ps1` | `clippy` + clang-tidy + license-header check |
| `test.sh` / `test.ps1` | run C++ (`ctest`) and Rust (`cargo test`) suites |

> Conventions: scripts are thin and readable; they echo the underlying commands they
> run so they double as documentation. Added during Milestone 0 (build bootstrap).
