# ADR-0006: Native windowing & input (no GLFW/SDL)

- Status: Accepted
- Date: 2026-06-17

## Context

Milestone 2 builds `engine/platform` — window, input, filesystem, timers, threads — behind a
seam that keeps OS `#ifdef`s out of the rest of the engine. ROADMAP brick **M2.2** left one
decision open: the windowing/input *backend*. Three options were on the table:

- **GLFW** — small, Vulkan-friendly, cross-platform; the proposed fast path to first pixels.
- **SDL** — broader (audio, gamepads, haptics, …), heavier than a windowing seam needs.
- **Native** — Win32 / Cocoa / Xlib + Wayland directly: maximum control, ~3–4× the work.

The forces. Rime's first principle is *power before portability* (VISION #1/#2). A AAA target
leans hard on precisely the things a wrapper smooths over or lags behind on: per-monitor DPI,
raw/relative mouse for camera control, cursor capture, event timing, fullscreen/borderless
behaviour, and IME/text input. A library is also code we ship, debug, and are bound by
(`third_party/README.md` policy). Crucially, *everything* above the platform layer reaches the OS
through our own `Window`/event seam regardless of backend — so the cost of going native is
contained to the backend files and does not leak upward, and the seam can still host a GLFW
backend later if a new platform needs quick bring-up.

A secondary scope question: how far does "native" go? The monotonic clock and the filesystem are
*already* thin wrappers over the OS in the standard library (`std::chrono::steady_clock` →
QueryPerformanceCounter / `mach_absolute_time` / `clock_gettime(MONOTONIC)`; `std::filesystem` →
the native FS calls).

## Decision

**The platform layer is implemented natively, with no GLFW/SDL dependency.** One backend per OS,
compiled only on its OS, behind the OS-agnostic `rime::platform` interface:

- **macOS** — Cocoa (`NSApplication`/`NSWindow`, a layer-backed `NSView` + `CAMetalLayer`), in
  Objective-C++.
- **Windows** — Win32 (`RegisterClassExW`/`WNDPROC`, the message pump, per-monitor DPI-awareness v2).
- **Linux** — **both** Xlib (X11) **and** Wayland (xdg-shell, libxkbcommon), chosen at runtime
  (prefer Wayland, fall back to X11).

Implementation order is **Cocoa → Win32 → X11 → Wayland**, and M2.2 is decomposed into per-OS
bricks (M2.2a–M2.2d) to keep each one small and verifiable.

**"Native" is scoped to where the OS APIs genuinely differ in capability — windowing and input.**
The monotonic clock uses `std::chrono::steady_clock` and filesystem/path work uses
`std::filesystem`; hand-written native code is added only where the standard library has no answer
(thread naming, executable path, per-OS user data/config/cache dirs).

A window exposes its native handles through an OS-type-free `NativeWindow { system, display,
handle }` (a tag plus two `void*`s), so the M3 Vulkan backend — the only code permitted to include
Vulkan headers (ADR-0002) — can build a `VkSurfaceKHR` without the platform layer ever touching
Vulkan.

## Consequences

**Good**

- Full, direct control of DPI, raw/relative mouse, cursor capture, event timing, and fullscreen —
  the capabilities a AAA renderer and camera actually depend on.
- Zero windowing dependency to ship, debug, license, or wait on for OS updates; the engine owns
  its own front door (`third_party` policy, VISION #1).
- The `Window`/event seam stays swappable: a backend can be replaced — or a GLFW backend added for
  bring-up — without touching engine code above the seam. The control is not a lock-in.
- Using `std` for the clock/FS avoids re-implementing the standard library where its OS wrapper is
  already optimal.

**Costs we accept**

- ~3–4× the implementation work of a single GLFW backend, and per-OS expertise: the Win32 message
  pump, Cocoa's main-thread / retained-mode model, and X11 *and* Wayland.
- Two Linux backends to maintain, plus Wayland-specific wrinkles (a surface needs a buffer before
  it is shown; keymaps via xkbcommon; `wayland-scanner` codegen for xdg-shell).
- Headless-CI plumbing: a **null backend** for deterministic, displayless tests, plus Xvfb (X11)
  and a headless compositor (Wayland) to exercise real windows on Linux runners.

## Alternatives considered

- **GLFW behind the seam** (the prior proposal). Fast to first pixels and Vulkan-friendly, and the
  seam would keep it swappable — but it cedes control of exactly the edges (DPI, raw input,
  capture, timing) a AAA engine cares about, and adds a dependency. Rejected for the engine's front
  door; still available later as an *additional* backend for bringing up a new platform quickly.
- **SDL.** A much larger surface (audio, gamepads, haptics) than a windowing seam needs; we prefer
  to own those subsystems deliberately and separately. Rejected as over-scoped.
- **Native, but Linux = X11 only (Wayland later).** Lower cost, and X11 covers Wayland sessions via
  XWayland — but Wayland is the direction of the Linux desktop, and building it now, while the
  backend seam is fresh, avoids a later retrofit. We chose both.
- **Native everything, including the clock and filesystem.** Re-implements the standard library's
  OS wrappers (QPC/`mach_absolute_time`/`clock_gettime`, raw `open`/`CreateFile`) for no measurable
  gain; we reserve native effort for windowing/input, where it actually pays.
