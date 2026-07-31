#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <filesystem>

#include "adb_bridge.h"
#include "sdk_builder.h"
#include "sdk_cache.h"
#include "command_resolver.h"
#include "fuzzy.h"
#include "phone_info.h"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Console colors (Windows)
// ─────────────────────────────────────────────

static void set_color(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)color);
}

static void print_green (const std::string& s) { set_color(10); std::cout << s; set_color(7); }
static void print_red   (const std::string& s) { set_color(12); std::cout << s; set_color(7); }
static void print_yellow(const std::string& s) { set_color(14); std::cout << s; set_color(7); }
static void print_cyan  (const std::string& s) { set_color(11); std::cout << s; set_color(7); }

// ─────────────────────────────────────────────
// Help
// ─────────────────────────────────────────────

static void print_help(const DeviceSdk* sdk = nullptr) {
    print_cyan("NADB - New Android Debug Bridge\n");
    print_cyan("================================\n");
    std::cout << "Usage: nadb <command> [options]\n\n";

    std::cout << "Options:\n";
    print_yellow("  --rebuild-sdk       ");
    std::cout << "Force rebuild device SDK cache\n";
    print_yellow("  --no-cache          ");
    std::cout << "Ignore cache, rebuild SDK for this run only\n";
    print_yellow("  --list              ");
    std::cout << "List all available commands for connected device\n";
    print_yellow("  --list <category>   ");
    std::cout << "List commands for category (android, phone, battery, network, display, system, apps)\n";
    print_yellow("  --fullhelp          ");
    std::cout << "Full list of all commands for this device\n";
    print_yellow("  --showraw           ");
    std::cout << "Full list with raw adb commands shown\n";
    print_yellow("  --showraw <category>");
    std::cout << "Same but filtered by category\n";
    print_yellow("  --raw <command>     ");
    std::cout << "Show raw adb command without executing\n";
    print_yellow("  --help              ");
    std::cout << "Show this help\n\n";

    std::cout << "Command format:\n";
    print_green("  nadb get.<category>.<property>\n");
    print_green("  nadb set.<category>.<property> <value>\n");
    print_green("  nadb toggle.<category>.<property>\n\n");

    std::cout << "Examples (read):\n";
    print_green("  nadb get.phone                 ");
    std::cout << "- Device overview (like neofetch)\n";
    print_green("  nadb get.android.version       ");
    std::cout << "- Android version\n";
    print_green("  nadb get.android.build         ");
    std::cout << "- Build ID\n";
    print_green("  nadb get.phone.model           ");
    std::cout << "- Device model\n";
    print_green("  nadb get.phone.serial          ");
    std::cout << "- Serial number\n";
    print_green("  nadb get.phone.imei            ");
    std::cout << "- IMEI (if available)\n";
    print_green("  nadb get.battery.level         ");
    std::cout << "- Battery level\n";
    print_green("  nadb get.battery.all           ");
    std::cout << "- All battery info\n";
    print_green("  nadb get.network.wifi          ");
    std::cout << "- WiFi info\n";
    print_green("  nadb get.display.brightness    ");
    std::cout << "- Screen brightness\n";
    print_green("  nadb get.system.memory         ");
    std::cout << "- Memory info\n\n";

    std::cout << "Examples (write):\n";
    print_green("  nadb set.battery.percent 50    ");
    std::cout << "- Force battery level to 50%\n";
    print_green("  nadb set.battery.reset         ");
    std::cout << "- Reset battery to real values\n";
    print_green("  nadb set.display.brightness 128");
    std::cout << " - Set screen brightness\n";
    print_green("  nadb set.network.wifi disable  ");
    std::cout << "- Turn WiFi off\n";
    print_green("  nadb set.network.airplane 1    ");
    std::cout << "- Enable airplane mode\n";
    print_green("  nadb set.system.volume.media 7 ");
    std::cout << "- Set media volume\n";
    print_green("  nadb set.system.dnd 1           ");
    std::cout << " - Enable Do Not Disturb (priority)\n\n";

    std::cout << "Examples (toggle, no value needed):\n";
    print_green("  nadb toggle.network.wifi       ");
    std::cout << "- Flip WiFi on/off\n";
    print_green("  nadb toggle.network.airplane   ");
    std::cout << "- Flip airplane mode on/off\n";
    print_green("  nadb toggle.display.rotation   ");
    std::cout << "- Flip auto-rotation on/off\n\n";

    if (sdk && !sdk->commands.empty()) {
        std::cout << "Connected device: ";
        print_green(sdk->serial + "\n");
        std::cout << "Android " << sdk->android_version
                  << " | Build " << sdk->build_id
                  << " | API " << sdk->sdk_version << "\n";
        std::cout << "Available commands: " << sdk->commands.size() << "\n";
        std::cout << "(use --list to see all)\n";
    }
}

