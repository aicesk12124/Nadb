// adb_bridge.cpp — оркестрация: находит adb.exe, поднимает adb server при
// необходимости, и диспетчеризует команды либо на raw socket bridge
// (adb_socket_proto / adb_sync_proto), либо на fallback-процесс.
//
// TODO (осознанно не реализовано в этой итерации):
//   - install (нужен либо push+pm install, либо cmd package install-* стрим)
//   - logcat (отдельный длительный стрим, нужна другая модель работы с AdbResult)
//   - bugreport, forward/reverse, connect/disconnect по TCP
//   - LIST (листинг директорий устройства) и рекурсивный push/pull директорий
//   - переиспользование TCP-соединения между вызовами (сейчас новое
//     соединение на каждый run()/shell() — всё равно на порядки быстрее
//     процесса, но можно оптимизировать далее)
//   - shell v2: pty/interactive режим, resize окна, stdin (нужен только для
//     не-интерактивных команд, что и есть 100% текущих сценариев nadb)
//   - CLI-флаг -s <serial>: транспорт уже умеет адресоваться к серийнику
//     (set_serial), осталось прокинуть флаг из main.cpp

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "adb_bridge.h"
#include "adb_socket_proto.h"
#include "adb_sync_proto.h"

#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>

namespace fs = std::filesystem;

// ───────────────────────────────────────────
// helpers
// ───────────────────────────────────────────

static std::string get_exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
}

// Явный поиск по PATH.
//
// Намеренно не используем ни SearchPath(), ни передачу голого имени в
// CreateProcess: оба способа смотрят в текущий рабочий каталог раньше
// системных, т.е. подложенный в папку adb.exe выигрывает у настоящего.
static std::string find_in_path(const std::string& exe_name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";

    const std::string paths(path_env);
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t sep = paths.find(';', start);
        const std::string dir = (sep == std::string::npos)
            ? paths.substr(start)
            : paths.substr(start, sep - start);

        if (!dir.empty()) {
            std::error_code ec;
            const fs::path candidate = fs::path(dir) / exe_name;
            if (fs::exists(candidate, ec) && !ec) return candidate.string();
        }

        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return "";
}

// Экранирование аргумента по правилам CommandLineToArgvW.
// Раньше аргументы просто склеивались через пробел, и любой путь
// с пробелом разваливался на два аргумента.
static std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;

    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// ───────────────────────────────────────────
// AdbBridge — construction / adb.exe discovery
// ───────────────────────────────────────────

AdbBridge::AdbBridge(const std::string& adb_path) {
    adb_path_ = !adb_path.empty() ? adb_path : find_adb();
}

std::string AdbBridge::find_adb() {
    const std::string exe_dir = get_exe_dir();

    std::string candidate = exe_dir + "\\ADB\\adb.exe";
    if (fs::exists(candidate)) return candidate;

    candidate = exe_dir + "\\adb.exe";
    if (fs::exists(candidate)) return candidate;

    // Раньше здесь было `return "adb";`. Голое имя уходило в CreateProcessA,
    // который ищет бинарник в т.ч. в текущем рабочем каталоге — классический
    // binary planting. Теперь разбираем PATH сами и возвращаем абсолютный путь.
    const std::string from_path = find_in_path("adb.exe");
    if (!from_path.empty()) return from_path;

    return ""; // не найден — честно сообщаем об этом выше по стеку
}

// ───────────────────────────────────────────
// Server bootstrap
// ───────────────────────────────────────────

