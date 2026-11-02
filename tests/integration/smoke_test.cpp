// End-to-end smoke test for src/layer.cpp: drives a real OpenXR instance/session/swapchain
// through the layer against stub_runtime.cpp (a fake runtime with no presentation, see its header
// comment) on a real -- possibly software -- Vulkan device. Feeds `kFrames` of synthetic sensor
// noise through xrAcquireSwapchainImage/xrReleaseSwapchainImage and checks that the per-pixel
// variance actually dropped by the end, i.e. that the layer's temporal-denoise compute shader
// really ran on the GPU, not just that nothing crashed (same check as the "noise variance drops"
// case in tests/test_pipeline.cpp, but against the real Vulkan path instead of the CPU reference).
//
// Needs XR_RUNTIME_JSON pointing at the stub runtime's manifest and XR_API_LAYER_PATH /
// XR_ENABLE_API_LAYERS pointing at the built layer; `ctest` wires these up (see CMakeLists.txt).
// If no Vulkan device is present, or it lacks shaderStorageImageExtendedFormats (the same
// requirement src/layer.cpp itself checks), the test is skipped (exit 77) rather than failed --
// that is an environment limitation, not a layer bug. See tests/integration/README.md.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kW = 64, kH = 64, kFrames = 40, kVarWindow = 10;
constexpr int kSkipExitCode = 77;  // CTest's SKIP_RETURN_CODE convention

