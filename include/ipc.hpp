// Command protocol shared by the layer (server) and steamframe-gui (client).
// One line per command over a UNIX stream socket; the layer replies with one line.
#pragma once
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <string>

#include "denoise.hpp"

namespace sf {

inline std::string socketPath() {
    const char* e = std::getenv("SF_SOCKET");
    return e && *e ? e : "/tmp/steamframe_passthrough.sock";
}

// Applies one command to cfg/bypass and returns the reply line. Caller serialises access to cfg.
// Commands: TOGGLE, ENABLE, DISABLE, SET_ALPHA <v>, SET_MOTION <v>, SET_SHARPNESS <v>, STATUS.
inline std::string handleCommand(const std::string& line, Config& cfg, std::atomic<bool>& bypass, double gpuMs = 0.0) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    for (auto& ch : cmd) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    auto setf = [&](float lo, float hi, float Config::*m) {
        float v;
        if (!(in >> v) || !(v == v)) return false;
        cfg.*m = v < lo ? lo : (v > hi ? hi : v);
        return true;
    };
    bool ok = true;
    if (cmd == "TOGGLE") bypass = !bypass.load();
    else if (cmd == "ENABLE") bypass = false;
    else if (cmd == "DISABLE") bypass = true;
    else if (cmd == "SET_ALPHA") ok = setf(0.f, 1.f, &Config::temporal_alpha);
    else if (cmd == "SET_MOTION") ok = setf(0.f, 0.2f, &Config::motion_threshold);
    else if (cmd == "SET_SHARPNESS") ok = setf(0.f, 1.f, &Config::sharpness_weight);
    else if (cmd != "STATUS") return "ERR unknown command";
    if (!ok) return "ERR bad value";
    char buf[160];
    std::snprintf(buf, sizeof buf, "OK %s alpha=%.3f motion=%.3f sharpness=%.3f gpu_ms=%.3f",
                  bypass.load() ? "bypassed" : "active", cfg.temporal_alpha, cfg.motion_threshold,
                  cfg.sharpness_weight, gpuMs);
    return buf;
}

}  // namespace sf
