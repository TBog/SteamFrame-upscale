// OpenXR API layer: runs temporal denoise + CAS (shaders/denoise_cas.comp) in-place on
// R8_UNORM Vulkan swapchain images just before xrReleaseSwapchainImage.
// Each swapchain (or array layer) is treated as one eye and owns a ping-pong R16F history.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "denoise.hpp"
#include "denoise_spv.h"
#include "loader_interfaces.h"

#define SF_EXPORT extern "C" __attribute__((visibility("default")))
static const char* kLayerName = "XR_APILAYER_STEAMFRAME_passthrough_denoise";

namespace {

constexpr uint32_t kMaxImages = 8;
constexpr uint32_t kMaxLayers = 2;
constexpr uint32_t kRingSize = kMaxImages * 4;
constexpr VkFormat kSwapFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kHistFormat = VK_FORMAT_R16_SFLOAT;

struct PushConstants {
    float temporalAlpha, motionThreshold, sharpnessWeight, noiseFloor;
    int32_t pass, hasHistory;
};

struct SessionCtx {
    XrSession session = XR_NULL_HANDLE;
    VkInstance instance{};
    VkPhysicalDevice phys{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    float tsPeriod = 1.f;
    bool tsValid = false;
    VkCommandPool pool{};
    VkShaderModule shader{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipeLayout{};
    VkPipeline pipeline{};
    bool ok = false;
};

struct LayerRes {  // per array layer ("eye")
    VkImage hist[2]{};
    VkDeviceMemory mem[2]{};
    VkImageView histView[2]{};
    bool histInit = false;
    int read = 0;
    bool valid = false;
};

struct ImageRes {  // per swapchain image
    VkImage image{};
    VkImageView view[kMaxLayers]{};
    VkDescriptorSet sets[kMaxLayers][2][2]{};  // [layer][parity][pass]
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkQueryPool qp{};
    bool submitted = false;
    bool layoutInit = false;  // true once we've barriered this image out of its runtime-provided initial layout
};

struct SwapchainCtx {
    XrSwapchain handle{};
    std::shared_ptr<SessionCtx> sc;
    uint32_t width = 0, height = 0, layers = 1;
    bool process = false;
    bool resourcesReady = false;
    VkDescriptorPool dpool{};
    LayerRes layer[kMaxLayers];
    std::vector<ImageRes> images;
    uint32_t ring[kRingSize]{};  // acquire order -> release order
    uint32_t ringHead = 0, ringTail = 0;
    double gpuMsSum = 0, gpuMsMax = 0;
    uint32_t gpuMsCount = 0;
    bool loggedProfile = false;
    int lastEnabled = -1;  // -1 = no frame seen yet; lets the first frame and each on/off flip be logged once
};

struct Dispatch {
    PFN_xrGetInstanceProcAddr gipa{};
    PFN_xrDestroyInstance DestroyInstance{};
    PFN_xrCreateSession CreateSession{};
    PFN_xrDestroySession DestroySession{};
    PFN_xrCreateSwapchain CreateSwapchain{};
    PFN_xrDestroySwapchain DestroySwapchain{};
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages{};
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage{};
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage{};
};

std::mutex g_mutex;
Dispatch g_next;
std::unordered_map<XrSession, std::shared_ptr<SessionCtx>> g_sessions;
std::unordered_map<XrSwapchain, std::shared_ptr<SwapchainCtx>> g_swapchains;
sf::Config g_cfg;
std::string g_cfgPath;
std::filesystem::file_time_type g_cfgTime{};
uint32_t g_frame = 0;
bool g_profile = false;

// Diagnostic log. The layer runs inside someone else's process (a game started from the headset's
// dashboard) with no terminal attached, so a user whose install "does nothing" can't otherwise tell
// "never loaded" from "loaded but never saw a passthrough swapchain". Each notable event is one line
// appended to SF_LOG, else $XDG_CACHE_HOME (or ~/.cache)/steamframe-passthrough/layer.log;
// scripts/steamframe-passthrough-ctl shows it. Events are rare (a handful per app run), so the file
// is reopened per line, and each line is a single O_APPEND write so concurrent processes don't mix.
constexpr uintmax_t kMaxLogBytes = 256 * 1024;
std::string g_logPath;
std::once_flag g_logOnce;

void logInit() {
    std::call_once(g_logOnce, [] {
        if (const char* e = std::getenv("SF_LOG"); e && *e) {
            g_logPath = e;
        } else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) {
            g_logPath = std::string(x) + "/steamframe-passthrough/layer.log";
        } else if (const char* h = std::getenv("HOME"); h && *h) {
            g_logPath = std::string(h) + "/.cache/steamframe-passthrough/layer.log";
        }
        if (g_logPath.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_logPath).parent_path(), ec);
        // Every OpenXR app loads this layer, so cap the file instead of letting it grow forever.
        auto size = std::filesystem::file_size(g_logPath, ec);
        if (!ec && size > kMaxLogBytes) std::filesystem::resize_file(g_logPath, 0, ec);
    });
}

__attribute__((format(printf, 1, 2))) void logLine(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char comm[32] = "?";
    if (FILE* f = std::fopen("/proc/self/comm", "r")) {
        if (std::fgets(comm, sizeof comm, f)) comm[std::strcspn(comm, "\n")] = 0;
        std::fclose(f);
    }
    char ts[32] = "";
    time_t now = std::time(nullptr);
    tm local{};
    if (localtime_r(&now, &local)) std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &local);
    char line[768];
    int n = std::snprintf(line, sizeof line, "%s [%s:%d] %s\n", ts, comm, int(getpid()), msg);
    if (n <= 0) return;
    if (n >= int(sizeof line)) n = int(sizeof line) - 1;
    int fd = open(g_logPath.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, line, size_t(n));
    (void)w;
    close(fd);
}