bool AdbBridge::ensure_server_running(std::string& err_out) const {
    {
        SOCKET probe = adbproto::connect_localhost(server_port_);
        if (probe != INVALID_SOCKET) {
            closesocket(probe);
            return true;
        }
    }

    if (adb_path_.empty()) {
        err_out = "adb.exe не найден (ни в ADB\\, ни рядом с nadb.exe, ни в PATH) — "
                  "некому поднять adb server";
        return false;
    }

    // Сервера нет — поднимаем его один раз через бандлированный adb.exe.
    // Единственное место, где мы всё ещё порождаем процесс на "горячем" пути,
    // и то — только при самом первом запуске / если сервер упал.
    std::string cmd = quote_arg(adb_path_) + " -P " + std::to_string(server_port_) + " start-server";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::string cmd_buf = cmd;
    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        err_out = "не удалось запустить процесс: " + adb_path_;
        return false;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    for (int i = 0; i < 20; ++i) {
        SOCKET probe = adbproto::connect_localhost(server_port_);
        if (probe != INVALID_SOCKET) {
            closesocket(probe);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    err_out = "adb server не поднялся на порту " + std::to_string(server_port_);
    return false;
}

// ───────────────────────────────────────────
// host: queries (без переключения на устройство)
// ───────────────────────────────────────────

AdbResult AdbBridge::query_host(const std::string& service) const {
    AdbResult result;
    result.exit_code = -1;

    std::string boot_err;
    if (!ensure_server_running(boot_err)) {
        result.stderr_data = boot_err;
        return result;
    }

    SOCKET s = adbproto::connect_localhost(server_port_);
    if (s == INVALID_SOCKET) {
        result.stderr_data = "не удалось подключиться к adb server";
        return result;
    }

    if (!adbproto::write_request(s, service)) {
        closesocket(s);
        result.stderr_data = "не удалось отправить запрос";
        return result;
    }

    std::string fail_msg;
    if (!adbproto::read_okay_or_fail(s, fail_msg)) {
        closesocket(s);
        result.stderr_data = fail_msg;
        return result;
    }

    std::string content;
    adbproto::read_protocol_string(s, content); // не все host: сервисы шлют доп. данные — это ок
    closesocket(s);

    result.exit_code = 0;
    result.stdout_data = content;
    return result;
}

// ───────────────────────────────────────────
// Выбор транспорта
// ───────────────────────────────────────────

std::vector<std::pair<std::string, std::string>> AdbBridge::parse_device_list(const std::string& raw) {
    std::vector<std::pair<std::string, std::string>> devices;

    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;

        devices.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    return devices;
}

std::optional<std::string> AdbBridge::resolve_transport_service(std::string& err_out) const {
    if (!serial_.empty()) return "host:transport:" + serial_;
    if (resolved_transport_.has_value()) return *resolved_transport_;

    auto r = query_host("host:devices");
    if (!r.success()) {
        err_out = r.stderr_data.empty() ? "adb server недоступен" : r.stderr_data;
        return std::nullopt;
    }

    std::vector<std::string> online;
    for (const auto& [serial, state] : parse_device_list(r.stdout_data)) {
        if (state == "device") online.push_back(serial);
    }

    if (online.empty()) {
        err_out = "нет подключённого устройства";
        return std::nullopt;
    }

    if (online.size() > 1) {
        // Раньше здесь безусловно шёл host:transport-any, и команды уходили
        // непредсказуемо на одно из устройств, хотя SDK строился по другому.
        err_out = "подключено несколько устройств (";
        for (size_t i = 0; i < online.size(); ++i) {
            if (i > 0) err_out += ", ";
            err_out += online[i];
        }
        err_out += ") — укажите нужное явно";
        return std::nullopt;
    }

    resolved_transport_ = "host:transport:" + online.front();
    return *resolved_transport_;
}

bool AdbBridge::open_transport(SOCKET s, std::string& err_out) const {
    auto service = resolve_transport_service(err_out);
    if (!service) return false;

    std::string fail_msg;
    if (!adbproto::write_request(s, *service) || !adbproto::read_okay_or_fail(s, fail_msg)) {
        err_out = fail_msg.empty() ? "не удалось переключиться на устройство" : fail_msg;
        return false;
    }
    return true;
}

// ───────────────────────────────────────────
// device: локальные сервисы (после host:transport:<serial>)
// ───────────────────────────────────────────

AdbResult AdbBridge::device_service(const std::string& service, bool read_until_close_flag) const {
    AdbResult result;
    result.exit_code = -1;

    std::string boot_err;
    if (!ensure_server_running(boot_err)) {
        result.stderr_data = boot_err;
        return result;
    }

    SOCKET s = adbproto::connect_localhost(server_port_);
    if (s == INVALID_SOCKET) {
        result.stderr_data = "не удалось подключиться к adb server";
        return result;
    }

    std::string transport_err;
    if (!open_transport(s, transport_err)) {
        closesocket(s);
        result.stderr_data = transport_err;
        return result;
    }

    std::string fail_msg;
    if (!adbproto::write_request(s, service) || !adbproto::read_okay_or_fail(s, fail_msg)) {
        closesocket(s);
        result.stderr_data = fail_msg;
        return result;
    }

    if (read_until_close_flag) {
        result.stdout_data = adbproto::read_until_close(s);
    }
    closesocket(s);

    // Раньше здесь безусловно стояло exit_code = 0, поэтому reboot/root/
    // remount всегда выглядели успешными, даже когда adbd отвечал
    // "error: ..." или "adbd cannot run as root ...".
    const std::string& body = result.stdout_data;
    const bool device_reported_error =
        body.compare(0, 7, "error: ") == 0 ||
        body.find("cannot run as root") != std::string::npos ||
        body.find("Permission denied") != std::string::npos ||
        body.find("remount failed") != std::string::npos;

    if (device_reported_error) {
        result.exit_code = -1;
        result.stderr_data = body;
        result.stdout_data.clear();
        return result;
    }

    result.exit_code = 0;
    return result;
}

// ───────────────────────────────────────────
// shell v2 (нормальные exit-коды + раздельные stdout/stderr)
// ───────────────────────────────────────────

bool AdbBridge::supports_shell_v2() const {
    if (shell_v2_supported_.has_value()) return *shell_v2_supported_;

    auto r = query_host("host:features");
    bool supported = r.success() && r.stdout_data.find("shell_v2") != std::string::npos;
    shell_v2_supported_ = supported;
    return supported;
}

AdbResult AdbBridge::shell_v2(const std::string& command) const {
    AdbResult result;
    result.exit_code = -2; // сентинел: handshake не удался -> caller откатится на v1

    std::string boot_err;
    if (!ensure_server_running(boot_err)) return result;

    SOCKET s = adbproto::connect_localhost(server_port_);
    if (s == INVALID_SOCKET) return result;

    std::string transport_err;
    if (!open_transport(s, transport_err)) {
        closesocket(s);
        return result;
    }

    // "shell,v2,raw:" — см. services.h (kShellServiceArgShellProtocol="v2",
    // kShellServiceArgRaw="raw") и ShellServiceString() в client/commandline.cpp
    std::string fail_msg;
    if (!adbproto::write_request(s, "shell,v2,raw:" + command) ||
        !adbproto::read_okay_or_fail(s, fail_msg)) {
        closesocket(s);
        return result;
    }

    // С этого момента протокол v2 гарантированно принят — дальше уже не
    // откатываемся на v1 при ошибках чтения, это реальный обрыв соединения.
    result.exit_code = -1;

    adbproto::ShellPacket pkt;
    while (adbproto::read_shell_packet(s, pkt)) {
        if (pkt.id == adbproto::ShellPacket::kIdStdout) {
            result.stdout_data += pkt.data;
        } else if (pkt.id == adbproto::ShellPacket::kIdStderr) {
            result.stderr_data += pkt.data;
        } else if (pkt.id == adbproto::ShellPacket::kIdExit) {
            result.exit_code = pkt.data.empty() ? -1 : static_cast<uint8_t>(pkt.data[0]);
            break;
        }
        // kIdCloseStdin / kIdWindowSizeChange от устройства не ожидаются, игнорируем.
    }

    closesocket(s);
    return result;
}

AdbResult AdbBridge::shell(const std::string& command) const {
    if (supports_shell_v2()) {
        auto r = shell_v2(command);
        if (r.exit_code != -2) return r; // handshake прошёл — это финальный результат
        // handshake неожиданно не удался несмотря на заявленную поддержку — в v1
    }
    return device_service("shell:" + command, /*read_until_close=*/true);
}

// ───────────────────────────────────────────
// push / pull (sync-подпротокол)
// ───────────────────────────────────────────

namespace {
// Передача большого файла легко превышает общий 30-секундный таймаут.
constexpr int kSyncTimeoutMs = 300000;
} // namespace

AdbResult AdbBridge::do_push(const std::string& local_path, const std::string& remote_path) const {
    AdbResult result;
    result.exit_code = -1;

    std::string boot_err;
    if (!ensure_server_running(boot_err)) {
        result.stderr_data = boot_err;
        return result;
    }

    SOCKET s = adbproto::connect_localhost(server_port_);
    if (s == INVALID_SOCKET) {
        result.stderr_data = "не удалось подключиться к adb server";
        return result;
    }
    adbproto::set_socket_timeouts(s, kSyncTimeoutMs, kSyncTimeoutMs);

    std::string transport_err;
    if (!open_transport(s, transport_err)) {
        closesocket(s);
        result.stderr_data = transport_err;
        return result;
    }

    std::string fail_msg;
    if (!adbproto::write_request(s, "sync:") || !adbproto::read_okay_or_fail(s, fail_msg)) {
        closesocket(s);
        result.stderr_data = fail_msg;
        return result;
    }

    auto sync_res = adbproto::sync_push(s, local_path, remote_path);
    closesocket(s);

    result.exit_code = sync_res.ok ? 0 : -1;
    if (sync_res.ok) result.stdout_data = sync_res.message;
    else result.stderr_data = sync_res.message;
    return result;
}

AdbResult AdbBridge::do_pull(const std::string& remote_path, const std::string& local_path) const {
    AdbResult result;
    result.exit_code = -1;

    std::string boot_err;
    if (!ensure_server_running(boot_err)) {
        result.stderr_data = boot_err;
        return result;
    }

    SOCKET s = adbproto::connect_localhost(server_port_);
    if (s == INVALID_SOCKET) {
        result.stderr_data = "не удалось подключиться к adb server";
        return result;
    }
    adbproto::set_socket_timeouts(s, kSyncTimeoutMs, kSyncTimeoutMs);

    std::string transport_err;
    if (!open_transport(s, transport_err)) {
        closesocket(s);
        result.stderr_data = transport_err;
        return result;
    }

    std::string fail_msg;
    if (!adbproto::write_request(s, "sync:") || !adbproto::read_okay_or_fail(s, fail_msg)) {
        closesocket(s);
        result.stderr_data = fail_msg;
        return result;
    }

    auto sync_res = adbproto::sync_pull(s, remote_path, local_path);
    closesocket(s);

    result.exit_code = sync_res.ok ? 0 : -1;
    if (sync_res.ok) result.stdout_data = sync_res.message;
    else result.stderr_data = sync_res.message;
    return result;
}

// ───────────────────────────────────────────
// run() — диспетчеризация
// ───────────────────────────────────────────

AdbResult AdbBridge::run(const std::vector<std::string>& args) const {
    if (args.empty()) {
        AdbResult r;
        r.exit_code = -1;
        r.stderr_data = "пустая команда";
        return r;
    }

    const std::string& cmd = args[0];

    if (cmd == "version") {
        return query_host("host:version");
    }
    if (cmd == "devices") {
        bool long_form = args.size() > 1 && args[1] == "-l";
        return query_host(long_form ? "host:devices-l" : "host:devices");
    }
    if (cmd == "shell") {
        std::string joined;
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) joined += " ";
            joined += args[i];
        }
        return shell(joined);
    }
    if (cmd == "reboot") {
        std::string target = args.size() > 1 ? args[1] : "";
        return device_service("reboot:" + target, /*read_until_close=*/false);
    }
    if (cmd == "root")   return device_service("root:",   true);
    if (cmd == "unroot") return device_service("unroot:", true);
    if (cmd == "remount") return device_service("remount:", true);
    if (cmd == "wait-for-device") return device_service("wait-for-device", false);

    if (cmd == "push" && args.size() >= 3) {
        return do_push(args[1], args[2]);
    }
    if (cmd == "pull" && args.size() >= 3) {
        return do_pull(args[1], args[2]);
    }

    // Всё остальное (install/logcat/bugreport/forward/connect/... — см. TODO
    // в начале файла) — честный fallback на процесс.
    if (adb_path_.empty()) {
        AdbResult r;
        r.exit_code = -1;
        r.stderr_data = "команда \"" + cmd + "\" требует adb.exe, но он не найден";
        return r;
    }

    // Каждый аргумент экранируется отдельно — раньше была наивная
    // конкатенация через пробел без кавычек.
    std::string full_cmd = quote_arg(adb_path_);
    for (const auto& a : args) full_cmd += " " + quote_arg(a);
    return execute_process(full_cmd);
}

