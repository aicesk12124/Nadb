#include "sdk_cache.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <cctype>
#include <cstdio>
#include <windows.h>

namespace fs = std::filesystem;

// ───────────────────────────────────────────
// Минимальный JSON writer/reader (без зависимостей)
// ───────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (static_cast<unsigned char>(c) < 0x20) {
            // Вывод устройства (dumpsys/getprop) регулярно содержит управляющие
            // символы. Раньше они попадали в файл как есть и делали JSON невалидным.
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            out += buf;
        }
        else out += c;
    }
    return out;
}

static std::string json_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            if      (s[i] == '"')  out += '"';
            else if (s[i] == '\\') out += '\\';
            else if (s[i] == 'n')  out += '\n';
            else if (s[i] == 'r')  out += '\r';
            else if (s[i] == 't')  out += '\t';
            else if (s[i] == 'b')  out += '\b';
            else if (s[i] == 'f')  out += '\f';
            else if (s[i] == 'u' && i + 4 < s.size()) {
                // Поддерживаем только то, что сами записываем: \u00XX.
                const std::string hex = s.substr(i + 1, 4);
                try {
                    const int code = std::stoi(hex, nullptr, 16);
                    if (code >= 0 && code < 0x80) out += static_cast<char>(code);
                    i += 4;
                } catch (...) {
                    out += 'u';
                }
            }
            else out += s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

// Извлечь значение по ключу из плоского JSON объекта
static std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    // Пропускаем пробелы
    ++pos;
    while (pos < json.size() && std::isspace((unsigned char)json[pos])) ++pos;

    if (pos < json.size() && json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                val += '\\'; val += json[++pos];
            } else {
                val += json[pos];
            }
            ++pos;
        }
        return json_unescape(val);
    }
    return "";
}

// Найти индекс '}', закрывающей объект, открытый на позиции open.
// Раньше бралась первая встречная '}', и любой вложенный объект
// (или '}' внутри строкового значения) обрезал запись посередине.
static size_t find_object_end(const std::string& s, size_t open) {
    int depth = 0;
    bool in_string = false;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (in_string) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_s(&tm_buf, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// ───────────────────────────────────────────
// SdkCache
// ───────────────────────────────────────────

static std::string get_exe_dir_cache() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
}

SdkCache::SdkCache(const std::string& cache_dir) {
    if (!cache_dir.empty()) {
        cache_dir_ = cache_dir;
    } else {
        cache_dir_ = get_default_cache_dir();
    }
    fs::create_directories(cache_dir_);
}

std::string SdkCache::get_default_cache_dir() {
    return get_exe_dir_cache() + "\\device_sdks";
}

std::string SdkCache::cache_path(const std::string& serial) const {
    // Sanitize serial для имени файла.
    //
    // Раньше заменялись только ':', '/' и '\\'. Серийный номер приходит с
    // устройства, т.е. контролируется тем, кто его подключает; значение ".."
    // выводило запись и invalidate()-удаление за пределы кэш-директории.
    // Теперь whitelist вместо точечных замен.
    std::string safe;
    safe.reserve(serial.size());
    for (char c : serial) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '.' || c == '_' || c == '-') safe += c;
        else safe += '_';
    }

    // "." и ".." состоят только из разрешённых символов — отсекаем отдельно.
    if (safe.empty() || safe == "." || safe == "..") safe = "unknown_device";
    if (safe.size() > 128) safe.resize(128);

    return cache_dir_ + "\\" + safe + ".json";
}

