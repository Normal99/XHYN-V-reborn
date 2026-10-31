// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
//
// Host test driver. It plays the part of libgtav_vulkan.so (building loader link
// chains and calling the shim's three exports) and of DXVK (creating a swapchain,
// rendering into the images it gets back, presenting).
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/loader_abi.h"

using namespace xhyn;

namespace {

struct Options {
    std::string shim;
    uint32_t width = 640, height = 360;
    bool bgra = true, srgb = false, mutableFormat = false, validate = false, recreate = false;
    std::string pattern = "edges";
    std::string image;  // optional P6 PPM with exactly the swapchain size, used instead of a pattern
    std::vector<std::string> ignoreVuids;
    uint32_t frames = 6;
    bool checkDiagnostics = false;
    bool diagnosticsBypassed = false;
    bool sizeFromCaps = false;  // size the swapchain from the surface's current extent, as DXVK does
};

int g_validationErrors = 0;
std::vector<std::string> g_ignoredIds;
int g_failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "HARNESS FAIL: " __VA_ARGS__);      \
            fprintf(stderr, "\n");                              \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

#define VK_OK(call)                                                                  \
    do {                                                                             \
        VkResult r_ = (call);                                                        \
        if (r_ != VK_SUCCESS) {                                                      \
            fprintf(stderr, "HARNESS FAIL: %s returned %d (line %d)\n", #call, r_, __LINE__); \
            exit(2);                                                                 \
        }                                                                            \
    } while (0)

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        for (const auto& id : g_ignoredIds)
            if (data->pMessageIdName && id == data->pMessageIdName) {
                fprintf(stderr, "VALIDATION (ignored %s)\n", id.c_str());
                return VK_FALSE;
            }
        ++g_validationErrors;
        fprintf(stderr, "VALIDATION %s: %s\n", (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARNING",
                data->pMessage);
    }
    return VK_FALSE;
}

// Test image, RGB 0..255 at (x, y) in a w*h picture.
void patternPixel(const std::string& pattern, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t rgb[3], uint32_t frame = 0) {
    if (pattern == "moving") {  // a white box that moves 16 pixels right every frame
        const uint32_t left = 100 + 16 * frame;
        const bool box = x >= left && x < left + 48 && y >= 100 && y < 148;
        rgb[0] = rgb[1] = rgb[2] = box ? 255 : 30;
        return;
    }
    if (pattern == "flat") {
        rgb[0] = 100; rgb[1] = 150; rgb[2] = 200;
        return;
    }
    // Smooth gradient background.
    double r = 255.0 * x / (w - 1), g = 255.0 * y / (h - 1), b = 128.0;
    const double cx = w * 0.5, cy = h * 0.5;
    // Checkerboard in the middle quarter.
    if (x > w / 4 && x < w * 3 / 4 && y > h / 4 && y < h * 3 / 4) {
        const bool on = (((x - w / 4) / 8) + ((y - h / 4) / 8)) & 1;
        r = g = b = on ? 235 : 20;
    }
    // Anti-aliased-free diagonal line and a ring.
    if (std::abs(static_cast<int>(x) - static_cast<int>(y * w / h)) < 2) r = g = b = 255;
    const double d = std::hypot(x - cx, y - cy);
    if (std::abs(d - h * 0.42) < 1.5) { r = 255; g = 40; b = 40; }
    // Thin horizontal and vertical lines.
    if (y == h / 8 || x == w / 8) r = g = b = 0;
    rgb[0] = static_cast<uint8_t>(r);
    rgb[1] = static_cast<uint8_t>(g);
    rgb[2] = static_cast<uint8_t>(b);
}

struct Vk {
    void* loader = nullptr;
    void* shim = nullptr;
    PFN_vkGetInstanceProcAddr loaderGipa = nullptr;
    PFN_vkGetInstanceProcAddr shimGipa = nullptr;
    PFN_vkGetDeviceProcAddr shimGdpa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
};

template <class T>
T instanceFn(Vk& vk, const char* name) {
    auto fn = reinterpret_cast<T>(vk.shimGipa(vk.instance, name));
    if (!fn) { fprintf(stderr, "HARNESS FAIL: %s not available\n", name); exit(2); }
    return fn;
}

template <class T>
T deviceFn(Vk& vk, const char* name) {
    auto fn = reinterpret_cast<T>(vk.shimGdpa(vk.device, name));
    if (!fn) { fprintf(stderr, "HARNESS FAIL: %s not available via device lookup\n", name); exit(2); }
    return fn;
}