static void print_list(const DeviceSdk& sdk, const std::string& category = "", bool full = false) {
    std::string filter = category;

    // Группируем по категориям
    std::map<std::string, std::vector<const SdkEntry*>> by_cat;
    for (const auto& [key, entry] : sdk.commands) {
        if (filter.empty() || entry.category == filter) {
            by_cat[entry.category].push_back(&entry);
        }
    }

    if (by_cat.empty()) {
        print_red("No commands found");
        if (!filter.empty()) std::cout << " for category '" + filter + "'";
        std::cout << "\n";
        return;
    }

    // Шапка
    print_cyan("NADB Command Reference");
    if (!filter.empty()) { std::cout << " ["; print_yellow(filter); std::cout << "]"; }
    std::cout << "\n";

    std::string device_title = sdk.serial + "  |  Android " + sdk.android_version
                             + "  |  API " + sdk.sdk_version
                             + "  |  Build " + sdk.build_id;
    print_cyan("Device: ");
    std::cout << device_title << "\n";
    print_cyan(std::string(60, '-') + "\n");

    int total = 0;
    for (const auto& [cat, entries] : by_cat) {
        // Заголовок категории
        std::cout << "\n";
        print_yellow("  [" + cat + "]");
        std::cout << "  (" << entries.size() << " commands)\n";

        for (const auto* e : entries) {
            // Строка 1: [R]/[W] тег + nadb команда + описание
            std::cout << "    ";
            if (e->toggleable)     { set_color(13); std::cout << "[T] "; set_color(7); }
            else if (e->writable)  { set_color(12); std::cout << "[W] "; set_color(7); }
            else                    { set_color(11); std::cout << "[R] "; set_color(7); }
            print_green(e->nadb_command);
            int pad = 34 - static_cast<int>(e->nadb_command.size());
            if (pad > 0) std::cout << std::string(pad, ' ');
            std::cout << e->description;
            if (e->writable && !e->value_hint.empty()) {
                print_yellow("  (value: " + e->value_hint + ")");
            }
            std::cout << "\n";

            // Строка 2 (только в --fullhelp): raw adb команда
            if (full) {
                std::cout << "    ";
                print_yellow("  → adb ");
                std::cout << e->adb_command << "\n";
            }

            ++total;
        }
    }

    std::cout << "\n";
    print_cyan(std::string(60, '-') + "\n");
    std::cout << "Total: ";
    print_green(std::to_string(total));
    std::cout << " commands available for this device.\n";

    if (!full) {
        std::cout << "Tip: use ";
        print_yellow("nadb --fullhelp");
        std::cout << " to see raw adb commands.\n";
    }
}

// ─────────────────────────────────────────────
// SDK loading with progress
// ─────────────────────────────────────────────

