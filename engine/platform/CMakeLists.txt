# rime_platform — the thin OS abstraction (window, input, filesystem, timers, threads).
#
# The seam that makes M2 portable lives in *this file*: OS-specific code is in src/<platform>/
# files that are compiled exclusively on their target OS (selected below), so a backend .cpp is
# built wholesale on its platform and not at all elsewhere. That is what keeps the public headers
# (and the rest of the engine) free of OS #ifdefs — adding a platform means adding a backend dir,
# not branching a header. (See docs/ARCHITECTURE.md and docs/adr/0006-native-windowing.md.)

# OS-agnostic sources — built on every platform.
add_library(rime_platform STATIC
    src/platform.cpp
    src/clock.cpp
    src/event_queue.cpp
    src/window.cpp
    src/input.cpp
    src/filesystem.cpp
    src/socket.cpp
    src/null/window_null.cpp)

# Namespaced alias so dependents link rime::platform (matching rime::core).
add_library(rime::platform ALIAS rime_platform)

# Public headers: the OS-agnostic interface, visible to dependents and to our own backends.
target_include_directories(rime_platform
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src) # private seam headers (platform_backend.hpp)

# Depends only on the layer below (the layer cake). Threads::Threads arrives transitively
# (rime::core links it PUBLIC) and is what the pthread-based backends below need.
target_link_libraries(rime_platform
    PUBLIC rime::core)

# ── Per-OS backend selection ──────────────────────────────────────────────────────
# Each branch contributes only its OS's sources. As more of M2 lands (window, input, base dirs)
# each backend dir grows; the selection logic stays here, one place, and the headers stay clean.
if(APPLE)
    # The Cocoa backend is Objective-C++ (.mm); enable that language and compile it with ARC so
    # Objective-C object lifetimes are automatic. Cocoa (windows/events) + QuartzCore (CAMetalLayer).
    enable_language(OBJCXX)
    target_sources(rime_platform PRIVATE
        src/cocoa/threads_cocoa.cpp
        src/cocoa/window_cocoa.mm
        src/cocoa/paths_cocoa.cpp
        src/posix/socket_posix.cpp) # BSD sockets — shared with Linux
    set_source_files_properties(src/cocoa/window_cocoa.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    set_target_properties(rime_platform PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_link_libraries(rime_platform PRIVATE "-framework Cocoa" "-framework QuartzCore")
elseif(WIN32)
    target_sources(rime_platform PRIVATE
        src/win32/threads_win32.cpp
        src/win32/window_win32.cpp
        src/win32/paths_win32.cpp
        src/win32/socket_win32.cpp)
    # Winsock: the TCP sockets backend links the Windows sockets library.
    target_link_libraries(rime_platform PRIVATE ws2_32)
elseif(UNIX)
    # Linux chooses its windowing system at runtime (Wayland or X11), so both backends are compiled
    # in and window_linux.cpp dispatches between them. keymap_linux is shared (evdev -> Key).
    target_sources(rime_platform PRIVATE
        src/linux/threads_posix.cpp
        src/linux/window_linux.cpp
        src/linux/window_x11.cpp
        src/linux/window_wayland.cpp
        src/linux/keymap_linux.cpp
        src/linux/paths_linux.cpp
        src/posix/socket_posix.cpp) # BSD sockets — shared with macOS

    # X11 (Xlib) backend. CMake ships FindX11; X11::X11 is the imported Xlib target.
    find_package(X11 REQUIRED)
    target_link_libraries(rime_platform PRIVATE X11::X11)

    # Wayland backend: the client library + xkbcommon (keyboard), located via pkg-config.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

    # Wayland ships its protocols as XML; wayland-scanner turns the xdg-shell description into C
    # client glue — a header we include and a marshalling .c we compile. Locate the scanner and the
    # protocol data dir from their .pc files (with a find_program fallback for the scanner).
    pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
    if(NOT WAYLAND_SCANNER)
        find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
    endif()
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(_xdg_xml "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
    set(_wl_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland")
    file(MAKE_DIRECTORY "${_wl_gen}")
    add_custom_command(
        OUTPUT "${_wl_gen}/xdg-shell-client-protocol.h"
        COMMAND ${WAYLAND_SCANNER} client-header "${_xdg_xml}"
                "${_wl_gen}/xdg-shell-client-protocol.h"
        DEPENDS "${_xdg_xml}" VERBATIM)
    add_custom_command(
        OUTPUT "${_wl_gen}/xdg-shell-protocol.c"
        COMMAND ${WAYLAND_SCANNER} private-code "${_xdg_xml}" "${_wl_gen}/xdg-shell-protocol.c"
        DEPENDS "${_xdg_xml}" VERBATIM)

    # The generated marshalling code is C and not ours to hold to -Werror, so it builds as its own
    # static lib with default flags; rime_platform links it and inherits the generated header's dir.
    enable_language(C)
    add_library(rime_xdg_shell STATIC
        "${_wl_gen}/xdg-shell-protocol.c"
        "${_wl_gen}/xdg-shell-client-protocol.h")
    # The generated-header dir is a BUILD_INTERFACE include only — it must never leak into the
    # install export as an absolute build path (and the xdg-shell glue is internal to rime_platform;
    # no consumer includes it). SDK install (M6.8): a STATIC PRIVATE dep still contributes symbols
    # the final consumer must link, so this archive is exported alongside the modules.
    target_include_directories(rime_xdg_shell PUBLIC $<BUILD_INTERFACE:${_wl_gen}>)
    target_link_libraries(rime_xdg_shell PUBLIC PkgConfig::WAYLAND)
    rime_install_support_lib(rime_xdg_shell)

    target_link_libraries(rime_platform
        PRIVATE rime_xdg_shell PkgConfig::WAYLAND PkgConfig::XKBCOMMON)
    add_dependencies(rime_platform rime_xdg_shell) # generate the protocol header before compiling
endif()

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_platform)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_platform)
