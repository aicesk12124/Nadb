#include "command_resolver.h"
#include <sstream>
#include <algorithm>
#include <vector>

CommandResolver::CommandResolver(const AdbBridge& adb, const DeviceSdk& sdk)
    : adb_(adb), sdk_(sdk) {}

std::optional<SdkEntry> CommandResolver::lookup(const std::string& nadb_command) const {
    auto it = sdk_.commands.find(nadb_command);
    if (it == sdk_.commands.end()) return std::nullopt;
    return it->second;
}

std::string CommandResolver::clean_output(const std::string& raw) {
    std::string out = raw;
    // Убираем \r
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    // Trim trailing newlines
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

// Подставить пользовательское значение вместо плейсхолдера %value%
static std::string substitute_value(const std::string& cmd, const std::string& value) {
    static const std::string placeholder = "%value%";
    std::string out = cmd;
    size_t pos = 0;
    while ((pos = out.find(placeholder, pos)) != std::string::npos) {
        out.replace(pos, placeholder.size(), value);
        pos += value.size();
    }
    return out;
}

// Полный trim (пробелы, \r, \n, \t) с обеих сторон — нужен, чтобы надёжно
// сравнивать вывод "settings get ..." (0/1/null) при переключении toggle.*
static std::string full_trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    auto end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Валидация значения по машиночитаемому правилу ("range:MIN:MAX" / "enum:a,b,c")
static bool validate_value(const std::string& rule, const std::string& value, std::string& err_out) {
    if (rule.empty()) return true;

    auto colon = rule.find(':');
    if (colon == std::string::npos) return true; // неизвестный формат правила — пропускаем
    std::string kind = rule.substr(0, colon);
    std::string rest = rule.substr(colon + 1);

    if (kind == "range") {
        auto c2 = rest.find(':');
        if (c2 == std::string::npos) return true;
        try {
            int lo = std::stoi(rest.substr(0, c2));
            int hi = std::stoi(rest.substr(c2 + 1));
            size_t consumed = 0;
            int v = std::stoi(value, &consumed);
            if (consumed != value.size()) {
                err_out = "Value must be a whole number";
                return false;
            }
            if (v < lo || v > hi) {
                err_out = "Value must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi);
                return false;
            }
        } catch (...) {
            err_out = "Value must be a whole number";
            return false;
        }
        return true;
    }

    if (kind == "enum") {
        std::istringstream ss(rest);
        std::string opt;
        while (std::getline(ss, opt, ',')) {
            if (opt == value) return true;
        }
        err_out = "Value must be one of: " + rest;
        return false;
    }

    return true;
}

void CommandResolver::execute_adb_command(const std::string& final_cmd, std::string& out, std::string& err) const {
    // Для команд с pipe/&&/;  выполняем через shell целиком, а не по токенам,
    // чтобы спецсимволы не терялись/не рвались на отдельные аргументы
    bool needs_raw_shell = final_cmd.find('|') != std::string::npos ||
                           final_cmd.find('&') != std::string::npos ||
                           final_cmd.find(';') != std::string::npos;

    if (needs_raw_shell) {
        // Всё что после "shell " передаём целиком
        std::string shell_cmd = final_cmd;
        if (shell_cmd.substr(0, 6) == "shell ") {
            shell_cmd = shell_cmd.substr(6);
        }
        auto r = adb_.shell(shell_cmd);
        out = clean_output(r.stdout_data);
        err = clean_output(r.stderr_data);
    } else {
        std::vector<std::string> args;
        std::istringstream ss(final_cmd);
        std::string token;
        while (ss >> token) args.push_back(token);

        auto r = adb_.run(args);
        out = clean_output(r.stdout_data);
        err = clean_output(r.stderr_data);
    }
}

ResolveResult CommandResolver::resolve_toggle(const SdkEntry& entry, const std::string& nadb_command) const {
    ResolveResult result;
    result.found = true;

    auto read_it = sdk_.commands.find(entry.toggle_read_command);
    if (read_it == sdk_.commands.end()) {
        result.error = "Internal error: toggle read command \"" + entry.toggle_read_command + "\" is missing from this device's SDK.";
        return result;
    }

    // Читаем текущее состояние через связанную get.* команду
    std::string read_out, read_err;
    execute_adb_command(read_it->second.adb_command, read_out, read_err);
    std::string state = to_lower_copy(full_trim(read_out));

    bool currently_on = (state == "1" || state == "true" || state.find("enabled") != std::string::npos);

    std::string new_value = currently_on ? entry.toggle_off_value : entry.toggle_on_value;
    std::string final_cmd = substitute_value(entry.adb_command, new_value);
    result.adb_command = final_cmd;

    std::string out, err;
    execute_adb_command(final_cmd, out, err);
    result.error = err;
    result.executed = true;

    std::string prev_label = currently_on ? "ON" : "OFF";
    std::string new_label  = currently_on ? "OFF" : "ON";
    result.output = nadb_command + ": " + prev_label + " -> " + new_label;
    if (!out.empty()) {
        result.output += "\n" + out;
    }

    return result;
}

ResolveResult CommandResolver::resolve(const std::string& nadb_command, const std::string& value) const {
    ResolveResult result;

    auto entry = lookup(nadb_command);
    if (!entry) {
        result.found = false;
        return result;
    }

    result.found = true;

    if (entry->toggleable) {
        return resolve_toggle(*entry, nadb_command);
    }

    bool needs_value = entry->adb_command.find("%value%") != std::string::npos;

    if (needs_value && value.empty()) {
        // Команда найдена, но writable-команде требуется значение — не выполняем её
        result.needs_value = true;
        result.value_hint  = entry->value_hint;
        result.adb_command = entry->adb_command;
        return result;
    }

    if (needs_value) {
        std::string err_msg;
        if (!validate_value(entry->value_rule, value, err_msg)) {
            result.invalid_value = true;
            result.error         = err_msg;
            result.value_hint    = entry->value_hint;
            return result;
        }
    }

    std::string final_cmd = needs_value ? substitute_value(entry->adb_command, value)
                                         : entry->adb_command;
    result.adb_command = final_cmd;

    execute_adb_command(final_cmd, result.output, result.error);
    result.executed = true;
    return result;
}
