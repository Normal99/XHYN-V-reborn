// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
//
// Wraps the BCn layer that libgtav_vulkan.so loads. Everything is forwarded to the
// original layer; only swapchain creation and presentation are intercepted.
#include "layer.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "loader_abi.h"
#include "sgsr.h"

namespace xhyn {
namespace {

std::mutex g_mutex;
std::vector<std::unique_ptr<InstanceInfo>> g_instances;
std::vector<std::unique_ptr<DeviceInfo>> g_devices;
PFN_vkGetInstanceProcAddr g_anyNextGipa = nullptr;

#define PFN_CAST(type, ptr) reinterpret_cast<PFN_##type>(ptr)

InstanceInfo* findInstance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& i : g_instances)
        if (i->handle == instance) return i.get();
    return nullptr;
}

// Physical devices carry the dispatch pointer of their instance as first word.
InstanceInfo* instanceForPhysical(VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_instances.size() == 1) return g_instances.front().get();
    for (auto& i : g_instances)
        if (*reinterpret_cast<void**>(i->handle) == *reinterpret_cast<void**>(physical)) return i.get();
    return nullptr;
}

DeviceInfo* deviceForQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& d : g_devices) {
        std::lock_guard<std::mutex> deviceLock(d->mutex);
        if (d->queueFamily.count(queue)) return d.get();
    }
    return g_devices.size() == 1 ? g_devices.front().get() : nullptr;
}

// Calls fn with the loader link node temporarily removed from a pNext chain, for the
// case where there is no original layer to consume it.
template <class Fn>
auto withoutLink(const void* createInfo, const void* link, Fn&& fn) {
    if (!link) return fn();
    auto* head = const_cast<VkStructHeader*>(static_cast<const VkStructHeader*>(createInfo));
    VkStructHeader* prev = head;
    while (prev && prev->pNext != link) prev = const_cast<VkStructHeader*>(static_cast<const VkStructHeader*>(prev->pNext));
    const void* saved = prev ? prev->pNext : nullptr;
    if (prev) prev->pNext = static_cast<const VkStructHeader*>(link)->pNext;
    auto result = fn();
    if (prev) prev->pNext = saved;
    return result;
}

Config g_config;
bool g_configLoaded = false;

Config currentConfig(bool reload) {
    if (!reload) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_configLoaded) return g_config;
    }
    Config loaded = loadConfig();  // file probing happens outside the lock the present path takes
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config = loaded;
    g_configLoaded = true;
    return g_config;
}

const char* resultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        default: return "other";
    }
}