uint32_t findMemory(const Vk& vk, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    exit(2);
}

// Renders one swapchain's worth of frames the way a game would.
void runSwapchain(Vk& vk, const Options& o, uint32_t width, uint32_t height, VkSwapchainKHR old, VkSwapchainKHR* outChain,
                  bool destroyAtEnd) {
    auto createSwapchain = deviceFn<PFN_vkCreateSwapchainKHR>(vk, "vkCreateSwapchainKHR");
    auto destroySwapchain = deviceFn<PFN_vkDestroySwapchainKHR>(vk, "vkDestroySwapchainKHR");
    auto getImages = deviceFn<PFN_vkGetSwapchainImagesKHR>(vk, "vkGetSwapchainImagesKHR");
    auto acquire = deviceFn<PFN_vkAcquireNextImageKHR>(vk, "vkAcquireNextImageKHR");
    auto present = deviceFn<PFN_vkQueuePresentKHR>(vk, "vkQueuePresentKHR");
    auto createSemaphore = deviceFn<PFN_vkCreateSemaphore>(vk, "vkCreateSemaphore");
    auto destroySemaphore = deviceFn<PFN_vkDestroySemaphore>(vk, "vkDestroySemaphore");
    auto createFence = deviceFn<PFN_vkCreateFence>(vk, "vkCreateFence");
    auto destroyFence = deviceFn<PFN_vkDestroyFence>(vk, "vkDestroyFence");
    auto waitFences = deviceFn<PFN_vkWaitForFences>(vk, "vkWaitForFences");
    auto resetFences = deviceFn<PFN_vkResetFences>(vk, "vkResetFences");
    auto createPool = deviceFn<PFN_vkCreateCommandPool>(vk, "vkCreateCommandPool");
    auto destroyPool = deviceFn<PFN_vkDestroyCommandPool>(vk, "vkDestroyCommandPool");
    auto allocCmd = deviceFn<PFN_vkAllocateCommandBuffers>(vk, "vkAllocateCommandBuffers");
    auto beginCmd = deviceFn<PFN_vkBeginCommandBuffer>(vk, "vkBeginCommandBuffer");
    auto endCmd = deviceFn<PFN_vkEndCommandBuffer>(vk, "vkEndCommandBuffer");
    auto resetCmd = deviceFn<PFN_vkResetCommandBuffer>(vk, "vkResetCommandBuffer");
    auto barrierFn = deviceFn<PFN_vkCmdPipelineBarrier>(vk, "vkCmdPipelineBarrier");
    auto copyBuf = deviceFn<PFN_vkCmdCopyBufferToImage>(vk, "vkCmdCopyBufferToImage");
    auto submit = deviceFn<PFN_vkQueueSubmit>(vk, "vkQueueSubmit");
    auto createBuffer = deviceFn<PFN_vkCreateBuffer>(vk, "vkCreateBuffer");
    auto destroyBuffer = deviceFn<PFN_vkDestroyBuffer>(vk, "vkDestroyBuffer");
    auto bufReq = deviceFn<PFN_vkGetBufferMemoryRequirements>(vk, "vkGetBufferMemoryRequirements");
    auto allocMem = deviceFn<PFN_vkAllocateMemory>(vk, "vkAllocateMemory");
    auto freeMem = deviceFn<PFN_vkFreeMemory>(vk, "vkFreeMemory");
    auto bindBuf = deviceFn<PFN_vkBindBufferMemory>(vk, "vkBindBufferMemory");
    auto mapMem = deviceFn<PFN_vkMapMemory>(vk, "vkMapMemory");
    auto unmapMem = deviceFn<PFN_vkUnmapMemory>(vk, "vkUnmapMemory");
    auto idle = deviceFn<PFN_vkDeviceWaitIdle>(vk, "vkDeviceWaitIdle");
    auto getCaps = instanceFn<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(vk, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    VkSurfaceCapabilitiesKHR caps;
    VK_OK(getCaps(vk.physical, vk.surface, &caps));
    if (o.sizeFromCaps) {
        CHECK(caps.currentExtent.width != UINT32_MAX, "surface reports no current extent");
        width = caps.currentExtent.width;
        height = caps.currentExtent.height;
    }

    VkFormat format = o.bgra ? (o.srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM)
                             : (o.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
    VkFormat twin = o.srgb ? (o.bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM) : format;
    VkFormat listFormats[2] = {format, twin};
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    list.viewFormatCount = 2;
    list.pViewFormats = listFormats;

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.pNext = o.mutableFormat ? &list : nullptr;
    sc.flags = o.mutableFormat ? VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR : 0;
    sc.surface = vk.surface;
    sc.minImageCount = std::max(3u, caps.minImageCount);
    sc.imageFormat = format;
    sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc.imageExtent = {width, height};
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    sc.oldSwapchain = old;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    VK_OK(createSwapchain(vk.device, &sc, nullptr, &chain));

    uint32_t count = 0;
    VK_OK(getImages(vk.device, chain, &count, nullptr));
    std::vector<VkImage> images(count);
    VK_OK(getImages(vk.device, chain, &count, images.data()));
    // Two-call API behavior with a short array must report VK_INCOMPLETE.
    uint32_t one = 1;
    VkImage first;
    CHECK(getImages(vk.device, chain, &one, &first) == VK_INCOMPLETE || count == 1, "short image query should be VK_INCOMPLETE");
    fprintf(stderr, "HARNESS: swapchain %ux%u has %u images\n", width, height, count);

    // Pattern in a staging buffer (RGBA/BGRA ordering matches the swapchain format).
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = bytes;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging;
    VK_OK(createBuffer(vk.device, &bc, nullptr, &staging));
    VkMemoryRequirements req;
    bufReq(vk.device, staging, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stagingMem;
    VK_OK(allocMem(vk.device, &ai, nullptr, &stagingMem));
    VK_OK(bindBuf(vk.device, staging, stagingMem, 0));
    void* mapped;
    VK_OK(mapMem(vk.device, stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped));
    auto* px = static_cast<uint8_t*>(mapped);
    std::vector<uint8_t> loaded;
    if (!o.image.empty()) {
        FILE* f = fopen(o.image.c_str(), "rb");
        unsigned iw = 0, ih = 0;
        if (!f || fscanf(f, "P6 %u %u 255", &iw, &ih) != 2 || iw != width || ih != height) {
            fprintf(stderr, "HARNESS FAIL: --image must be a P6 PPM of %ux%u\n", width, height);
            exit(2);
        }
        fgetc(f);
        loaded.resize(static_cast<size_t>(width) * height * 3);
        if (fread(loaded.data(), 1, loaded.size(), f) != loaded.size()) exit(2);
        fclose(f);
    }
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t rgb[3];
            if (!loaded.empty()) memcpy(rgb, &loaded[(static_cast<size_t>(y) * width + x) * 3], 3);
            else patternPixel(o.pattern, x, y, width, height, rgb);
            uint8_t* p = px + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = o.bgra ? rgb[2] : rgb[0];
            p[1] = rgb[1];
            p[2] = o.bgra ? rgb[0] : rgb[2];
            p[3] = 255;
        }
    unmapMem(vk.device, stagingMem);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk.family;
    VkCommandPool pool;
    VK_OK(createPool(vk.device, &pci, nullptr, &pool));
    const uint32_t ring = 3;
    std::vector<VkCommandBuffer> cmds(ring);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = ring;
    VK_OK(allocCmd(vk.device, &cai, cmds.data()));

    std::vector<VkSemaphore> acquireSem(ring), renderSem(count);
    std::vector<VkFence> frameFence(ring);
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& s : acquireSem) VK_OK(createSemaphore(vk.device, &semInfo, nullptr, &s));
    for (auto& s : renderSem) VK_OK(createSemaphore(vk.device, &semInfo, nullptr, &s));
    for (auto& f : frameFence) VK_OK(createFence(vk.device, &fenceInfo, nullptr, &f));

    for (uint32_t frame = 0; frame < o.frames; ++frame) {
        const uint32_t slot = frame % ring;
        if (o.pattern == "moving") {
            VK_OK(idle(vk.device));
            VK_OK(mapMem(vk.device, stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped));
            auto* moving = static_cast<uint8_t*>(mapped);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    uint8_t rgb[3];
                    patternPixel(o.pattern, x, y, width, height, rgb, frame);
                    uint8_t* p = moving + (static_cast<size_t>(y) * width + x) * 4;
                    p[0] = rgb[0];
                    p[1] = rgb[1];
                    p[2] = rgb[2];
                    p[3] = 255;
                }
            unmapMem(vk.device, stagingMem);
        }
        VK_OK(waitFences(vk.device, 1, &frameFence[slot], VK_TRUE, UINT64_MAX));
        VK_OK(resetFences(vk.device, 1, &frameFence[slot]));
        uint32_t index = 0;
        VkResult ar = acquire(vk.device, chain, UINT64_MAX, acquireSem[slot], VK_NULL_HANDLE, &index);
        CHECK(ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR, "acquire returned %d", ar);

        VkCommandBuffer cmd = cmds[slot];  // its previous use finished (frame fence)
        VK_OK(resetCmd(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_OK(beginCmd(cmd, &bi));
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // like DXVK: previous contents are not needed
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrierFn(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        copyBuf(cmd, staging, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        barrierFn(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VK_OK(endCmd(cmd));

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquireSem[slot];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderSem[index];
        VK_OK(submit(vk.queue, 1, &si, frameFence[slot]));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderSem[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &chain;
        pi.pImageIndices = &index;
        VkResult pr = present(vk.queue, &pi);
        CHECK(pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR, "present returned %d", pr);
    }
    VK_OK(idle(vk.device));

    for (auto s : acquireSem) destroySemaphore(vk.device, s, nullptr);
    for (auto s : renderSem) destroySemaphore(vk.device, s, nullptr);
    for (auto f : frameFence) destroyFence(vk.device, f, nullptr);
    destroyPool(vk.device, pool, nullptr);
    destroyBuffer(vk.device, staging, nullptr);
    freeMem(vk.device, stagingMem, nullptr);
    if (destroyAtEnd) destroySwapchain(vk.device, chain, nullptr);
    *outChain = chain;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--shim") o.shim = next();
        else if (a == "--rgba") o.bgra = false;
        else if (a == "--srgb") o.srgb = true;
        else if (a == "--mutable") o.mutableFormat = true;
        else if (a == "--validate") o.validate = true;
        else if (a == "--recreate") o.recreate = true;
        else if (a == "--diag") o.checkDiagnostics = true;
        else if (a == "--diag-bypassed") { o.checkDiagnostics = true; o.diagnosticsBypassed = true; }
        else if (a == "--pattern") o.pattern = next();
        else if (a == "--image") o.image = next();
        else if (a == "--ignore") g_ignoredIds.push_back(next());
        else if (a == "--frames") o.frames = atoi(next().c_str());
        else if (a == "--size-from-caps") o.sizeFromCaps = true;
        else if (a == "--size") sscanf(next().c_str(), "%ux%u", &o.width, &o.height);
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    Vk vk;
    vk.loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk.loader) { fprintf(stderr, "no Vulkan loader: %s\n", dlerror()); return 2; }
    vk.loaderGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk.loader, "vkGetInstanceProcAddr"));
    vk.shim = dlopen(o.shim.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!vk.shim) { fprintf(stderr, "cannot load shim %s: %s\n", o.shim.c_str(), dlerror()); return 2; }
    vk.shimGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk.shim, "BCnLayer_GetInstanceProcAddr"));
    vk.shimGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(vk.shim, "BCnLayer_GetDeviceProcAddr"));
    auto diagnostics = reinterpret_cast<const char* (*)()>(dlsym(vk.shim, "BCnLayer_Diagnostics"));
    CHECK(vk.shimGipa && vk.shimGdpa && diagnostics, "shim does not export the three BCnLayer_* symbols");
    if (!vk.shimGipa || !vk.shimGdpa) return 2;
    if (o.checkDiagnostics) {
        // The launcher calls this before any instance exists and appends the result to a std::string.
        const char* v = diagnostics();
        CHECK(v && !*v, "BCnLayer_Diagnostics before any instance returned %s, expected an empty string", v ? v : "null");
    }

    // --- instance, the way LayerCreateInstance builds it ---
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    LayerInstanceLink instLink{nullptr, vk.loaderGipa, nullptr};
    LayerInstanceCreateInfo instChain{kStructureLoaderInstanceCreateInfo, nullptr, kLayerLinkInfo, {&instLink}};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = &instChain;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 3;
    ici.ppEnabledExtensionNames = extensions;
    ici.enabledLayerCount = o.validate ? 1 : 0;
    ici.ppEnabledLayerNames = layers;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(vk.shimGipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VK_OK(createInstance(&ici, nullptr, &vk.instance));
    if (o.checkDiagnostics) {
        // Forwarded to the original layer only when the instance went through it, else a stub ("").
        const char* v = diagnostics();
        const char* expected = o.diagnosticsBypassed ? "" : "inner stub diagnostics";
        CHECK(v && !strcmp(v, expected), "BCnLayer_Diagnostics returned %s, expected \"%s\"", v ? v : "null", expected);
    }

    auto createMessenger = instanceFn<PFN_vkCreateDebugUtilsMessengerEXT>(vk, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = debugCallback;
    VK_OK(createMessenger(vk.instance, &mci, nullptr, &vk.messenger));

    auto enumerate = instanceFn<PFN_vkEnumeratePhysicalDevices>(vk, "vkEnumeratePhysicalDevices");
    uint32_t n = 0;
    VK_OK(enumerate(vk.instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    VK_OK(enumerate(vk.instance, &n, devices.data()));
    auto getProps = instanceFn<PFN_vkGetPhysicalDeviceProperties>(vk, "vkGetPhysicalDeviceProperties");
    vk.physical = devices[0];
    for (auto d : devices) {
        VkPhysicalDeviceProperties p;
        getProps(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) vk.physical = d;
    }
    VkPhysicalDeviceProperties props;
    getProps(vk.physical, &props);
    fprintf(stderr, "HARNESS: using %s\n", props.deviceName);
    instanceFn<PFN_vkGetPhysicalDeviceMemoryProperties>(vk, "vkGetPhysicalDeviceMemoryProperties")(vk.physical, &vk.memory);
    auto getFamilies = instanceFn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(vk, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t fc = 0;
    getFamilies(vk.physical, &fc, nullptr);
    std::vector<VkQueueFamilyProperties> families(fc);
    getFamilies(vk.physical, &fc, families.data());
    for (uint32_t i = 0; i < fc; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk.family = i; break; }

    // --- device, the way LayerCreateDevice builds it ---
    auto loaderGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk.loaderGipa(vk.instance, "vkGetDeviceProcAddr"));
    LayerDeviceLink devLink{nullptr, vk.loaderGipa, loaderGdpa};
    LayerDeviceCreateInfo devChain{kStructureLoaderDeviceCreateInfo, nullptr, kLayerLinkInfo, {&devLink}};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    std::vector<const char*> devExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (o.mutableFormat) { devExt.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME); devExt.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME); devExt.push_back(VK_KHR_MAINTENANCE2_EXTENSION_NAME); }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &devChain;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(devExt.size());
    dci.ppEnabledExtensionNames = devExt.data();
    auto createDevice = instanceFn<PFN_vkCreateDevice>(vk, "vkCreateDevice");
    VK_OK(createDevice(vk.physical, &dci, nullptr, &vk.device));
    // DXVK calls these unchecked; the extension is not enabled here, so the driver has nothing to return.
    CHECK(vk.shimGdpa(vk.device, "vkCmdSetSampleLocationsEnableEXT"), "no stand-in for vkCmdSetSampleLocationsEnableEXT");
    CHECK(vk.shimGdpa(vk.device, "vkCmdSetSampleLocationsEXT"), "no stand-in for vkCmdSetSampleLocationsEXT");
    deviceFn<PFN_vkGetDeviceQueue>(vk, "vkGetDeviceQueue")(vk.device, vk.family, 0, &vk.queue);

    auto createSurface = instanceFn<PFN_vkCreateHeadlessSurfaceEXT>(vk, "vkCreateHeadlessSurfaceEXT");
    VkHeadlessSurfaceCreateInfoEXT hsi{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VK_OK(createSurface(vk.instance, &hsi, nullptr, &vk.surface));

    VkSwapchainKHR chain = VK_NULL_HANDLE;
    runSwapchain(vk, o, o.width, o.height, VK_NULL_HANDLE, &chain, !o.recreate);
    if (o.recreate) {
        // Resize: the new swapchain retires the old one, which is destroyed afterwards.
        VkSwapchainKHR second = VK_NULL_HANDLE;
        runSwapchain(vk, o, o.width + 32, o.height + 18, chain, &second, true);
        deviceFn<PFN_vkDestroySwapchainKHR>(vk, "vkDestroySwapchainKHR")(vk.device, chain, nullptr);
    }

    instanceFn<PFN_vkDestroySurfaceKHR>(vk, "vkDestroySurfaceKHR")(vk.instance, vk.surface, nullptr);
    deviceFn<PFN_vkDestroyDevice>(vk, "vkDestroyDevice")(vk.device, nullptr);
    instanceFn<PFN_vkDestroyDebugUtilsMessengerEXT>(vk, "vkDestroyDebugUtilsMessengerEXT")(vk.instance, vk.messenger, nullptr);
    instanceFn<PFN_vkDestroyInstance>(vk, "vkDestroyInstance")(vk.instance, nullptr);

    fprintf(stderr, "HARNESS: VALIDATION_ISSUES=%d FAILURES=%d\n", g_validationErrors, g_failures);
    return (g_validationErrors || g_failures) ? 1 : 0;
}
