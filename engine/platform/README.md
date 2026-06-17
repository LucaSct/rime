# engine/platform — the thin OS abstraction

`rime::platform` is the seam between cross-platform engine code and the operating system:
**window, input, filesystem, timers, and thread utilities**, with one OS-agnostic interface
and a separate backend per OS (macOS/Cocoa, Windows/Win32, Linux/X11 + Wayland).

**The one rule:** *no OS `#ifdef`s leak upward.* OS-specific code lives only in
`src/<platform>/` files that are compiled exclusively on their target OS (selected in
`CMakeLists.txt`). The public headers under `include/rime/platform/` never include an OS header
or branch on the OS — so the rest of the engine is written once, and adding a platform means
adding a backend directory, not editing a header.

Depends only on `core` (the layer below). See [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md)
for the layer cake and [ADR-0006](../../docs/adr/0006-native-windowing.md) for why windowing is
native (no GLFW/SDL).

## Status (built bottom-up, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| M2.1 | module + seam, `init`/`shutdown` + main-thread contract, monotonic `Clock`, `set_thread_name` | landed |
| M2.2a | `Window` + event pump + native-handle seam + null backend + **Cocoa** | landed |
| M2.2b–d | **Win32** · **X11** · **Wayland** window backends (Linux selects at runtime) | landed |
| M2.3 | keyboard/mouse events + polled `Input` state | landed |
| M2.4 | filesystem (file I/O, exe + per-user base dirs) + frame timer | landed |
| M2.5 | `00-hello-window` proof — live FPS in the title + polled input | landed |

## Layout

```
include/rime/platform/   # OS-agnostic public interface (no OS headers here, ever)
src/                     # OS-agnostic implementation (clock, lifetime, …)
src/cocoa/  src/win32/  src/linux/   # per-OS backends, compiled only on their OS
```

`std` is used where it already wraps the OS-native call (the monotonic clock is
`std::chrono::steady_clock`; filesystem is `std::filesystem`); native code is written only where the
standard library has no answer — windowing/input, thread naming, the executable path, and user dirs.
