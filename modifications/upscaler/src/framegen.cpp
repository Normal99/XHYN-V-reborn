// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#include "framegen.h"

#include <algorithm>

#include "log.h"
#include "shaders_spv.h"

namespace xhyn {

std::unique_ptr<FrameGen> FrameGen::create(DeviceInfo& device, VkFormat format, VkExtent2D extent, VkSampler sampler,
                                           const std::vector<VkImageView>& gameViews) {
    std::unique_ptr<FrameGen> fg(new FrameGen(device));
    if (!fg->prepare(format, extent, sampler, gameViews)) return nullptr;
    return fg;
}

FrameGen::~FrameGen() {
    auto& fn = d_.fn;
    if (pool_) fn.vkDestroyDescriptorPool(d_.handle, pool_, nullptr);
    for (Pass* pass : {&lumaPass_, &motionPass_, &interpolatePass_}) destroyPass(*pass);
    for (Target* target : {&luma_[0], &luma_[1], &motion_, &history_, &generated_}) destroyTarget(*target);
}

void FrameGen::destroyPass(Pass& pass) {
    auto& fn = d_.fn;
    if (pass.pipeline) fn.vkDestroyPipeline(d_.handle, pass.pipeline, nullptr);
    if (pass.layout) fn.vkDestroyPipelineLayout(d_.handle, pass.layout, nullptr);
    if (pass.setLayout) fn.vkDestroyDescriptorSetLayout(d_.handle, pass.setLayout, nullptr);
    if (pass.renderPass) fn.vkDestroyRenderPass(d_.handle, pass.renderPass, nullptr);
}

void FrameGen::destroyTarget(Target& target) {
    auto& fn = d_.fn;
    if (target.framebuffer) fn.vkDestroyFramebuffer(d_.handle, target.framebuffer, nullptr);
    if (target.view) fn.vkDestroyImageView(d_.handle, target.view, nullptr);
    if (target.image) fn.vkDestroyImage(d_.handle, target.image, nullptr);
    if (target.memory) fn.vkFreeMemory(d_.handle, target.memory, nullptr);
}

bool FrameGen::createPass(Pass& pass, VkFormat format, VkExtent2D extent, uint32_t bindings, const uint32_t* code, size_t codeSize) {
    auto& fn = d_.fn;
    const VkDevice dev = d_.handle;

    std::vector<VkDescriptorSetLayoutBinding> list(bindings);
    for (uint32_t i = 0; i < bindings; ++i)
        list[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lc.bindingCount = bindings;
    lc.pBindings = list.data();
    if (fn.vkCreateDescriptorSetLayout(dev, &lc, nullptr, &pass.setLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &pass.setLayout;
    if (fn.vkCreatePipelineLayout(dev, &plc, nullptr, &pass.layout) != VK_SUCCESS) return false;

    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rc.attachmentCount = 1;
    rc.pAttachments = &att;
    rc.subpassCount = 1;
    rc.pSubpasses = &sub;
    rc.dependencyCount = 2;
    rc.pDependencies = deps;
    if (fn.vkCreateRenderPass(dev, &rc, nullptr, &pass.renderPass) != VK_SUCCESS) return false;

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smc.codeSize = sizeof kFullscreenVert;
    smc.pCode = kFullscreenVert;
    if (fn.vkCreateShaderModule(dev, &smc, nullptr, &vert) != VK_SUCCESS) return false;
    smc.codeSize = codeSize;
    smc.pCode = code;
    if (fn.vkCreateShaderModule(dev, &smc, nullptr, &frag) != VK_SUCCESS) {
        fn.vkDestroyShaderModule(dev, vert, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    vp.pViewports = &viewport;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;
    VkGraphicsPipelineCreateInfo pc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pc.stageCount = 2;
    pc.pStages = stages;
    pc.pVertexInputState = &vi;
    pc.pInputAssemblyState = &ia;
    pc.pViewportState = &vp;
    pc.pRasterizationState = &rs;
    pc.pMultisampleState = &ms;
    pc.pColorBlendState = &cb;
    pc.layout = pass.layout;
    pc.renderPass = pass.renderPass;
    const VkResult r = fn.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pc, nullptr, &pass.pipeline);
    fn.vkDestroyShaderModule(dev, vert, nullptr);
    fn.vkDestroyShaderModule(dev, frag, nullptr);
    return r == VK_SUCCESS;
}

bool FrameGen::createImage(Target& target, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    auto& fn = d_.fn;
    target.extent = extent;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = format;
    ic.extent = {extent.width, extent.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = usage;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fn.vkCreateImage(d_.handle, &ic, nullptr, &target.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    fn.vkGetImageMemoryRequirements(d_.handle, target.image, &req);
    uint32_t type = UINT32_MAX;
    for (VkMemoryPropertyFlags wanted : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), VkMemoryPropertyFlags(0)}) {
        for (uint32_t i = 0; i < d_.memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (d_.memory.memoryTypes[i].propertyFlags & wanted) == wanted) type = i;
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (fn.vkAllocateMemory(d_.handle, &ai, nullptr, &target.memory) != VK_SUCCESS) return false;
    if (fn.vkBindImageMemory(d_.handle, target.image, target.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = target.image;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return fn.vkCreateImageView(d_.handle, &vc, nullptr, &target.view) == VK_SUCCESS;
}

bool FrameGen::createFramebuffer(Target& target, const Pass& pass) {
    VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fc.renderPass = pass.renderPass;
    fc.attachmentCount = 1;
    fc.pAttachments = &target.view;
    fc.width = target.extent.width;
    fc.height = target.extent.height;
    fc.layers = 1;
    return d_.fn.vkCreateFramebuffer(d_.handle, &fc, nullptr, &target.framebuffer) == VK_SUCCESS;
}

void FrameGen::writeSet(VkDescriptorSet set, const std::vector<VkImageView>& views) {
    std::vector<VkDescriptorImageInfo> infos(views.size());
    std::vector<VkWriteDescriptorSet> writes(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        infos[i] = {sampler_, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    d_.fn.vkUpdateDescriptorSets(d_.handle, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool FrameGen::prepare(VkFormat format, VkExtent2D extent, VkSampler sampler, const std::vector<VkImageView>& gameViews) {
    auto& fn = d_.fn;
    sampler_ = sampler;
    const VkExtent2D lumaSize{std::max(extent.width / 4, 1u), std::max(extent.height / 4, 1u)};
    const VkExtent2D motionSize{std::max(extent.width / 8, 1u), std::max(extent.height / 8, 1u)};
    if (!createPass(lumaPass_, VK_FORMAT_R8_UNORM, lumaSize, 1, kFgLumaFrag, sizeof kFgLumaFrag) ||
        !createPass(motionPass_, VK_FORMAT_R16G16B16A16_SFLOAT, motionSize, 2, kFgMotionFrag, sizeof kFgMotionFrag) ||
        !createPass(interpolatePass_, format, extent, 3, kFgInterpolateFrag, sizeof kFgInterpolateFrag))
        return false;
    const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (Target& luma : luma_)
        if (!createImage(luma, VK_FORMAT_R8_UNORM, lumaSize, target) || !createFramebuffer(luma, lumaPass_)) return false;
    if (!createImage(motion_, VK_FORMAT_R16G16B16A16_SFLOAT, motionSize, target) || !createFramebuffer(motion_, motionPass_))
        return false;
    if (!createImage(generated_, format, extent, target | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !createFramebuffer(generated_, interpolatePass_))
        return false;
    if (!createImage(history_, format, extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;

    const uint32_t count = static_cast<uint32_t>(gameViews.size());
    const uint32_t sets = 3 * count + 2;
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * count + 3 * count + 4};
    VkDescriptorPoolCreateInfo dpc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpc.maxSets = sets;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &poolSize;
    if (fn.vkCreateDescriptorPool(d_.handle, &dpc, nullptr, &pool_) != VK_SUCCESS) return false;
    auto allocate = [&](const Pass& pass, uint32_t n, VkDescriptorSet* out) {
        std::vector<VkDescriptorSetLayout> layouts(n, pass.setLayout);
        VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsa.descriptorPool = pool_;
        dsa.descriptorSetCount = n;
        dsa.pSetLayouts = layouts.data();
        return fn.vkAllocateDescriptorSets(d_.handle, &dsa, out) == VK_SUCCESS;
    };
    for (auto& list : lumaSets_) {
        list.resize(count);
        if (!allocate(lumaPass_, count, list.data())) return false;
    }
    interpolateSets_.resize(count);
    if (!allocate(interpolatePass_, count, interpolateSets_.data()) || !allocate(motionPass_, 2, motionSets_)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        writeSet(lumaSets_[0][i], {gameViews[i]});
        writeSet(lumaSets_[1][i], {gameViews[i]});
        writeSet(interpolateSets_[i], {history_.view, gameViews[i], motion_.view});
    }
    // motionSets_[k] searches with luma_[k] as the current frame.
    writeSet(motionSets_[0], {luma_[1].view, luma_[0].view});
    writeSet(motionSets_[1], {luma_[0].view, luma_[1].view});
    return true;
}

void FrameGen::draw(VkCommandBuffer cmd, const Pass& pass, const Target& target, VkDescriptorSet set) {
    auto& fn = d_.fn;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass.renderPass;
    rp.framebuffer = target.framebuffer;
    rp.renderArea = {{0, 0}, target.extent};
    fn.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 1, &set, 0, nullptr);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRenderPass(cmd);
}

void FrameGen::recordLuma(VkCommandBuffer cmd, uint32_t gameIndex) {
    draw(cmd, lumaPass_, luma_[currentLuma_], lumaSets_[currentLuma_][gameIndex]);
}

void FrameGen::recordInterpolate(VkCommandBuffer cmd, uint32_t gameIndex) {
    draw(cmd, motionPass_, motion_, motionSets_[currentLuma_]);
    draw(cmd, interpolatePass_, generated_, interpolateSets_[gameIndex]);
}

void FrameGen::recordHistory(VkCommandBuffer cmd, VkImage game) {
    auto& fn = d_.fn;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the previous contents are not needed
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = history_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {history_.extent.width, history_.extent.height, 1};
    fn.vkCmdCopyImage(cmd, game, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    currentLuma_ ^= 1;
}

}  // namespace xhyn
