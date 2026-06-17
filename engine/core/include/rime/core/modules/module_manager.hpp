// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rime/core/modules/module.hpp"

// The ModuleManager registers module factories by name and, at runtime, loads them — resolving
// dependencies into the correct order, detecting missing dependencies and cycles, and driving the
// IModule lifecycle. The engine talks only to this and to IModule, never to a concrete module:
// compiled-in modules register a factory here; a future dynamic loader would register one after
// opening a shared library. Removing a module simply means not registering it — everything else
// still builds and runs. Design: docs/design/module-system.md.
namespace rime::core {

class ModuleManager {
public:
    // Creates a fresh module instance. Construction must be cheap and side-effect-free — real work
    // belongs in IModule::on_load, which the manager calls after dependencies are ready.
    using Factory = std::function<std::unique_ptr<IModule>()>;

    ModuleManager() = default;
    ~ModuleManager(); // unloads everything still loaded, in reverse order

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    // Make a module known under `name`. Convenience overload registers a default-constructible
    // concrete module type T. Re-registering a name replaces its factory (only affects future
    // loads).
    void register_module(std::string name, Factory factory);

    template <class T> void register_module(std::string name) {
        register_module(std::move(name), [] { return std::make_unique<T>(); });
    }

    [[nodiscard]] bool is_registered(std::string_view name) const;

    // Load a module by name at runtime: instantiate it and any not-yet-loaded dependencies, in
    // dependency order, calling on_load on each. Returns false (loading nothing new) if the module
    // or a dependency is not registered, or if a dependency cycle is detected. Already-loaded
    // modules are a no-op success (idempotent).
    bool load(std::string_view name);

    // Load every registered module, each with its dependencies, in a valid order.
    bool load_all();

    [[nodiscard]] bool is_loaded(std::string_view name) const;

    [[nodiscard]] std::size_t loaded_count() const noexcept { return loaded_.size(); }

    // The loaded instance, or nullptr if not loaded. Typically called from a dependent's on_load to
    // reach a dependency's interface.
    [[nodiscard]] IModule* get(std::string_view name);

    // Unload a module: first unloads anything that depends on it (recursively), then calls its
    // on_unload and destroys it. No-op if not loaded.
    void unload(std::string_view name);

    // Unload everything in reverse load order (so dependents go before dependencies).
    void unload_all();

private:
    bool load_recursive(std::string_view name, std::vector<std::string_view>& visiting);

    std::unordered_map<std::string, Factory> registry_;
    std::unordered_map<std::string, std::unique_ptr<IModule>> loaded_;
    std::vector<std::string> load_order_; // order modules were loaded; reverse is the unload order
};

} // namespace rime::core