void logSwapchain(DeviceInfo& d, const VkSwapchainCreateInfoKHR& ci, const Config& config) {
    XLOGI("swapchain create: %ux%u format=%d colorSpace=%d presentMode=%d minImages=%u usage=0x%x flags=0x%x "
          "transform=0x%x alpha=0x%x layers=%u sharing=%d clipped=%d old=%s",
          ci.imageExtent.width, ci.imageExtent.height, ci.imageFormat, ci.imageColorSpace, ci.presentMode,
          ci.minImageCount, ci.imageUsage, ci.flags, ci.preTransform, ci.compositeAlpha, ci.imageArrayLayers,
          ci.imageSharingMode, ci.clipped, ci.oldSwapchain ? "yes" : "no");
    for (auto* n = static_cast<const VkStructHeader*>(ci.pNext); n; n = static_cast<const VkStructHeader*>(n->pNext))
        XLOGI("  swapchain pNext sType=%d", static_cast<int>(n->sType));
    VkSurfaceCapabilitiesKHR caps{};
    if (d.instance->getSurfaceCapabilities &&
        d.instance->getSurfaceCapabilities(d.physical, ci.surface, &caps) == VK_SUCCESS) {
        XLOGI("  surface: current=%ux%u min=%ux%u max=%ux%u images=%u..%u usage=0x%x transforms=0x%x current=0x%x",
              caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width,
              caps.minImageExtent.height, caps.maxImageExtent.width, caps.maxImageExtent.height,
              caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags, caps.supportedTransforms,
              caps.currentTransform);
    }
    if (d.instance->getSurfaceFormats) {
        uint32_t n = 0;
        d.instance->getSurfaceFormats(d.physical, ci.surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        if (n && d.instance->getSurfaceFormats(d.physical, ci.surface, &n, formats.data()) >= 0) {
            std::string list;
            for (uint32_t i = 0; i < n && i < formats.size(); ++i)
                list += " " + std::to_string(formats[i].format) + "/" + std::to_string(formats[i].colorSpace);
            XLOGI("  surface formats (format/colorSpace):%s", list.c_str());
        }
    }
    if (d.instance->getPresentModes) {
        uint32_t n = 0;
        d.instance->getPresentModes(d.physical, ci.surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        if (n && d.instance->getPresentModes(d.physical, ci.surface, &n, modes.data()) >= 0) {
            std::string list;
            for (uint32_t i = 0; i < n && i < modes.size(); ++i) list += " " + std::to_string(modes[i]);
            XLOGI("  present modes:%s", list.c_str());
        }
    }
    (void)config;
}

// ---- instance ------------------------------------------------------------------------

// True when every GPU decodes BC textures natively, so the original BCn layer is not needed.
bool hardwareBcOnAllDevices(PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
    if (getenv("XHYN_UPSCALE_ASSUME_NO_HARDWARE_BC")) return false;  // for host tests
    auto enumerate = PFN_CAST(vkEnumeratePhysicalDevices, gipa(instance, "vkEnumeratePhysicalDevices"));
    auto features = PFN_CAST(vkGetPhysicalDeviceFeatures, gipa(instance, "vkGetPhysicalDeviceFeatures"));
    uint32_t n = 0;
    if (!enumerate || !features || enumerate(instance, &n, nullptr) != VK_SUCCESS || n == 0) return false;
    std::vector<VkPhysicalDevice> devices(n);
    if (enumerate(instance, &n, devices.data()) < 0) return false;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceFeatures f{};
        features(devices[i], &f);
        if (!f.textureCompressionBC) return false;
    }
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                                  VkInstance* out) {
    const Config config = currentConfig(true);
    const InnerLayer& inner = innerLayer();
    auto* link = findLink<LayerInstanceCreateInfo>(ci->pNext, kStructureLoaderInstanceCreateInfo);
    const PFN_vkGetInstanceProcAddr nextGipa =
        link && link->u.pLayerInfo ? link->u.pLayerInfo->pfnNextGetInstanceProcAddr : nullptr;
    if (nextGipa) g_anyNextGipa = nextGipa;

    // Through the original layer (it consumes the link chain), or straight to the next layer.
    auto createWith = [&](bool viaInner) -> VkResult {
        const PFN_vkGetInstanceProcAddr gipa = viaInner ? inner.gipa : nextGipa;
        if (!gipa) return VK_ERROR_INITIALIZATION_FAILED;
        auto create = PFN_CAST(vkCreateInstance, gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!create) return VK_ERROR_INITIALIZATION_FAILED;
        if (viaInner) return create(ci, alloc, out);
        return withoutLink(ci, link, [&] { return create(ci, alloc, out); });
    };

    // The original layer is only worth its cost where a GPU lacks native BC decoding.
    bool viaInner = inner.gipa && (config.bcn == BcnPolicy::Inner || !nextGipa);
    VkResult r = createWith(viaInner);
    if (r != VK_SUCCESS) return r;
    const char* route = viaInner      ? "through the original BCn layer"
                        : inner.gipa ? "hardware BC, original BCn layer bypassed"
                                     : "original BCn layer not present (pass-through)";
    if (!viaInner && inner.gipa && config.bcn == BcnPolicy::Auto && !hardwareBcOnAllDevices(nextGipa, *out)) {
        auto destroy = PFN_CAST(vkDestroyInstance, nextGipa(*out, "vkDestroyInstance"));
        if (destroy) destroy(*out, alloc);
        r = createWith(true);
        if (r != VK_SUCCESS) return r;
        viaInner = true;
        route = "through the original BCn layer (a GPU lacks native BC)";
    }
    if (!viaInner && inner.gipa) {
        // InitializeBcn announced an active decoder; nothing decodes on this path.
        setenv("GTAV_BCN_DECODE_ACTIVE", "0", 1);
    }
    useInnerDiagnostics(viaInner);  // the original layer only has state for instances created through it

    auto info = std::make_unique<InstanceInfo>();
    info->handle = *out;
    info->nextGipa = nextGipa;
    info->bypass = !viaInner;
    info->innerGipa = viaInner ? inner.gipa : nextGipa;
    const PFN_vkGetInstanceProcAddr lower = nextGipa ? nextGipa : info->innerGipa;
    info->innerDestroy = PFN_CAST(vkDestroyInstance, info->innerGipa(*out, "vkDestroyInstance"));
    info->innerCreateDevice = PFN_CAST(vkCreateDevice, info->innerGipa(*out, "vkCreateDevice"));
    info->getProperties = PFN_CAST(vkGetPhysicalDeviceProperties, lower(*out, "vkGetPhysicalDeviceProperties"));
    info->getMemoryProperties = PFN_CAST(vkGetPhysicalDeviceMemoryProperties, lower(*out, "vkGetPhysicalDeviceMemoryProperties"));
    info->getQueueFamilies = PFN_CAST(vkGetPhysicalDeviceQueueFamilyProperties, lower(*out, "vkGetPhysicalDeviceQueueFamilyProperties"));
    info->getFormatProperties = PFN_CAST(vkGetPhysicalDeviceFormatProperties, lower(*out, "vkGetPhysicalDeviceFormatProperties"));
    info->getImageFormatProperties = PFN_CAST(vkGetPhysicalDeviceImageFormatProperties, lower(*out, "vkGetPhysicalDeviceImageFormatProperties"));
    info->getSurfaceCapabilities = PFN_CAST(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, lower(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    info->getSurfaceCapabilities2 = PFN_CAST(vkGetPhysicalDeviceSurfaceCapabilities2KHR, lower(*out, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    info->getSurfaceFormats = PFN_CAST(vkGetPhysicalDeviceSurfaceFormatsKHR, lower(*out, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    info->getPresentModes = PFN_CAST(vkGetPhysicalDeviceSurfacePresentModesKHR, lower(*out, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    XLOGI("instance created, mode=%s, bcn=%s: %s; settings: %s", modeName(config.mode), bcnName(config.bcn), route,
          config.source.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances.push_back(std::move(info));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    InstanceInfo* info = findInstance(instance);
    if (!info) return;
    PFN_vkDestroyInstance destroy = info->innerDestroy;
    if (destroy) destroy(instance, alloc);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances.erase(std::remove_if(g_instances.begin(), g_instances.end(),
                                     [&](const std::unique_ptr<InstanceInfo>& i) { return i->handle == instance; }),
                      g_instances.end());
}

// ---- device --------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* ci,
                                                const VkAllocationCallbacks* alloc, VkDevice* out) {
    InstanceInfo* inst = instanceForPhysical(physical);
    if (!inst || !inst->innerCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link = findLink<LayerDeviceCreateInfo>(ci->pNext, kStructureLoaderDeviceCreateInfo);
    // The original layer advances the link chain while creating the device; copy first.
    const PFN_vkGetDeviceProcAddr nextGdpa =
        link && link->u.pLayerInfo ? link->u.pLayerInfo->pfnNextGetDeviceProcAddr : nullptr;
    const InnerLayer& inner = innerLayer();
    const bool viaInner = !inst->bypass && inner.gdpa;

    VkResult r;
    if (viaInner) r = inst->innerCreateDevice(physical, ci, alloc, out);
    else r = withoutLink(ci, link, [&] { return inst->innerCreateDevice(physical, ci, alloc, out); });
    if (r != VK_SUCCESS) return r;

    auto d = std::make_unique<DeviceInfo>();
    d->handle = *out;
    d->physical = physical;
    d->instance = inst;
    d->nextGdpa = nextGdpa;
    d->innerGdpa = viaInner ? inner.gdpa : nextGdpa;
    PFN_vkGetDeviceProcAddr lower = nextGdpa ? nextGdpa : d->innerGdpa;
    if (!lower) lower = PFN_CAST(vkGetDeviceProcAddr, inst->nextGipa ? inst->nextGipa(inst->handle, "vkGetDeviceProcAddr") : nullptr);
    if (!d->innerGdpa) d->innerGdpa = lower;
    if (!lower) {
        XLOGW("device created without a usable vkGetDeviceProcAddr; upscaling unavailable");
    } else {
        d->fnComplete = true;
#define X(name)                                                  \
    d->fn.name = PFN_CAST(name, lower(*out, #name));             \
    if (!d->fn.name) {                                           \
        XLOGW("device function %s missing", #name);              \
        d->fnComplete = false;                                   \
    }
        XHYN_DEVICE_FUNCTIONS(X)
#undef X
        d->innerDestroy = PFN_CAST(vkDestroyDevice, d->innerGdpa(*out, "vkDestroyDevice"));
        d->innerGetQueue = PFN_CAST(vkGetDeviceQueue, d->innerGdpa(*out, "vkGetDeviceQueue"));
        d->innerGetQueue2 = PFN_CAST(vkGetDeviceQueue2, d->innerGdpa(*out, "vkGetDeviceQueue2"));
        d->innerCreateSwapchain = PFN_CAST(vkCreateSwapchainKHR, d->innerGdpa(*out, "vkCreateSwapchainKHR"));
        d->innerDestroySwapchain = PFN_CAST(vkDestroySwapchainKHR, d->innerGdpa(*out, "vkDestroySwapchainKHR"));
        d->innerGetImages = PFN_CAST(vkGetSwapchainImagesKHR, d->innerGdpa(*out, "vkGetSwapchainImagesKHR"));
        d->innerPresent = PFN_CAST(vkQueuePresentKHR, d->innerGdpa(*out, "vkQueuePresentKHR"));
        d->innerAcquire = PFN_CAST(vkAcquireNextImageKHR, d->innerGdpa(*out, "vkAcquireNextImageKHR"));
        if (!d->innerCreateSwapchain || !d->innerDestroySwapchain || !d->innerGetImages || !d->innerPresent)
            d->fnComplete = false;  // device was created without VK_KHR_swapchain
    }
    // Record every queue now, so presentation does not depend on the game's use of vkGetDeviceQueue.
    if (d->innerGetQueue) {
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
            const VkDeviceQueueCreateInfo& q = ci->pQueueCreateInfos[i];
            if (q.flags) continue;  // protected queues need vkGetDeviceQueue2
            for (uint32_t index = 0; index < q.queueCount; ++index) {
                VkQueue queue = VK_NULL_HANDLE;
                d->innerGetQueue(*out, q.queueFamilyIndex, index, &queue);
                if (queue) d->queueFamily[queue] = q.queueFamilyIndex;
            }
        }
    }
    if (inst->getProperties) inst->getProperties(physical, &d->properties);
    if (inst->getMemoryProperties) inst->getMemoryProperties(physical, &d->memory);
    if (inst->getQueueFamilies) {
        uint32_t n = 0;
        inst->getQueueFamilies(physical, &n, nullptr);
        d->families.resize(n);
        inst->getQueueFamilies(physical, &n, d->families.data());
    }
    XLOGI("device created: %s (api %u.%u.%u, driver 0x%x), upscaler %s", d->properties.deviceName,
          VK_VERSION_MAJOR(d->properties.apiVersion), VK_VERSION_MINOR(d->properties.apiVersion),
          VK_VERSION_PATCH(d->properties.apiVersion), d->properties.driverVersion,
          d->fnComplete ? "can attach" : "unavailable on this device");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices.push_back(std::move(d));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DeviceInfo* d = findDevice(device);
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->chains.clear();  // destroys any upscaler resources while the device is still alive
    }
    PFN_vkDestroyDevice destroy = d->innerDestroy;
    if (destroy) destroy(device, alloc);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices.erase(std::remove_if(g_devices.begin(), g_devices.end(),
                                   [&](const std::unique_ptr<DeviceInfo>& i) { return i->handle == device; }),
                    g_devices.end());
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceInfo* d = findDevice(device);
    if (!d || !d->innerGetQueue) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    d->innerGetQueue(device, family, index, queue);
    if (*queue) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->queueFamily[*queue] = family;
    }
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceInfo* d = findDevice(device);
    if (!d || !d->innerGetQueue2) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    d->innerGetQueue2(device, info, queue);
    if (*queue) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->queueFamily[*queue] = info->queueFamilyIndex;
    }
}

// ---- render size ---------------------------------------------------------------------

// DXVK sizes its swapchain from the surface's current extent and stretches the game's frame to
// fit. Reporting a smaller extent gets the upscaler the game's real pixels; the real swapchain
// still uses the surface's own size (planFx).
void reportRenderSize(VkSurfaceCapabilitiesKHR& caps) {
    const Config config = currentConfig(false);
    if (config.mode != Mode::Sgsr1) return;
    const VkExtent2D surface = surfaceExtent(config, caps);
    if (surface.width == UINT32_MAX) return;
    VkExtent2D render{};
    if (config.renderWidth && config.renderHeight) {
        render = {std::min(config.renderWidth, surface.width), std::min(config.renderHeight, surface.height)};
    } else {
        render.width = static_cast<uint32_t>(std::lround(surface.width / config.scale));
        render.height = static_cast<uint32_t>(std::lround(surface.height / config.scale));
    }
    caps.currentExtent = {std::max(render.width, caps.minImageExtent.width), std::max(render.height, caps.minImageExtent.height)};
    static std::atomic<uint64_t> logged{0};
    const uint64_t key = (static_cast<uint64_t>(surface.width) << 48) | (static_cast<uint64_t>(surface.height) << 32) |
                         (static_cast<uint64_t>(caps.currentExtent.width) << 16) | caps.currentExtent.height;
    if (logged.exchange(key) != key)
        XLOGI("render size: reporting %ux%u for a %ux%u surface", caps.currentExtent.width, caps.currentExtent.height,
              surface.width, surface.height);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetSurfaceCapabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                          VkSurfaceCapabilitiesKHR* caps) {
    InstanceInfo* inst = instanceForPhysical(physical);
    if (!inst || !inst->getSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = inst->getSurfaceCapabilities(physical, surface, caps);
    if (r == VK_SUCCESS) reportRenderSize(*caps);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetSurfaceCapabilities2(VkPhysicalDevice physical,
                                                           const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                           VkSurfaceCapabilities2KHR* caps) {
    InstanceInfo* inst = instanceForPhysical(physical);
    if (!inst || !inst->getSurfaceCapabilities2) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = inst->getSurfaceCapabilities2(physical, info, caps);
    if (r == VK_SUCCESS) reportRenderSize(caps->surfaceCapabilities);
    return r;
}

// ---- swapchain -----------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                   const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceInfo* d = findDevice(device);
    if (!d || !d->innerCreateSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    const Config config = currentConfig(true);
    if (config.mode == Mode::Off) return d->innerCreateSwapchain(device, ci, alloc, out);
    logSwapchain(*d, *ci, config);

    auto track = [&](VkSwapchainKHR chain, VkExtent2D extent, std::unique_ptr<SwapchainFx> fx) {
        auto state = std::make_unique<ChainState>();
        state->format = ci->imageFormat;
        state->extent = extent;
        state->logInterval = config.logInterval;
        state->fx = std::move(fx);
        std::lock_guard<std::mutex> lock(d->mutex);
        d->chains[reinterpret_cast<uint64_t>(chain)] = std::move(state);
    };

    // Everything that can reject the upscaler happens before the real swapchain exists: creating
    // it retires the game's oldSwapchain, after which the game's own request can no longer be
    // repeated with that oldSwapchain.
    FxPlan plan;
    std::unique_ptr<SwapchainFx> fx;
    if (config.mode == Mode::Sgsr1) {
        plan = planFx(*d, *ci, config);
        if (!plan.ok) {
            XLOGW("sgsr1 not applied to this swapchain: %s", plan.reason);
        } else {
            fx = SwapchainFx::create(*d, *ci, plan.out, config);
            if (!fx) {
                plan.ok = false;
                XLOGW("sgsr1 not applied to this swapchain: setup failed");
            }
        }
    }
    if (!plan.ok) {
        VkResult r = d->innerCreateSwapchain(device, ci, alloc, out);
        if (r == VK_SUCCESS) track(*out, ci->imageExtent, nullptr);
        return r;
    }

    VkSwapchainCreateInfoKHR real = *ci;
    real.imageExtent = plan.out;
    real.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (config.dumpFrame) real.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    real.minImageCount = plan.minImages;
    // Mailbox would throw away the generated frame when the real one follows it. The game's list of
    // allowed present modes has to go too, as FIFO may not be on it.
    const void* presentModes = nullptr;
    if (config.frameGen) {
        real.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto* n = static_cast<const VkStructHeader*>(ci->pNext); n; n = static_cast<const VkStructHeader*>(n->pNext))
            if (static_cast<int>(n->sType) == 1000275002) presentModes = n;  // VkSwapchainPresentModesCreateInfoEXT
    }
    VkResult r = withoutLink(&real, presentModes, [&] { return d->innerCreateSwapchain(device, &real, alloc, out); });
    if (r != VK_SUCCESS) {
        // Last resort: the game's own request (its oldSwapchain may already count as retired).
        XLOGW("real swapchain at %ux%u failed (%d); retrying at the game's size", plan.out.width, plan.out.height, r);
        fx.reset();
        r = d->innerCreateSwapchain(device, ci, alloc, out);
        if (r == VK_SUCCESS) track(*out, ci->imageExtent, nullptr);
        return r;
    }
    if (!fx->attach(*out, *ci)) {
        // Replace the enlarged swapchain with one of the game's size. It is still current, so it is a
        // valid oldSwapchain and the game's own oldSwapchain is not needed again.
        fx.reset();
        VkSwapchainCreateInfoKHR retry = *ci;
        retry.oldSwapchain = *out;
        VkSwapchainKHR plain = VK_NULL_HANDLE;
        r = d->innerCreateSwapchain(device, &retry, alloc, &plain);
        d->innerDestroySwapchain(device, *out, alloc);
        if (r == VK_SUCCESS) {
            *out = plain;
            track(plain, ci->imageExtent, nullptr);
        }
        return r;
    }
    track(*out, ci->imageExtent, std::move(fx));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroySwapchain(VkDevice device, VkSwapchainKHR chain, const VkAllocationCallbacks* alloc) {
    DeviceInfo* d = findDevice(device);
    if (!d || !d->innerDestroySwapchain) return;
    std::unique_ptr<ChainState> state;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->chains.find(reinterpret_cast<uint64_t>(chain));
        if (it != d->chains.end()) {
            state = std::move(it->second);
            d->chains.erase(it);
        }
    }
    state.reset();  // upscaler resources first, then the real swapchain
    d->innerDestroySwapchain(device, chain, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetSwapchainImages(VkDevice device, VkSwapchainKHR chain, uint32_t* count, VkImage* images) {
    DeviceInfo* d = findDevice(device);
    if (!d || !d->innerGetImages) return VK_ERROR_INITIALIZATION_FAILED;
    SwapchainFx* fx = nullptr;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->chains.find(reinterpret_cast<uint64_t>(chain));
        if (it != d->chains.end()) fx = it->second->fx.get();
    }
    return fx ? fx->getImages(count, images) : d->innerGetImages(device, chain, count, images);
}

// Presents the generated frame, then the game's own frame on an extra image. The caller's pNext (such
// as a present fence) stays with the first present, so it still fires if the second one has to be dropped.
VkResult presentWithFrameGen(DeviceInfo* d, VkQueue queue, uint32_t family, const VkPresentInfoKHR* pi, SwapchainFx* fx) {
    VkSemaphore signal = VK_NULL_HANDLE;
    const uint32_t source = pi->pImageIndices[0];
    if (fx->submitGenerated(queue, family, source, pi->waitSemaphoreCount, pi->pWaitSemaphores, &signal) != VK_SUCCESS)
        return VK_ERROR_DEVICE_LOST;
    VkPresentInfoKHR first = *pi;
    first.waitSemaphoreCount = 1;
    first.pWaitSemaphores = &signal;
    first.pResults = nullptr;
    VkResult result = d->innerPresent(queue, &first);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
    fx->paceRealFrame();

    uint32_t index = 0;
    VkSemaphore acquired = VK_NULL_HANDLE;
    const VkResult ar = fx->acquireReal(&index, &acquired);
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY) return result;  // this one frame is shown as generated only
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return ar;
    if (fx->submit(queue, family, source, index, 1, &acquired, &signal) != VK_SUCCESS) return VK_ERROR_DEVICE_LOST;
    VkPresentInfoKHR second{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    second.waitSemaphoreCount = 1;
    second.pWaitSemaphores = &signal;
    second.swapchainCount = 1;
    second.pSwapchains = pi->pSwapchains;
    second.pImageIndices = &index;
    const VkResult last = d->innerPresent(queue, &second);
    return last == VK_SUCCESS ? result : last;
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresent(VkQueue queue, const VkPresentInfoKHR* pi) {
    DeviceInfo* d = deviceForQueue(queue);
    if (!d || !d->innerPresent) return VK_ERROR_DEVICE_LOST;

    struct Item {
        ChainState* state;
        SwapchainFx* fx;
    };
    std::vector<Item> items(pi->swapchainCount);
    bool anyFx = false;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
            auto it = d->chains.find(reinterpret_cast<uint64_t>(pi->pSwapchains[i]));
            if (it == d->chains.end()) continue;
            items[i] = {it->second.get(), it->second->fx.get()};
            anyFx = anyFx || items[i].fx;
        }
    }

    VkResult result;
    std::vector<VkSemaphore> signals;
    if (!anyFx) {
        result = d->innerPresent(queue, pi);
    } else {
        uint32_t family = 0;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            auto it = d->queueFamily.find(queue);
            if (it != d->queueFamily.end()) {
                family = it->second;
            } else {
                for (uint32_t f = 0; f < d->families.size(); ++f)
                    if (d->families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = f; break; }
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    XLOGW("present queue family unknown; assuming %u", family);
                }
            }
        }
        if (pi->swapchainCount == 1 && items[0].fx->frameGenReady()) {
            result = presentWithFrameGen(d, queue, family, pi, items[0].fx);
            if (pi->pResults) pi->pResults[0] = result;
            if (items[0].state->stats.tick(items[0].state->logInterval)) {
                uint32_t samples = 0, genSamples = 0;
                const double gpuMs = items[0].fx->takeGpuMs(&samples);
                const double genMs = items[0].fx->takeGenMs(&genSamples);
                items[0].state->stats.report("sgsr1+fg", items[0].state->extent, gpuMs, samples, genMs, genSamples);
            }
            return result;
        }
        bool first = true;
        VkResult failed = VK_SUCCESS;
        for (uint32_t i = 0; i < pi->swapchainCount && failed == VK_SUCCESS; ++i) {
            if (!items[i].fx) continue;
            VkSemaphore signal = VK_NULL_HANDLE;
            // The game's wait semaphores are consumed by the first upscale submission.
            failed = items[i].fx->submit(queue, family, pi->pImageIndices[i], pi->pImageIndices[i], first ? pi->waitSemaphoreCount : 0,
                                         first ? pi->pWaitSemaphores : nullptr, &signal);
            first = false;
            if (failed == VK_SUCCESS) signals.push_back(signal);
        }
        if (failed != VK_SUCCESS) {
            // Nothing can be presented from here; DEVICE_LOST is the only error that fits.
            XLOGE("upscale submit failed (%d); reporting VK_ERROR_DEVICE_LOST", static_cast<int>(failed));
            result = VK_ERROR_DEVICE_LOST;
            if (pi->pResults)
                for (uint32_t i = 0; i < pi->swapchainCount; ++i) pi->pResults[i] = result;
        } else {
            VkPresentInfoKHR forwarded = *pi;
            forwarded.waitSemaphoreCount = static_cast<uint32_t>(signals.size());
            forwarded.pWaitSemaphores = signals.data();
            result = d->innerPresent(queue, &forwarded);
        }
    }

    for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
        ChainState* state = items[i].state;
        if (!state || !state->stats.tick(state->logInterval)) continue;
        uint32_t samples = 0;
        const double gpuMs = items[i].fx ? items[i].fx->takeGpuMs(&samples) : 0.0;
        state->stats.report(items[i].fx ? "sgsr1" : "probe", state->extent, gpuMs, samples, 0.0, 0);
    }
    static bool warned = false;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && !warned) {
        warned = true;
        XLOGW("vkQueuePresentKHR returned %s (%d)", resultName(result), result);
    }
    return result;
}

// ---- lookup tables -------------------------------------------------------------------

struct Hook {
    const char* name;
    PFN_vkVoidFunction fn;
};

#define HOOK(fnName) {#fnName, reinterpret_cast<PFN_vkVoidFunction>(hook##fnName)}

const Hook kDeviceHooks[] = {
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(hookDestroyDevice)},
    {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceQueue)},
    {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceQueue2)},
    {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(hookCreateSwapchain)},
    {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(hookDestroySwapchain)},
    {"vkGetSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(hookGetSwapchainImages)},
    {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(hookQueuePresent)},
};

PFN_vkVoidFunction deviceHook(const char* name) {
    for (const Hook& h : kDeviceHooks)
        if (!strcmp(h.name, name)) return h.fn;
    return nullptr;
}

// DXVK calls these two without checking the pointers it got, and Adreno drivers return null
// for them. Sample locations only matter when multisampling, so doing nothing is safe with MSAA off.
VKAPI_ATTR void VKAPI_CALL standInSetSampleLocationsEnable(VkCommandBuffer, VkBool32) {}
VKAPI_ATTR void VKAPI_CALL standInSetSampleLocations(VkCommandBuffer, const VkSampleLocationsInfoEXT*) {}

const Hook kStandIns[] = {
    {"vkCmdSetSampleLocationsEnableEXT", reinterpret_cast<PFN_vkVoidFunction>(standInSetSampleLocationsEnable)},
    {"vkCmdSetSampleLocationsEXT", reinterpret_cast<PFN_vkVoidFunction>(standInSetSampleLocations)},
};

PFN_vkVoidFunction orStandIn(const char* name, PFN_vkVoidFunction fn) {
    if (fn) return fn;
    for (const Hook& h : kStandIns) {
        if (!strcmp(h.name, name)) {
            XLOGW("driver has no %s; using a stand-in that does nothing", name);
            return h.fn;
        }
    }
    if (!strncmp(name, "vkCmd", 5)) XLOGI("driver has no %s", name);
    return nullptr;
}

}  // namespace

DeviceInfo* findDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& d : g_devices)
        if (d->handle == device) return d.get();
    return nullptr;
}

const InnerLayer& innerLayer() {
    static const InnerLayer inner = [] {
        InnerLayer l;
        const char* env = getenv("XHYN_UPSCALE_INNER");
        const char* name = env && *env ? env : "libxhyn_bcn.so";
        void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            XLOGW("original BCn layer %s not loaded (%s); running without it", name, dlerror());
            return l;
        }
        l.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "BCnLayer_GetInstanceProcAddr"));
        l.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(handle, "BCnLayer_GetDeviceProcAddr"));
        l.diagnostics = dlsym(handle, "BCnLayer_Diagnostics");
        if (!l.gipa || !l.gdpa) {
            XLOGE("%s lacks BCnLayer_Get*ProcAddr; ignoring it", name);
            l.gipa = nullptr;
            l.gdpa = nullptr;
        } else {
            XLOGI("original BCn layer loaded from %s", name);
        }
        return l;
    }();
    return inner;
}