// Name for the swapchain formats an app is likely to pick, so the log is readable without a table.
const char* vkFormatName(int64_t f) {
    switch (VkFormat(f)) {
        case VK_FORMAT_R8_UNORM: return "R8_UNORM";
        case VK_FORMAT_R8_SRGB: return "R8_SRGB";
        case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        default: return "other";
    }
}

void logConfig(const char* what) {
    logLine("%s %s: enabled=%g temporal_alpha=%g motion_threshold=%g sharpness_weight=%g noise_floor=%g", what,
            g_cfgPath.c_str(), g_cfg.enabled, g_cfg.temporal_alpha, g_cfg.motion_threshold, g_cfg.sharpness_weight,
            g_cfg.noise_floor);
}

void loadConfig() {
    if (const char* e = std::getenv("SF_CONFIG")) g_cfgPath = e;
    else if (std::filesystem::exists("config.ini")) g_cfgPath = "config.ini";
    else if (const char* h = std::getenv("HOME")) g_cfgPath = std::string(h) + "/.config/steamframe-passthrough/config.ini";
    g_profile = std::getenv("SF_PROFILE") != nullptr;
    std::error_code ec;
    if (!g_cfgPath.empty() && std::filesystem::exists(g_cfgPath, ec)) {
        g_cfg = sf::Config::load(g_cfgPath);
        g_cfgTime = std::filesystem::last_write_time(g_cfgPath, ec);
        logConfig("config loaded:");
    } else {
        logLine("no config file at '%s'; using built-in defaults (filter on)", g_cfgPath.c_str());
    }
}

// Live parameters: poll the config mtime every 120 released images (caller holds g_mutex).
void maybeReloadConfig() {
    if (g_cfgPath.empty() || (++g_frame % 120) != 0) return;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(g_cfgPath, ec);
    if (!ec && t != g_cfgTime) {
        g_cfg = sf::Config::load(g_cfgPath);
        g_cfgTime = t;
        logConfig("config reloaded:");
    }
}

