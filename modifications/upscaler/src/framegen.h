// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#pragma once
#include <vector>

#include "layer.h"

namespace xhyn {

// Colour-only frame interpolation at the game's size. The layer gets no depth or motion data, so
// motion comes from block search on two finished frames; see shaders/fg_*.frag.
class FrameGen {
public:
    // gameViews are the sampled views of the game's images, indexed like them.
    static std::unique_ptr<FrameGen> create(DeviceInfo& device, VkFormat format, VkExtent2D extent, VkSampler sampler,
                                            const std::vector<VkImageView>& gameViews);
    ~FrameGen();

    // Each needs the game's image in SHADER_READ_ONLY_OPTIMAL. Luma comes first in every frame.
    void recordLuma(VkCommandBuffer cmd, uint32_t gameIndex);
    void recordInterpolate(VkCommandBuffer cmd, uint32_t gameIndex);
    // Needs the game's image in TRANSFER_SRC_OPTIMAL. Makes this frame the previous one.
    void recordHistory(VkCommandBuffer cmd, VkImage game);

    VkImage generatedImage() const { return generated_.image; }
    VkImageView generatedView() const { return generated_.view; }

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkExtent2D extent{};
    };
    struct Pass {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    explicit FrameGen(DeviceInfo& device) : d_(device) {}
    bool prepare(VkFormat format, VkExtent2D extent, VkSampler sampler, const std::vector<VkImageView>& gameViews);
    bool createPass(Pass& pass, VkFormat format, VkExtent2D extent, uint32_t bindings, const uint32_t* code, size_t codeSize);
    bool createImage(Target& target, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage);
    bool createFramebuffer(Target& target, const Pass& pass);
    void writeSet(VkDescriptorSet set, const std::vector<VkImageView>& views);
    void draw(VkCommandBuffer cmd, const Pass& pass, const Target& target, VkDescriptorSet set);
    void destroyPass(Pass& pass);
    void destroyTarget(Target& target);

    DeviceInfo& d_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    Pass lumaPass_, motionPass_, interpolatePass_;
    Target luma_[2], motion_, history_, generated_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> lumaSets_[2], interpolateSets_;
    VkDescriptorSet motionSets_[2]{};
    uint32_t currentLuma_ = 0;  // which of luma_ holds the frame being presented
};

}  // namespace xhyn
