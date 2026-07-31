#include "adb_socket_proto.h"

#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace adbproto {

static void ensure_wsa_init() {
    static bool initialized = false;
    if (initialized) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    initialized = true;
    // Намеренно не вызываем WSACleanup — короткоживущий CLI-процесс,
    // ОС подчистит сокеты при завершении.
}

SOCKET connect_localhost(int port) {
    ensure_wsa_init();

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool send_all(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact(SOCKET s, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(s, buf + got, static_cast<int>(len - got), 0);
        if (n <= 0) return false; // закрыто или ошибка
        got += static_cast<size_t>(n);
    }
    return true;
}

bool write_request(SOCKET s, const std::string& service) {
    char len_buf[5];
    std::snprintf(len_buf, sizeof(len_buf), "%04x", static_cast<unsigned>(service.size()));
    std::string frame = std::string(len_buf, 4) + service;
    return send_all(s, frame.data(), frame.size());
}

bool read_protocol_string(SOCKET s, std::string& out) {
    char len_buf[5] = {};
    if (!recv_exact(s, len_buf, 4)) return false;
    unsigned len = 0;
    if (std::sscanf(len_buf, "%4x", &len) != 1) return false;

    out.resize(len);
    if (len == 0) return true;
    return recv_exact(s, out.data(), len);
}

bool read_okay_or_fail(SOCKET s, std::string& fail_message) {
    char status[4];
    if (!recv_exact(s, status, 4)) return false;

    if (std::memcmp(status, "OKAY", 4) == 0) return true;
    if (std::memcmp(status, "FAIL", 4) == 0) {
        read_protocol_string(s, fail_message);
        return false;
    }
    fail_message = "protocol fault: unexpected status bytes";
    return false;
}

std::string read_until_close(SOCKET s) {
    std::string out;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

bool read_shell_packet(SOCKET s, ShellPacket& out) {
    // Заголовок: 1 байт id + 4 байта длины (native LE — оба конца x86).
    char header[5];
    if (!recv_exact(s, header, 5)) return false;

    out.id = static_cast<uint8_t>(header[0]);

    uint32_t len = 0;
    std::memcpy(&len, header + 1, 4);

    out.data.clear();
    if (len == 0) return true;

    out.data.resize(len);
    return recv_exact(s, out.data.data(), len);
}

} // namespace adbproto