uint32_t findMemType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool formatUsable(VkPhysicalDevice pd, VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

void destroySessionCtx(SessionCtx& s) {
    if (!s.device) return;
    if (s.pipeline) vkDestroyPipeline(s.device, s.pipeline, nullptr);
    if (s.pipeLayout) vkDestroyPipelineLayout(s.device, s.pipeLayout, nullptr);
    if (s.setLayout) vkDestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
    if (s.shader) vkDestroyShaderModule(s.device, s.shader, nullptr);
    if (s.pool) vkDestroyCommandPool(s.device, s.pool, nullptr);
    XrSession keep = s.session;
    s = SessionCtx{};
    s.session = keep;
}

bool sessionFail(const char* why) {
    logLine("session setup failed, layer inactive for this session: %s", why);
    return false;
}

bool initSessionCtx(SessionCtx& s, const XrGraphicsBindingVulkanKHR& b) {
    s.instance = b.instance;
    s.phys = b.physicalDevice;
    s.device = b.device;
    s.family = b.queueFamilyIndex;
    vkGetDeviceQueue(s.device, b.queueFamilyIndex, b.queueIndex, &s.queue);

    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &n, qf.data());
    if (s.family >= n || !(qf[s.family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        return sessionFail("the app's queue family has no compute support");
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(s.phys, &pp);
    s.tsPeriod = pp.limits.timestampPeriod;
    s.tsValid = qf[s.family].timestampValidBits > 0;

    VkPhysicalDeviceFeatures feat;
    vkGetPhysicalDeviceFeatures(s.phys, &feat);
    if (!feat.shaderStorageImageExtendedFormats) return sessionFail("device lacks shaderStorageImageExtendedFormats");
    if (!formatUsable(s.phys, kSwapFormat) || !formatUsable(s.phys, kHistFormat))
        return sessionFail("device cannot use R8_UNORM / R16_SFLOAT as storage images");

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s.family;
    if (vkCreateCommandPool(s.device, &pi, nullptr, &s.pool) != VK_SUCCESS) return sessionFail("vkCreateCommandPool failed");

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(denoise_cas_spv);
    sm.pCode = denoise_cas_spv;
    if (vkCreateShaderModule(s.device, &sm, nullptr, &s.shader) != VK_SUCCESS)
        return sessionFail("vkCreateShaderModule failed");

    VkDescriptorSetLayoutBinding bind[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bind[i].binding = i;
        bind[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bind[i].descriptorCount = 1;
        bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4;
    dl.pBindings = bind;
    if (vkCreateDescriptorSetLayout(s.device, &dl, nullptr, &s.setLayout) != VK_SUCCESS)
        return sessionFail("vkCreateDescriptorSetLayout failed");

    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(s.device, &pl, nullptr, &s.pipeLayout) != VK_SUCCESS)
        return sessionFail("vkCreatePipelineLayout failed");

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                s.shader, "main", nullptr};
    ci.layout = s.pipeLayout;
    if (vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipeline) != VK_SUCCESS)
        return sessionFail("vkCreateComputePipelines failed");
    return true;
}

VkImageView makeView(VkDevice d, VkImage img, VkFormat f, uint32_t layer) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    VkImageView v{};
    vkCreateImageView(d, &vi, nullptr, &v);
    return v;
}

void destroySwapchainRes(SwapchainCtx& w) {
    SessionCtx& s = *w.sc;
    if (!s.device) return;
    for (auto& im : w.images) {
        for (auto v : im.view)
            if (v) vkDestroyImageView(s.device, v, nullptr);
        if (im.fence) vkDestroyFence(s.device, im.fence, nullptr);
        if (im.qp) vkDestroyQueryPool(s.device, im.qp, nullptr);
        if (im.cmd) vkFreeCommandBuffers(s.device, s.pool, 1, &im.cmd);
    }
    w.images.clear();
    if (w.dpool) vkDestroyDescriptorPool(s.device, w.dpool, nullptr);
    w.dpool = VK_NULL_HANDLE;
    for (auto& l : w.layer) {
        for (int i = 0; i < 2; ++i) {
            if (l.histView[i]) vkDestroyImageView(s.device, l.histView[i], nullptr);
            if (l.hist[i]) vkDestroyImage(s.device, l.hist[i], nullptr);
            if (l.mem[i]) vkFreeMemory(s.device, l.mem[i], nullptr);
        }
        l = LayerRes{};
    }
    w.resourcesReady = false;
}

bool buildSwapchainRes(SwapchainCtx& w, const VkImage* imgs, uint32_t count) {
    SessionCtx& s = *w.sc;
    if (count == 0 || count > kMaxImages) return false;
    for (uint32_t l = 0; l < w.layers; ++l)
        for (int i = 0; i < 2; ++i) {
            LayerRes& L = w.layer[l];
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = kHistFormat;
            ii.extent = {w.width, w.height, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(s.device, &ii, nullptr, &L.hist[i]) != VK_SUCCESS) return false;
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(s.device, L.hist[i], &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = findMemType(s.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (ai.memoryTypeIndex == UINT32_MAX) return false;
            if (vkAllocateMemory(s.device, &ai, nullptr, &L.mem[i]) != VK_SUCCESS) return false;
            vkBindImageMemory(s.device, L.hist[i], L.mem[i], 0);
            L.histView[i] = makeView(s.device, L.hist[i], kHistFormat, 0);
        }

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count * w.layers * 4 * 4};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = count * w.layers * 4;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.device, &pi, nullptr, &w.dpool) != VK_SUCCESS) return false;

    w.images.assign(count, ImageRes{});
    for (uint32_t k = 0; k < count; ++k) {
        ImageRes& im = w.images[k];
        im.image = imgs[k];
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = s.pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.device, &ca, &im.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(s.device, &fi, nullptr, &im.fence) != VK_SUCCESS) return false;
        if (s.tsValid) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (vkCreateQueryPool(s.device, &qi, nullptr, &im.qp) != VK_SUCCESS) return false;
        }
        for (uint32_t l = 0; l < w.layers; ++l) {
            im.view[l] = makeView(s.device, im.image, kSwapFormat, l);
            const LayerRes& L = w.layer[l];
            for (int p = 0; p < 2; ++p)
                for (int pass = 0; pass < 2; ++pass) {
                    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    da.descriptorPool = w.dpool;
                    da.descriptorSetCount = 1;
                    da.pSetLayouts = &s.setLayout;
                    VkDescriptorSet& set = im.sets[l][p][pass];
                    if (vkAllocateDescriptorSets(s.device, &da, &set) != VK_SUCCESS) return false;
                    // pass 0 (temporal): in=swap, hist=H[p], next=H[1-p]
                    // pass 1 (CAS):      hist=H[1-p] (temporal result), out=swap
                    VkImageView hv = pass == 0 ? L.histView[p] : L.histView[1 - p];
                    VkImageView nv = pass == 0 ? L.histView[1 - p] : L.histView[p];
                    VkImageView views[4] = {im.view[l], hv, nv, im.view[l]};
                    VkDescriptorImageInfo info[4];
                    VkWriteDescriptorSet wr[4];
                    for (uint32_t b = 0; b < 4; ++b) {
                        info[b] = {VK_NULL_HANDLE, views[b], VK_IMAGE_LAYOUT_GENERAL};
                        wr[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, b, 0, 1,
                                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &info[b], nullptr, nullptr};
                    }
                    vkUpdateDescriptorSets(s.device, 4, wr, 0, nullptr);
                }
        }
    }
    w.resourcesReady = true;
    return true;
}