[[noreturn]] void skip(const char* why) {
    std::fprintf(stderr, "SKIP: %s\n", why);
    std::exit(kSkipExitCode);
}
[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
}
[[noreturn]] void failXr(const char* what, XrResult r) {
    std::fprintf(stderr, "FAIL: %s (XrResult %d)\n", what, (int)r);
    std::exit(1);
}
[[noreturn]] void failVk(const char* what, VkResult r) {
    std::fprintf(stderr, "FAIL: %s (VkResult %d)\n", what, (int)r);
    std::exit(1);
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

// Test-harness-only barrier: access/stage masks are intentionally broad (ALL_COMMANDS /
// MEMORY_READ|WRITE) since every step below is followed by a full vkDeviceWaitIdle anyway -- the
// idle wait is what actually guarantees visibility, not these masks.
void transition(VkCommandBuffer cb, VkImage img, VkImageLayout oldL, VkImageLayout newL) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

template <class Fn>
void oneShot(VkDevice dev, VkCommandPool pool, VkQueue q, Fn&& fn) {
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    fn(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

// Uploads `src` ([0,1] floats, kW*kH) into `img` via the staging buffer, leaving it in
// COLOR_ATTACHMENT_OPTIMAL -- the layout a real app's render pass would leave a swapchain image in
// before handing it to xrReleaseSwapchainImage.
void uploadFrame(VkDevice dev, VkCommandPool pool, VkQueue q, VkImage img, VkBuffer staging, void* stagingPtr,
                 const std::vector<float>& src, bool everUsed) {
    auto* b = static_cast<uint8_t*>(stagingPtr);
    for (uint32_t i = 0; i < kW * kH; ++i) b[i] = static_cast<uint8_t>(std::clamp(src[i], 0.f, 1.f) * 255.f + 0.5f);
    oneShot(dev, pool, q, [&](VkCommandBuffer cb) {
        transition(cb, img, everUsed ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {kW, kH, 1};
        vkCmdCopyBufferToImage(cb, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    });
}

// Reads `img` back into `dst` ([0,1] floats). Leaves the image in COLOR_ATTACHMENT_OPTIMAL (the
// state the layer's own post-pass barrier leaves it in, and the invariant uploadFrame assumes).
void downloadFrame(VkDevice dev, VkCommandPool pool, VkQueue q, VkImage img, VkBuffer staging, void* stagingPtr,
                   std::vector<float>& dst) {
    oneShot(dev, pool, q, [&](VkCommandBuffer cb) {
        transition(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {kW, kH, 1};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &r);
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    });
    auto* b = static_cast<uint8_t*>(stagingPtr);
    dst.resize(kW * kH);
    for (uint32_t i = 0; i < kW * kH; ++i) dst[i] = b[i] / 255.f;
}

}  // namespace

int main() {
    VkInstance vi{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "sf_smoke_test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (VkResult r = vkCreateInstance(&ici, nullptr, &vi); r != VK_SUCCESS) failVk("vkCreateInstance", r);

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(vi, &gpuCount, nullptr);
    if (gpuCount == 0) skip("no Vulkan device present");
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(vi, &gpuCount, gpus.data());
    VkPhysicalDevice phys = gpus[0];

    VkPhysicalDeviceFeatures feat;
    vkGetPhysicalDeviceFeatures(phys, &feat);
    if (!feat.shaderStorageImageExtendedFormats) skip("device lacks shaderStorageImageExtendedFormats");
    if (!formatUsable(phys, VK_FORMAT_R8_UNORM) || !formatUsable(phys, VK_FORMAT_R16_SFLOAT))
        skip("device lacks storage-image support for R8_UNORM/R16_SFLOAT");

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            family = i;
            break;
        }
    if (family == UINT32_MAX) skip("device has no compute-capable queue family");

    float prio = 1.f;
    VkDeviceQueueCreateInfo dqi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dqi.queueFamilyIndex = family;
    dqi.queueCount = 1;
    dqi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures enable{};
    enable.shaderStorageImageExtendedFormats = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqi;
    dci.pEnabledFeatures = &enable;
    VkDevice dev{};
    if (VkResult r = vkCreateDevice(phys, &dci, nullptr, &dev); r != VK_SUCCESS) failVk("vkCreateDevice", r);
    VkQueue queue{};
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool{};
    vkCreateCommandPool(dev, &pci, nullptr, &pool);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kW * kH;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging{};
    vkCreateBuffer(dev, &bci, nullptr, &staging);
    VkMemoryRequirements bmr;
    vkGetBufferMemoryRequirements(dev, staging, &bmr);
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = bmr.size;
    bai.memoryTypeIndex =
        findMemType(phys, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stagingMem{};
    vkAllocateMemory(dev, &bai, nullptr, &stagingMem);
    vkBindBufferMemory(dev, staging, stagingMem, 0);
    void* stagingPtr = nullptr;
    vkMapMemory(dev, stagingMem, 0, VK_WHOLE_SIZE, 0, &stagingPtr);

    XrInstance xri{};
    const char* layerName = "XR_APILAYER_STEAMFRAME_passthrough_denoise";
    XrApplicationInfo xrApp{};
    std::snprintf(xrApp.applicationName, sizeof(xrApp.applicationName), "sf_smoke_test");
    xrApp.applicationVersion = 1;
    xrApp.apiVersion = XR_CURRENT_API_VERSION;
    XrInstanceCreateInfo xici{XR_TYPE_INSTANCE_CREATE_INFO};
    xici.applicationInfo = xrApp;
    xici.enabledApiLayerCount = 1;
    xici.enabledApiLayerNames = &layerName;
    if (XrResult r = xrCreateInstance(&xici, &xri); r != XR_SUCCESS) failXr("xrCreateInstance", r);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId{};
    if (XrResult r = xrGetSystem(xri, &sgi, &systemId); r != XR_SUCCESS) failXr("xrGetSystem", r);

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vi;
    binding.physicalDevice = phys;
    binding.device = dev;
    binding.queueFamilyIndex = family;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = systemId;
    XrSession session{};
    if (XrResult r = xrCreateSession(xri, &sci, &session); r != XR_SUCCESS) failXr("xrCreateSession", r);

    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swci.format = static_cast<int64_t>(VK_FORMAT_R8_UNORM);
    swci.sampleCount = 1;
    swci.width = kW;
    swci.height = kH;
    swci.faceCount = 1;
    swci.arraySize = 1;
    swci.mipCount = 1;
    XrSwapchain swapchain{};
    if (XrResult r = xrCreateSwapchain(session, &swci, &swapchain); r != XR_SUCCESS)
        failXr("xrCreateSwapchain", r);

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &imgCount, nullptr);
    if (imgCount == 0) fail("swapchain reports zero images");
    std::vector<XrSwapchainImageVulkanKHR> images(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (XrResult r = xrEnumerateSwapchainImages(swapchain, imgCount, &imgCount,
                                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        r != XR_SUCCESS)
        failXr("xrEnumerateSwapchainImages", r);
    std::vector<bool> everUsed(imgCount, false);

    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.f, 0.03f);
    double inVar = 0, outVar = 0;
    int varSamples = 0;
    std::vector<float> frame(kW * kH), readback;

    for (uint32_t f = 0; f < kFrames; ++f) {
        for (auto& v : frame) v = std::clamp(0.5f + noise(rng), 0.f, 1.f);

        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t idx = 0;
        if (XrResult r = xrAcquireSwapchainImage(swapchain, &acq, &idx); r != XR_SUCCESS)
            failXr("xrAcquireSwapchainImage", r);
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (XrResult r = xrWaitSwapchainImage(swapchain, &wait); r != XR_SUCCESS)
            failXr("xrWaitSwapchainImage", r);

        uploadFrame(dev, pool, queue, images[idx].image, staging, stagingPtr, frame, everUsed[idx]);
        everUsed[idx] = true;

        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XrResult r = xrReleaseSwapchainImage(swapchain, &rel); r != XR_SUCCESS)
            failXr("xrReleaseSwapchainImage", r);
        // The layer submits its compute pass asynchronously inside xrReleaseSwapchainImage; wait
        // for it (same VkDevice/VkQueue) before touching the image again.
        vkDeviceWaitIdle(dev);

        if (f >= kFrames - kVarWindow) {
            downloadFrame(dev, pool, queue, images[idx].image, staging, stagingPtr, readback);
            for (uint32_t i = 0; i < kW * kH; ++i) {
                inVar += double(frame[i] - 0.5f) * double(frame[i] - 0.5f);
                outVar += double(readback[i] - 0.5f) * double(readback[i] - 0.5f);
            }
            ++varSamples;
        }
    }

    std::printf("input variance %.6f, output variance %.6f over %d frames\n", inVar / varSamples,
                outVar / varSamples, varSamples);
    if (!(outVar < inVar * 0.5))
        fail("output variance did not drop vs input -- temporal denoise does not appear to have run on the GPU");

    xrDestroySwapchain(swapchain);
    xrDestroySession(session);
    xrDestroyInstance(xri);
    vkUnmapMemory(dev, stagingMem);
    vkFreeMemory(dev, stagingMem, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(vi, nullptr);
    std::printf("OK\n");
    return 0;
}
