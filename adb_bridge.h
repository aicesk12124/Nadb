#pragma once
#include <string>
#include <vector>
#include <utility>
#include <optional>

struct AdbResult {
    int exit_code;
    std::string stdout_data;
    std::string stderr_data;
    bool success() const { return exit_code == 0; }
};

// AdbBridge — гибридный "raw" мост к adb server.
//
// Реализовано напрямую по TCP на localhost:5037 (без порождения процесса
// на каждую команду), разговаривая с уже запущенным adb server его
// нативным "smart socket" протоколом:
//   - shell   — shell protocol v2 (нормальные exit-коды, раздельные
//               stdout/stderr), с авто-fallback на v1, если устройство
//               слишком старое и не поддерживает shell_v2
//   - devices, version, root/unroot, reboot, remount, wait-for-device
//   - push / pull — через sync-подпротокол (SEND/RECV)
//
// adb.exe (в ADB/) используется только чтобы один раз поднять adb server,
// если он ещё не запущен (start-server), и как fallback для сервисов,
// которые пока не реализованы напрямую (install, logcat, bugreport,
// forward, connect/disconnect, LIST/директории и т.д. — см. TODO в .cpp).
class AdbBridge {
public:
    explicit AdbBridge(const std::string& adb_path = "");

    // Запустить adb команду и вернуть результат.
    // Пример: run({"shell", "getprop", "ro.build.version.release"})
    //          run({"push", "local.txt", "/sdcard/local.txt"})
    //          run({"pull", "/sdcard/local.txt", "local.txt"})
    AdbResult run(const std::vector<std::string>& args) const;

    // Короткая версия для adb shell команд (shell protocol v2 с fallback на v1)
    AdbResult shell(const std::string& command) const;

    bool is_available() const;
    bool has_device() const;
    std::optional<std::string> get_device_serial() const;

    const std::string& adb_path() const { return adb_path_; }

    // false — adb.exe не найден ни рядом с nadb.exe, ни в PATH.
    bool adb_binary_found() const { return !adb_path_.empty(); }

    // Явно зафиксировать устройство (аналог adb -s <serial>).
    // Нужно, когда подключено больше одного устройства.
    void set_serial(const std::string& serial) { serial_ = serial; resolved_transport_.reset(); }

private:
    std::string adb_path_;
    std::string serial_;
    int server_port_ = 5037;
    mutable std::optional<bool> shell_v2_supported_;
    mutable std::optional<std::string> resolved_transport_;

    // ── Fallback: старый способ через порождение процесса adb.exe ──
    AdbResult execute_process(const std::string& command) const;

    // ── Raw socket bridge ──
    bool ensure_server_running(std::string& err_out) const;
    AdbResult query_host(const std::string& service) const;
    AdbResult device_service(const std::string& service, bool read_until_close) const;

    // Разбор вывода host:devices в пары (serial, state).
    static std::vector<std::pair<std::string, std::string>> parse_device_list(const std::string& raw);

    // Возвращает "host:transport:<serial>" для единственного онлайн-устройства
    // либо для явно заданного serial_. transport-any больше не используется:
    // при двух устройствах он либо падает, либо выбирает не то, что надо.
    std::optional<std::string> resolve_transport_service(std::string& err_out) const;

    // Переключает уже открытый сокет на устройство.
    bool open_transport(SOCKET s, std::string& err_out) const;

    // shell v2: возвращает exit_code == -2 как сигнал "handshake не удался,
    // нужно откатиться на v1" (см. shell() в .cpp)
    bool supports_shell_v2() const;
    AdbResult shell_v2(const std::string& command) const;

    // push/pull через sync-подпротокол
    AdbResult do_push(const std::string& local_path, const std::string& remote_path) const;
    AdbResult do_pull(const std::string& remote_path, const std::string& local_path) const;

    static std::string find_adb();
};