void collectTimestamps(SwapchainCtx& w, ImageRes& im) {
    if (!im.qp || !im.submitted) return;
    uint64_t t[2];
    if (vkGetQueryPoolResults(w.sc->device, im.qp, 0, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) !=
        VK_SUCCESS)
        return;
    double ms = double(t[1] - t[0]) * w.sc->tsPeriod * 1e-6;
    w.gpuMsSum += ms;
    ++w.gpuMsCount;
    w.gpuMsMax = std::max(w.gpuMsMax, ms);
    if (w.gpuMsCount == 300) {
        if (g_profile)
            std::fprintf(stderr, "[%s] GPU %.3f ms avg, %.3f ms max over %u frames (%u layer(s)) %s\n", kLayerName,
                         w.gpuMsSum / w.gpuMsCount, w.gpuMsMax, w.gpuMsCount, w.layers,
                         w.gpuMsMax > 2.0 ? "OVER 2ms BUDGET" : "ok");
        if (!w.loggedProfile) {  // only the first window: later ones would grow the log without bound
            w.loggedProfile = true;
            logLine("GPU time over the first %u processed frames: %.3f ms avg, %.3f ms max (%u layer(s)) %s",
                    w.gpuMsCount, w.gpuMsSum / w.gpuMsCount, w.gpuMsMax, w.layers,
                    w.gpuMsMax > 2.0 ? "OVER 2ms BUDGET" : "ok");
        }
        w.gpuMsSum = 0;
        w.gpuMsCount = 0;
        w.gpuMsMax = 0;
    }
}