// ───────────────────────────────────────────
// device queries на базе host:devices
// ───────────────────────────────────────────

bool AdbBridge::is_available() const {
    return query_host("host:version").success();
}

bool AdbBridge::has_device() const {
    auto r = query_host("host:devices");
    if (!r.success()) return false;

    // Раньше искалась подстрока "device" где угодно в строке, т.е. серийник
    // вроде "my-device-01" в состоянии unauthorized считался готовым к работе.
    for (const auto& [serial, state] : parse_device_list(r.stdout_data)) {
        (void)serial;
        if (state == "device") return true;
    }
    return false;
}

std::optional<std::string> AdbBridge::get_device_serial() const {
    auto r = query_host("host:devices");
    if (!r.success()) return std::nullopt;

    for (const auto& [serial, state] : parse_device_list(r.stdout_data)) {
        if (state == "device") return serial;
    }
    return std::nullopt;
}

// ───────────────────────────────────────────
// Fallback: старый способ через CreateProcess
// (используется только для сервисов без raw-реализации)
// ───────────────────────────────────────────

AdbResult AdbBridge::execute_process(const std::string& command) const {
    AdbResult result;
    result.exit_code = -1;

    HANDLE h_stdout_r, h_stdout_w;
    HANDLE h_stderr_r, h_stderr_w;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&h_stdout_r, &h_stdout_w, &sa, 0)) return result;
    SetHandleInformation(h_stdout_r, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&h_stderr_r, &h_stderr_w, &sa, 0)) {
        CloseHandle(h_stdout_r); CloseHandle(h_stdout_w);
        return result;
    }
    SetHandleInformation(h_stderr_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(STARTUPINFOA);
    si.hStdOutput = h_stdout_w;
    si.hStdError  = h_stderr_w;
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_buf = command;

    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(h_stdout_w);
    CloseHandle(h_stderr_w);

    if (!ok) {
        CloseHandle(h_stdout_r); CloseHandle(h_stderr_r);
        result.stderr_data = "не удалось запустить процесс";
        return result;
    }

    // Раньше stdout читался до EOF, и только потом stderr. Если дочерний
    // процесс успевал забить буфер пайпа stderr (типично ~4–64 КБ), он
    // блокировался на записи, а мы ждали его stdout — классический deadlock.
    // Например, на `adb logcat` через fallback это вешало nadb намертво.
    auto drain = [](HANDLE h, std::string& sink) {
        char buf[4096];
        DWORD bytes_read = 0;
        while (ReadFile(h, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
            sink.append(buf, bytes_read);
        }
    };

    std::string out, err;
    std::thread err_reader(drain, h_stderr_r, std::ref(err));
    drain(h_stdout_r, out);
    err_reader.join();

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(h_stdout_r);
    CloseHandle(h_stderr_r);

    result.exit_code   = static_cast<int>(exit_code);
    result.stdout_data = out;
    result.stderr_data = err;
    return result;
}
