#include "sdk_builder.h"
#include <sstream>
#include <algorithm>
#include <fstream>
#include <iostream>

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

// Простой парсер builtin_map.json
// Формат: { "nadb.cmd": { "adb": "shell ...", "desc": "...", "category": "..." }, ... }
static std::string simple_json_get(const std::string& obj, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = obj.find(search);
    if (pos == std::string::npos) return "";
    pos = obj.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < obj.size() && std::isspace((unsigned char)obj[pos])) ++pos;
    if (obj[pos] != '"') return "";
    ++pos;
    std::string val;
    while (pos < obj.size() && obj[pos] != '"') {
        if (obj[pos] == '\\' && pos + 1 < obj.size()) {
            char next = obj[++pos];
            if      (next == 'n')  val += '\n';
            else if (next == 't')  val += '\t';
            else                   val += next;
        } else {
            val += obj[pos];
        }
        ++pos;
    }
    return val;
}

// ─────────────────────────────────────────────
// SdkBuilder
// ─────────────────────────────────────────────

SdkBuilder::SdkBuilder(const AdbBridge& adb) : adb_(adb) {}

void SdkBuilder::add_entry(DeviceSdk& sdk,
                            const std::string& nadb_cmd,
                            const std::string& adb_cmd,
                            const std::string& description,
                            const std::string& category,
                            bool writable,
                            const std::string& value_hint,
                            const std::string& value_rule) {
    SdkEntry e;
    e.nadb_command = nadb_cmd;
    e.adb_command  = adb_cmd;
    e.description  = description;
    e.category     = category;
    e.writable     = writable;
    e.value_hint   = value_hint;
    e.value_rule   = value_rule;
    sdk.commands[nadb_cmd] = e;
}

void SdkBuilder::add_toggle_entry(DeviceSdk& sdk,
                                   const std::string& nadb_cmd,
                                   const std::string& set_adb_cmd,
                                   const std::string& read_command_key,
                                   const std::string& on_value,
                                   const std::string& off_value,
                                   const std::string& description,
                                   const std::string& category) {
    SdkEntry e;
    e.nadb_command         = nadb_cmd;
    e.adb_command          = set_adb_cmd;
    e.description          = description;
    e.category             = category;
    e.writable             = true;
    e.toggleable           = true;
    e.toggle_read_command  = read_command_key;
    e.toggle_on_value      = on_value;
    e.toggle_off_value     = off_value;
    sdk.commands[nadb_cmd] = e;
}

void SdkBuilder::load_builtin_map(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Парсим все записи вида "nadb.cmd": { ... }
    size_t pos = 0;
    while (pos < content.size()) {
        auto q1 = content.find('"', pos);
        if (q1 == std::string::npos) break;
        auto q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) break;

        std::string key = content.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;

        // Ожидаем "key": { ... }
        auto obj_start = content.find('{', pos);
        auto obj_end   = content.find('}', obj_start);
        if (obj_start == std::string::npos || obj_end == std::string::npos) break;

        // Проверяем что это не корневой объект
        if (key.find('.') == std::string::npos) {
            pos = obj_start + 1;
            continue;
        }

        std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

        SdkEntry entry;
        entry.nadb_command = key;
        entry.adb_command  = simple_json_get(obj, "adb");
        entry.description  = simple_json_get(obj, "desc");
        entry.category     = simple_json_get(obj, "category");

        if (!entry.adb_command.empty()) {
            builtin_commands_[key] = entry;
        }

        pos = obj_end + 1;
    }
}

// ─────────────────────────────────────────────
// Meta info
// ─────────────────────────────────────────────

void SdkBuilder::fill_device_meta(DeviceSdk& sdk) const {
    auto get = [&](const std::string& prop) -> std::string {
        auto r = adb_.shell("getprop " + prop);
        return trim(r.stdout_data);
    };

    sdk.android_version = get("ro.build.version.release");
    sdk.sdk_version     = get("ro.build.version.sdk");
    sdk.build_id        = get("ro.build.id");
}

// ─────────────────────────────────────────────
// Parsers
// ─────────────────────────────────────────────