void record(SwapchainCtx& w, ImageRes& im, const sf::Config& cfg) {
    SessionCtx& s = *w.sc;
    VkCommandBuffer cb = im.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    if (im.qp) {
        vkCmdResetQueryPool(cb, im.qp, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, im.qp, 0);
    }

    VkImageMemoryBarrier pre[1 + 2 * kMaxLayers]{};
    uint32_t np = 0;
    // The first time we process a given swapchain image it is still in whatever layout the
    // runtime created it with (VK_IMAGE_LAYOUT_UNDEFINED, same assumption as histInit below for
    // the history images we allocate ourselves); claiming COLOR_ATTACHMENT_OPTIMAL as oldLayout
    // on that first transition is a lie Vulkan doesn't require the driver to tolerate. From the
    // second use onward our own barrier below always leaves it in COLOR_ATTACHMENT_OPTIMAL, which
    // we assume (same convention as a regular color-attachment swapchain) is what the app renders
    // into and hands back for release.
    pre[np++] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
                 im.layoutInit ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VkAccessFlags(0),
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 im.layoutInit ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im.image,
                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, w.layers}};
    im.layoutInit = true;
    for (uint32_t l = 0; l < w.layers; ++l)
        if (!w.layer[l].histInit) {
            for (int i = 0; i < 2; ++i)
                pre[np++] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                             w.layer[l].hist[i], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            w.layer[l].histInit = true;
        }
    // Wait for the app's rendering (all prior queue work) before the compute pass reads the image.
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, np, pre);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    const uint32_t gx = (w.width + 7) / 8, gy = (w.height + 7) / 8;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    for (uint32_t l = 0; l < w.layers; ++l) {
        LayerRes& L = w.layer[l];
        PushConstants pc{cfg.temporal_alpha, cfg.motion_threshold, cfg.sharpness_weight, cfg.noise_floor, 0,
                         L.valid ? 1 : 0};
        for (int pass = 0; pass < 2; ++pass) {
            pc.pass = pass;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeLayout, 0, 1,
                                    &im.sets[l][L.read][pass], 0, nullptr);
            vkCmdPushConstants(cb, s.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cb, gx, gy, 1);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 1, &mb, 0, nullptr, 0, nullptr);
        }
        L.read = 1 - L.read;
        L.valid = true;
    }

    VkImageMemoryBarrier post{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, im.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, w.layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &post);
    if (im.qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, im.qp, 1);
    vkEndCommandBuffer(cb);
}

void freeSwapchain(const std::shared_ptr<SwapchainCtx>& w) {
    if (w->sc && w->sc->device) {
        vkDeviceWaitIdle(w->sc->device);
        destroySwapchainRes(*w);
    }
}

