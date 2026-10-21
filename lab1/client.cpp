#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "protocol.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr unsigned short kDefaultPort = 8888;
constexpr const char* kReset = "\x1b[0m";
constexpr const char* kCyan = "\x1b[96m";
constexpr const char* kGreen = "\x1b[92m";
constexpr const char* kYellow = "\x1b[93m";
constexpr const char* kMagenta = "\x1b[95m";
constexpr const char* kRed = "\x1b[91m";
constexpr const char* kGray = "\x1b[90m";

std::atomic<bool> g_connected{false};
std::mutex g_output_mutex;
std::string g_username;

void enable_console_utf8() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleTitleW(L"NEXUS CHAT - Client");
}

std::string trim_ascii_spaces(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool contains_control_characters(const std::string& value) {
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7F) return true;
    }
    return false;
}

void print_plain(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << text << "\n" << std::flush;
}

void print_system(const std::string& timestamp, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << kMagenta << "[" << timestamp << "] [SYSTEM] "
              << kReset << text << "\n" << std::flush;
}

void print_chat(const std::string& timestamp, const std::string& sender,
                std::uint64_t sequence, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    const char* color = (sender == g_username) ? kCyan : kYellow;
    std::cout << color << "[" << timestamp << "] " << sender
              << kGray << " #" << sequence << kReset << ": "
              << message << "\n" << std::flush;
}

void print_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << kRed << "[ERROR] " << message << kReset << "\n" << std::flush;
}

SOCKET connect_to_server(const std::string& host, unsigned short port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (lookup != 0) {
        print_error("getaddrinfo() failed: " + std::to_string(lookup));
        return INVALID_SOCKET;
    }

    SOCKET connected_socket = INVALID_SOCKET;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        const SOCKET candidate = ::socket(
            item->ai_family, item->ai_socktype, item->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        if (::connect(candidate, item->ai_addr,
                      static_cast<int>(item->ai_addrlen)) == 0) {
            connected_socket = candidate;
            break;
        }
        ::closesocket(candidate);
    }
    freeaddrinfo(result);

    if (connected_socket == INVALID_SOCKET) {
        print_error("Could not connect to " + host + ":" + std::to_string(port) +
                    ". Check the server, port, and firewall.");
    }
    return connected_socket;
}

bool parse_port(const std::string& text, unsigned short& port) {
    try {
        const int value = std::stoi(text);
        if (value < 1 || value > 65535) return false;
        port = static_cast<unsigned short>(value);
        return true;
    } catch (...) {
        return false;
    }
}

