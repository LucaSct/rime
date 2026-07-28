// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The m11.2 two-process smoke test (ADR-0033 §7): the milestone's proof sentence — "two processes
// connect, exchange hello, detect peer death" — executed across a real process boundary, over real
// loopback UDP, on the real clock.
//
// Why this exists when session_test.cpp already covers all three behaviours: those run two drivers
// inside ONE process, which cannot catch anything that depends on genuine process separation, and a
// milestone whose proof sentence says "two processes" should have two processes. This is the same
// split as m11.1's `udp link loopback` case — the scripted harness proves the algorithm, a real
// socket proves the socket path, and now a real process proves the deployment shape.
//
// One binary, three modes, so CI runs a single test with a single result:
//
//     rime_net_smoke duo                          # CI: parent = server, self-spawns the client
//     rime_net_smoke server --host 0.0.0.0 --port 7777   # manual LAN mode
//     rime_net_smoke client --host 10.0.0.5 --port 7777  # manual LAN mode
//
// `duo` binds the server on port 0 (ephemeral — never a hard-coded port, which races anything else
// on the box), reads the port back, and re-executes ITSELF as the client against it. There is no
// startup ordering to manage and no sleep anywhere: the client retries its ConnectRequest for
// several seconds by design, so it does not care that the server may not be listening yet. Every
// wait is a deadline loop, so a failure is the deadline expiring, never a hang.
//
// The LAN mode is deliberately NOT a CI test. CI is one headless box; "two machines on a LAN" is a
// human gate, run on the dev server. Registering it as an automated test would be a lie about what
// was verified.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rime/net/net_driver.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

using namespace rime::net;

namespace {

// Short timers: this is a smoke test, and a five-second peer-death timeout would put five seconds
// of pure waiting into every CI run on every OS.
constexpr std::uint64_t kTimeoutMs = 1000;
constexpr std::uint64_t kHeartbeatMs = 100;

// Generous relative to the timers above, because CI machines stall. A deadline is a failure
// condition, not a schedule — the happy path never comes close to these.
constexpr int kConnectDeadlineMs = 20000;
constexpr int kExchangeDeadlineMs = 20000;
constexpr int kDeathDeadlineMs = 20000;

// The two builds must agree, and in duo mode they are the same binary, so any constant works. A
// real game passes ecs::component_schema_hash(world) here.
constexpr std::uint64_t kSchemaHash = 0x9c41e2ff00112233ull;
constexpr std::uint32_t kAppId = 0x52494D45; // 'RIME'

constexpr const char* kClientHello = "hello from client";
constexpr const char* kServerHello = "hello from server";

NetDriver::Config smoke_config(std::uint64_t salt_seed) {
    NetDriver::Config config;
    config.app_id = kAppId;
    config.schema_hash = kSchemaHash;
    config.heartbeat_interval_ms = kHeartbeatMs;
    config.timeout_ms = kTimeoutMs;
    config.connect_retry_ms = 100;
    config.connect_attempts = 200; // ~20 s of patience: the server may not be up yet
    config.resend_ms = 100;
    config.salt_seed = salt_seed;
    return config;
}

std::vector<std::byte> bytes_of(const char* text) {
    const auto* p = reinterpret_cast<const std::byte*>(text);
    return std::vector<std::byte>(p, p + std::strlen(text));
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class Stopwatch {
public:
    [[nodiscard]] std::uint64_t now_ms() const {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - start_)
                                              .count());
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "net_smoke: FAILED — %s\n", what);
    std::fflush(stderr);
    std::exit(1);
}

// ── Spawning ourselves ──────────────────────────────────────────────────────────────────
//
// argv[0] is the executable path, which is what CTest invoked us with, so we re-run it rather than
// asking the OS where we live (/proc/self/exe is Linux-only, _NSGetExecutablePath is macOS-only,
// GetModuleFileName is Win32 — three answers to a question argv[0] already answers).
#if defined(_WIN32)
using ChildHandle = PROCESS_INFORMATION;