// ---- OpenXR hooks ----

XRAPI_ATTR XrResult XRAPI_CALL hCreateSession(XrInstance inst, const XrSessionCreateInfo* ci, XrSession* out) {
    XrResult r = g_next.CreateSession(inst, ci, out);
    if (XR_FAILED(r)) return r;
    bool sawVulkan = false;
    for (auto n = static_cast<const XrBaseInStructure*>(ci->next); n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {  // the VULKAN2 binding is an alias of this
            sawVulkan = true;
            auto sc = std::make_shared<SessionCtx>();
            sc->session = *out;
            sc->ok = initSessionCtx(*sc, *reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(n));
            if (!sc->ok) {
                std::fprintf(stderr, "[%s] Vulkan setup unsupported; passthrough denoise disabled\n", kLayerName);
                destroySessionCtx(*sc);
            } else {
                logLine("session created: Vulkan compute path ready (queue family %u)", sc->family);
            }
            std::lock_guard<std::mutex> lk(g_mutex);
            g_sessions[*out] = sc;
            break;
        }
    }
    if (!sawVulkan) logLine("session created without a Vulkan graphics binding; layer inactive for this session");
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hDestroySession(XrSession session) {
    std::vector<std::shared_ptr<SwapchainCtx>> doomed;
    std::shared_ptr<SessionCtx> sc;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto it = g_swapchains.begin(); it != g_swapchains.end();) {
            if (it->second->sc && it->second->sc->session == session) {
                doomed.push_back(it->second);
                it = g_swapchains.erase(it);
            } else {
                ++it;
            }
        }
        auto it = g_sessions.find(session);
        if (it != g_sessions.end()) {
            sc = it->second;
            g_sessions.erase(it);
        }
    }
    for (auto& w : doomed) freeSwapchain(w);
    if (sc && sc->device) {
        vkDeviceWaitIdle(sc->device);
        destroySessionCtx(*sc);
    }
    return g_next.DestroySession(session);
}

