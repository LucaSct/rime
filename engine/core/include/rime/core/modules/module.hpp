// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <string_view>
#include <vector>

// The module interface — the seam that makes Rime modular (the O3DE-"Gem" idea, see VISION /
// ARCHITECTURE). A *module* is a self-contained unit of engine functionality (rendering, physics,
// destruction, …) that the engine drives entirely through this interface: it never depends on a
// module's concrete type. That is what lets a feature module be added or removed without the rest
// of the engine caring (CLAUDE.md guardrail #2) and is the foundation for plug-in-style extension.
//
// Modules are loaded *at runtime* by the ModuleManager, which resolves dependencies and drives the
// lifecycle below. (Loading from an actual shared library via dlopen/LoadLibrary is a later
// extension that will live behind the platform layer in M2; the interface here is deliberately
// agnostic to where the module came from.) Design: docs/design/module-system.md.
namespace rime::core {

class ModuleManager; // passed to lifecycle hooks so a module can reach its dependencies

struct ModuleVersion {
    int major = 0;
    int minor = 1;
    int patch = 0;
};

class IModule {
public:
    virtual ~IModule() = default;

    // Stable identifier, unique within a ModuleManager. Return a string literal / static storage;
    // the manager keys on a copy but other modules may hold the view.
    [[nodiscard]] virtual std::string_view name() const = 0;

    [[nodiscard]] virtual ModuleVersion version() const { return {}; }

    // Names of modules that must be loaded (and on_load'd) before this one. The manager guarantees
    // that order and rejects missing dependencies and cycles.
    [[nodiscard]] virtual std::vector<std::string_view> dependencies() const { return {}; }

    // Called once when the module is loaded, AFTER all its dependencies are loaded — acquire
    // resources / register systems here. The manager is provided so the module can fetch the
    // interfaces of the dependencies it declared.
    virtual void on_load(ModuleManager& /*manager*/) {}

    // Called once when the module is unloaded, BEFORE its dependencies are unloaded and after any
    // modules depending on it have already been unloaded — release what on_load acquired.
    virtual void on_unload(ModuleManager& /*manager*/) {}
};

} // namespace rime::core
