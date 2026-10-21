#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr unsigned short kDefaultPort = 8888;
constexpr std::size_t kMaxClients = 256;
constexpr const char* kReset = "\x1b[0m";
constexpr const char* kCyan = "\x1b[96m";
constexpr const char* kGreen = "\x1b[92m";
constexpr const char* kYellow = "\x1b[93m";
constexpr const char* kRed = "\x1b[91m";
constexpr const char* kGray = "\x1b[90m";

std::atomic<bool> g_running{true};
std::atomic<SOCKET> g_listen_socket{INVALID_SOCKET};
std::mutex g_clients_mutex;
std::vector<std::shared_ptr<struct ClientSession>> g_clients;
std::mutex g_all_sessions_mutex;
std::vector<std::shared_ptr<struct ClientSession>> g_all_sessions;
std::vector<std::thread> g_workers;

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

void enable_console_utf8() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleTitleW(L"NEXUS CHAT - TCP Server");
}

void log_line(const char* color, const std::string& message) {
    std::cout << color << "[" << timestamp_now() << "] " << message
              << kReset << "\n" << std::flush;
}

bool contains_control_characters(const std::string& value) {
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7F) return true;
    }
    return false;
}

std::string trim_ascii_spaces(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

struct ClientSession {
    explicit ClientSession(SOCKET value, std::string peer_value)
        : socket(value), peer(std::move(peer_value)) {}

    SOCKET socket = INVALID_SOCKET;
    std::string peer;
    std::string username;
    std::mutex send_mutex;
    std::atomic<bool> alive{true};
    std::atomic<bool> registered{false};
    std::uint64_t last_private_sequence = 0;
    std::uint64_t last_client_sequence = 0;  // Accessed only by this session's worker.

    bool send_frame(const chatproto::Frame& frame) {
        std::lock_guard<std::mutex> lock(send_mutex);
        if (!alive.load() || socket == INVALID_SOCKET) return false;
        if (!chatproto::send_frame(socket, frame)) {
            alive.store(false);
            ::shutdown(socket, SD_BOTH);
            return false;
        }
        return true;
    }

    void shutdown_io() {
        std::lock_guard<std::mutex> lock(send_mutex);
        alive.store(false);
        if (socket != INVALID_SOCKET) ::shutdown(socket, SD_BOTH);
    }

    void close_socket() {
        std::lock_guard<std::mutex> lock(send_mutex);
        alive.store(false);
        if (socket != INVALID_SOCKET) {
            ::shutdown(socket, SD_BOTH);
            ::closesocket(socket);
            socket = INVALID_SOCKET;
        }
    }
};

std::vector<std::shared_ptr<ClientSession>> snapshot_clients() {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    return g_clients;
}

chatproto::Frame make_error_frame(const std::string& message) {
    chatproto::Writer writer;
    writer.put_string(message);
    return {chatproto::Type::ERROR_MESSAGE, std::move(writer.data)};
}

chatproto::Frame make_system_frame(const std::string& message) {
    chatproto::Writer writer;
    writer.put_string(timestamp_now());
    writer.put_string(message);
    return {chatproto::Type::SYSTEM, std::move(writer.data)};
}

chatproto::Frame make_user_list_frame(
    const std::vector<std::shared_ptr<ClientSession>>& sessions) {
    chatproto::Writer writer;
    const auto count = static_cast<std::uint16_t>(
        std::min<std::size_t>(sessions.size(), 65535U));
    writer.put_u16(count);
    for (std::size_t i = 0; i < count; ++i) {
        writer.put_string(sessions[i]->username);
    }
    return {chatproto::Type::USER_LIST, std::move(writer.data)};
}

void send_error(const std::shared_ptr<ClientSession>& session,
                const std::string& message) {
    session->send_frame(make_error_frame(message));
}

void broadcast_system(const std::string& message,
                      const ClientSession* excluded = nullptr) {
    const auto frame = make_system_frame(message);
    const auto sessions = snapshot_clients();
    for (const auto& session : sessions) {
        if (session.get() != excluded) session->send_frame(frame);
    }
}

void send_user_list_to(const std::shared_ptr<ClientSession>& session) {
    const auto sessions = snapshot_clients();
    session->send_frame(make_user_list_frame(sessions));
}

void broadcast_user_list() {
    const auto sessions = snapshot_clients();
    const auto frame = make_user_list_frame(sessions);
    for (const auto& session : sessions) session->send_frame(frame);
}

std::size_t broadcast_frame(const chatproto::Frame& frame) {
    const auto sessions = snapshot_clients();
    std::size_t delivered = 0;
    for (const auto& session : sessions) {
        if (session->send_frame(frame)) ++delivered;
    }
    return delivered;
}

void finish_session(const std::shared_ptr<ClientSession>& session,
                    bool announce_leave) {
    const bool was_registered = session->registered.exchange(false);
    const std::string username = session->username;

    if (was_registered) {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.erase(
            std::remove_if(
                g_clients.begin(), g_clients.end(),
                [&](const std::shared_ptr<ClientSession>& item) {
                    return item.get() == session.get();
                }),
            g_clients.end());
    }

    session->close_socket();

    if (was_registered && announce_leave && g_running.load()) {
        log_line(kYellow, "Client left: " + username + " (" + session->peer + ")");
        broadcast_system(username + " left the chat");
        broadcast_user_list();
    }
}

void handle_client(const std::shared_ptr<ClientSession>& session) {
    chatproto::Frame first;
    if (!chatproto::recv_frame(session->socket, first)) {
        finish_session(session, false);
        return;
    }

    if (first.type != chatproto::Type::HELLO) {
        send_error(session, "The first message must be HELLO.");
        finish_session(session, false);
        return;
    }

    chatproto::Reader hello_reader(first.payload);
    std::string username;
    if (!hello_reader.get_string(username) || !hello_reader.at_end()) {
        send_error(session, "Malformed HELLO message.");
        finish_session(session, false);
        return;
    }

    username = trim_ascii_spaces(username);
    if (username.empty() || username.size() > chatproto::kMaxUsernameBytes ||
        contains_control_characters(username)) {
        send_error(session, "Username must be 1-48 UTF-8 bytes and contain no control characters.");
        finish_session(session, false);
        return;
    }

    bool accepted = false;
    std::string rejection;
    {
        // Hold the registry lock while sending WELCOME and registering the client.
        // This prevents another broadcast from reaching the client before WELCOME.
        std::lock_guard<std::mutex> lock(g_clients_mutex);

        const bool duplicate = std::any_of(
            g_clients.begin(), g_clients.end(),
            [&](const std::shared_ptr<ClientSession>& item) {
                return item->registered.load() && item->username == username;
            });

        if (duplicate) {
            rejection = "That username is already online. Choose another username.";
        } else if (g_clients.size() >= kMaxClients) {
            rejection = "The server is full. Please try again later.";
        } else {
            session->username = username;
            chatproto::Writer welcome_writer;
            welcome_writer.put_string(timestamp_now());
            welcome_writer.put_string("Welcome, " + username + "!");
            const chatproto::Frame welcome{
                chatproto::Type::WELCOME, std::move(welcome_writer.data)};

            if (session->send_frame(welcome)) {
                session->registered.store(true);
                g_clients.push_back(session);
                accepted = true;
            } else {
                rejection = "Unable to send the welcome message.";
            }
        }
    }

    if (!accepted) {
        if (!rejection.empty()) send_error(session, rejection);
        finish_session(session, false);
        return;
    }

    log_line(kGreen, "Client joined: " + username + " (" + session->peer + ")");
    broadcast_system(username + " joined the chat", session.get());
    broadcast_user_list();

    while (g_running.load() && session->alive.load()) {
        chatproto::Frame frame;
        if (!chatproto::recv_frame(session->socket, frame)) break;

        switch (frame.type) {
        case chatproto::Type::CHAT: {
            chatproto::Reader reader(frame.payload);
            std::uint64_t sequence = 0;
            std::string message;
            if (!reader.get_u64(sequence) || !reader.get_string(message) ||
                !reader.at_end()) {
                send_error(session, "Malformed CHAT message.");
                break;
            }
            if (message.empty() || message.size() > chatproto::kMaxMessageBytes ||
                contains_control_characters(message)) {
                send_error(session, "Message must be 1-4096 UTF-8 bytes and contain no control characters.");
                break;
            }
            if (sequence <= session->last_client_sequence) {
                send_error(session, "Chat sequence number must increase for every message.");
                break;
            }
            if (session->last_client_sequence != 0 &&
                sequence - session->last_client_sequence > 1) {
                log_line(kYellow, "Sequence gap from " + username + ": previous=" +
                    std::to_string(session->last_client_sequence) +
                    ", current=" + std::to_string(sequence));
            }
            session->last_client_sequence = sequence;

            chatproto::Writer delivery_writer;
            delivery_writer.put_u64(sequence);
            delivery_writer.put_string(timestamp_now());
            delivery_writer.put_string(username);
            delivery_writer.put_string(message);
            const chatproto::Frame delivery{
                chatproto::Type::CHAT_DELIVERY, std::move(delivery_writer.data)};
            const auto delivered = broadcast_frame(delivery);
            log_line(kCyan, "CHAT seq=" + std::to_string(sequence) + " from " +
                username + "; successful socket sends=" + std::to_string(delivered));
            break;
        }
        case chatproto::Type::PRIVATE_CHAT: {
            chatproto::Reader reader(frame.payload);
            std::uint64_t sequence = 0;
            std::string target, message;
            if (!reader.get_u64(sequence) || !reader.get_string(target) ||
                !reader.get_string(message) || !reader.at_end()) {
                send_error(session, "Malformed PRIVATE_CHAT message.");
                break;
            }
            if (target.empty() || target.size() > chatproto::kMaxUsernameBytes ||
                contains_control_characters(target) || message.empty() ||
                trim_ascii_spaces(message).empty() ||
                message.size() > chatproto::kMaxMessageBytes ||
                contains_control_characters(message)) {
                send_error(session, "Private target must be 1-48 UTF-8 bytes; message must be 1-4096 UTF-8 bytes, nonblank, with no control characters.");
                break;
            }
            if (sequence <= session->last_private_sequence) {
                send_error(session, "Private sequence number must increase for every private message.");
                break;
            }
            session->last_private_sequence = sequence;
            std::shared_ptr<ClientSession> recipient;
            for (const auto& candidate : snapshot_clients()) {
                if (candidate->registered.load() && candidate->alive.load() &&
                    candidate->username == target) {
                    recipient = candidate;
                    break;
                }
            }
            if (!recipient) {
                send_error(session, "Private recipient is not online.");
                break;
            }
            chatproto::Writer writer;
            writer.put_u64(sequence);
            writer.put_string(timestamp_now());
            writer.put_string(session->username);
            writer.put_string(recipient->username);
            writer.put_string(message);
            const chatproto::Frame delivery{
                chatproto::Type::PRIVATE_DELIVERY, std::move(writer.data)};
            // Send to the recipient first; never echo success on a known send failure.
            // shared_ptr keeps the session alive; send_frame locks socket lifetime.
            if (!recipient->registered.load() || !recipient->send_frame(delivery)) {
                send_error(session, "Private recipient disconnected; delivery failed.");
                break;
            }
            if (recipient.get() != session.get()) session->send_frame(delivery);
            break;
        }
        case chatproto::Type::LIST_REQUEST:
            if (!frame.payload.empty()) {
                send_error(session, "LIST_REQUEST must have an empty payload.");
            } else {
                send_user_list_to(session);
            }
            break;
        case chatproto::Type::GOODBYE:
            // The reason is optional, but if present it must be a well-formed string.
            if (!frame.payload.empty()) {
                chatproto::Reader reader(frame.payload);
                std::string reason;
                if (!reader.get_string(reason) || !reader.at_end()) {
                    send_error(session, "Malformed GOODBYE message.");
                }
            }
            finish_session(session, true);
            return;
        default:
            send_error(session, "Message type is not valid in this connection state.");
            break;
        }
    }

    finish_session(session, true);
}

BOOL WINAPI console_handler(DWORD event_type) {
    if (event_type == CTRL_C_EVENT ||
        event_type == CTRL_BREAK_EVENT ||
        event_type == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        const SOCKET listener = g_listen_socket.exchange(INVALID_SOCKET);
        if (listener != INVALID_SOCKET) {
            ::shutdown(listener, SD_BOTH);
            ::closesocket(listener);
        }
        return TRUE;
    }
    return FALSE;
}

bool parse_port(const char* text, unsigned short& port) {
    try {
        const int value = std::stoi(text);
        if (value < 1 || value > 65535) return false;
        port = static_cast<unsigned short>(value);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    enable_console_utf8();

    unsigned short port = kDefaultPort;
    if (argc >= 2 && !parse_port(argv[1], port)) {
        std::cerr << "Usage: server.exe [port: 1-65535]\n";
        return 2;
    }

    WSADATA wsa_data{};
    const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (startup_result != 0) {
        std::cerr << "WSAStartup failed: " << startup_result << "\n";
        return 1;
    }

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "bind() failed: " << WSAGetLastError()
                  << " (is the port already in use?)\n";
        ::closesocket(listener);
        WSACleanup();
        return 1;
    }

    if (::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed: " << WSAGetLastError() << "\n";
        ::closesocket(listener);
        WSACleanup();
        return 1;
    }

    g_listen_socket.store(listener);
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::cout << kCyan
              << "============================================================\n"
              << "                    NEXUS CHAT SERVER                       \n"
              << "============================================================\n"
              << kReset;
    log_line(kGreen, "Listening on 0.0.0.0:" + std::to_string(port));
    log_line(kGray, "Press Ctrl+C to stop the server gracefully.");
    log_line(kGray, "Maximum concurrent clients: " + std::to_string(kMaxClients));

    while (g_running.load()) {
        sockaddr_storage peer_address{};
        int peer_length = sizeof(peer_address);
        const SOCKET client_socket = ::accept(
            listener, reinterpret_cast<sockaddr*>(&peer_address), &peer_length);

        if (client_socket == INVALID_SOCKET) {
            if (!g_running.load()) break;
            const int error = WSAGetLastError();
            if (error == WSAEINTR) continue;
            log_line(kRed, "accept() failed: " + std::to_string(error));
            continue;
        }

        char host[NI_MAXHOST]{};
        char service[NI_MAXSERV]{};
        std::string peer = "unknown peer";
        if (getnameinfo(
                reinterpret_cast<sockaddr*>(&peer_address), peer_length,
                host, sizeof(host), service, sizeof(service),
                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            peer = std::string(host) + ":" + service;
        }

        auto session = std::make_shared<ClientSession>(client_socket, peer);
        {
            std::lock_guard<std::mutex> lock(g_all_sessions_mutex);
            g_all_sessions.push_back(session);
        }
        g_workers.emplace_back(handle_client, session);
        log_line(kGray, "Accepted TCP connection from " + peer);
    }

    g_running.store(false);
    const SOCKET remaining_listener = g_listen_socket.exchange(INVALID_SOCKET);
    if (remaining_listener != INVALID_SOCKET) ::closesocket(remaining_listener);

    std::vector<std::shared_ptr<ClientSession>> all_sessions;
    {
        std::lock_guard<std::mutex> lock(g_all_sessions_mutex);
        all_sessions = g_all_sessions;
    }
    for (const auto& session : all_sessions) session->shutdown_io();

    for (auto& worker : g_workers) {
        if (worker.joinable()) worker.join();
    }
    for (const auto& session : all_sessions) session->close_socket();

    SetConsoleCtrlHandler(console_handler, FALSE);
    log_line(kGreen, "Server stopped.");
    WSACleanup();
    return 0;
}
