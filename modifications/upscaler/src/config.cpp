// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "log.h"

namespace xhyn {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseBool(const std::string& v, bool fallback) {
    const std::string s = lower(v);
    if (s == "1" || s == "true" || s == "on" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "off" || s == "no") return false;
    return fallback;
}

float parseFloat(const std::string& v, float fallback, float lo, float hi) {
    char* end = nullptr;
    const float f = strtof(v.c_str(), &end);
    if (end == v.c_str() || *end) return fallback;
    return std::min(hi, std::max(lo, f));
}

uint32_t parseUint(const std::string& v, uint32_t fallback, uint32_t hi) {
    char* end = nullptr;
    const unsigned long n = strtoul(v.c_str(), &end, 10);
    if (end == v.c_str() || *end) return fallback;
    return static_cast<uint32_t>(std::min<unsigned long>(n, hi));
}

void apply(Config& c, const std::string& key, const std::string& value) {
    if (key == "mode") {
        const std::string m = lower(value);
        if (m == "off") c.mode = Mode::Off;
        else if (m == "probe") c.mode = Mode::Probe;
        else if (m == "sgsr1" || m == "sgsr" || m == "on") c.mode = Mode::Sgsr1;
        else XLOGW("unknown mode '%s' (use off, probe or sgsr1)", value.c_str());
    } else if (key == "bcn") {
        const std::string m = lower(value);
        if (m == "auto") c.bcn = BcnPolicy::Auto;
        else if (m == "inner" || m == "layer") c.bcn = BcnPolicy::Inner;
        else if (m == "native" || m == "bypass") c.bcn = BcnPolicy::Native;
        else XLOGW("unknown bcn policy '%s' (use auto, inner or native)", value.c_str());
    } else if (key == "scale") c.scale = parseFloat(value, c.scale, 1.0f, 4.0f);
    else if (key == "out_width") c.outWidth = parseUint(value, 0, 8192);
    else if (key == "out_height") c.outHeight = parseUint(value, 0, 8192);
    else if (key == "edge_sharpness") c.edgeSharpness = parseFloat(value, c.edgeSharpness, 1.0f, 2.0f);
    else if (key == "edge_threshold") c.edgeThreshold = parseFloat(value, c.edgeThreshold, 1.0f, 64.0f);
    else if (key == "edge_direction") c.edgeDirection = parseBool(value, c.edgeDirection);
    else if (key == "gpu_timing") c.gpuTiming = parseBool(value, c.gpuTiming);
    else if (key == "log_interval") c.logInterval = parseUint(value, c.logInterval, 1000000);
    else if (key == "dump_frame") c.dumpFrame = parseUint(value, 0, 1000000);
    else if (key == "dump_dir") c.dumpDir = value;
    else XLOGW("unknown setting '%s'", key.c_str());
}

bool readFile(const std::string& path, Config& c) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        apply(c, lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1)));
    }
    c.source = path;
    return true;
}

std::string packageName() {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string name;
    std::getline(in, name, '\0');
    // Only a plausible Android application ID is used to build file paths.
    if (name.find('/') != std::string::npos || name.find('.') == std::string::npos) return "";
    return name;
}

}  // namespace

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Off: return "off";
        case Mode::Probe: return "probe";
        case Mode::Sgsr1: return "sgsr1";
    }
    return "?";
}

const char* bcnName(BcnPolicy policy) {
    switch (policy) {
        case BcnPolicy::Auto: return "auto";
        case BcnPolicy::Inner: return "inner";
        case BcnPolicy::Native: return "native";
    }
    return "?";
}

Config loadConfig() {
    Config c;
    std::vector<std::string> candidates;
    if (const char* env = getenv("XHYN_UPSCALE_CONF")) candidates.push_back(env);
    const std::string pkg = packageName();
    if (!pkg.empty()) {
        // adb can write to the external app directory; the internal one is app-only.
        for (const char* root : {"/storage/emulated/0/Android/data/", "/sdcard/Android/data/"})
            candidates.push_back(root + pkg + "/files/xhyn-upscale.conf");
        for (const char* root : {"/data/user/0/", "/data/data/"})
            candidates.push_back(root + pkg + "/files/xhyn-upscale.conf");
    } else {
        candidates.push_back("xhyn-upscale.conf");
    }
    // The launcher asks for all-files access, so a file in the shared storage root also works.
    candidates.push_back("/storage/emulated/0/xhyn-upscale.conf");
    for (const auto& path : candidates)
        if (readFile(path, c)) break;
    if (c.source.empty()) c.source = "defaults (no xhyn-upscale.conf found)";

    // Environment overrides are mainly for host tests.
    static const char* const keys[] = {"mode", "bcn", "scale", "out_width", "out_height", "edge_sharpness",
                                       "edge_threshold", "edge_direction", "gpu_timing",
                                       "log_interval", "dump_frame", "dump_dir"};
    for (const char* key : keys) {
        std::string name = "XHYN_UPSCALE_";
        for (const char* p = key; *p; ++p) name += static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        if (const char* v = getenv(name.c_str())) apply(c, key, v);
    }
    if (c.dumpDir.empty()) {
        c.dumpDir = pkg.empty() ? "." : "/storage/emulated/0/Android/data/" + pkg + "/files";
    }
    return c;
}

}  // namespace xhyn
