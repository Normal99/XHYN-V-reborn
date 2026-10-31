// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#pragma once
#include <unordered_map>

#include "framegen.h"
#include "layer.h"

namespace xhyn {

struct FxPlan {
    bool ok = false;
    const char* reason = "";
    VkExtent2D out{};
    uint32_t minImages = 0;
};

// Decides whether a swapchain the game asks for can be upscaled and at what size.
FxPlan planFx(DeviceInfo& device, const VkSwapchainCreateInfoKHR& ci, const Config& config);

// Virtual swapchain: the game renders into images of its own size; each present runs
// Snapdragon GSR 1 from that image into the real (larger) swapchain image.
// Image index i of the game maps 1:1 to image index i of the real swapchain, so
// vkAcquireNextImageKHR and the swapchain handle stay untouched.
class SwapchainFx {
public:
    // Builds everything that does not need the real swapchain (pipeline, render pass, sampler).
    // Nothing in here may fail after the real swapchain exists, so it is called first.
    static std::unique_ptr<SwapchainFx> create(DeviceInfo& device, const VkSwapchainCreateInfoKHR& ci,
                                               VkExtent2D out, const Config& config);
    ~SwapchainFx();

    // Second step, once the real swapchain exists: images, views, descriptors, sync objects.
    bool attach(VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& ci);

    VkResult getImages(uint32_t* count, VkImage* images) const;

    // Records and submits the upscale of game image `source` into real image `target`, after the
    // game's wait semaphores. *signal is the semaphore the real vkQueuePresentKHR must wait on.
    VkResult submit(VkQueue queue, uint32_t family, uint32_t source, uint32_t target, uint32_t waitCount,
                    const VkSemaphore* waits, VkSemaphore* signal);

    // Frame generation. Each game frame is presented twice: the frame between it and the previous
    // one goes to the image the game acquired (submitGenerated), then the frame itself to an extra
    // image (acquireReal, submit). The first frame has no previous one, so it uses plain submit.
    bool frameGenReady() const { return fg_ && historyValid_; }
    VkResult submitGenerated(VkQueue queue, uint32_t family, uint32_t index, uint32_t waitCount,
                             const VkSemaphore* waits, VkSemaphore* signal);
    VkResult acquireReal(uint32_t* index, VkSemaphore* semaphore);
    // Called right after the generated frame is presented; waits until the real frame is due.
    void paceRealFrame();

    // Mean GPU time of the upscale pass since the last call; sets *samples to 0 if none.
    double takeGpuMs(uint32_t* samples);
    // The same for a generated frame, including its upscale.
    double takeGenMs(uint32_t* samples);

    VkExtent2D inputExtent() const { return in_; }
    VkExtent2D outputExtent() const { return out_; }

private:
    struct Family {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
        uint32_t timestampBits = 0;
    };

    SwapchainFx(DeviceInfo& device, const VkSwapchainCreateInfoKHR& ci, VkExtent2D out, const Config& config);
    bool prepare();
    bool fail(const char* what, VkResult result);
    void destroy();
    Family* family(uint32_t index);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const;
    bool createHostBuffer(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory);
    void writeDump(const char* tag, VkDeviceMemory memory, VkExtent2D extent, uint64_t frame) const;
    VkResult claimSlot(uint32_t index, Family* fam);
    void recordUpscale(VkCommandBuffer cmd, uint32_t target, VkDescriptorSet set);

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
    std::vector<bool> timedGen_;
    std::vector<VkSemaphore> acquireSemaphores_;
    uint32_t nextAcquireSemaphore_ = 0;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, Family> families_;
    std::unique_ptr<FrameGen> fg_;
    VkDescriptorSet generatedSet_ = VK_NULL_HANDLE;
    bool historyValid_ = false;
    std::chrono::steady_clock::time_point lastGenerated_{};
    double frameIntervalMs_ = 0;

    uint64_t presents_ = 0;
    double gpuSumMs_ = 0;
    uint32_t gpuSamples_ = 0;
    double genSumMs_ = 0;
    uint32_t genSamples_ = 0;
};

}  // namespace xhyn
