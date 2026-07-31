#include "phone_info.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#define NOMINMAX
#include <windows.h>
#include <filesystem>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Console colors
// ─────────────────────────────────────────────

static void set_color(int c) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)c);
}

// Цвета Android: зелёный = 10, белый = 7, cyan = 11, yellow = 14
#define COL_LOGO   10   // зелёный
#define COL_KEY    11   // cyan
#define COL_VAL    7    // белый
#define COL_SEP    14   // жёлтый
#define COL_RESET  7

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

static std::string trim_r(const std::string& s) {
    std::string out = s;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    return out;
}

// Количество видимых колонок для строки с Unicode (braille = 1 символ = 1 колонка)
// Считаем по UTF-8 кодпоинтам, пропуская байты продолжения (10xxxxxx)
static int visible_width(const std::string& s) {
    int w = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++w; // не байт-продолжение UTF-8
    }
    return w;
}

static std::string pad_to(const std::string& s, int width) {
    int vw = visible_width(s);
    int pad = width - vw;
    if (pad > 0) return s + std::string(pad, ' ');
    return s;
}

static std::string get_exe_dir_pi() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
}

// ─────────────────────────────────────────────
// PhoneInfo
// ─────────────────────────────────────────────

PhoneInfo::PhoneInfo(const AdbBridge& adb, const DeviceSdk& sdk)
    : adb_(adb), sdk_(sdk) {}

std::string PhoneInfo::prop(const std::string& key) const {
    auto r = adb_.shell("getprop " + key);
    return trim_r(r.stdout_data);
}

std::vector<std::string> PhoneInfo::load_logo() {
    std::string path = get_exe_dir_pi() + "\\ADB\\logo.txt";
    std::ifstream f(path, std::ios::binary);
    std::vector<std::string> lines;

    if (!f) {
        // Fallback: простой текстовый лого если файл не найден
        lines = {
            "  _   _   _   _  ",
            " / \\ / \\ / \\ / \\ ",
            "| A | N | D | R |",
            " \\_/ \\_/ \\_/ \\_/ ",
            "  _   _   _   _  ",
            " / \\ / \\ / \\ / \\ ",
            "| O | I | D | ! |",
            " \\_/ \\_/ \\_/ \\_/ ",
        };
        return lines;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Убираем \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::pair<std::string, std::string>> PhoneInfo::get_info_lines() const {
    std::vector<std::pair<std::string, std::string>> info;

    // Устройство
    std::string brand  = prop("ro.product.brand");
    std::string model  = prop("ro.product.model");
    if (!brand.empty() && model.find(brand) == std::string::npos)
        info.push_back({"Device",    brand + " " + model});
    else
        info.push_back({"Device",    model});

    info.push_back({"Manufacturer", prop("ro.product.manufacturer")});
    info.push_back({"Android",     prop("ro.build.version.release")});
    info.push_back({"API Level",   prop("ro.build.version.sdk")});
    info.push_back({"Build",       prop("ro.build.id")});

    std::string patch = prop("ro.build.version.security_patch");
    if (!patch.empty())
        info.push_back({"Security Patch", patch});

    info.push_back({"CPU",         prop("ro.product.cpu.abi")});

    std::string serial = prop("ro.serialno");
    if (serial.empty()) serial = prop("ro.boot.serialno");
    if (!serial.empty())
        info.push_back({"Serial", serial});

    // Батарея — быстрый dumpsys
    auto bat = adb_.shell("dumpsys battery | grep -E \"level|status\"");
    if (bat.success()) {
        std::istringstream ss(bat.stdout_data);
        std::string line;
        while (std::getline(ss, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = trim_r(line.substr(0, colon));
            std::string v = trim_r(line.substr(colon + 1));
            // trim leading spaces
            while (!k.empty() && k[0] == ' ') k = k.substr(1);
            while (!v.empty() && v[0] == ' ') v = v.substr(1);
            if (k == "level")  info.push_back({"Battery", v + "%"});
            if (k == "status") {
                // status: 2=charging 3=discharging 4=not charging 5=full
                std::string s;
                if      (v == "2") s = "Charging";
                else if (v == "3") s = "Discharging";
                else if (v == "4") s = "Not charging";
                else if (v == "5") s = "Full";
                else               s = v;
                info.push_back({"Battery Status", s});
            }
        }
    }

    // RAM через /proc/meminfo
    auto mem = adb_.shell("cat /proc/meminfo | grep -E \"MemTotal|MemAvailable\"");
    if (mem.success()) {
        std::istringstream ss(mem.stdout_data);
        std::string line;
        long total_kb = 0, avail_kb = 0;
        while (std::getline(ss, line)) {
            if (line.find("MemTotal") != std::string::npos) {
                std::istringstream ls(line);
                std::string key; long val; std::string unit;
                ls >> key >> val >> unit;
                total_kb = val;
            }
            if (line.find("MemAvailable") != std::string::npos) {
                std::istringstream ls(line);
                std::string key; long val; std::string unit;
                ls >> key >> val >> unit;
                avail_kb = val;
            }
        }
        if (total_kb > 0) {
            long used_mb  = (total_kb - avail_kb) / 1024;
            long total_mb = total_kb / 1024;
            info.push_back({"RAM", std::to_string(used_mb) + "MB / " + std::to_string(total_mb) + "MB"});
        }
    }

    // Разрешение экрана
    auto wm = adb_.shell("wm size");
    if (wm.success()) {
        std::string s = trim_r(wm.stdout_data);
        auto colon = s.find(':');
        if (colon != std::string::npos) {
            std::string res = s.substr(colon + 1);
            while (!res.empty() && res[0] == ' ') res = res.substr(1);
            info.push_back({"Resolution", res});
        }
    }

    return info;
}

void PhoneInfo::print_overview() const {
    auto logo  = load_logo();
    auto info  = get_info_lines();

    // Ширина лого колонки (в видимых символах) + отступ
    const int LOGO_COL_WIDTH = 26;
    const int GAP = 2;

    size_t rows = std::max(logo.size(), info.size());

    // Шапка: имя устройства
    std::string brand = prop("ro.product.brand");
    std::string model = prop("ro.product.model");
    std::string title = brand + " " + model;

    // Отступ равный ширине лого
    std::cout << std::string(LOGO_COL_WIDTH + GAP, ' ');
    set_color(COL_LOGO);
    std::cout << title << "\n";
    set_color(COL_RESET);

    std::cout << std::string(LOGO_COL_WIDTH + GAP, ' ');
    set_color(COL_SEP);
    std::cout << std::string(title.size(), '-') << "\n";
    set_color(COL_RESET);

    for (size_t i = 0; i < rows; ++i) {
        // Лого колонка
        if (i < logo.size()) {
            set_color(COL_LOGO);
            std::cout << pad_to(logo[i], LOGO_COL_WIDTH);
            set_color(COL_RESET);
        } else {
            std::cout << std::string(LOGO_COL_WIDTH, ' ');
        }

        std::cout << std::string(GAP, ' ');

        // Инфо колонка
        if (i < info.size()) {
            set_color(COL_KEY);
            std::cout << info[i].first;
            set_color(COL_SEP);
            std::cout << ": ";
            set_color(COL_VAL);
            std::cout << info[i].second;
            set_color(COL_RESET);
        }

        std::cout << "\n";
    }

    std::cout << "\n";
}