ChildHandle spawn_client(const char* self, std::uint16_t port) {
    std::string command = std::string("\"") + self + "\" client --port " + std::to_string(port);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr,
                        command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        fail("CreateProcess for the client failed");
    }
    return process;
}

int wait_for_child(ChildHandle& child) {
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    CloseHandle(child.hThread);
    return static_cast<int>(code);
}
#else
using ChildHandle = pid_t;

ChildHandle spawn_client(const char* self, std::uint16_t port) {
    const std::string port_text = std::to_string(port);
    // posix_spawn wants a mutable argv; these strings outlive the call, which is all that matters.
    std::string self_text = self;
    std::string mode = "client";
    std::string flag = "--port";
    char* args[] = {
        self_text.data(), mode.data(), flag.data(), const_cast<char*>(port_text.c_str()), nullptr};

    pid_t pid = 0;
    if (posix_spawn(&pid, self, nullptr, nullptr, args, environ) != 0) {
        fail("posix_spawn for the client failed");
    }
    return pid;
}

int wait_for_child(ChildHandle& child) {
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

// ── The two roles ───────────────────────────────────────────────────────────────────────

// Returns 0 on success. `spawn_self` is null in manual LAN mode (a human starts the client).
int run_server(std::uint16_t port, const char* host, const char* spawn_self) {
    auto link = UdpLink::bind(port, host);
    if (!link) {
        fail("could not bind the server socket");
    }
    const std::uint16_t actual_port = link->local_endpoint().port;
    std::printf("net_smoke: server listening on %s:%u\n", host, actual_port);
    std::fflush(stdout);

    NetDriver server(*link, smoke_config(0x1111222233334444ull));
    server.listen();

    ChildHandle child{};
    bool have_child = false;
    if (spawn_self != nullptr) {
        child = spawn_client(spawn_self, actual_port);
        have_child = true;
    }

    Stopwatch clock;
    std::vector<SessionEvent> events;
    std::vector<Received> received;

    const auto pump = [&] {
        server.update(clock.now_ms(), events);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

    // 1. Two processes connect.
    SessionId peer{};
    bool connected = false;
    while (clock.now_ms() < kConnectDeadlineMs && !connected) {
        pump();
        for (const SessionEvent& event : events) {
            if (event.kind == SessionEvent::Kind::Connected) {
                peer = event.id;
                connected = true;
            }
        }
    }
    if (!connected) {
        fail("no client connected before the deadline");
    }
    std::printf("net_smoke: CONNECTED\n");
    std::fflush(stdout);

    // 2. Exchange hello. The server answers only after hearing the client, so a single assertion
    //    covers both directions once the client confirms receipt by exiting.
    bool heard_client = false;
    const std::uint64_t exchange_start = clock.now_ms();
    while (clock.now_ms() - exchange_start < kExchangeDeadlineMs && !heard_client) {
        pump();
        Session* session = server.session(peer);
        if (session == nullptr) {
            fail("the session died before the hello exchange completed");
        }
        received.clear();
        session->drain_received(received);
        for (const Received& message : received) {
            if (text_of(message.bytes) == kClientHello) {
                heard_client = true;
            }
        }
    }
    if (!heard_client) {
        fail("never received the client's hello");
    }
    if (Session* session = server.session(peer)) {
        if (!session->send_reliable(bytes_of(kServerHello), clock.now_ms())) {
            fail("could not send the server's hello");
        }
    }
    std::printf("net_smoke: HELLO-EXCHANGED\n");
    std::fflush(stdout);

    // 3. Detect peer death. The client hard-exits once it has our hello — no graceful disconnect,
    //    no goodbye packet — so the ONLY thing that can end this session is the timeout.
    const std::uint64_t death_start = clock.now_ms();
    bool died = false;
    DisconnectReason reason = DisconnectReason::Graceful;
    while (clock.now_ms() - death_start < kDeathDeadlineMs && !died) {
        const std::size_t before = events.size();
        pump();
        for (std::size_t i = before; i < events.size(); ++i) {
            if (events[i].kind == SessionEvent::Kind::Disconnected) {
                died = true;
                reason = events[i].reason;
            }
        }
    }
    if (!died) {
        fail("the dead peer was never reaped");
    }
    if (reason != DisconnectReason::Timeout) {
        fail("the peer was reaped, but not by the timeout path");
    }
    if (server.session_count() != 0) {
        fail("the session table still holds the dead peer");
    }
    std::printf("net_smoke: PEER-DEATH-DETECTED\n");
    std::fflush(stdout);

    if (have_child) {
        const int code = wait_for_child(child);
        if (code != 0) {
            fail("the client process exited non-zero");
        }
    }
    std::printf("net_smoke: OK\n");
    return 0;
}

int run_client(std::uint16_t port, const char* host) {
    auto link = UdpLink::bind(0);
    if (!link) {
        fail("could not bind the client socket");
    }
    const auto server_endpoint = Endpoint::from_string(host, port);
    if (!server_endpoint) {
        fail("malformed server address");
    }

    NetDriver client(*link, smoke_config(0xAAAABBBBCCCCDDDDull));
    Stopwatch clock;
    const auto id = client.connect(*server_endpoint, clock.now_ms());
    if (!id) {
        fail("could not start connecting");
    }

    std::vector<SessionEvent> events;
    std::vector<Received> received;
    const auto pump = [&] {
        client.update(clock.now_ms(), events);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

    bool connected = false;
    while (clock.now_ms() < kConnectDeadlineMs && !connected) {
        pump();
        for (const SessionEvent& event : events) {
            if (event.kind == SessionEvent::Kind::Connected) {
                connected = true;
            }
            if (event.kind == SessionEvent::Kind::ConnectFailed) {
                std::fprintf(stderr, "net_smoke: %s\n", format(event).c_str());
                fail("the connect attempt was refused");
            }
        }
    }
    if (!connected) {
        fail("could not connect to the server before the deadline");
    }

    Session* session = client.session(*id);
    if (session == nullptr || !session->send_reliable(bytes_of(kClientHello), clock.now_ms())) {
        fail("could not send the client's hello");
    }

    bool heard_server = false;
    const std::uint64_t exchange_start = clock.now_ms();
    while (clock.now_ms() - exchange_start < kExchangeDeadlineMs && !heard_server) {
        pump();
        Session* live = client.session(*id);
        if (live == nullptr) {
            fail("the session died before the server's hello arrived");
        }
        received.clear();
        live->drain_received(received);
        for (const Received& message : received) {
            if (text_of(message.bytes) == kServerHello) {
                heard_server = true;
            }
        }
    }
    if (!heard_server) {
        fail("never received the server's hello");
    }

    // Die abruptly. NOT session->disconnect(): the whole point of the server's third assertion is
    // that it detects a peer that vanished WITHOUT saying goodbye, which is what a crash, a pulled
    // cable, or a killed process actually looks like. _Exit skips static destructors and any
    // buffered cleanup, so nothing on the way out can accidentally announce our departure.
    std::fflush(stdout);
    std::_Exit(0);
}

std::uint16_t parse_port(int argc, char** argv, std::uint16_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            return static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        }
    }
    return fallback;
}

const char* parse_host(int argc, char** argv, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "duo";

    if (std::strcmp(mode, "duo") == 0) {
        // CI's entry point: bind an ephemeral port and re-run ourselves as the client against it.
        return run_server(0, "127.0.0.1", argv[0]);
    }
    if (std::strcmp(mode, "server") == 0) {
        return run_server(parse_port(argc, argv, 7777), parse_host(argc, argv, "0.0.0.0"), nullptr);
    }
    if (std::strcmp(mode, "client") == 0) {
        return run_client(parse_port(argc, argv, 7777), parse_host(argc, argv, "127.0.0.1"));
    }

    std::fprintf(stderr, "usage: %s [duo | server | client] [--host H] [--port P]\n", argv[0]);
    return 2;
}
