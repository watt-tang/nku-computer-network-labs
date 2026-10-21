#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace chatproto {

constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 12;
constexpr std::uint32_t kMaxPayloadSize = 1024U * 1024U;
constexpr std::size_t kMaxUsernameBytes = 48;
constexpr std::size_t kMaxMessageBytes = 4096;

enum class Type : std::uint8_t {
    HELLO = 1,
    WELCOME = 2,
    CHAT = 3,
    CHAT_DELIVERY = 4,
    SYSTEM = 5,
    USER_LIST = 6,
    GOODBYE = 7,
    ERROR_MESSAGE = 8,
    LIST_REQUEST = 9
};

struct Frame {
    Type type{};
    std::vector<std::uint8_t> payload;
};

inline bool is_valid_type(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(Type::HELLO) &&
           value <= static_cast<std::uint8_t>(Type::LIST_REQUEST);
}

class Writer {
public:
    std::vector<std::uint8_t> data;

    void put_u16(std::uint16_t value) {
        data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

    void put_u64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            data.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
        }
    }

    void put_string(const std::string& value) {
        if (value.size() > 65535U) {
            throw std::length_error("Protocol string exceeds 65535 bytes");
        }
        put_u16(static_cast<std::uint16_t>(value.size()));
        data.insert(data.end(), value.begin(), value.end());
    }
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

    bool get_u16(std::uint16_t& value) {
        if (remaining() < 2) return false;
        value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data_[pos_]) << 8) |
            static_cast<std::uint16_t>(data_[pos_ + 1]));
        pos_ += 2;
        return true;
    }

    bool get_u64(std::uint64_t& value) {
        if (remaining() < 8) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | data_[pos_ + static_cast<std::size_t>(i)];
        }
        pos_ += 8;
        return true;
    }

    bool get_string(std::string& value) {
        std::uint16_t length = 0;
        if (!get_u16(length) || remaining() < length) return false;
        if (length == 0) {
            value.clear();
            return true;
        }
        value.assign(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length;
        return true;
    }

    std::size_t remaining() const {
        return data_.size() - pos_;
    }

    bool at_end() const {
        return pos_ == data_.size();
    }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_ = 0;
};

inline bool send_all(SOCKET socket, const std::uint8_t* data, std::size_t length) {
    std::size_t sent_total = 0;
    while (sent_total < length) {
        const auto remaining = length - sent_total;
        const int chunk = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(INT_MAX)));
        const int sent = ::send(
            socket,
            reinterpret_cast<const char*>(data + sent_total),
            chunk,
            0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool recv_exact(SOCKET socket, std::uint8_t* data, std::size_t length) {
    std::size_t received_total = 0;
    while (received_total < length) {
        const auto remaining = length - received_total;
        const int chunk = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(INT_MAX)));
        const int received = ::recv(
            socket,
            reinterpret_cast<char*>(data + received_total),
            chunk,
            0);
        if (received == SOCKET_ERROR || received == 0) return false;
        received_total += static_cast<std::size_t>(received);
    }
    return true;
}

// Frame header (12 bytes):
// magic[4]="NCHT", version[1], type[1], reserved[2]=0, payload length[4] big-endian.
inline bool send_frame(SOCKET socket, const Frame& frame) {
    if (frame.payload.size() > kMaxPayloadSize) return false;

    std::uint8_t header[kHeaderSize] = {
        'N', 'C', 'H', 'T',
        kVersion,
        static_cast<std::uint8_t>(frame.type),
        0, 0,
        0, 0, 0, 0
    };

    const auto length = static_cast<std::uint32_t>(frame.payload.size());
    header[8] = static_cast<std::uint8_t>((length >> 24) & 0xFF);
    header[9] = static_cast<std::uint8_t>((length >> 16) & 0xFF);
    header[10] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
    header[11] = static_cast<std::uint8_t>(length & 0xFF);

    if (!send_all(socket, header, sizeof(header))) return false;
    if (!frame.payload.empty() &&
        !send_all(socket, frame.payload.data(), frame.payload.size())) {
        return false;
    }
    return true;
}

inline bool recv_frame(SOCKET socket, Frame& frame) {
    std::uint8_t header[kHeaderSize]{};
    if (!recv_exact(socket, header, sizeof(header))) return false;

    if (header[0] != 'N' || header[1] != 'C' ||
        header[2] != 'H' || header[3] != 'T') {
        return false;
    }
    if (header[4] != kVersion || header[6] != 0 || header[7] != 0) {
        return false;
    }
    if (!is_valid_type(header[5])) return false;

    const std::uint32_t length =
        (static_cast<std::uint32_t>(header[8]) << 24) |
        (static_cast<std::uint32_t>(header[9]) << 16) |
        (static_cast<std::uint32_t>(header[10]) << 8) |
        static_cast<std::uint32_t>(header[11]);

    if (length > kMaxPayloadSize) return false;

    frame.type = static_cast<Type>(header[5]);
    frame.payload.assign(length, 0);
    if (length > 0 && !recv_exact(socket, frame.payload.data(), length)) {
        return false;
    }
    return true;
}

}  // namespace chatproto