void receive_loop(SOCKET socket) {
    std::map<std::string, std::uint64_t> last_sequence_by_user;

    while (g_connected.load()) {
        chatproto::Frame frame;
        if (!chatproto::recv_frame(socket, frame)) {
            if (g_connected.exchange(false)) {
                print_system("connection", "Server disconnected or the TCP connection closed.");
            }
            break;
        }

        switch (frame.type) {
        case chatproto::Type::CHAT_DELIVERY: {
            chatproto::Reader reader(frame.payload);
            std::uint64_t sequence = 0;
            std::string timestamp, sender, message;
            if (!reader.get_u64(sequence) || !reader.get_string(timestamp) ||
                !reader.get_string(sender) || !reader.get_string(message) ||
                !reader.at_end()) {
                print_error("Malformed CHAT_DELIVERY frame received.");
                g_connected.store(false);
                break;
            }

            const auto found = last_sequence_by_user.find(sender);
            if (found != last_sequence_by_user.end()) {
                if (sequence > found->second && sequence - found->second > 1) {
                    print_error("Possible application-message gap from " + sender +
                        ": previous sequence=" + std::to_string(found->second) +
                        ", current sequence=" + std::to_string(sequence) +
                        ". Check server logs and packet capture.");
                } else if (sequence <= found->second) {
                    print_error("Duplicate/out-of-order application sequence from " + sender +
                        ": " + std::to_string(sequence));
                }
                if (sequence > found->second) found->second = sequence;
            } else {
                // The first observed sequence can be greater than 1 if this client
                // joined after the sender had already sent messages.
                last_sequence_by_user.emplace(sender, sequence);
            }

            print_chat(timestamp, sender, sequence, message);
            break;
        }
        case chatproto::Type::SYSTEM: {
            chatproto::Reader reader(frame.payload);
            std::string timestamp, message;
            if (!reader.get_string(timestamp) || !reader.get_string(message) ||
                !reader.at_end()) {
                print_error("Malformed SYSTEM frame received.");
                break;
            }
            print_system(timestamp, message);
            break;
        }
        case chatproto::Type::USER_LIST: {
            chatproto::Reader reader(frame.payload);
            std::uint16_t count = 0;
            if (!reader.get_u16(count)) {
                print_error("Malformed USER_LIST frame received.");
                break;
            }
            std::string output = "Online users (" + std::to_string(count) + "): ";
            for (std::uint16_t i = 0; i < count; ++i) {
                std::string name;
                if (!reader.get_string(name)) {
                    output = "Malformed USER_LIST frame received.";
                    break;
                }
                if (i != 0) output += ", ";
                output += name;
            }
            if (!reader.at_end()) output = "Malformed USER_LIST frame received.";
            print_plain(kGreen + output + kReset);
            break;
        }
        case chatproto::Type::ERROR_MESSAGE: {
            chatproto::Reader reader(frame.payload);
            std::string message;
            if (!reader.get_string(message) || !reader.at_end()) {
                message = "Malformed ERROR frame received.";
            }
            print_error(message);
            break;
        }
        case chatproto::Type::GOODBYE:
            print_system("server", "Server ended the connection.");
            g_connected.store(false);
            break;
        default:
            print_error("Unexpected message type received from server.");
            break;
        }
    }
}

bool send_chat(SOCKET socket, std::uint64_t& sequence,
               const std::string& message) {
    if (message.empty() || message.size() > chatproto::kMaxMessageBytes ||
        contains_control_characters(message)) {
        print_error("Message must be 1-4096 UTF-8 bytes and contain no control characters.");
        return false;
    }

    chatproto::Writer writer;
    writer.put_u64(++sequence);
    writer.put_string(message);
    const chatproto::Frame frame{chatproto::Type::CHAT, std::move(writer.data)};
    if (!chatproto::send_frame(socket, frame)) {
        g_connected.store(false);
        print_error("Failed to send message. The connection may have closed.");
        return false;
    }
    return true;
}

void print_help() {
    print_plain(
        std::string(kCyan) + "Commands" + kReset + "\n"
        "  /help       Show this help\n"
        "  /list       Request the current online-user list from the server\n"
        "  /burst N    Send N numbered test messages (1 <= N <= 500)\n"
        "  /quit       Leave the chat\n"
        "Any other line is sent as a chat message. Messages support Chinese and English.");
}

}  // namespace

