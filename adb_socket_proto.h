#pragma once
// adb_socket_proto — низкоуровневый слой: TCP до adb server + "smart socket"
// текстовый протокол (SERVICES.TXT) + shell protocol v2 (packet framing).
//
// Ничего не знает про AdbBridge/AdbResult — чистая обвязка над протоколом,
// чтобы её можно было переиспользовать и в sync-протоколе, и в будущих
// сервисах (forward, track-devices и т.п.).

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include <cstdint>
#include <string>

namespace adbproto {

// ── Лимиты и таймауты ──
// adb оперирует буфером MAX_PAYLOAD = 1 МБ, поэтому любой пакет больше этого
// считаем протокольным сбоем, а не поводом выделять память по запросу
// удалённой стороны.
constexpr uint32_t kMaxShellPayload = 1u * 1024u * 1024u;

// Значения по умолчанию для SO_RCVTIMEO / SO_SNDTIMEO, мс.
constexpr int kDefaultRecvTimeoutMs = 30000;
constexpr int kDefaultSendTimeoutMs = 30000;

// ── Сокеты ──
SOCKET connect_localhost(int port);
bool send_all(SOCKET s, const char* data, size_t len);
bool recv_exact(SOCKET s, char* buf, size_t len);

// Выставляет таймауты на приём/отправку. 0 = блокироваться бесконечно.
// connect_localhost вызывает это сам со значениями по умолчанию; отдельный
// вызов нужен для долгих операций (push/pull больших файлов).
bool set_socket_timeouts(SOCKET s, int recv_ms, int send_ms);

// ── Smart-socket текстовый протокол (host <-> adb server) ──
// Запрос: 4 hex-символа длины (lowercase) + ASCII-строка сервиса.
bool write_request(SOCKET s, const std::string& service);

// Ответ вида "4 hex длины + N байт содержимого" (host:devices, host:version, ...)
bool read_protocol_string(SOCKET s, std::string& out);

// Первые 4 байта ответа: "OKAY" или "FAIL" (+ read_protocol_string на error message).
bool read_okay_or_fail(SOCKET s, std::string& fail_message);

// Читает всё до закрытия соединения сервером (для shell: v1, reboot: и т.п.)
std::string read_until_close(SOCKET s);

// ── Shell protocol v2 (packet framing: 1 байт id + 4 байта LE длины + payload) ──
struct ShellPacket {
    enum Id : uint8_t {
        kIdStdin = 0,
        kIdStdout = 1,
        kIdStderr = 2,
        kIdExit = 3,
        kIdCloseStdin = 4,
        kIdWindowSizeChange = 5,
        kIdInvalid = 255,
    };
    uint8_t id = kIdInvalid;
    std::string data;
};

// Читает один пакет. false = соединение закрыто/ошибка/длина вне лимита.
bool read_shell_packet(SOCKET s, ShellPacket& out);

} // namespace adbproto
