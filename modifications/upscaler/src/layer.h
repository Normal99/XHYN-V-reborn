// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#pragma once
#include <vulkan/vulkan.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"

namespace xhyn {

class SwapchainFx;

// Device functions the upscaler calls itself. They are resolved below the whole
// layer chain, so these calls never loop back through the game's hooks.
#define XHYN_DEVICE_FUNCTIONS(X)                                                                   \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory)        \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)                \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)   \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)               \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)          \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateSampler) X(vkDestroySampler)      \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)              \
    X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool)                      \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer)                   \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)   \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw)            \
    X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCmdCopyImageToBuffer)                       \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit)          \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence)                 \
    X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer)  \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory)        \
    X(vkInvalidateMappedMemoryRanges)

struct DeviceFns {
#define X(name) PFN_##name name = nullptr;
    XHYN_DEVICE_FUNCTIONS(X)
#undef X
};

struct InstanceInfo {
    VkInstance handle = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr nextGipa = nullptr;   // below the BCn layer: the driver/loader
    PFN_vkGetInstanceProcAddr innerGipa = nullptr;  // the BCn layer, or nextGipa when bypassed
    bool bypass = false;                            // original BCn layer not used for this instance
    PFN_vkDestroyInstance innerDestroy = nullptr;
    PFN_vkCreateDevice innerCreateDevice = nullptr;
    PFN_vkGetPhysicalDeviceProperties getProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilies = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getSurfaceCapabilities = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR getSurfaceFormats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR getPresentModes = nullptr;
};

// Frame-time statistics of vkQueuePresentKHR calls, printed every logInterval frames.
struct PresentStats {
    std::chrono::steady_clock::time_point last{};
    uint64_t frames = 0;
    uint64_t windowFrames = 0;
    double sumMs = 0, minMs = 1e9, maxMs = 0;
    // Records one present; returns true when a report is due (every logInterval frames).
    bool tick(uint32_t logInterval);
    // Formats and clears the current window.
    void report(const char* label, VkExtent2D extent, double gpuMs, uint32_t gpuSamples);
};

struct ChainState {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t logInterval = 0;
    PresentStats stats;
    std::unique_ptr<SwapchainFx> fx;  // set only when the swapchain is virtualized
};

struct DeviceInfo {
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceInfo* instance = nullptr;
    PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
    PFN_vkGetDeviceProcAddr innerGdpa = nullptr;
    DeviceFns fn;
    bool fnComplete = false;

    PFN_vkDestroyDevice innerDestroy = nullptr;
    PFN_vkGetDeviceQueue innerGetQueue = nullptr;
    PFN_vkGetDeviceQueue2 innerGetQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR innerCreateSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR innerDestroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR innerGetImages = nullptr;
    PFN_vkQueuePresentKHR innerPresent = nullptr;

    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    std::vector<VkQueueFamilyProperties> families;

    std::mutex mutex;  // guards queues and chains (not the per-swapchain frame work)
    std::unordered_map<VkQueue, uint32_t> queueFamily;
    std::unordered_map<uint64_t, std::unique_ptr<ChainState>> chains;
};

// Layer entry points exposed through BCnLayer_Get*ProcAddr (shim.cpp).
PFN_vkVoidFunction layerGetInstanceProcAddr(VkInstance instance, const char* name);
PFN_vkVoidFunction layerGetDeviceProcAddr(VkDevice device, const char* name);

// The renamed original BCn layer, resolved once (may be absent).
struct InnerLayer {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    void* diagnostics = nullptr;
};
const InnerLayer& innerLayer();

DeviceInfo* findDevice(VkDevice device);

}  // namespace xhyn