XRAPI_ATTR XrResult XRAPI_CALL hCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci,
                                                XrSwapchain* out) {
    std::shared_ptr<SessionCtx> sc;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_sessions.find(session);
        if (it != g_sessions.end()) sc = it->second;
    }
    const char* skip = nullptr;  // why this swapchain is left alone; null = we process it
    if (!sc) skip = "no Vulkan session";
    else if (!sc->ok) skip = "session setup failed";
    else if (ci->format != int64_t(kSwapFormat)) skip = "format is not R8_UNORM";
    else if (ci->arraySize > kMaxLayers) skip = "arraySize > 2";
    else if (ci->sampleCount != 1 || ci->faceCount != 1 || ci->mipCount != 1) skip = "multisampled, cube or mipmapped";
    else if (!(ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) skip = "no color-attachment usage";
    const bool want = skip == nullptr;
    // An app can create many swapchains; log the first few, which is plenty to see what it uses.
    static std::atomic<int> loggedSwapchains{0};
    if (loggedSwapchains.fetch_add(1) < 24)
        logLine("swapchain %ux%u format=%lld(%s) arraySize=%u usage=0x%llx: %s", ci->width, ci->height,
                (long long)ci->format, vkFormatName(ci->format), ci->arraySize,
                (unsigned long long)ci->usageFlags, want ? "processing" : skip);
    XrSwapchainCreateInfo mod = *ci;
    if (want) mod.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
    XrResult r = g_next.CreateSwapchain(session, want ? &mod : ci, out);
    if (XR_SUCCEEDED(r) && want) {
        auto w = std::make_shared<SwapchainCtx>();
        w->handle = *out;
        w->sc = sc;
        w->width = ci->width;
        w->height = ci->height;
        w->layers = ci->arraySize ? ci->arraySize : 1;
        w->process = true;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_swapchains[*out] = w;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hDestroySwapchain(XrSwapchain sw) {
    std::shared_ptr<SwapchainCtx> w;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_swapchains.find(sw);
        if (it != g_swapchains.end()) {
            w = it->second;
            g_swapchains.erase(it);
        }
    }
    if (w) freeSwapchain(w);
    return g_next.DestroySwapchain(sw);
}

XRAPI_ATTR XrResult XRAPI_CALL hEnumerateSwapchainImages(XrSwapchain sw, uint32_t cap, uint32_t* count,
                                                         XrSwapchainImageBaseHeader* images) {
    XrResult r = g_next.EnumerateSwapchainImages(sw, cap, count, images);
    if (XR_FAILED(r) || !images || cap == 0) return r;
    std::shared_ptr<SwapchainCtx> w;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_swapchains.find(sw);
        if (it != g_swapchains.end()) w = it->second;
    }
    if (!w || w->resourcesReady || images[0].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return r;
    uint32_t n = std::min(*count, cap);
    std::vector<VkImage> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images)[i].image;
    if (!buildSwapchainRes(*w, v.data(), n)) {
        std::fprintf(stderr, "[%s] swapchain resource allocation failed; swapchain passes through\n", kLayerName);
        logLine("swapchain %p: resource allocation failed (%u images); frames pass through untouched",
                static_cast<void*>(w->handle), n);
        freeSwapchain(w);
        w->process = false;
    } else {
        logLine("swapchain %p: %u images ready", static_cast<void*>(w->handle), n);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hAcquireSwapchainImage(XrSwapchain sw, const XrSwapchainImageAcquireInfo* ai,
                                                      uint32_t* index) {
    XrResult r = g_next.AcquireSwapchainImage(sw, ai, index);
    if (XR_FAILED(r)) return r;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_swapchains.find(sw);
    if (it != g_swapchains.end() && it->second->process) {
        SwapchainCtx& w = *it->second;
        w.ring[w.ringHead++ % kRingSize] = *index;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hReleaseSwapchainImage(XrSwapchain sw, const XrSwapchainImageReleaseInfo* ri) {
    std::shared_ptr<SwapchainCtx> w;
    sf::Config cfg;
    uint32_t idx = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_swapchains.find(sw);
        if (it != g_swapchains.end() && it->second->process && it->second->resourcesReady &&
            it->second->ringTail != it->second->ringHead) {
            w = it->second;
            idx = w->ring[w->ringTail++ % kRingSize];
            maybeReloadConfig();
            cfg = g_cfg;
        }
    }
    if (w && idx < w->images.size()) {
        ImageRes& im = w->images[idx];
        SessionCtx& s = *w->sc;
        const int enabledNow = cfg.enabled < 0.5f ? 0 : 1;
        if (w->lastEnabled != enabledNow) {
            logLine("swapchain %p: %s", static_cast<void*>(w->handle),
                    enabledNow ? "filtering frames" : "bypassed (enabled = 0): frames pass through untouched");
            w->lastEnabled = enabledNow;
        }
        if (cfg.enabled < 0.5f) {  // raw passthrough: skip the dispatch, just drain any in-flight work
            if (im.submitted) {
                vkWaitForFences(s.device, 1, &im.fence, VK_TRUE, UINT64_MAX);
                vkResetFences(s.device, 1, &im.fence);
                im.submitted = false;
            }
            // The app still renders into (and leaves in COLOR_ATTACHMENT_OPTIMAL) this image even
            // while we're bypassed; record()'s first-use UNDEFINED assumption must not apply to a
            // bypassed-then-re-enabled image, so mark it as no longer pristine here too.
            im.layoutInit = true;
            return g_next.ReleaseSwapchainImage(sw, ri);
        }
        if (im.submitted) {
            vkWaitForFences(s.device, 1, &im.fence, VK_TRUE, UINT64_MAX);
            vkResetFences(s.device, 1, &im.fence);
            collectTimestamps(*w, im);
        }
        vkResetCommandBuffer(im.cmd, 0);
        record(*w, im, cfg);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &im.cmd;
        im.submitted = vkQueueSubmit(s.queue, 1, &si, im.fence) == VK_SUCCESS;
    }
    return g_next.ReleaseSwapchainImage(sw, ri);
}

XRAPI_ATTR XrResult XRAPI_CALL hDestroyInstance(XrInstance inst) {
    std::vector<std::shared_ptr<SwapchainCtx>> sw;
    std::vector<std::shared_ptr<SessionCtx>> ss;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& p : g_swapchains) sw.push_back(p.second);
        for (auto& p : g_sessions) ss.push_back(p.second);
        g_swapchains.clear();
        g_sessions.clear();
    }
    for (auto& w : sw) freeSwapchain(w);
    for (auto& s : ss)
        if (s->device) {
            vkDeviceWaitIdle(s->device);
            destroySessionCtx(*s);
        }
    return g_next.DestroyInstance(inst);
}

XRAPI_ATTR XrResult XRAPI_CALL hGetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
#define HOOK(n)                                                 \
    if (!std::strcmp(name, "xr" #n)) {                          \
        *fn = reinterpret_cast<PFN_xrVoidFunction>(h##n);       \
        return XR_SUCCESS;                                      \
    }
    HOOK(DestroyInstance) HOOK(CreateSession) HOOK(DestroySession) HOOK(CreateSwapchain) HOOK(DestroySwapchain)
    HOOK(EnumerateSwapchainImages) HOOK(AcquireSwapchainImage) HOOK(ReleaseSwapchainImage)
#undef HOOK
    return g_next.gipa(inst, name, fn);
}

XRAPI_ATTR XrResult XRAPI_CALL hCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                       const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    logInit();
    if (!layerInfo || !layerInfo->nextInfo || std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0) {
        logLine("layer load failed: the loader's layer chain does not name %s", kLayerName);
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    PFN_xrGetInstanceProcAddr gipa = layerInfo->nextInfo->nextGetInstanceProcAddr;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (XR_FAILED(r)) return r;

    std::lock_guard<std::mutex> lk(g_mutex);
    g_next = Dispatch{};
    g_next.gipa = gipa;
    bool ok = true;
#define LOAD(n)                                                                                           \
    do {                                                                                                   \
        XrResult lr = gipa(*instance, "xr" #n, reinterpret_cast<PFN_xrVoidFunction*>(&g_next.n));          \
        if (XR_FAILED(lr) || !g_next.n) {                                                                  \
            std::fprintf(stderr, "[%s] failed to resolve xr" #n " (XrResult %d)\n", kLayerName, (int)lr);  \
            logLine("layer load failed: could not resolve xr" #n " (XrResult %d)", (int)lr);               \
            ok = false;                                                                                    \
        }                                                                                                   \
    } while (0)
    LOAD(DestroyInstance);
    LOAD(CreateSession);
    LOAD(DestroySession);
    LOAD(CreateSwapchain);
    LOAD(DestroySwapchain);
    LOAD(EnumerateSwapchainImages);
    LOAD(AcquireSwapchainImage);
    LOAD(ReleaseSwapchainImage);
#undef LOAD
    if (!ok) {
        // The instance below us was already created by nextCreateApiLayerInstance above; tear it
        // down before reporting failure so we don't leak it.
        PFN_xrDestroyInstance destroy = nullptr;
        if (XR_SUCCEEDED(gipa(*instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroy))) &&
            destroy)
            destroy(*instance);
        g_next = Dispatch{};
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    logLine("layer loaded into this OpenXR app");
    loadConfig();
    return XR_SUCCESS;
}

}  // namespace

SF_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                 const char* layerName,
                                                                 XrNegotiateApiLayerRequest* req) {
    if (!loaderInfo || !req || !layerName || std::strcmp(layerName, kLayerName) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = hGetInstanceProcAddr;
    req->createApiLayerInstance = hCreateApiLayerInstance;
    return XR_SUCCESS;
}