void SdkBuilder::parse_getprop(DeviceSdk& sdk) const {
    // Получаем все свойства устройства
    auto r = adb_.shell("getprop");
    if (!r.success()) return;

    std::istringstream ss(r.stdout_data);
    std::string line;

    while (std::getline(ss, line)) {
        // Формат: [ro.build.version.release]: [15]
        if (line.empty() || line[0] != '[') continue;
        auto close1 = line.find(']');
        if (close1 == std::string::npos) continue;
        std::string prop = line.substr(1, close1 - 1);

        auto open2 = line.find('[', close1);
        auto close2 = line.find(']', open2);
        if (open2 == std::string::npos || close2 == std::string::npos) continue;
        std::string value = line.substr(open2 + 1, close2 - open2 - 1);

        if (value.empty()) continue;

        // Маппинг prop → nadb команда
        // ro.build.version.release → get.android.version
        // ro.product.model         → get.phone.model
        // ro.product.manufacturer  → get.phone.manufacturer
        // ro.serialno              → get.phone.serial
        // etc.

        std::string nadb_cmd, desc, category;

        if      (prop == "ro.build.version.release")    { nadb_cmd = "get.android.version";       desc = "Android version";        category = "android"; }
        else if (prop == "ro.build.version.sdk")        { nadb_cmd = "get.android.sdk";            desc = "Android API level";      category = "android"; }
        else if (prop == "ro.build.id")                 { nadb_cmd = "get.android.build";          desc = "Build ID";               category = "android"; }
        else if (prop == "ro.build.version.security_patch") { nadb_cmd = "get.android.patch";     desc = "Security patch level";   category = "android"; }
        else if (prop == "ro.product.model")            { nadb_cmd = "get.phone.model";            desc = "Device model";           category = "phone"; }
        else if (prop == "ro.product.manufacturer")     { nadb_cmd = "get.phone.manufacturer";     desc = "Device manufacturer";    category = "phone"; }
        else if (prop == "ro.product.brand")            { nadb_cmd = "get.phone.brand";            desc = "Device brand";           category = "phone"; }
        else if (prop == "ro.product.name")             { nadb_cmd = "get.phone.name";             desc = "Device name";            category = "phone"; }
        else if (prop == "ro.serialno" || prop == "ro.boot.serialno") { nadb_cmd = "get.phone.serial"; desc = "Device serial number"; category = "phone"; }
        else if (prop == "ro.product.cpu.abi")          { nadb_cmd = "get.phone.cpu";              desc = "CPU architecture";       category = "phone"; }
        else if (prop == "ro.hardware")                 { nadb_cmd = "get.phone.hardware";         desc = "Hardware platform";      category = "phone"; }
        else if (prop == "ro.build.fingerprint")        { nadb_cmd = "get.android.fingerprint";    desc = "Build fingerprint";      category = "android"; }
        else if (prop == "persist.sys.language" || prop == "ro.product.locale") { nadb_cmd = "get.phone.locale"; desc = "Device locale"; category = "phone"; }
        else if (prop == "ro.board.platform")           { nadb_cmd = "get.phone.platform";         desc = "Board platform";         category = "phone"; }
        else if (prop == "gsm.version.baseband")        { nadb_cmd = "get.phone.baseband";         desc = "Baseband version";       category = "phone"; }
        else if (prop == "ro.build.version.incremental"){ nadb_cmd = "get.android.incremental";    desc = "Incremental build";      category = "android"; }
        else { continue; } // Пропускаем неизвестные props

        add_entry(sdk, nadb_cmd,
                  "shell getprop " + prop,
                  desc, category);
    }
}

void SdkBuilder::parse_dumpsys(DeviceSdk& sdk) const {
    // battery info
    auto r = adb_.shell("dumpsys battery");
    if (r.success()) {
        add_entry(sdk, "get.battery.level",
                  "shell dumpsys battery | grep level",
                  "Battery level (%)", "battery");
        add_entry(sdk, "get.battery.status",
                  "shell dumpsys battery | grep status",
                  "Battery status", "battery");
        add_entry(sdk, "get.battery.health",
                  "shell dumpsys battery | grep health",
                  "Battery health", "battery");
        add_entry(sdk, "get.battery.temp",
                  "shell dumpsys battery | grep temperature",
                  "Battery temperature", "battery");
        add_entry(sdk, "get.battery.voltage",
                  "shell dumpsys battery | grep voltage",
                  "Battery voltage", "battery");
        add_entry(sdk, "get.battery.all",
                  "shell dumpsys battery",
                  "All battery info", "battery");
    }

    // wifi info
    auto w = adb_.shell("dumpsys wifi | grep -E \"(mWifiInfo|SSID|BSSID|ipAddress)\" | head -5");
    if (w.success() && !trim(w.stdout_data).empty()) {
        add_entry(sdk, "get.network.wifi",
                  "shell dumpsys wifi | grep mWifiInfo",
                  "WiFi connection info", "network");
    }

    // display info
    auto d = adb_.shell("dumpsys display | grep -E \"(mBaseDisplayInfo|density)\" | head -3");
    if (d.success() && !trim(d.stdout_data).empty()) {
        add_entry(sdk, "get.display.info",
                  "shell dumpsys display | grep mBaseDisplayInfo",
                  "Display information", "display");
        add_entry(sdk, "get.display.density",
                  "shell dumpsys display | grep density",
                  "Display density", "display");
    }

    // memory
    add_entry(sdk, "get.system.memory",
              "shell dumpsys meminfo | head -20",
              "System memory info", "system");

    // CPU usage
    add_entry(sdk, "get.system.cpu",
              "shell dumpsys cpuinfo | head -10",
              "CPU usage info", "system");
}

