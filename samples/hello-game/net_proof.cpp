// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// This two-process proof shows that a remote UDP client drives dedicated's actual simulation:
// the server records the ActionState fed to each tick, and an independent in-process replay of
// those states must reproduce its digest and differ from a zero-input control. It does not prove
// snapshot replication or multiplayer. Replaying the server's decisions, rather than the script
// the client hoped to send, is evidence that executed input reached authoritative game state.

#include <fmt/core.h>

#if defined(_WIN32)
// The build already passes NOMINMAX and WIN32_LEAN_AND_MEAN on the command line (top-level
// CMakeLists.txt); redefining a macro with a different token sequence is MSVC C4005, an error under
// /WX, so each define is guarded. LEAN_AND_MEAN also keeps <winsock.h> out of this TU: the socket
// work goes through rime::platform, never raw Winsock, so <windows.h> is the only system header.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "game.hpp"
#include "rime/app/application.hpp"
#include "rime/app/content_root.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/platform/socket.hpp"
#include "rime/replication/input_v2.hpp"
#include "rime/scene/derive_transforms.hpp"
#include "rime/scene/scene_format.hpp"

namespace {
using namespace rime;
using namespace std::chrono_literals;

int failures = 0;

void check(bool good, std::string_view label) {
    fmt::print("[{}] {}\n", good ? " ok " : "FAIL", label);
    if (!good)
        ++failures;
}

std::optional<std::string> field(std::string_view line, std::string_view key) {
    const std::string needle = fmt::format("\"{}\":", key);
    auto p = line.find(needle);
    if (p == std::string_view::npos)
        return std::nullopt;
    p += needle.size();
    if (p < line.size() && line[p] == '"') {
        auto end = line.find('"', ++p);
        if (end == std::string_view::npos)
            return std::nullopt;
        return std::string(line.substr(p, end - p));
    }
    auto end = line.find_first_of(",}", p);
    return std::string(line.substr(p, end - p));
}

std::uint64_t number(std::string_view line, std::string_view key) {
    const auto value = field(line, key);
    if (!value)
        return 0;
    try {
        return std::stoull(*value, nullptr, 0);
    } catch (...) {
        return 0;
    }
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct Child {
#if defined(_WIN32)
    DWORD pid = 0;
    HANDLE process = nullptr;
    HANDLE pipe_read = nullptr;
#else
    pid_t pid = -1;
    int fd = -1;
#endif
    int status = -1;
    std::string pending;
    std::vector<std::string> lines;

    // CreateProcessW receives one mutable command line. Quotes and backslashes must follow the
    // Windows argument rules so a checkout path (or trace path) with spaces reaches the game whole.
#if defined(_WIN32)
    static std::wstring quoted(std::wstring_view argument) {
        std::wstring result = L"\"";
        std::size_t slashes = 0;
        for (const wchar_t c : argument) {
            if (c == L'\\') {
                ++slashes;
            } else if (c == L'"') {
                result.append(slashes * 2 + 1, L'\\');
                result.push_back(c);
                slashes = 0;
            } else {
                result.append(slashes, L'\\');
                result.push_back(c);
                slashes = 0;
            }
        }
        result.append(slashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }
#endif

    Child(const std::filesystem::path& binary, int ticks, const std::filesystem::path& trace = {}) {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES inherited{};
        inherited.nLength = sizeof(inherited);
        inherited.bInheritHandle = TRUE;
        HANDLE pipe_write = nullptr;
        if (!::CreatePipe(&pipe_read, &pipe_write, &inherited, 0))
            return;
        if (!::SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
            ::CloseHandle(pipe_write);
            ::CloseHandle(pipe_read);
            pipe_read = nullptr;
            return;
        }
        // A real inheritable stdin keeps STARTF_USESTDHANDLES valid even under a test runner
        // whose own standard handles are absent or not inheritable.
        const HANDLE input = ::CreateFileW(L"NUL",
                                           GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &inherited,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            ::CloseHandle(pipe_write);
            ::CloseHandle(pipe_read);
            pipe_read = nullptr;
            return;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = pipe_write;
        startup.hStdError = pipe_write;
        PROCESS_INFORMATION info{};
        const std::wstring application = binary.wstring();
        std::wstring command = quoted(application) + L" dedicated --listen 127.0.0.1:0 --ticks " +
                               std::to_wstring(ticks);
        if (!trace.empty())
            command += L" --input-trace " + quoted(trace.wstring());
        const BOOL started = ::CreateProcessW(application.c_str(),
                                              command.data(),
                                              nullptr,
                                              nullptr,
                                              TRUE,
                                              CREATE_NO_WINDOW,
                                              nullptr,
                                              nullptr,
                                              &startup,
                                              &info);
        ::CloseHandle(input);
        ::CloseHandle(pipe_write);
        if (!started) {
            ::CloseHandle(pipe_read);
            pipe_read = nullptr;
            return;
        }
        process = info.hProcess;
        pid = info.dwProcessId;
        ::CloseHandle(info.hThread);
#else
        int pipefd[2];
        if (::pipe(pipefd) != 0)
            return;
        pid = ::fork();
        if (pid == 0) {
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            const std::string count = std::to_string(ticks);
            if (trace.empty()) {
                ::execl(binary.c_str(),
                        binary.c_str(),
                        "dedicated",
                        "--listen",
                        "127.0.0.1:0",
                        "--ticks",
                        count.c_str(),
                        static_cast<char*>(nullptr));
            } else {
                ::execl(binary.c_str(),
                        binary.c_str(),
                        "dedicated",
                        "--listen",
                        "127.0.0.1:0",
                        "--ticks",
                        count.c_str(),
                        "--input-trace",
                        trace.c_str(),
                        static_cast<char*>(nullptr));
            }
            ::_exit(127);
        }
        ::close(pipefd[1]);
        fd = pipefd[0];
        if (pid < 0) {
            ::close(fd);
            fd = -1;
            return;
        }
        (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
    }

    ~Child() {
#if defined(_WIN32)
        if (process) {
            if (::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                ::TerminateProcess(process, 1);
                ::WaitForSingleObject(process, INFINITE);
            }
            ::CloseHandle(process);
        }
        if (pipe_read)
            ::CloseHandle(pipe_read);
#else
        if (pid > 0 && status == -1) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
        if (fd >= 0)
            ::close(fd);
#endif
    }

    void pump() {
#if defined(_WIN32)
        if (pipe_read) {
#else
        if (fd >= 0) {
#endif
            char buffer[4096];
            for (;;) {
#if defined(_WIN32)
                DWORD available = 0;
                if (!::PeekNamedPipe(pipe_read, nullptr, 0, nullptr, &available, nullptr) ||
                    available == 0)
                    break;
                DWORD n = 0;
                // Read no more than is already buffered so ReadFile can never block; the explicit
                // cast is MSVC C4267 (size_t -> DWORD) under /W4 /WX.
                const DWORD count =
                    available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
                if (!::ReadFile(pipe_read, buffer, count, &n, nullptr) || n == 0)
                    break;
#else
                const auto n = ::read(fd, buffer, sizeof(buffer));
                if (n <= 0)
                    break;
#endif
                pending.append(buffer, static_cast<std::size_t>(n));
                std::size_t pos;
                while ((pos = pending.find('\n')) != std::string::npos) {
                    lines.push_back(pending.substr(0, pos));
                    pending.erase(0, pos + 1);
                }
            }
        }
#if defined(_WIN32)
        if (process && status == -1 && ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            if (::GetExitCodeProcess(process, &code))
                status = static_cast<int>(code);
            else
                status = 1;
        }
#else
        if (pid > 0 && status == -1) {
            int result = 0;
            if (::waitpid(pid, &result, WNOHANG) == pid)
                status = result;
        }
#endif
    }

    std::optional<std::string> event(std::string_view name) const {
        for (const auto& line : lines)
            if (field(line, "event") == name)
                return line;
        return std::nullopt;
    }

    std::uint16_t ready() {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (auto line = event("ready")) {
                const auto bind = field(*line, "bind");
                if (!bind || !bind->starts_with("127.0.0.1:"))
                    return 0;
                try {
                    return static_cast<std::uint16_t>(std::stoul(bind->substr(10)));
                } catch (...) {
                    return 0;
                }
            }
            if (status != -1)
                break;
            std::this_thread::sleep_for(2ms);
        }
        return 0;
    }

    bool finish() {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (status != -1) {
                pump();
#if defined(_WIN32)
                return status == 0;
#else
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }
};

struct Client {
    std::optional<net::UdpLink> link;
    std::unique_ptr<net::NetDriver> driver;
    std::optional<net::SessionId> id;
    bool connected = false;
    bool replied = false;
    replication::InputHelloReply reply{};
    replication::InputWindow window;
    bool prune_ok = true;
    std::uint32_t highest_retired = 0;
    bool has_retirement = false;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> messages;

    explicit Client(std::uint16_t port) {
        link = net::UdpLink::bind(0, "127.0.0.1");
        if (!link)
            return;
        driver = std::make_unique<net::NetDriver>(*link, net::NetDriver::Config{});
        auto endpoint = platform::Endpoint::from_string("127.0.0.1", port);
        if (endpoint)
            id = driver->connect(*endpoint, now_ms());
    }

    void pump() {
        if (!driver || !id)
            return;
        events.clear();
        driver->update(now_ms(), events);
        messages.clear();
        for (auto& event : events) {
            if (event.kind == net::SessionEvent::Kind::Connected)
                connected = true;
            // A refusal is a reliable reply followed by the server's disconnect; read in one
            // update, the reply arrives on the Disconnected event, not in a session that is gone.
            if (event.kind == net::SessionEvent::Kind::Disconnected)
                for (auto& message : event.undrained)
                    messages.push_back(std::move(message));
        }
        if (auto* session = driver->session(*id))
            session->drain_received(messages);
        for (const auto& message : messages) {
            replication::InputHelloReply decoded{};
            if (replication::decode_input_hello_reply(message.bytes, decoded)) {
                reply = decoded;
                replied = true;
            }
            replication::InputResult result{};
            if (replication::decode_input_result(message.bytes, result)) {
                const auto before = std::vector<replication::InputCommand>(
                    window.unretired().begin(), window.unretired().end());
                window.apply_result(result);
                for (const auto& command : before) {
                    bool remains = false;
                    for (const auto& held : window.unretired())
                        if (held.sequence == command.sequence)
                            remains = true;
                    if (!remains &&
                        (!result.any_retired || command.sequence > result.retired_through))
                        prune_ok = false;
                }
                if (result.any_retired) {
                    highest_retired = result.retired_through;
                    has_retirement = true;
                }
            }
        }
    }

    bool wait_connected() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (connected)
                return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

    bool hello(std::uint16_t min = 2, std::uint16_t max = 2, bool wrong_schema = false) {
        if (!wait_connected())
            return false;
        const auto def = hello_game::definition();
        const auto& profile = *def.network_input;
        replication::InputHello offer{
            static_cast<std::uint8_t>(min),
            static_cast<std::uint8_t>(max),
            app::input_schema_identity(profile, def.remote_play ? &*def.remote_play : nullptr) ^
                static_cast<std::uint64_t>(wrong_schema)};
        std::array<std::byte, replication::kInputHelloBytes> bytes{};
        const auto n = replication::encode_input_hello(offer, bytes);
        auto* session = driver->session(*id);
        if (!session || !session->send_reliable(std::span(bytes).first(n), now_ms()))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (replied)
                return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

    bool send(std::span<const std::byte> bytes) {
        auto* session = driver && id ? driver->session(*id) : nullptr;
        return session && session->send_unreliable(bytes, now_ms());
    }
};

replication::InputCommand command(std::uint32_t sequence) {
    replication::InputCommand cmd{};
    cmd.sequence = sequence;
    // Horizontal motion is enough to change the state without ending the bounded game early.
    cmd.move_x = sequence < 120 ? 1.0f : -1.0f;
    cmd.move_y = sequence % 80 < 40 ? -0.5f : 0.5f;
    return cmd;
}

std::vector<std::byte> packet(std::uint32_t sequence = 1) {
    const auto cmd = command(sequence);
    std::array<std::byte, 4 + 28> bytes{};
    const auto n = replication::encode_input_v2(std::span(&cmd, 1), bytes);
    return {bytes.begin(), bytes.begin() + n};
}

std::uint64_t replay(const std::vector<app::ActionState>& inputs) {
    app::AppConfig config{};
    config.gpu = false;
    config.tick_hz = 60.0;
    config.worker_threads = 1;
    app::Application application(config);
    const auto definition = hello_game::definition();
    app::ContentSearch search{};
    search.dev_content_dir = definition.dev_content_dir;
    search.dev_binary_dir = definition.dev_binary_dir;
    search.required = hello_game::kEntryScene;
    const auto root = app::resolve_content_root(search);
    if (!root.ok) {
        fmt::print(stderr, "replay content: {}\n", app::describe_content_failure(root));
        return 0;
    }
    hello_game::register_components(application.world());
    if (!scene::load_scene_file(application.world(), root.dir / hello_game::kEntryScene).ok) {
        fmt::print(stderr, "replay scene load failed\n");
        return 0;
    }
    scene::derive_world_transforms(application.world(), application.jobs());
    hello_game::HelloGame game;
    app::SetupContext setup{
        application.world(), application.jobs(), application.fixed_dt(), root.dir};
    if (!game.setup(setup)) {
        fmt::print(stderr, "replay setup failed\n");
        return 0;
    }
    std::size_t tick = 0;
    application.on_fixed_tick([&](ecs::World& world, double dt) {
        app::TickContext context{world, dt, tick, inputs[tick]};
        game.fixed_tick(context);
        ++tick;
    });
    for (std::size_t i = 0; i < inputs.size(); ++i)
        (void)application.step(application.fixed_dt());
    return game.state_digest(application.world());
}

std::optional<bool> vulkan_mapped(std::uint64_t pid) {
#if defined(__linux__)
    std::ifstream maps(fmt::format("/proc/{}/maps", pid));
    if (!maps)
        return std::nullopt;
    std::string line;
    while (std::getline(maps, line))
        if (line.find("libvulkan") != std::string::npos)
            return true;
    return false;
#else
    (void)pid;
    return std::nullopt;
#endif
}

int positive(const std::filesystem::path& binary) {
#if defined(_WIN32)
    const auto trace_path =
        binary.parent_path() / fmt::format("net-proof-{}.trace", ::GetCurrentProcessId());
#else
    const auto trace_path = binary.parent_path() / fmt::format("net-proof-{}.trace", ::getpid());
#endif
    Child child(binary, 240, trace_path);
    const auto port = child.ready();
    check(port != 0, "ready parsed with nonzero port");
    // Read only by the Linux-only /proc/<pid>/maps check below; [[maybe_unused]] keeps Apple
    // Clang's -Wunused-but-set-variable (an error under -Werror, CI on #349) quiet on the other
    // platforms.
    [[maybe_unused]] bool mapped = false;
    [[maybe_unused]] bool maps_read = false;
    if (auto value = vulkan_mapped(child.pid)) {
        mapped = *value;
        maps_read = true;
    }
    Client client(port);
    const bool welcomed = client.hello();
    check(welcomed && client.reply.refusal == replication::InputHelloRefusal::None,
          "InputHello codec and schema accepted");
    std::uint32_t sequence = 1;
    auto next = std::chrono::steady_clock::now();
    while (child.status == -1 && sequence <= 260) {
        child.pump();
        client.pump();
        if (std::chrono::steady_clock::now() >= next && welcomed) {
            client.window.push(command(sequence++));
            std::array<std::byte, 4 + 16 * 28> bytes{};
            const auto n = replication::encode_input_v2(client.window.packet(), bytes);
            (void)client.send(std::span(bytes).first(n));
            next += 16667us;
        }
        std::this_thread::sleep_for(1ms);
    }
    const bool exited = child.finish();
    client.pump();
    check(exited, "child exit 0");
    check(child.event("client_connected").has_value(), "client_connected seen");
    std::vector<app::ActionState> inputs;
    std::size_t applied_lines = 0;
    bool trace_valid = true;
    {
        std::ifstream trace(trace_path);
        std::string line;
        while (std::getline(trace, line)) {
            std::istringstream stream(line);
            std::size_t index = 0;
            int applied = 0;
            app::ActionState state{};
            if (!(stream >> index >> applied) || index != inputs.size() ||
                (applied != 0 && applied != 1))
                trace_valid = false;
            for (float& axis : state.axes)
                if (!(stream >> axis))
                    trace_valid = false;
            if (!(stream >> state.held >> state.pressed))
                trace_valid = false;
            if (applied)
                ++applied_lines;
            inputs.push_back(state);
        }
    }
    std::error_code remove_error;
    std::filesystem::remove(trace_path, remove_error);
    check(trace_valid && inputs.size() == 240, "trace has exactly 240 valid tick lines");
    const auto finished = child.event("finished");
    const auto server_digest = finished ? number(*finished, "digest") : 0;
    const auto measured_applied = finished ? number(*finished, "applied") : 0;
    const auto replay_digest = inputs.size() == 240 ? replay(inputs) : 0;
    fmt::print("replay digest 0x{:016x}, server digest 0x{:016x}\n", replay_digest, server_digest);
    check(finished && replay_digest != 0 && replay_digest == server_digest,
          "trace replay digest equals finished digest and is nonzero");
    check(inputs.size() == 240 && replay_digest != replay(std::vector<app::ActionState>(240)),
          "replay digest differs from 240-tick zero-input control");
    check(measured_applied >= 120 && measured_applied == applied_lines,
          fmt::format("applied {}/240 (trace agrees)", measured_applied));
    check(client.window.retired_applied() > 0, "client retired_applied > 0");
    check(client.prune_ok && client.window.evicted() == 0,
          "client pruned only commands covered by received retired_through");
#if defined(__linux__)
    check(maps_read && !mapped, "server mapped no libvulkan");
#else
    fmt::print("[ ok ] libvulkan check skipped: /proc/<pid>/maps is Linux-only\n");
#endif
    return failures ? 1 : 0;
}

void send_oversized(std::uint16_t port) {
    // The engine's own UDP socket, so POSIX and Winsock share one code path (it also does the
    // WSAStartup the raw call would have needed). 1201 bytes is one over the 1200-byte packet cap.
    auto udp = platform::UdpSocket::bind(0);
    const auto endpoint = platform::Endpoint::from_string("127.0.0.1", port);
    if (!udp || !endpoint)
        return;
    std::array<std::byte, 1201> bytes{};
    (void)udp->send_to(*endpoint, bytes);
}

int negative(const std::filesystem::path& binary) {
    constexpr std::array<std::string_view, 13> cases{"truncated",
                                                     "bad_count",
                                                     "unknown_codec",
                                                     "reserved_nonzero",
                                                     "non_finite",
                                                     "sequence_order",
                                                     "oversized",
                                                     "hello_version",
                                                     "hello_schema",
                                                     "full",
                                                     "silence",
                                                     "unadmitted_messages",
                                                     "bad_length"};
    for (const auto name : cases) {
        // "full" needs two complete handshakes in sequence before the refusal can be counted, and
        // the server stops after its tick budget whether or not they finished. One second (60
        // ticks) was enough on Linux but a stalled hosted macOS runner missed it once (#349), so
        // that case gets three seconds; the rest need only one exchange.
        Child child(binary, name == "full" ? 180 : 60);
        const auto port = child.ready();
        bool sent = false;
        bool refusal_reply = true;
        if (port && name == "oversized") {
            send_oversized(port);
            sent = true;
        } else if (port) {
            Client client(port);
            if (name == "hello_version") {
                sent = client.hello(3, 3);
                refusal_reply = client.reply.refusal == replication::InputHelloRefusal::Version;
            } else if (name == "hello_schema") {
                sent = client.hello(2, 2, true);
                refusal_reply = client.reply.refusal == replication::InputHelloRefusal::Schema;
            } else if (name == "unadmitted_messages") {
                sent = client.wait_connected() && client.send(packet());
            } else if (name == "full") {
                sent = client.hello();
                Client second(port);
                sent = sent && second.hello();
                refusal_reply = second.reply.refusal == replication::InputHelloRefusal::Full;
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                while (child.status == -1 && std::chrono::steady_clock::now() < deadline) {
                    child.pump();
                    client.pump();
                    second.pump();
                    std::this_thread::sleep_for(2ms);
                }
            } else if (name == "silence") {
                sent = client.hello();
                for (std::uint32_t i = 1; i <= 3 && sent; ++i) {
                    sent = client.send(packet(i));
                    std::this_thread::sleep_for(17ms);
                    client.pump();
                }
            } else {
                sent = client.hello();
                auto bytes = packet();
                if (name == "truncated")
                    bytes.resize(3);
                if (name == "bad_length")
                    bytes.push_back(std::byte{0});
                if (name == "bad_count")
                    bytes[2] = std::byte{0};
                if (name == "unknown_codec")
                    bytes[1] = std::byte{3};
                if (name == "reserved_nonzero")
                    bytes[3] = std::byte{1};
                if (name == "non_finite") {
                    bytes[8] = std::byte{0};
                    bytes[9] = std::byte{0};
                    bytes[10] = std::byte{0xc0};
                    bytes[11] = std::byte{0x7f};
                }
                if (name == "sequence_order") {
                    const std::array cmds{command(2), command(1)};
                    std::array<std::byte, 60> ordered{};
                    const auto n = replication::encode_input_v2(cmds, ordered);
                    bytes.assign(ordered.begin(), ordered.begin() + n);
                }
                sent = sent && client.send(bytes);
            }
            if (name != "full") {
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                while (child.status == -1 && std::chrono::steady_clock::now() < deadline) {
                    child.pump();
                    client.pump();
                    std::this_thread::sleep_for(2ms);
                }
            }
        }
        const bool exited = child.finish();
        const auto finished = child.event("finished");
        const std::string key = name == "hello_version"  ? "hello_version"
                                : name == "hello_schema" ? "hello_schema"
                                : name == "full"         ? "clients_refused"
                                : name == "silence"      ? "neutralized"
                                                         : std::string(name);
        bool good = sent && refusal_reply && exited && finished && number(*finished, key) > 0;
        if (finished) {
            if (name == "silence")
                good = good && number(*finished, "synthesized") > 0 &&
                       number(*finished, "applied") > 0;
            else
                good = good && number(*finished, "applied") == 0;
            if (name == "full")
                good = good && number(*finished, "clients_refused") == 1;
        }
        // Every input to the verdict is printed, so a red run on a machine we cannot log into
        // says which part failed instead of only that something did.
        check(good,
              fmt::format("negative {}: {} counter {}, applied {}, child exit 0 (sent {}, "
                          "refusal reply {}, exited {}, finished {})",
                          name,
                          key,
                          finished ? number(*finished, key) : 0,
                          finished ? number(*finished, "applied") : 0,
                          sent,
                          refusal_reply,
                          exited,
                          finished.has_value()));
    }
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    auto binary = std::filesystem::absolute(argv[0]).parent_path() / "hello_game";
    binary.replace_extension(std::filesystem::path(argv[0]).extension());
    return argc == 2 && std::string_view(argv[1]) == "--negative" ? negative(binary)
                                                                  : positive(binary);
}
