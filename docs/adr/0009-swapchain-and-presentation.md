# ADR-0009: Swapchain, presentation & frame pacing (M3.4)

- Status: Accepted
- Date: 2026-06-18

## Context

M3.1–M3.3 brought the Vulkan backend up *headless*: a `Device` renders a triangle to an off-screen
image and reads it back (the GPU-free CI proof, ADR-0007). M3.4 must put those pixels **on screen** —
turn `platform::NativeWindow` into something the renderer presents to — without undoing the headless
design that lets the proof run on lavapipe with no display.

Several decisions shape every line of the presentation path:

1. **Where the window lives.** Does the `Device` learn about a window (a surface in `DeviceDesc`), or
   does presentation become a separate object? The Device is deliberately window-agnostic today.
2. **Creating the surface across four window systems** (Cocoa, Win32, Xlib, Wayland) without leaking
   an OS type into the RHI — the same "no `#ifdef` upward" rule the seam already enforces, and the
   complication that the platform-specific `vkCreate*SurfaceKHR` entry points sit behind
   `VK_USE_PLATFORM_*` macros and may not be compiled into the prebuilt `volk`.
3. **Frame pacing.** The M3.3 model is submit-one-frame-and-block. A real window needs
   frames-in-flight (overlap CPU recording with GPU work) and correct acquire→render→present
   synchronization — the part that is easy to get subtly wrong.
4. **Resize / out-of-date** handling, and keeping all of this verifiable when only one of the four
   window systems (Cocoa/MoltenVK) is runnable on the dev machine and CI has no display at all.

## Decision

**Presentation is a separate, window-agnostic-Device + `rhi::Swapchain` object.**

- **The `Device` stays headless.** `create_device()` takes no window; instead `Device::create_swapchain(SwapchainDesc{NativeWindow, extent})` returns a `Swapchain` — the *only* RHI object that owns a `VkSurfaceKHR`. This preserves the M3.1–M3.3 split exactly: no window, no surface, no swapchain ⇒ the off-screen proof still runs GPU-free in CI.
- **Surface extensions are enabled opportunistically.** At instance creation we add `VK_KHR_surface` + this OS's platform surface extension *if the driver lists them*; at device creation we add `VK_KHR_swapchain` likewise. Present on a real GPU / MoltenVK, absent on lavapipe — so enabling presentation never breaks the headless path.
- **Surface creation is the one OS-touching spot in the backend.** `surface_vulkan.cpp` switches on `NativeWindow.system` and calls the matching `vkCreate*SurfaceKHR`, each branch guarded by the platform's `VK_USE_PLATFORM_*` macro (set per-OS in CMake — Linux compiles both Xlib and Wayland, since it picks at runtime). The platform create function is resolved with **`vkGetInstanceProcAddr`**, not volk's global pointer, so it works regardless of how the packaged volk was configured. The `NativeWindow` stays type-erased (`void*`s); only this file reinterprets them.
- **Frames-in-flight = 2, FIFO present by default.** Each in-flight slot owns an image-available semaphore and an in-flight fence; each swapchain *image* owns a render-finished semaphore (so a present never waits on a semaphore still pending from a different image). FIFO is vsync, tear-free, and universally supported; mailbox is used only when the caller opts out of vsync and the surface offers it. Backbuffer images are registered as ordinary RHI textures, so the existing command encoder (begin_rendering, layout transitions) drives them unchanged.
- **Recreate on resize / `OUT_OF_DATE` / `SUBOPTIMAL`,** sized to `Window::framebuffer_size()` (or the surface's `currentExtent` where it dictates one).
- **The present queue is the graphics queue.** We verify `vkGetPhysicalDeviceSurfaceSupportKHR` and fail loudly if it isn't supported — true for all our targets (desktop GPUs, MoltenVK). A dedicated present queue is a later refinement only if a real target needs it.

## Consequences

**Good**
- The headless proof is untouched — presentation is purely additive, and CI stays green GPU-free.
- One surface-creation file is the entire OS-windowing surface area of the backend; adding a platform is adding a branch + a CMake macro, exactly like `engine/platform`.
- Backbuffers being normal RHI textures means the render path has no swapchain special-casing — and it is what the M5 render graph will target.
- Verified end-to-end on macOS/MoltenVK (Vulkan 1.3.334): a 3-image FIFO swapchain presents the triangle; the M2 Wayland surface will finally map (it shows only once a buffer is attached).

**Costs we accept**
- Only Cocoa/MoltenVK is *runtime*-verifiable on the dev box; Win32/X11/Wayland are compile/link-verified locally and gated by CI — the same accepted split as M2's window backends.
- One queue for graphics + present; per-frame command-buffer alloc/free (pooling is a labeled later optimization); single-field layout tracking per texture — all carried over from the M3-simple model and owned properly by the render graph later.

## Alternatives considered

- **Put the surface in `DeviceDesc` (Device owns presentation).** Simpler call site, but it drags a window into every Device consumer and breaks the headless-first design that the CI proof depends on. Rejected.
- **Per-OS surface translation units under `src/<os>/` (mirroring `engine/platform`).** Cleaner isolation, but four files for what is one small switch; a single guarded TU is less ceremony for the same seam. Revisit if surface handling grows.
- **Use volk's global `vkCreate*SurfaceKHR`.** Fewer lines, but it ties us to how the prebuilt volk was compiled (the platform symbol may be absent → link error). `vkGetInstanceProcAddr` is robust and standard. Rejected.
- **Mailbox by default for lowest latency.** Not universally supported and drops frames; FIFO is the correct, portable default, with mailbox available opt-in. Rejected as a default.