PFN_vkVoidFunction layerGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
    if (!strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyInstance);
    if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(layerGetInstanceProcAddr);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
    if (PFN_vkVoidFunction hook = deviceHook(name)) return hook;
    InstanceInfo* info = instance ? findInstance(instance) : nullptr;
    if (info && !strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") && info->getSurfaceCapabilities)
        return reinterpret_cast<PFN_vkVoidFunction>(hookGetSurfaceCapabilities);
    if (info && !strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") && info->getSurfaceCapabilities2)
        return reinterpret_cast<PFN_vkVoidFunction>(hookGetSurfaceCapabilities2);
    if (info) return orStandIn(name, info->innerGipa(instance, name));
    const InnerLayer& inner = innerLayer();
    if (inner.gipa) return orStandIn(name, inner.gipa(instance, name));
    return orStandIn(name, g_anyNextGipa ? g_anyNextGipa(instance, name) : nullptr);
}

PFN_vkVoidFunction layerGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
    if (PFN_vkVoidFunction hook = deviceHook(name)) return hook;
    DeviceInfo* d = device ? findDevice(device) : nullptr;
    if (d && d->innerGdpa) return orStandIn(name, d->innerGdpa(device, name));
    const InnerLayer& inner = innerLayer();
    return orStandIn(name, inner.gdpa ? inner.gdpa(device, name) : nullptr);
}

