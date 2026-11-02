// Minimal fake OpenXR runtime, for testing src/layer.cpp without a real HMD/compositor.
// Implements just enough of the loader<->runtime interface and the handful of core functions the
// layer hooks (CreateSession, CreateSwapchain, EnumerateSwapchainImages, Acquire/ReleaseSwapchainImage,
// ...) to drive a real Vulkan swapchain. There is no presentation: swapchain images are plain
// VkImages allocated on the session's device, never shown anywhere. See smoke_test.cpp for the
// app side and tests/integration/README.md for how the two are wired together.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstring>

#include "loader_runtime_interfaces.h"

#define SF_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr uint32_t kImageCount = 3;

uint32_t findMemType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

VkImageUsageFlags toVkUsage(XrSwapchainUsageFlags x) {
    // Transfer bits are added unconditionally so the test app can seed/read back frames; a real
    // compositor would not necessarily do this, but nothing here is presented on screen anyway.
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (x & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (x & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (x & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    return u;
}

}  // namespace

// openxr.h only forward-declares e.g. `struct XrInstance_T;` via XR_DEFINE_HANDLE; defining it
// here is the standard way a (fake) runtime backs these opaque handles with real state.
struct XrInstance_T {};
struct XrSession_T {
    VkPhysicalDevice phys{};
    VkDevice device{};
    uint32_t family = 0;
};
struct XrSwapchain_T {
    VkDevice device{};
    VkImage images[kImageCount]{};
    VkDeviceMemory mem[kImageCount]{};
    uint32_t nextAcquire = 0;
};

namespace {

XRAPI_ATTR XrResult XRAPI_CALL rCreateInstance(const XrInstanceCreateInfo*, XrInstance* instance) {
    *instance = reinterpret_cast<XrInstance>(new XrInstance_T());
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rDestroyInstance(XrInstance instance) {
    delete reinterpret_cast<XrInstance_T*>(instance);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rEnumerateInstanceExtensionProperties(const char*, uint32_t, uint32_t* count,
                                                                     XrExtensionProperties*) {
    *count = 0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* systemId) {
    *systemId = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rCreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* session) {
    auto* s = new XrSession_T();
    for (auto n = static_cast<const XrBaseInStructure*>(ci->next); n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            auto* b = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(n);
            s->phys = b->physicalDevice;
            s->device = b->device;
            s->family = b->queueFamilyIndex;
            break;
        }
    }
    *session = reinterpret_cast<XrSession>(s);
    return s->device ? XR_SUCCESS : XR_ERROR_GRAPHICS_DEVICE_INVALID;
}

XRAPI_ATTR XrResult XRAPI_CALL rDestroySession(XrSession session) {
    delete reinterpret_cast<XrSession_T*>(session);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci,
                                                XrSwapchain* swapchain) {
    auto& s = *reinterpret_cast<XrSession_T*>(session);
    auto* w = new XrSwapchain_T();
    w->device = s.device;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = static_cast<VkFormat>(ci->format);
    ii.extent = {ci->width, ci->height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = ci->arraySize ? ci->arraySize : 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = toVkUsage(ci->usageFlags);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (uint32_t i = 0; i < kImageCount; ++i) {
        if (vkCreateImage(s.device, &ii, nullptr, &w->images[i]) != VK_SUCCESS) {
            delete w;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(s.device, w->images[i], &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemType(s.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(s.device, &ai, nullptr, &w->mem[i]) != VK_SUCCESS) {
            delete w;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        vkBindImageMemory(s.device, w->images[i], w->mem[i], 0);
    }
    *swapchain = reinterpret_cast<XrSwapchain>(w);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rDestroySwapchain(XrSwapchain swapchain) {
    auto* w = reinterpret_cast<XrSwapchain_T*>(swapchain);
    for (uint32_t i = 0; i < kImageCount; ++i) {
        if (w->images[i]) vkDestroyImage(w->device, w->images[i], nullptr);
        if (w->mem[i]) vkFreeMemory(w->device, w->mem[i], nullptr);
    }
    delete w;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t* count,
                                                         XrSwapchainImageBaseHeader* images) {
    auto& w = *reinterpret_cast<XrSwapchain_T*>(swapchain);
    *count = kImageCount;
    if (cap == 0 || !images) return XR_SUCCESS;
    auto* v = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
    for (uint32_t i = 0; i < std::min(cap, kImageCount); ++i) v[i].image = w.images[i];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*,
                                                      uint32_t* index) {
    auto& w = *reinterpret_cast<XrSwapchain_T*>(swapchain);
    *index = w.nextAcquire;
    w.nextAcquire = (w.nextAcquire + 1) % kImageCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
#define HOOK(n)                                                 \
    if (!std::strcmp(name, "xr" #n)) {                          \
        *fn = reinterpret_cast<PFN_xrVoidFunction>(r##n);       \
        return XR_SUCCESS;                                      \
    }
    HOOK(CreateInstance) HOOK(DestroyInstance) HOOK(EnumerateInstanceExtensionProperties) HOOK(GetSystem)
    HOOK(CreateSession) HOOK(DestroySession) HOOK(CreateSwapchain) HOOK(DestroySwapchain)
    HOOK(EnumerateSwapchainImages) HOOK(AcquireSwapchainImage) HOOK(WaitSwapchainImage)
    HOOK(ReleaseSwapchainImage)
#undef HOOK
    *fn = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

}  // namespace

SF_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                 XrNegotiateRuntimeRequest* runtimeRequest) {
    if (!loaderInfo || !runtimeRequest || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
    runtimeRequest->getInstanceProcAddr = rGetInstanceProcAddr;
    return XR_SUCCESS;
}