void SdkBuilder::parse_pm(DeviceSdk& sdk) const {
    // Проверяем что pm работает
    auto r = adb_.shell("pm list packages --help 2>&1 | head -3");
    (void)r; // pm всегда есть, просто добавляем стандартные команды

    add_entry(sdk, "get.apps.all",
              "shell pm list packages",
              "List all installed packages", "apps");
    add_entry(sdk, "get.apps.system",
              "shell pm list packages -s",
              "List system packages", "apps");
    add_entry(sdk, "get.apps.user",
              "shell pm list packages -3",
              "List user-installed packages", "apps");
    add_entry(sdk, "get.apps.disabled",
              "shell pm list packages -d",
              "List disabled packages", "apps");
}

void SdkBuilder::parse_settings(DeviceSdk& sdk) const {
    add_entry(sdk, "get.display.brightness",
              "shell settings get system screen_brightness",
              "Screen brightness (0-255)", "display");
    add_entry(sdk, "get.display.timeout",
              "shell settings get system screen_off_timeout",
              "Screen off timeout (ms)", "display");
    add_entry(sdk, "get.display.rotation",
              "shell settings get system accelerometer_rotation",
              "Auto-rotation enabled", "display");
    add_entry(sdk, "get.network.airplane",
              "shell settings get global airplane_mode_on",
              "Airplane mode status", "network");
    add_entry(sdk, "get.network.bluetooth",
              "shell settings get global bluetooth_on",
              "Bluetooth status", "network");
    add_entry(sdk, "get.network.data",
              "shell settings get global mobile_data",
              "Mobile data status", "network");
    add_entry(sdk, "get.network.wifi.enabled",
              "shell settings get global wifi_on",
              "WiFi enabled status", "network");
    add_entry(sdk, "get.system.volume.ring",
              "shell settings get system volume_ring",
              "Ring volume", "system");
    add_entry(sdk, "get.system.volume.media",
              "shell settings get system volume_music",
              "Media volume", "system");
    add_entry(sdk, "get.system.location",
              "shell settings get secure location_mode",
              "Location mode", "system");
}

void SdkBuilder::parse_set_commands(DeviceSdk& sdk) const {
    // ── Battery ──────────────────────────────────────────
    add_entry(sdk, "set.battery.percent",
              "shell dumpsys battery set level %value%",
              "Force battery level (%)", "battery",
              true, "0-100", "range:0:100");
    add_entry(sdk, "set.battery.status",
              "shell dumpsys battery set status %value%",
              "Force battery status (1=unknown 2=charging 3=discharging 4=not-charging 5=full)",
              "battery", true, "1 | 2 | 3 | 4 | 5", "range:1:5");
    add_entry(sdk, "set.battery.health",
              "shell dumpsys battery set health %value%",
              "Force battery health (1=unknown 2=good 3=overheat 4=dead 5=overvoltage 6=failure 7=cold)",
              "battery", true, "1-7", "range:1:7");
    add_entry(sdk, "set.battery.ac",
              "shell dumpsys battery set ac %value%",
              "Force AC power connected state", "battery", true, "0 | 1", "range:0:1");
    add_entry(sdk, "set.battery.usb",
              "shell dumpsys battery set usb %value%",
              "Force USB power connected state", "battery", true, "0 | 1", "range:0:1");
    add_entry(sdk, "set.battery.reset",
              "shell dumpsys battery reset",
              "Reset battery values back to real hardware readings", "battery");

    // ── Display ──────────────────────────────────────────
    add_entry(sdk, "set.display.brightness",
              "shell settings put system screen_brightness %value%",
              "Set screen brightness", "display", true, "0-255", "range:0:255");
    add_entry(sdk, "set.display.timeout",
              "shell settings put system screen_off_timeout %value%",
              "Set screen off timeout (ms)", "display", true, "e.g. 30000", "");
    add_entry(sdk, "set.display.rotation",
              "shell settings put system accelerometer_rotation %value%",
              "Enable/disable auto-rotation", "display", true, "0 | 1", "range:0:1");

    // ── Network ──────────────────────────────────────────
    add_entry(sdk, "set.network.wifi",
              "shell svc wifi %value%",
              "Enable/disable WiFi", "network", true, "enable | disable", "enum:enable,disable");
    add_entry(sdk, "set.network.data",
              "shell svc data %value%",
              "Enable/disable mobile data", "network", true, "enable | disable", "enum:enable,disable");
    add_entry(sdk, "set.network.bluetooth",
              "shell svc bluetooth %value%",
              "Enable/disable Bluetooth", "network", true, "enable | disable", "enum:enable,disable");
    // ВАЖНО: "settings put ... airplane_mode_on" хочет число (0/1), а
    // "am broadcast --ez state" хочет строго "true"/"false" (Boolean.parseBoolean).
    // Поэтому нельзя просто подставить один и тот же %value% в оба места —
    // конвертируем его в самой shell-команде через if/else.
    add_entry(sdk, "set.network.airplane",
              "shell V=%value%; settings put global airplane_mode_on $V; "
              "if [ \"$V\" = \"1\" ]; then am broadcast -a android.intent.action.AIRPLANE_MODE --ez state true; "
              "else am broadcast -a android.intent.action.AIRPLANE_MODE --ez state false; fi",
              "Enable/disable airplane mode", "network", true, "0 | 1", "range:0:1");

    // ── System ───────────────────────────────────────────
    add_entry(sdk, "set.system.volume.ring",
              "shell settings put system volume_ring %value%",
              "Set ring volume", "system", true, "0-7", "range:0:7");
    add_entry(sdk, "set.system.volume.media",
              "shell settings put system volume_music %value%",
              "Set media volume", "system", true, "0-15", "range:0:15");
    add_entry(sdk, "set.system.location",
              "shell settings put secure location_mode %value%",
              "Set location mode (0=off 3=high accuracy)", "system", true, "0-3", "range:0:3");
    add_entry(sdk, "set.system.keep_screen_on",
              "shell settings put global stay_on_while_plugged_in %value%",
              "Keep screen on while charging (bitmask, e.g. 0 or 7)", "system", true,
              "bitmask, e.g. 0 or 7", "");
    add_entry(sdk, "set.system.dnd",
              "shell settings put global zen_mode %value%",
              "Set Do Not Disturb mode (0=off 1=priority 2=total-silence 3=alarms-only)",
              "system", true, "0-3", "range:0:3");
    add_entry(sdk, "set.phone.locale",
              "shell settings put system system_locales %value%",
              "Set device locale (e.g. en-US, ru-RU)", "phone", true, "e.g. en-US, ru-RU", "");
}