bool PresentStats::tick(uint32_t logInterval) {
    const auto now = std::chrono::steady_clock::now();
    if (frames > 0) {
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        sumMs += ms;
        minMs = std::min(minMs, ms);
        maxMs = std::max(maxMs, ms);
        ++windowFrames;
    }
    last = now;
    ++frames;
    return logInterval && windowFrames >= logInterval;
}

void PresentStats::report(const char* label, VkExtent2D extent, double gpuMs, uint32_t gpuSamples, double genMs, uint32_t genSamples) {
    const double mean = windowFrames ? sumMs / windowFrames : 0.0;
    char gpu[96] = "";
    int used = 0;
    if (gpuSamples) used = snprintf(gpu, sizeof gpu, ", upscale GPU %.3f ms", gpuMs);
    if (genSamples) snprintf(gpu + used, sizeof gpu - used, ", generated frame GPU %.3f ms", genMs);
    XLOGI("[%s] frame %llu: %llu presents, avg %.2f ms (%.1f fps), min %.2f, max %.2f, game size %ux%u%s", label,
          static_cast<unsigned long long>(frames), static_cast<unsigned long long>(windowFrames), mean,
          mean > 0 ? 1000.0 / mean : 0.0, minMs, maxMs, extent.width, extent.height, gpu);
    windowFrames = 0;
    sumMs = 0;
    minMs = 1e9;
    maxMs = 0;
}

}  // namespace xhyn
