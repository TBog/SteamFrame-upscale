// OpenXR API layer: runs temporal denoise + CAS (shaders/denoise_cas.comp) in-place on
// R8_UNORM Vulkan swapchain images just before xrReleaseSwapchainImage.
// Each swapchain (or array layer) is treated as one eye and owns a ping-pong R16F history.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "denoise.hpp"
#include "denoise_spv.h"
#include "ipc.hpp"
#include "loader_interfaces.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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
std::atomic<bool> g_bypassFilter{false};  // true = raw passthrough, no compute dispatch
std::atomic<double> g_gpuMs{0.0};         // smoothed GPU time per processed frame
std::atomic<bool> g_ipcStop{false};
std::thread g_ipcThread;
int g_ipcFd = -1;

// UNIX socket server: one command line per connection, one reply line. Last layer instance to start owns the path.
void ipcServe() {
    while (!g_ipcStop) {
        pollfd p{g_ipcFd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        int c = accept(g_ipcFd, nullptr, nullptr);
        if (c < 0) continue;
        timeval tv{0, 200000};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char buf[256];
        ssize_t n = read(c, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            std::string reply;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                reply = sf::handleCommand(buf, g_cfg, g_bypassFilter, g_gpuMs.load()) + "\n";
            }
            ssize_t w = write(c, reply.data(), reply.size());
            (void)w;
        }
        close(c);
    }
}

void startIpc() {
    if (g_ipcThread.joinable()) return;
    std::string path = sf::socketPath();
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return;
    std::strcpy(a.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return;
    }
    chmod(path.c_str(), 0600);
    g_ipcFd = fd;
    g_ipcStop = false;
    g_ipcThread = std::thread(ipcServe);
}

void stopIpc() {
    if (!g_ipcThread.joinable()) return;
    g_ipcStop = true;
    g_ipcThread.join();
    close(g_ipcFd);
    g_ipcFd = -1;
    unlink(sf::socketPath().c_str());
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
        g_bypassFilter = g_cfg.enabled < 0.5f;
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
        g_bypassFilter = g_cfg.enabled < 0.5f;
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
    if (s.family >= n || !(qf[s.family].queueFlags & VK_QUEUE_COMPUTE_BIT)) return false;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(s.phys, &pp);
    s.tsPeriod = pp.limits.timestampPeriod;
    s.tsValid = qf[s.family].timestampValidBits > 0;

    VkPhysicalDeviceFeatures feat;
    vkGetPhysicalDeviceFeatures(s.phys, &feat);
    if (!feat.shaderStorageImageExtendedFormats) return false;
    if (!formatUsable(s.phys, kSwapFormat) || !formatUsable(s.phys, kHistFormat)) return false;

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s.family;
    if (vkCreateCommandPool(s.device, &pi, nullptr, &s.pool) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(denoise_cas_spv);
    sm.pCode = denoise_cas_spv;
    if (vkCreateShaderModule(s.device, &sm, nullptr, &s.shader) != VK_SUCCESS) return false;

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
    if (vkCreateDescriptorSetLayout(s.device, &dl, nullptr, &s.setLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(s.device, &pl, nullptr, &s.pipeLayout) != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                s.shader, "main", nullptr};
    ci.layout = s.pipeLayout;
    return vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipeline) == VK_SUCCESS;
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
    g_gpuMs = g_gpuMs.load() * 0.95 + ms * 0.05;
    w.gpuMsSum += ms;
    ++w.gpuMsCount;
    w.gpuMsMax = std::max(w.gpuMsMax, ms);
    if (w.gpuMsCount == 300) {
        if (g_profile)
            std::fprintf(stderr, "[%s] GPU %.3f ms avg, %.3f ms max over %u frames (%u layer(s)) %s\n", kLayerName,
                         w.gpuMsSum / w.gpuMsCount, w.gpuMsMax, w.gpuMsCount, w.layers,
                         w.gpuMsMax > 2.0 ? "OVER 2ms BUDGET" : "ok");
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
    pre[np++] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im.image,
                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, w.layers}};
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
    for (auto n = static_cast<const XrBaseInStructure*>(ci->next); n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {  // the VULKAN2 binding is an alias of this
            auto sc = std::make_shared<SessionCtx>();
            sc->session = *out;
            sc->ok = initSessionCtx(*sc, *reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(n));
            if (!sc->ok) {
                std::fprintf(stderr, "[%s] Vulkan setup unsupported; passthrough denoise disabled\n", kLayerName);
                destroySessionCtx(*sc);
            }
            std::lock_guard<std::mutex> lk(g_mutex);
            g_sessions[*out] = sc;
            break;
        }
    }
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
    const bool want = sc && sc->ok && ci->format == int64_t(kSwapFormat) && ci->arraySize <= kMaxLayers &&
                      ci->sampleCount == 1 && ci->faceCount == 1 && ci->mipCount == 1 &&
                      (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
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
        freeSwapchain(w);
        w->process = false;
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
        if (g_bypassFilter) {  // raw passthrough: no dispatch; just drain any earlier in-flight work
            if (im.submitted) {
                vkWaitForFences(s.device, 1, &im.fence, VK_TRUE, UINT64_MAX);
                vkResetFences(s.device, 1, &im.fence);
                im.submitted = false;
            }
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
    stopIpc();
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
    if (!layerInfo || !layerInfo->nextInfo || std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    PFN_xrGetInstanceProcAddr gipa = layerInfo->nextInfo->nextGetInstanceProcAddr;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (XR_FAILED(r)) return r;

    std::lock_guard<std::mutex> lk(g_mutex);
    g_next = Dispatch{};
    g_next.gipa = gipa;
#define LOAD(n) gipa(*instance, "xr" #n, reinterpret_cast<PFN_xrVoidFunction*>(&g_next.n))
    LOAD(DestroyInstance);
    LOAD(CreateSession);
    LOAD(DestroySession);
    LOAD(CreateSwapchain);
    LOAD(DestroySwapchain);
    LOAD(EnumerateSwapchainImages);
    LOAD(AcquireSwapchainImage);
    LOAD(ReleaseSwapchainImage);
#undef LOAD
    loadConfig();
    startIpc();
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