void SdkBuilder::parse_toggle_commands(DeviceSdk& sdk) const {
    // toggle.* читает текущее состояние через соответствующую get.* команду
    // и переключает его на противоположное — значение передавать не нужно:
    //   nadb toggle.network.wifi
    add_toggle_entry(sdk, "toggle.network.wifi",
                      "shell svc wifi %value%",
                      "get.network.wifi.enabled", "enable", "disable",
                      "Toggle WiFi on/off", "network");
    add_toggle_entry(sdk, "toggle.network.bluetooth",
                      "shell svc bluetooth %value%",
                      "get.network.bluetooth", "enable", "disable",
                      "Toggle Bluetooth on/off", "network");
    add_toggle_entry(sdk, "toggle.network.data",
                      "shell svc data %value%",
                      "get.network.data", "enable", "disable",
                      "Toggle mobile data on/off", "network");
    add_toggle_entry(sdk, "toggle.network.airplane",
                      "shell V=%value%; settings put global airplane_mode_on $V; "
                      "if [ \"$V\" = \"1\" ]; then am broadcast -a android.intent.action.AIRPLANE_MODE --ez state true; "
                      "else am broadcast -a android.intent.action.AIRPLANE_MODE --ez state false; fi",
                      "get.network.airplane", "1", "0",
                      "Toggle airplane mode on/off", "network");
    add_toggle_entry(sdk, "toggle.display.rotation",
                      "shell settings put system accelerometer_rotation %value%",
                      "get.display.rotation", "1", "0",
                      "Toggle auto-rotation on/off", "display");
}

// ─────────────────────────────────────────────
// Main build
// ─────────────────────────────────────────────

DeviceSdk SdkBuilder::build(const std::string& serial, ProgressCallback on_progress) const {
    DeviceSdk sdk;
    sdk.serial = serial;

    const int total_steps = 8;
    int step = 0;

    auto progress = [&](const std::string& msg) {
        ++step;
        if (on_progress) on_progress(msg, step, total_steps);
    };

    progress("Reading device information...");
    fill_device_meta(sdk);

    progress("Parsing device properties (getprop)...");
    parse_getprop(sdk);

    progress("Loading builtin command map...");
    for (const auto& [key, entry] : builtin_commands_) {
        if (sdk.commands.find(key) == sdk.commands.end()) {
            sdk.commands[key] = entry;
        }
    }

    progress("Scanning package manager (pm)...");
    parse_pm(sdk);

    progress("Scanning system services (dumpsys)...");
    parse_dumpsys(sdk);

    progress("Scanning settings...");
    parse_settings(sdk);

    progress("Building set.* write commands...");
    parse_set_commands(sdk);

    progress("Building toggle.* commands...");
    parse_toggle_commands(sdk);

    return sdk;
}