static DeviceSdk load_or_build_sdk(const AdbBridge& adb,
                                    SdkCache& cache,
                                    const std::string& serial,
                                    bool force_rebuild) {
    // Быстрая проверка актуальности кэша
    if (!force_rebuild) {
        auto sdk_ver = [&]() {
            auto r = adb.shell("getprop ro.build.version.sdk");
            std::string s = r.stdout_data;
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            return s;
        }();
        auto build_id = [&]() {
            auto r = adb.shell("getprop ro.build.id");
            std::string s = r.stdout_data;
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            return s;
        }();

        if (cache.is_valid(serial, sdk_ver, build_id)) {
            auto cached = cache.load(serial);
            if (cached) {
                print_green("[cache] ");
                std::cout << "Using cached SDK for " << serial << "\n\n";
                return *cached;
            }
        }
    }

    // Строим SDK заново
    print_yellow("[sdk] ");
    std::cout << "Building SDK for device " << serial << "...\n";

    // Путь к builtin_map.json рядом с exe
    char exe_buf[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
    std::string builtin_path = fs::path(exe_buf).parent_path().string() + "\\builtin_map.json";

    SdkBuilder builder(adb);
    builder.load_builtin_map(builtin_path);

    DeviceSdk sdk = builder.build(serial, [](const std::string& step, int cur, int total) {
        std::cout << "  [" << cur << "/" << total << "] " << step << "\n";
    });

    cache.save(sdk);
    print_green("[sdk] ");
    std::cout << "SDK built: " << sdk.commands.size() << " commands available.\n\n";

    return sdk;
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Включаем UTF-8 вывод в Windows консоли
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    // Без аргументов — help
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "-help") {
        print_help();
        return 0;
    }

    // Флаги
    bool force_rebuild = false;
    bool no_cache      = false;
    bool raw_mode      = false;
    bool full_help     = false;
    bool show_raw      = false;

    std::vector<std::string> cmd_args;
    for (const auto& a : args) {
        if      (a == "--rebuild-sdk") force_rebuild = true;
        else if (a == "--no-cache")    no_cache = true;
        else if (a == "--raw")         raw_mode = true;
        else if (a == "--fullhelp")    full_help = true;
        else if (a == "--showraw")     show_raw = true;
        else                           cmd_args.push_back(a);
    }

    // Инициализация ADB
    AdbBridge adb;

    if (!adb.is_available()) {
        print_red("Error: ");
        std::cout << "adb not found. Make sure ADB folder is next to nadb.exe\n";
        return 1;
    }

    if (!adb.has_device()) {
        print_red("Error: ");
        std::cout << "No device connected. Connect a device and try again.\n";
        return 1;
    }

    auto serial_opt = adb.get_device_serial();
    if (!serial_opt) {
        print_red("Error: ");
        std::cout << "Could not get device serial.\n";
        return 1;
    }
    std::string serial = *serial_opt;

    SdkCache cache;
    DeviceSdk sdk = load_or_build_sdk(adb, cache, serial, force_rebuild || no_cache);

    // --fullhelp — полный список всех команд
    if (full_help) {
        print_list(sdk, "", false);
        return 0;
    }

    // --showraw — полный список с raw adb командами
    if (show_raw) {
        std::string cat = cmd_args.size() > 0 ? cmd_args[0] : "";
        print_list(sdk, cat, true);
        return 0;
    }

    // --list
    if (!cmd_args.empty() && cmd_args[0] == "--list") {
        std::string cat = cmd_args.size() > 1 ? cmd_args[1] : "";
        print_list(sdk, cat, false);
        return 0;
    }

    // --help после загрузки SDK (покажем статистику)
    if (cmd_args.empty() || cmd_args[0] == "--help") {
        print_help(&sdk);
        return 0;
    }

    // Выполняем команду
    std::string user_cmd = cmd_args[0];
    // Для writable (set.*) команд — значение передаётся вторым аргументом:
    // nadb set.battery.percent 50
    std::string value = cmd_args.size() > 1 ? cmd_args[1] : "";
    CommandResolver resolver(adb, sdk);

    if (raw_mode) {
        auto entry = resolver.lookup(user_cmd);
        if (!entry) {
            print_red("Command not found: ");
            std::cout << user_cmd << "\n";
            return 1;
        }
        print_yellow("Raw adb command: ");
        std::cout << "adb " << entry->adb_command << "\n";
        return 0;
    }

    // get.phone — neofetch overview
    if (user_cmd == "get.phone") {
        PhoneInfo info(adb, sdk);
        info.print_overview();
        return 0;
    }

    auto result = resolver.resolve(user_cmd, value);

    if (!result.found) {
        print_red("Invalid command, or it was not found in your device's SDK.\n");
        std::cout << "Type `nadb --help` to get a list of commands.\n";

        // Fuzzy suggest
        std::string closest = FuzzyMatcher::find_closest(user_cmd, sdk.commands);
        if (!closest.empty()) {
            std::cout << "Perhaps you meant: ";
            print_green("\"" + closest + "\"");
            std::cout << "\n";
        }
        return 1;
    }

    if (result.needs_value) {
        print_red("This command requires a value.\n");
        std::cout << "Usage: ";
        print_green("nadb " + user_cmd + " <value>");
        std::cout << "\n";
        if (!result.value_hint.empty()) {
            std::cout << "Expected: " << result.value_hint << "\n";
        }
        return 1;
    }

    if (result.invalid_value) {
        print_red("Invalid value: ");
        std::cout << result.error << "\n";
        if (!result.value_hint.empty()) {
            std::cout << "Expected: " << result.value_hint << "\n";
        }
        return 1;
    }

    if (!result.output.empty()) {
        std::cout << result.output << "\n";
    }
    if (!result.error.empty()) {
        print_red(result.error + "\n");
    }

    if (result.executed && result.output.empty() && result.error.empty()) {
        // Многие set.* команды ничего не выводят при успехе
        print_green("OK");
        std::cout << " — applied: " << user_cmd
                   << (value.empty() ? "" : (" " + value)) << "\n";
    }

    return 0;
}
