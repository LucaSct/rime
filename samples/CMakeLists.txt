# Sample projects that exercise the engine. Each is a small executable that links the engine
# modules it demonstrates and doubles as a runnable proof for the milestone that introduced it.
# Built when RIME_BUILD_SAMPLES is ON (the default); see the top-level CMakeLists.
add_subdirectory(jobs_core_saturation) # M1.6: work-stealing job system saturating all cores
add_subdirectory(00-hello-window)      # M2: open a native window, pump input (preview of M2.5)
add_subdirectory(codec_bench)          # S0.3: measure the streaming codecs (raw/LZ4/JPEG); GPU-free
add_subdirectory(05-ecs-playground)    # M4.6: 200k entities in parallel + transforms composing; GPU-free
add_subdirectory(09-physics-playground) # M7.8: M7's "done when" — fall/collide/stack, raycast, parallel fixed tick; GPU-free

# Needs the Vulkan RHI backend (the agnostic interface alone can't render).
if(RIME_RHI_VULKAN)
    add_subdirectory(01-hello-triangle) # M3.3/M3.4: a triangle off-screen, then presented in a window
    add_subdirectory(02-textured-quad)  # M3.5: an indexed, textured quad (M3's "done when")
    add_subdirectory(03-icem-viewer)    # viewer B1: load a computed part (STL), draw it lit + depth-correct
    add_subdirectory(04-remote-view)    # Track S0.5: headless streaming server + a client (see + control)
    add_subdirectory(06-render-graph)   # M5.8: procedural PBR scene through the graph; "adding a pass is easy"
    add_subdirectory(07-first-light)    # M5.8: M5's "done when" — app-driven lit PBR scene (headless / --serve)
    add_subdirectory(08-gltf-zoo)       # M6.10: M6's "done when" — cooked glTF gallery (import→cook→load→render)
    add_subdirectory(10-destructible-wall) # M8.6: M8's "done when" — a cooked wall breaks, debris flies, one event stream fans out
    add_subdirectory(11-lit-rooms)         # M10.8: M10's "done when" — full lighting stack; break a wall, the dark room lights up
endif()