int main(int argc, char* argv[]) {
    enable_console_utf8();

    std::string host = "127.0.0.1";
    unsigned short port = kDefaultPort;
    if (argc >= 2) host = argv[1];
    if (argc >= 3 && !parse_port(argv[2], port)) {
        std::cerr << "Usage: client.exe [server_ip_or_host] [port: 1-65535] [username]\n";
        return 2;
    }

    std::string username;
    if (argc >= 4) {
        username = argv[3];
    } else {
        std::cout << "Enter your username (1-48 UTF-8 bytes): " << std::flush;
        std::getline(std::cin, username);
    }
    username = trim_ascii_spaces(username);
    if (username.empty() || username.size() > chatproto::kMaxUsernameBytes ||
        contains_control_characters(username)) {
        std::cerr << "Username must be 1-48 UTF-8 bytes and contain no control characters.\n";
        return 2;
    }

    WSADATA wsa_data{};
    const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (startup_result != 0) {
        std::cerr << "WSAStartup failed: " << startup_result << "\n";
        return 1;
    }

    SOCKET socket = connect_to_server(host, port);
    if (socket == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    chatproto::Writer hello_writer;
    hello_writer.put_string(username);
    const chatproto::Frame hello{chatproto::Type::HELLO, std::move(hello_writer.data)};
    if (!chatproto::send_frame(socket, hello)) {
        print_error("Could not send HELLO.");
        ::closesocket(socket);
        WSACleanup();
        return 1;
    }

    chatproto::Frame response;
    if (!chatproto::recv_frame(socket, response)) {
        print_error("Server closed the connection during login.");
        ::closesocket(socket);
        WSACleanup();
        return 1;
    }

    if (response.type == chatproto::Type::ERROR_MESSAGE) {
        chatproto::Reader reader(response.payload);
        std::string message;
        if (!reader.get_string(message) || !reader.at_end()) {
            message = "Login failed because the server returned a malformed error.";
        }
        print_error(message);
        ::closesocket(socket);
        WSACleanup();
        return 1;
    }

    if (response.type != chatproto::Type::WELCOME) {
        print_error("Expected WELCOME from the server.");
        ::closesocket(socket);
        WSACleanup();
        return 1;
    }

    chatproto::Reader welcome_reader(response.payload);
    std::string welcome_time, welcome_message;
    if (!welcome_reader.get_string(welcome_time) ||
        !welcome_reader.get_string(welcome_message) ||
        !welcome_reader.at_end()) {
        print_error("Malformed WELCOME frame.");
        ::closesocket(socket);
        WSACleanup();
        return 1;
    }

    g_username = username;
    g_connected.store(true);

    std::cout << kCyan
              << "============================================================\n"
              << "                       NEXUS CHAT                           \n"
              << "============================================================\n"
              << kReset
              << kGreen << "[" << welcome_time << "] " << welcome_message << kReset << "\n"
              << kGray << "Server: " << host << ":" << port
              << " | User: " << username << kReset << "\n"
              << kGray << "Type /help for commands. Use /quit to exit.\n"
              << kReset << "------------------------------------------------------------\n";

    std::thread receiver(receive_loop, socket);
    std::uint64_t outgoing_sequence = 0;

    while (g_connected.load()) {
        std::cout << kCyan << "you> " << kReset << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            chatproto::Writer goodbye_writer;
            goodbye_writer.put_string("stdin closed");
            chatproto::send_frame(
                socket, {chatproto::Type::GOODBYE, std::move(goodbye_writer.data)});
            break;
        }

        if (line == "/help") {
            print_help();
            continue;
        }
        if (line == "/list") {
            if (!chatproto::send_frame(socket, {chatproto::Type::LIST_REQUEST, {}})) {
                print_error("Failed to request the online-user list.");
                g_connected.store(false);
            }
            continue;
        }
        if (line == "/quit") {
            chatproto::Writer goodbye_writer;
            goodbye_writer.put_string("user requested disconnect");
            chatproto::send_frame(
                socket, {chatproto::Type::GOODBYE, std::move(goodbye_writer.data)});
            break;
        }
        if (line.rfind("/burst", 0) == 0) {
            std::istringstream parser(line);
            std::string command;
            int count = 0;
            std::string extra;
            if (!(parser >> command >> count) || (parser >> extra) ||
                count < 1 || count > 500) {
                print_error("Usage: /burst N, where N is from 1 to 500.");
                continue;
            }
            for (int i = 1; i <= count && g_connected.load(); ++i) {
                const std::string message = "[TEST] burst " + std::to_string(i) +
                    "/" + std::to_string(count);
                if (!send_chat(socket, outgoing_sequence, message)) break;
            }
            print_plain(kGray + std::string("Burst finished: requested ") +
                        std::to_string(count) + " test messages." + kReset);
            continue;
        }
        if (line.empty()) continue;
        if (!send_chat(socket, outgoing_sequence, line)) {
            if (!g_connected.load()) break;
        }
    }

    g_connected.store(false);
    ::shutdown(socket, SD_BOTH);
    if (receiver.joinable()) receiver.join();
    ::closesocket(socket);
    WSACleanup();
    print_plain("Disconnected. Goodbye.");
    return 0;
}
