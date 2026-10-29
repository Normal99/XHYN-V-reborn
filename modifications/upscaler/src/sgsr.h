// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#pragma once
#include <unordered_map>

#include "layer.h"

namespace xhyn {

struct FxPlan {
    bool ok = false;
    const char* reason = "";
    VkExtent2D out{};
};

// Decides whether a swapchain the game asks for can be upscaled and at what size.
FxPlan planFx(DeviceInfo& device, const VkSwapchainCreateInfoKHR& ci, const Config& config);

// Virtual swapchain: the game renders into images of its own size; each present runs
// Snapdragon GSR 1 from that image into the real (larger) swapchain image.
// Image index i of the game maps 1:1 to image index i of the real swapchain, so
// vkAcquireNextImageKHR and the swapchain handle stay untouched.
class SwapchainFx {
public:
    static std::unique_ptr<SwapchainFx> create(DeviceInfo& device, VkSwapchainKHR chain,
                                               const VkSwapchainCreateInfoKHR& ci, VkExtent2D out,
                                               const Config& config);
    ~SwapchainFx();

    VkResult getImages(uint32_t* count, VkImage* images) const;

    // Records and submits the upscale of one image after the game's wait semaphores.
    // *signal is the semaphore the real vkQueuePresentKHR must wait on.
    VkResult submit(VkQueue queue, uint32_t family, uint32_t imageIndex, uint32_t waitCount,
                    const VkSemaphore* waits, VkSemaphore* signal);

    // Mean GPU time of the upscale pass since the last call; sets *samples to 0 if none.
    double takeGpuMs(uint32_t* samples);

    VkExtent2D inputExtent() const { return in_; }
    VkExtent2D outputExtent() const { return out_; }

private:
    struct Family {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
        uint32_t timestampBits = 0;
    };

    SwapchainFx(DeviceInfo& device, VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& ci,
                VkExtent2D out, const Config& config);
    bool init(const VkSwapchainCreateInfoKHR& ci);
    bool fail(const char* what, VkResult result);
    void destroy();
    Family* family(uint32_t index);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const;
    bool createHostBuffer(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory);
    void writeDump(const char* tag, VkDeviceMemory memory, VkExtent2D extent) const;

    DeviceInfo& d_;
    VkSwapchainKHR chain_;
    Config config_;
    VkFormat format_;
    VkExtent2D in_, out_;
    uint32_t count_ = 0;

    std::vector<VkImage> game_;
    std::vector<VkDeviceMemory> gameMemory_;
    std::vector<VkImageView> gameView_;
    std::vector<VkImage> real_;
    std::vector<VkImageView> realView_;
    std::vector<VkFramebuffer> framebuffer_;
    std::vector<VkDescriptorSet> set_;
    std::vector<VkSemaphore> done_;
    std::vector<VkFence> fence_;
    std::vector<bool> timed_;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, Family> families_;

    uint64_t presents_ = 0;
    double gpuSumMs_ = 0;
    uint32_t gpuSamples_ = 0;
};

}  // namespace xhyn
