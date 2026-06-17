// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/modules/module_manager.hpp"

#include <algorithm>
#include <utility>

#include "rime/core/diagnostics/log.hpp"

// Runtime module loading: a small dependency resolver around a name->factory registry. load() is a
// depth-first walk that loads dependencies before dependents and uses an on-stack "visiting" set to
// catch cycles; unload() runs the reverse, removing dependents before the module they depend on.
// See docs/design/module-system.md.
namespace rime::core {

ModuleManager::~ModuleManager() {
    unload_all();
}

void ModuleManager::register_module(std::string name, Factory factory) {
    registry_[std::move(name)] = std::move(factory);
}

bool ModuleManager::is_registered(std::string_view name) const {
    return registry_.find(std::string(name)) != registry_.end();
}

bool ModuleManager::is_loaded(std::string_view name) const {
    return loaded_.find(std::string(name)) != loaded_.end();
}

IModule* ModuleManager::get(std::string_view name) {
    auto it = loaded_.find(std::string(name));
    return it != loaded_.end() ? it->second.get() : nullptr;
}

bool ModuleManager::load(std::string_view name) {
    std::vector<std::string_view> visiting;
    return load_recursive(name, visiting);
}

bool ModuleManager::load_recursive(std::string_view name, std::vector<std::string_view>& visiting) {
    const std::string key(name);

    if (loaded_.find(key) != loaded_.end()) {
        return true; // already loaded — idempotent
    }
    auto reg = registry_.find(key);
    if (reg == registry_.end()) {
        RIME_ERROR("module '{}' is not registered", name);
        return false;
    }
    if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
        RIME_ERROR("dependency cycle detected while loading module '{}'", name);
        return false;
    }

    visiting.push_back(name);
    // Construct first (cheap, side-effect-free) so we can query the module's declared dependencies,
    // then load those before running this module's on_load.
    std::unique_ptr<IModule> module = reg->second();
    for (std::string_view dependency : module->dependencies()) {
        if (!load_recursive(dependency, visiting)) {
            RIME_ERROR("module '{}' failed to load: dependency '{}' unavailable", name, dependency);
            visiting.pop_back();
            return false;
        }
    }
    visiting.pop_back();

    module->on_load(*this);
    loaded_.emplace(key, std::move(module));
    load_order_.push_back(key);
    RIME_INFO("loaded module '{}'", name);
    return true;
}

bool ModuleManager::load_all() {
    bool all_ok = true;
    for (const auto& [name, factory] : registry_) {
        std::vector<std::string_view> visiting;
        if (!load_recursive(name, visiting)) {
            all_ok = false;
        }
    }
    return all_ok;
}

void ModuleManager::unload(std::string_view name) {
    const std::string key(name);
    auto it = loaded_.find(key);
    if (it == loaded_.end()) {
        return; // not loaded
    }

    // Unload anything that depends on this module first (snapshot names, since unloading mutates
    // loaded_). Reverse-of-load-order is the safe direction for dependents.
    std::vector<std::string> dependents;
    for (const auto& [other_name, other_module] : loaded_) {
        for (std::string_view dependency : other_module->dependencies()) {
            if (dependency == key) {
                dependents.push_back(other_name);
                break;
            }
        }
    }
    for (const std::string& dependent : dependents) {
        unload(dependent);
    }

    // `it` may have been invalidated by the recursive unloads above; re-find before using.
    it = loaded_.find(key);
    if (it == loaded_.end()) {
        return;
    }
    it->second->on_unload(*this);
    loaded_.erase(it);
    std::erase(load_order_, key);
    RIME_INFO("unloaded module '{}'", name);
}

void ModuleManager::unload_all() {
    // Reverse load order: a module is always unloaded before the dependencies it was loaded after.
    for (auto it = load_order_.rbegin(); it != load_order_.rend(); ++it) {
        auto found = loaded_.find(*it);
        if (found != loaded_.end()) {
            found->second->on_unload(*this);
            loaded_.erase(found);
        }
    }
    load_order_.clear();
}

} // namespace rime::core
