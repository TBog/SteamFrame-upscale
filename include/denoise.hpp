// Reference implementation of the passthrough denoise pipeline.
// shaders/denoise_cas.comp mirrors this logic one-to-one for the GPU path.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace sf {

struct Config {
    float temporal_alpha = 0.15f;
    float motion_threshold = 0.08f;
    float sharpness_weight = 0.8f;
    float noise_floor = 0.02f;

    bool set(const std::string& k, float v) {
        if (k == "temporal_alpha") temporal_alpha = v;
        else if (k == "motion_threshold") motion_threshold = v;
        else if (k == "sharpness_weight") sharpness_weight = v;
        else if (k == "noise_floor") noise_floor = v;
        else return false;
        return true;
    }
    static Config load(const std::string& path) {
        Config c;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            auto p = line.find('=');
            if (p == std::string::npos || line[0] == ';' || line[0] == '#') continue;
            auto trim = [](std::string s) {
                const char* ws = " \t\r";
                s.erase(0, s.find_first_not_of(ws));
                s.erase(s.find_last_not_of(ws) + 1);
                return s;
            };
            try { c.set(trim(line.substr(0, p)), std::stof(trim(line.substr(p + 1)))); }
            catch (...) {}
        }
        return c;
    }
};

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;
    Image() = default;
    Image(int w_, int h_, float v = 0.f) : w(w_), h(h_), px(size_t(w_) * h_, v) {}
    float at(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return px[size_t(y) * w + x];
    }
};

inline float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Per-eye ping-pong history: allocated once, no allocation on the hot path.
struct EyeState {
    Image history[2];
    int read = 0;
    bool valid = false;
};

class Pipeline {
public:
    explicit Pipeline(Config c = {}) : cfg_(c) {}
    Config& config() { return cfg_; }

    // Single pass: motion-masked temporal blend then contrast-adaptive sharpen.
    // `out` must be pre-sized to match `in`.
    void process(int eye, const Image& in, Image& out) {
        EyeState& s = eyes_[eye];
        if (s.history[0].w != in.w || s.history[0].h != in.h) {
            s.history[0] = Image(in.w, in.h);
            s.history[1] = Image(in.w, in.h);
            s.valid = false;
        }
        const Image& hist = s.history[s.read];
        Image& next = s.history[1 - s.read];
        for (int y = 0; y < in.h; ++y)
            for (int x = 0; x < in.w; ++x) {
                float cur = in.at(x, y);
                float v = cur;
                if (s.valid) {
                    float d = std::fabs(cur - hist.at(x, y));
                    float m = smoothstep(cfg_.motion_threshold, 2.f * cfg_.motion_threshold, d);
                    v = hist.at(x, y) + (cur - hist.at(x, y)) * (cfg_.temporal_alpha + (1.f - cfg_.temporal_alpha) * m);
                }
                next.px[size_t(y) * in.w + x] = v;
            }
        if (out.w != in.w || out.h != in.h) out = Image(in.w, in.h);
        for (int y = 0; y < in.h; ++y)
            for (int x = 0; x < in.w; ++x) out.px[size_t(y) * in.w + x] = cas(next, x, y);
        s.read = 1 - s.read;
        s.valid = true;
    }

    void reset() { for (auto& e : eyes_) e = EyeState(); }

private:
    float cas(const Image& t, int x, int y) const {
        float c = t.at(x, y), n = t.at(x, y - 1), s = t.at(x, y + 1), w = t.at(x - 1, y), e = t.at(x + 1, y);
        float mn = std::min({c, n, s, w, e}), mx = std::max({c, n, s, w, e});
        if (mx <= 1e-5f) return c;
        float amp = std::sqrt(std::clamp(std::min(mn, 1.f - mx) / mx, 0.f, 1.f));
        amp *= smoothstep(cfg_.noise_floor, 2.f * cfg_.noise_floor, mx - mn);
        float peak = -1.f / (8.f - 3.f * std::clamp(cfg_.sharpness_weight, 0.f, 1.f));
        float k = amp * peak;
        return std::clamp((c + k * (n + s + w + e)) / (1.f + 4.f * k), 0.f, 1.f);
    }
    Config cfg_;
    EyeState eyes_[2];
};

}  // namespace sf
