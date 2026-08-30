# rime_worldkit — the component profile (m14.1, ADR-0037): one answer to "what components does a
# Rime world have?", in one function, in one order.
#
# It exists because there was no answer, and the drift between everyone's private list cost two
# separate silent failures in an afternoon — the editor unable to open the block, and 99-the-block's
# predictor/replication/audio all quietly dead. See include/rime/worldkit/profile.hpp.
#
# Layer-cake position: the TOP. It depends on every engine module that defines components, and
# NOTHING depends on it — samples, tools and the editor host call it, no engine module does. So
# deleting this directory leaves the engine building, exactly as guardrail 2 requires of blockkit and
# destruction_render. That is also why it is a profile rather than a registry: a module that
# self-registered into a global table would make "does the engine build without destruction?" a
# runtime question, and would need mutable global state to answer it (guardrail 4).
add_library(rime_worldkit STATIC
    src/profile.cpp)

# Namespaced alias so dependents link `rime::worldkit`, matching our C++ namespace.
add_library(rime::worldkit ALIAS rime_worldkit)

target_include_directories(rime_worldkit
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PRIVATE, all of it: the public header forward-declares `ecs::World` and names nothing else, so a
# dependent gets the profile without inheriting the include paths of eight modules it may not use.
# rime::ecs is PUBLIC because the one function takes an ecs::World&.
target_link_libraries(rime_worldkit
    PUBLIC rime::ecs
    PRIVATE rime::physics
            rime::render
            rime::destruction
            rime::destruction_net
            rime::gameplay
            rime::gameplay_net)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_worldkit)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_worldkit)