bool SdkCache::save(const DeviceSdk& sdk) const {
    std::ofstream f(cache_path(sdk.serial));
    if (!f) return false;

    f << "{\n";
    f << "  \"serial\": \""          << json_escape(sdk.serial)          << "\",\n";
    f << "  \"android_version\": \"" << json_escape(sdk.android_version) << "\",\n";
    f << "  \"build_id\": \""        << json_escape(sdk.build_id)        << "\",\n";
    f << "  \"sdk_version\": \""     << json_escape(sdk.sdk_version)     << "\",\n";
    f << "  \"generated_at\": \""    << json_escape(now_iso())           << "\",\n";
    f << "  \"commands\": {\n";

    bool first = true;
    for (const auto& [key, entry] : sdk.commands) {
        if (!first) f << ",\n";
        first = false;
        f << "    \"" << json_escape(key) << "\": {\n";
        f << "      \"nadb_command\": \""  << json_escape(entry.nadb_command)  << "\",\n";
        f << "      \"adb_command\": \""   << json_escape(entry.adb_command)   << "\",\n";
        f << "      \"description\": \""   << json_escape(entry.description)   << "\",\n";
        f << "      \"category\": \""      << json_escape(entry.category)      << "\",\n";
        f << "      \"writable\": \""      << (entry.writable ? "1" : "0")     << "\",\n";
        f << "      \"value_hint\": \""    << json_escape(entry.value_hint)    << "\",\n";
        f << "      \"value_rule\": \""    << json_escape(entry.value_rule)    << "\",\n";
        f << "      \"toggleable\": \""    << (entry.toggleable ? "1" : "0")   << "\",\n";
        f << "      \"toggle_read_command\": \"" << json_escape(entry.toggle_read_command) << "\",\n";
        f << "      \"toggle_on_value\": \""     << json_escape(entry.toggle_on_value)     << "\",\n";
        f << "      \"toggle_off_value\": \""    << json_escape(entry.toggle_off_value)    << "\"\n";
        f << "    }";
    }

    f << "\n  }\n}\n";
    f.flush();

    // Раньше всегда возвращался true — переполненный диск или потеря доступа
    // к файлу тихо давали "успешно сохранённый" пустой/обрезанный кэш.
    return static_cast<bool>(f);
}

std::optional<DeviceSdk> SdkCache::load(const std::string& serial) const {
    std::ifstream f(cache_path(serial));
    if (!f) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto cmd_start = content.find("\"commands\"");

    // Метаданные читаем только из шапки файла. Раньше json_get шёл по всему
    // документу, и запись команды с ключом вроде "build_id" могла подменить
    // метаданные устройства и сломать is_valid() — кэш переставал бы
    // инвалидироваться после обновления прошивки.
    const std::string header = (cmd_start == std::string::npos)
        ? content
        : content.substr(0, cmd_start);

    DeviceSdk sdk;
    sdk.serial          = json_get(header, "serial");
    sdk.android_version = json_get(header, "android_version");
    sdk.build_id        = json_get(header, "build_id");
    sdk.sdk_version     = json_get(header, "sdk_version");
    sdk.generated_at    = json_get(header, "generated_at");

    // Парсим commands: ищем все ключи внутри "commands": { ... }
    if (cmd_start == std::string::npos) return sdk;

    auto block_start = content.find('{', cmd_start + 10);
    if (block_start == std::string::npos) return sdk;

    const size_t block_end = find_object_end(content, block_start);
    const size_t limit = (block_end == std::string::npos) ? content.size() : block_end;

    // Ищем записи вида "nadb.cmd": { ... }
    size_t pos = block_start + 1;
    while (pos < limit) {
        // Ищем ключ
        auto q1 = content.find('"', pos);
        if (q1 == std::string::npos || q1 >= limit) break;
        auto q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= limit) break;

        std::string entry_key = content.substr(q1 + 1, q2 - q1 - 1);
        if (entry_key == "commands") { pos = q2 + 1; continue; }

        // Ищем открывающую скобку объекта
        auto obj_start = content.find('{', q2);
        if (obj_start == std::string::npos || obj_start >= limit) break;

        auto obj_end = find_object_end(content, obj_start);
        if (obj_end == std::string::npos || obj_end > limit) break;

        std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

        SdkEntry entry;
        entry.nadb_command = json_get(obj, "nadb_command");
        entry.adb_command  = json_get(obj, "adb_command");
        entry.description  = json_get(obj, "description");
        entry.category     = json_get(obj, "category");
        entry.writable      = json_get(obj, "writable") == "1";
        entry.value_hint    = json_get(obj, "value_hint");
        entry.value_rule    = json_get(obj, "value_rule");
        entry.toggleable    = json_get(obj, "toggleable") == "1";
        entry.toggle_read_command = json_get(obj, "toggle_read_command");
        entry.toggle_on_value     = json_get(obj, "toggle_on_value");
        entry.toggle_off_value    = json_get(obj, "toggle_off_value");

        if (!entry.nadb_command.empty()) {
            sdk.commands[entry_key] = entry;
        }

        pos = obj_end + 1;
    }

    return sdk;
}

bool SdkCache::is_valid(const std::string& serial,
                         const std::string& sdk_version,
                         const std::string& build_id) const {
    auto cached = load(serial);
    if (!cached) return false;

    // Пустые метаданные с обеих сторон раньше считались совпадением,
    // т.е. битый кэш без build_id выглядел валидным.
    if (sdk_version.empty() || build_id.empty()) return false;

    return cached->sdk_version == sdk_version && cached->build_id == build_id;
}

void SdkCache::invalidate(const std::string& serial) const {
    std::error_code ec;
    fs::remove(cache_path(serial), ec);
}
