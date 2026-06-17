// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.8 (module loader / "a module loads at runtime"). The manager loads a module by name
// at runtime, pulling in its dependencies first (lifecycle order verified via a trace); loading is
// idempotent; unloading removes dependents before the dependency, and unload_all reverses the load
// order; missing dependencies, unregistered names, and dependency cycles are rejected. The engine
// only ever touches IModule / ModuleManager — never a concrete module type after registration.

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "rime/core/diagnostics/log.hpp"
#include "rime/core/modules.hpp"

using namespace rime::core;

namespace {

// Lifecycle events are appended here so tests can assert exact order.
std::vector<std::string> g_trace;

// Silence the manager's info/error logging during tests (it logs cycle/missing-dependency errors).
void silence_logs() {
    set_log_sink([](const LogRecord&) {});
}

struct ModuleA : IModule {
    std::string_view name() const override { return "A"; }

    void on_load(ModuleManager&) override { g_trace.push_back("load A"); }

    void on_unload(ModuleManager&) override { g_trace.push_back("unload A"); }
};

struct ModuleB : IModule { // depends on A

    std::string_view name() const override { return "B"; }

    std::vector<std::string_view> dependencies() const override { return {"A"}; }

    void on_load(ModuleManager& manager) override {
        // Our dependency must already be loaded and reachable when our on_load runs.
        g_trace.push_back(manager.get("A") != nullptr ? "load B (A ready)" : "load B (A MISSING)");
    }

    void on_unload(ModuleManager&) override { g_trace.push_back("unload B"); }
};

struct ModuleC : IModule { // depends on B, giving the chain A <- B <- C

    std::string_view name() const override { return "C"; }

    std::vector<std::string_view> dependencies() const override { return {"B"}; }

    void on_load(ModuleManager&) override { g_trace.push_back("load C"); }

    void on_unload(ModuleManager&) override { g_trace.push_back("unload C"); }
};

// A mutually-dependent pair: D <-> E (a cycle).
struct ModuleD : IModule {
    std::string_view name() const override { return "D"; }

    std::vector<std::string_view> dependencies() const override { return {"E"}; }
};

struct ModuleE : IModule {
    std::string_view name() const override { return "E"; }

    std::vector<std::string_view> dependencies() const override { return {"D"}; }
};

} // namespace

TEST_CASE("a module and its dependencies load at runtime, in dependency order") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleA>("A");
    manager.register_module<ModuleB>("B");

    CHECK(manager.load("B")); // pulls in A first
    CHECK(manager.is_loaded("A"));
    CHECK(manager.is_loaded("B"));
    CHECK(manager.loaded_count() == 2);

    REQUIRE(g_trace.size() == 2);
    CHECK(g_trace[0] == "load A");           // dependency loaded first
    CHECK(g_trace[1] == "load B (A ready)"); // dependent sees its dependency

    // The engine reaches a module only through the interface.
    REQUIRE(manager.get("A") != nullptr);
    CHECK(manager.get("A")->name() == "A");
}

TEST_CASE("loading is idempotent") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleA>("A");
    CHECK(manager.load("A"));
    CHECK(manager.load("A")); // second load does nothing new
    CHECK(manager.loaded_count() == 1);
    CHECK(g_trace.size() == 1); // on_load ran exactly once
}

TEST_CASE("unloading a dependency unloads its dependents first") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleA>("A");
    manager.register_module<ModuleB>("B");
    manager.load("B");
    g_trace.clear();

    manager.unload("A"); // B depends on A, so B must go first
    REQUIRE(g_trace.size() == 2);
    CHECK(g_trace[0] == "unload B");
    CHECK(g_trace[1] == "unload A");
    CHECK(manager.loaded_count() == 0);
}

TEST_CASE("unload_all reverses the load order") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleA>("A");
    manager.register_module<ModuleB>("B");
    manager.register_module<ModuleC>("C");
    manager.load("C"); // loads A, B, C
    g_trace.clear();

    manager.unload_all();
    REQUIRE(g_trace.size() == 3);
    CHECK(g_trace[0] == "unload C");
    CHECK(g_trace[1] == "unload B");
    CHECK(g_trace[2] == "unload A");
}

TEST_CASE("a missing dependency fails the load") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleB>("B"); // B needs A, but A is not registered
    CHECK_FALSE(manager.load("B"));
    CHECK_FALSE(manager.is_loaded("B"));
    CHECK(manager.loaded_count() == 0);
}

TEST_CASE("an unregistered module name fails the load") {
    silence_logs();
    ModuleManager manager;
    CHECK_FALSE(manager.load("Nonexistent"));
    CHECK(manager.loaded_count() == 0);
}

TEST_CASE("a dependency cycle is detected and rejected") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleD>("D");
    manager.register_module<ModuleE>("E");
    CHECK_FALSE(manager.load("D")); // D -> E -> D
    CHECK(manager.loaded_count() == 0);
}

TEST_CASE("load_all loads every registered module respecting dependencies") {
    silence_logs();
    g_trace.clear();

    ModuleManager manager;
    manager.register_module<ModuleA>("A");
    manager.register_module<ModuleB>("B");
    manager.register_module<ModuleC>("C");
    CHECK(manager.load_all());
    CHECK(manager.is_loaded("A"));
    CHECK(manager.is_loaded("B"));
    CHECK(manager.is_loaded("C"));
    CHECK(manager.loaded_count() == 3);
}
