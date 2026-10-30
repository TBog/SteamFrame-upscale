// steamframe-gui: Dear ImGui (GLFW + OpenGL3) front-end for the passthrough layer.
// Saves ~/.config/steamframe-passthrough/config.ini and sends live commands over the layer's UNIX socket.
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "ipc.hpp"

namespace {

std::string configPath() {
    if (const char* e = std::getenv("SF_CONFIG")) return e;
    const char* h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.config/steamframe-passthrough/config.ini";
}

// Returns the layer's reply line, or "" if the layer is not running.
std::string send(const std::string& cmd) {
    std::string path = sf::socketPath();
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return "";
    std::strcpy(a.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "";
    timeval tv{0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    std::string reply;
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        std::string line = cmd + "\n";
        if (write(fd, line.data(), line.size()) > 0) {
            char buf[256];
            ssize_t n = read(fd, buf, sizeof buf - 1);
            if (n > 0) reply.assign(buf, size_t(n));
        }
    }
    close(fd);
    return reply;
}

struct Preset { const char* name; float enabled, alpha, motion, sharp; };
const Preset kPresets[] = {
    {"Default Clean", 1, 0.15f, 0.08f, 0.8f},
    {"High Motion", 1, 0.40f, 0.04f, 0.6f},
    {"Maximum Denoise", 1, 0.05f, 0.12f, 0.5f},
    {"Raw Bypass", 0, 0.15f, 0.08f, 0.8f},
};

}  // namespace

int main() {
    sf::Config cfg = sf::Config::load(configPath());

    if (!glfwInit()) return 1;
    GLFWwindow* win = glfwCreateWindow(480, 460, "SteamFrame Passthrough", nullptr, nullptr);
    if (!win) return 1;
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::string state = "Inactive";
    float gpuMs = 0;
    double lastPoll = -1;

    auto save = [&] {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(configPath()).parent_path(), ec);
        cfg.save(configPath());
    };
    // Persist, then push live values (the layer also re-reads the file on change).
    auto apply = [&] {
        save();
        char b[64];
        std::snprintf(b, sizeof b, "SET_ALPHA %.4f", cfg.temporal_alpha); send(b);
        std::snprintf(b, sizeof b, "SET_MOTION %.4f", cfg.motion_threshold); send(b);
        std::snprintf(b, sizeof b, "SET_SHARPNESS %.4f", cfg.sharpness_weight); send(b);
        send(cfg.enabled >= 0.5f ? "ENABLE" : "DISABLE");
    };

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        if (now - lastPoll > 0.5) {
            lastPoll = now;
            std::string r = send("STATUS");
            if (r.rfind("OK ", 0) == 0) {
                state = r.find("bypassed") != std::string::npos ? "Bypassed" : "Active";
                size_t p = r.find("gpu_ms=");
                if (p != std::string::npos) gpuMs = std::strtof(r.c_str() + p + 7, nullptr);
            } else {
                state = "Inactive";
                gpuMs = 0;
            }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        bool on = cfg.enabled >= 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.1f, 0.55f, 0.2f, 1) : ImVec4(0.6f, 0.2f, 0.1f, 1));
        if (ImGui::Button(on ? "Filter ON" : "Raw Passthrough", {-1, 70})) {
            cfg.enabled = on ? 0.f : 1.f;
            apply();
        }
        ImGui::PopStyleColor();

        bool changed = false;
        ImGui::Spacing();
        changed |= ImGui::SliderFloat("Temporal Blend Alpha", &cfg.temporal_alpha, 0.f, 1.f, "%.2f");
        changed |= ImGui::SliderFloat("Motion Threshold", &cfg.motion_threshold, 0.f, 0.2f, "%.2f");
        changed |= ImGui::SliderFloat("Sharpness Weight", &cfg.sharpness_weight, 0.f, 1.f, "%.2f");
        if (changed) apply();

        ImGui::Spacing();
        ImGui::Text("Presets");
        for (const Preset& p : kPresets) {
            if (ImGui::Button(p.name)) {
                cfg.enabled = p.enabled;
                cfg.temporal_alpha = p.alpha;
                cfg.motion_threshold = p.motion;
                cfg.sharpness_weight = p.sharp;
                apply();
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Separator();
        ImGui::Text("Layer: %s", state.c_str());
        if (std::getenv("SF_PROFILE")) ImGui::Text("GPU frame time: %.3f ms", gpuMs);
        else ImGui::TextDisabled("Set SF_PROFILE=1 (for the app and this GUI) to show GPU frame time");
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
