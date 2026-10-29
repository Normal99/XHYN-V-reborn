// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#include "sgsr.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "loader_abi.h"
#include "shaders_spv.h"

namespace xhyn {
namespace {

constexpr uint64_t kFenceTimeoutNs = 1000000000ull;

bool eligibleFormat(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

bool isSrgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

bool isBgr(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB; }

// SGSR works on gamma-encoded values, so the game's image is sampled through its
// UNORM twin and sees the stored bytes unchanged.
VkFormat unormTwin(VkFormat f) {
    if (f == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
    if (f == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
    return f;
}

struct Spec {
    VkBool32 edgeDirection;
    VkBool32 dstSrgb;
    float threshold;
    float sharpness;
};

}  // namespace

FxPlan planFx(DeviceInfo& d, const VkSwapchainCreateInfoKHR& ci, const Config& config) {
    FxPlan plan;
    if (!d.fnComplete) { plan.reason = "device functions unavailable"; return plan; }
    if (!eligibleFormat(ci.imageFormat) || ci.imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        plan.reason = "swapchain is not 8-bit SDR RGBA/BGRA";
        return plan;
    }
    if (ci.imageArrayLayers != 1) { plan.reason = "multi-layer swapchain"; return plan; }

    double w = ci.imageExtent.width, h = ci.imageExtent.height;
    if (config.outWidth && config.outHeight) {
        w = config.outWidth;
        h = config.outHeight;
    } else {
        w = std::round(w * config.scale);
        h = std::round(h * config.scale);
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (!d.instance->getSurfaceCapabilities ||
        d.instance->getSurfaceCapabilities(d.physical, ci.surface, &caps) != VK_SUCCESS) {
        plan.reason = "surface capabilities query failed";
        return plan;
    }
    const double fit = std::min(1.0, std::min<double>(caps.maxImageExtent.width / w, caps.maxImageExtent.height / h));
    plan.out.width = std::max<uint32_t>(1, static_cast<uint32_t>(w * fit));
    plan.out.height = std::max<uint32_t>(1, static_cast<uint32_t>(h * fit));
    if (plan.out.width <= ci.imageExtent.width && plan.out.height <= ci.imageExtent.height) {
        plan.reason = "output would not be larger than the game's swapchain";
        return plan;
    }
    VkImageUsageFlags need = ci.imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (config.dumpFrame) need |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((need & caps.supportedUsageFlags) != need) {
        plan.reason = "surface does not support the needed image usage";
        return plan;
    }
    // Once the real swapchain exists the game's old one is retired, so every check that can
    // reject the upscaler has to happen before that.
    if (plan.out.width < ci.imageExtent.width || plan.out.height < ci.imageExtent.height ||
        plan.out.width < caps.minImageExtent.width || plan.out.height < caps.minImageExtent.height) {
        plan.reason = "output size is below the game's size or the surface minimum";
        return plan;
    }
    if (!d.instance->getImageFormatProperties || !d.instance->getFormatProperties) {
        plan.reason = "instance lacks format query functions";
        return plan;
    }
    VkFormatProperties twin{}, target{};
    d.instance->getFormatProperties(d.physical, unormTwin(ci.imageFormat), &twin);
    d.instance->getFormatProperties(d.physical, ci.imageFormat, &target);
    const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((twin.optimalTilingFeatures & sampled) != sampled ||
        !(target.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        plan.reason = "format cannot be sampled/rendered as needed";
        return plan;
    }
    VkImageFormatProperties image{};
    const VkImageUsageFlags gameUsage = ci.imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        (config.dumpFrame ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    if (d.instance->getImageFormatProperties(d.physical, ci.imageFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                             gameUsage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &image) != VK_SUCCESS ||
        image.maxExtent.width < ci.imageExtent.width || image.maxExtent.height < ci.imageExtent.height) {
        plan.reason = "device cannot create the game's image with sampling usage";
        return plan;
    }
    plan.ok = true;
    return plan;
}

std::unique_ptr<SwapchainFx> SwapchainFx::create(DeviceInfo& device, const VkSwapchainCreateInfoKHR& ci,
                                                 VkExtent2D out, const Config& config) {
    std::unique_ptr<SwapchainFx> fx(new SwapchainFx(device, ci, out, config));
    if (!fx->prepare()) return nullptr;
    return fx;
}

SwapchainFx::SwapchainFx(DeviceInfo& device, const VkSwapchainCreateInfoKHR& ci, VkExtent2D out,
                         const Config& config)
    : d_(device), chain_(VK_NULL_HANDLE), config_(config), format_(ci.imageFormat), in_(ci.imageExtent), out_(out) {}

SwapchainFx::~SwapchainFx() { destroy(); }

bool SwapchainFx::fail(const char* what, VkResult result) {
    XLOGE("sgsr1 setup failed: %s (VkResult %d)", what, static_cast<int>(result));
    return false;
}

uint32_t SwapchainFx::memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const {
    const auto& mem = d_.memory;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

bool SwapchainFx::prepare() {
    auto& fn = d_.fn;
    const VkDevice dev = d_.handle;
    VkResult r;

    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = 0.25f;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    r = fn.vkCreateSampler(dev, &sc, nullptr, &sampler_);
    if (r != VK_SUCCESS) return fail("create sampler", r);

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lc.bindingCount = 1;
    lc.pBindings = &binding;
    r = fn.vkCreateDescriptorSetLayout(dev, &lc, nullptr, &setLayout_);
    if (r != VK_SUCCESS) return fail("create descriptor set layout", r);

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &setLayout_;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &push;
    r = fn.vkCreatePipelineLayout(dev, &plc, nullptr, &layout_);
    if (r != VK_SUCCESS) return fail("create pipeline layout", r);

    VkAttachmentDescription att{};
    att.format = format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rc.attachmentCount = 1;
    rc.pAttachments = &att;
    rc.subpassCount = 1;
    rc.pSubpasses = &sub;
    rc.dependencyCount = 2;
    rc.pDependencies = deps;
    r = fn.vkCreateRenderPass(dev, &rc, nullptr, &renderPass_);
    if (r != VK_SUCCESS) return fail("create render pass", r);

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smc.codeSize = sizeof kFullscreenVert;
    smc.pCode = kFullscreenVert;
    r = fn.vkCreateShaderModule(dev, &smc, nullptr, &vert);
    if (r != VK_SUCCESS) return fail("create vertex shader", r);
    smc.codeSize = sizeof kSgsr1Frag;
    smc.pCode = kSgsr1Frag;
    r = fn.vkCreateShaderModule(dev, &smc, nullptr, &frag);
    if (r != VK_SUCCESS) {
        fn.vkDestroyShaderModule(dev, vert, nullptr);
        return fail("create fragment shader", r);
    }

    const Spec spec{config_.edgeDirection ? VK_TRUE : VK_FALSE, isSrgb(format_) ? VK_TRUE : VK_FALSE,
                    config_.edgeThreshold / 255.0f, config_.edgeSharpness};
    const VkSpecializationMapEntry entries[] = {
        {0, offsetof(Spec, edgeDirection), sizeof(VkBool32)},
        {1, offsetof(Spec, dstSrgb), sizeof(VkBool32)},
        {2, offsetof(Spec, threshold), sizeof(float)},
        {3, offsetof(Spec, sharpness), sizeof(float)},
    };
    VkSpecializationInfo specInfo{4, entries, sizeof spec, &spec};
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &specInfo;

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, static_cast<float>(out_.width), static_cast<float>(out_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, out_};
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
    pc.layout = layout_;
    pc.renderPass = renderPass_;
    r = fn.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline_);
    fn.vkDestroyShaderModule(dev, vert, nullptr);
    fn.vkDestroyShaderModule(dev, frag, nullptr);
    if (r != VK_SUCCESS) return fail("create pipeline", r);

    return true;
}

bool SwapchainFx::attach(VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& ci) {
    chain_ = chain;
    if (getenv("XHYN_UPSCALE_TEST_FAIL_ATTACH")) return fail("test hook", VK_ERROR_UNKNOWN);  // host tests
    auto& fn = d_.fn;
    const VkDevice dev = d_.handle;
    VkResult r;

    r = d_.innerGetImages(dev, chain_, &count_, nullptr);
    if (r != VK_SUCCESS || count_ == 0) return fail("count real images", r);
    real_.resize(count_);
    r = d_.innerGetImages(dev, chain_, &count_, real_.data());
    if (r != VK_SUCCESS) return fail("get real images", r);

    // Game-visible images. Same format/usage as the swapchain the game asked for, plus
    // sampling; MUTABLE_FORMAT lets the UNORM twin view an sRGB image.
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    std::vector<VkFormat> listFormats;
    for (auto* n = static_cast<const VkStructHeader*>(ci.pNext); n; n = static_cast<const VkStructHeader*>(n->pNext)) {
        if (n->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) continue;
        auto* src = reinterpret_cast<const VkImageFormatListCreateInfo*>(n);
        listFormats.assign(src->pViewFormats, src->pViewFormats + src->viewFormatCount);
        break;
    }
    const VkFormat twin = unormTwin(format_);
    const bool haveList = !listFormats.empty();
    if (haveList) {
        for (VkFormat f : {format_, twin})
            if (std::find(listFormats.begin(), listFormats.end(), f) == listFormats.end()) listFormats.push_back(f);
        list.viewFormatCount = static_cast<uint32_t>(listFormats.size());
        list.pViewFormats = listFormats.data();
    }

    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.pNext = haveList ? &list : nullptr;
    ic.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = format_;
    ic.extent = {in_.width, in_.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = ci.imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT | (config_.dumpFrame ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ic.sharingMode = ci.imageSharingMode;
    ic.queueFamilyIndexCount = ci.queueFamilyIndexCount;
    ic.pQueueFamilyIndices = ci.pQueueFamilyIndices;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    game_.assign(count_, VK_NULL_HANDLE);
    gameMemory_.assign(count_, VK_NULL_HANDLE);
    gameView_.assign(count_, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count_; ++i) {
        r = fn.vkCreateImage(dev, &ic, nullptr, &game_[i]);
        if (r != VK_SUCCESS) return fail("create game image", r);
        VkMemoryRequirements req;
        fn.vkGetImageMemoryRequirements(dev, game_[i], &req);
        uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) type = memoryType(req.memoryTypeBits, 0);
        if (type == UINT32_MAX) return fail("no memory type for game image", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        r = fn.vkAllocateMemory(dev, &ai, nullptr, &gameMemory_[i]);
        if (r != VK_SUCCESS) return fail("allocate game image memory", r);
        r = fn.vkBindImageMemory(dev, game_[i], gameMemory_[i], 0);
        if (r != VK_SUCCESS) return fail("bind game image memory", r);

        VkImageViewUsageCreateInfo viewUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        viewUsage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;  // the twin format need not support the game's other usages
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.pNext = &viewUsage;
        vc.image = game_[i];
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = twin;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        r = fn.vkCreateImageView(dev, &vc, nullptr, &gameView_[i]);
        if (r != VK_SUCCESS) return fail("create game image view", r);
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count_};
    VkDescriptorPoolCreateInfo dpc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpc.maxSets = count_;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &poolSize;
    r = fn.vkCreateDescriptorPool(dev, &dpc, nullptr, &pool_);
    if (r != VK_SUCCESS) return fail("create descriptor pool", r);
    std::vector<VkDescriptorSetLayout> layouts(count_, setLayout_);
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = pool_;
    dsa.descriptorSetCount = count_;
    dsa.pSetLayouts = layouts.data();
    set_.resize(count_);
    r = fn.vkAllocateDescriptorSets(dev, &dsa, set_.data());
    if (r != VK_SUCCESS) return fail("allocate descriptor sets", r);
    for (uint32_t i = 0; i < count_; ++i) {
        VkDescriptorImageInfo info{sampler_, gameView_[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set_[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        fn.vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    realView_.assign(count_, VK_NULL_HANDLE);
    framebuffer_.assign(count_, VK_NULL_HANDLE);
    done_.assign(count_, VK_NULL_HANDLE);
    fence_.assign(count_, VK_NULL_HANDLE);
    timed_.assign(count_, false);
    for (uint32_t i = 0; i < count_; ++i) {
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = real_[i];
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = format_;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        r = fn.vkCreateImageView(dev, &vc, nullptr, &realView_[i]);
        if (r != VK_SUCCESS) return fail("create swapchain image view", r);
        VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fc.renderPass = renderPass_;
        fc.attachmentCount = 1;
        fc.pAttachments = &realView_[i];
        fc.width = out_.width;
        fc.height = out_.height;
        fc.layers = 1;
        r = fn.vkCreateFramebuffer(dev, &fc, nullptr, &framebuffer_[i]);
        if (r != VK_SUCCESS) return fail("create framebuffer", r);
        VkSemaphoreCreateInfo semc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        r = fn.vkCreateSemaphore(dev, &semc, nullptr, &done_[i]);
        if (r != VK_SUCCESS) return fail("create semaphore", r);
        VkFenceCreateInfo fenc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        r = fn.vkCreateFence(dev, &fenc, nullptr, &fence_[i]);
        if (r != VK_SUCCESS) return fail("create fence", r);
    }

    if (config_.gpuTiming && d_.properties.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qc.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qc.queryCount = 2 * count_;
        if (fn.vkCreateQueryPool(dev, &qc, nullptr, &queries_) != VK_SUCCESS) queries_ = VK_NULL_HANDLE;
    }
    XLOGI("sgsr1 ready: %ux%u -> %ux%u (x%.2f), %u images, format %d%s%s", in_.width, in_.height, out_.width,
          out_.height, static_cast<double>(out_.width) / in_.width, count_, static_cast<int>(format_),
          isSrgb(format_) ? " (sRGB output encode)" : "", queries_ ? ", GPU timing on" : "");
    return true;
}

void SwapchainFx::destroy() {
    if (!d_.fnComplete) return;
    auto& fn = d_.fn;
    const VkDevice dev = d_.handle;
    // Wait for our own submissions only; the game idles its queues before destroying a swapchain.
    std::vector<VkFence> pending;
    for (VkFence f : fence_)
        if (f) pending.push_back(f);
    if (!pending.empty())
        fn.vkWaitForFences(dev, static_cast<uint32_t>(pending.size()), pending.data(), VK_TRUE, kFenceTimeoutNs);
    for (auto& entry : families_)
        if (entry.second.pool) fn.vkDestroyCommandPool(dev, entry.second.pool, nullptr);
    families_.clear();
    if (queries_) fn.vkDestroyQueryPool(dev, queries_, nullptr);
    for (VkFence f : fence_) if (f) fn.vkDestroyFence(dev, f, nullptr);
    for (VkSemaphore s : done_) if (s) fn.vkDestroySemaphore(dev, s, nullptr);
    for (VkFramebuffer f : framebuffer_) if (f) fn.vkDestroyFramebuffer(dev, f, nullptr);
    for (VkImageView v : realView_) if (v) fn.vkDestroyImageView(dev, v, nullptr);
    if (pipeline_) fn.vkDestroyPipeline(dev, pipeline_, nullptr);
    if (renderPass_) fn.vkDestroyRenderPass(dev, renderPass_, nullptr);
    if (pool_) fn.vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (layout_) fn.vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) fn.vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (sampler_) fn.vkDestroySampler(dev, sampler_, nullptr);
    for (VkImageView v : gameView_) if (v) fn.vkDestroyImageView(dev, v, nullptr);
    for (VkImage i : game_) if (i) fn.vkDestroyImage(dev, i, nullptr);
    for (VkDeviceMemory m : gameMemory_) if (m) fn.vkFreeMemory(dev, m, nullptr);
}

VkResult SwapchainFx::getImages(uint32_t* count, VkImage* images) const {
    if (!images) {
        *count = count_;
        return VK_SUCCESS;
    }
    const uint32_t n = std::min(*count, count_);
    for (uint32_t i = 0; i < n; ++i) images[i] = game_[i];
    *count = n;
    return n < count_ ? VK_INCOMPLETE : VK_SUCCESS;
}

SwapchainFx::Family* SwapchainFx::family(uint32_t index) {
    auto found = families_.find(index);
    if (found != families_.end()) return &found->second;
    auto& fn = d_.fn;
    Family fam;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = index;
    if (fn.vkCreateCommandPool(d_.handle, &pc, nullptr, &fam.pool) != VK_SUCCESS) return nullptr;
    fam.buffers.resize(count_);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = fam.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = count_;
    if (fn.vkAllocateCommandBuffers(d_.handle, &ai, fam.buffers.data()) != VK_SUCCESS) {
        fn.vkDestroyCommandPool(d_.handle, fam.pool, nullptr);
        return nullptr;
    }
    if (index < d_.families.size()) fam.timestampBits = d_.families[index].timestampValidBits;
    return &families_.emplace(index, std::move(fam)).first->second;
}

bool SwapchainFx::createHostBuffer(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory) {
    auto& fn = d_.fn;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = size;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (fn.vkCreateBuffer(d_.handle, &bc, nullptr, buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    fn.vkGetBufferMemoryRequirements(d_.handle, *buffer, &req);
    const uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (fn.vkAllocateMemory(d_.handle, &ai, nullptr, memory) != VK_SUCCESS) return false;
    return fn.vkBindBufferMemory(d_.handle, *buffer, *memory, 0) == VK_SUCCESS;
}

void SwapchainFx::writeDump(const char* tag, VkDeviceMemory memory, VkExtent2D extent) const {
    void* mapped = nullptr;
    if (d_.fn.vkMapMemory(d_.handle, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return;
    const std::string path = config_.dumpDir + "/xhyn-upscale-" + tag + "-" + std::to_string(presents_) + ".ppm";
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fprintf(f, "P6\n%u %u\n255\n", extent.width, extent.height);
        const auto* px = static_cast<const uint8_t*>(mapped);
        std::vector<uint8_t> row(static_cast<size_t>(extent.width) * 3);
        for (uint32_t y = 0; y < extent.height; ++y) {
            for (uint32_t x = 0; x < extent.width; ++x) {
                const uint8_t* p = px + (static_cast<size_t>(y) * extent.width + x) * 4;
                row[x * 3 + 0] = isBgr(format_) ? p[2] : p[0];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = isBgr(format_) ? p[0] : p[2];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        XLOGI("wrote %s", path.c_str());
    } else {
        XLOGW("cannot write %s", path.c_str());
    }
    d_.fn.vkUnmapMemory(d_.handle, memory);
}

double SwapchainFx::takeGpuMs(uint32_t* samples) {
    *samples = gpuSamples_;
    const double mean = gpuSamples_ ? gpuSumMs_ / gpuSamples_ : 0.0;
    gpuSumMs_ = 0;
    gpuSamples_ = 0;
    return mean;
}

VkResult SwapchainFx::submit(VkQueue queue, uint32_t familyIndex, uint32_t i, uint32_t waitCount,
                             const VkSemaphore* waits, VkSemaphore* signal) {
    if (i >= count_) return VK_ERROR_OUT_OF_DATE_KHR;
    auto& fn = d_.fn;
    const VkDevice dev = d_.handle;
    Family* fam = family(familyIndex);
    if (!fam) return VK_ERROR_INITIALIZATION_FAILED;
    ++presents_;

    // Reuse of this slot: its previous submission must be finished.
    VkResult r = fn.vkWaitForFences(dev, 1, &fence_[i], VK_TRUE, kFenceTimeoutNs);
    if (r != VK_SUCCESS) {
        // Resetting or re-recording work the GPU still owns would be worse than failing the present.
        XLOGE("upscale fence wait returned %d; not reusing the slot", static_cast<int>(r));
        return VK_ERROR_DEVICE_LOST;
    }
    if (timed_[i]) {
        uint64_t ts[2];
        if (fn.vkGetQueryPoolResults(dev, queries_, 2 * i, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint32_t bits = fam->timestampBits;
            const uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            gpuSumMs_ += static_cast<double>((ts[1] - ts[0]) & mask) * d_.properties.limits.timestampPeriod * 1e-6;
            ++gpuSamples_;
        }
        timed_[i] = false;
    }
    fn.vkResetFences(dev, 1, &fence_[i]);

    const bool timing = queries_ && fam->timestampBits > 0;
    const bool dump = config_.dumpFrame && presents_ == config_.dumpFrame;
    VkBuffer dumpBuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory dumpMem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool dumpReady = false;
    if (dump) {
        dumpReady = createHostBuffer(static_cast<VkDeviceSize>(in_.width) * in_.height * 4, &dumpBuf[0], &dumpMem[0]) &&
                    createHostBuffer(static_cast<VkDeviceSize>(out_.width) * out_.height * 4, &dumpBuf[1], &dumpMem[1]);
        if (!dumpReady) XLOGW("frame dump skipped: cannot allocate host buffers");
    }

    VkCommandBuffer cmd = fam->buffers[i];
    fn.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(cmd, &bi);
    if (timing) {
        fn.vkCmdResetQueryPool(cmd, queries_, 2 * i, 2);
        fn.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 2 * i);
    }

    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                       VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // The game leaves its image in PRESENT_SRC_KHR, as for any presentable image.
    if (dump && dumpReady) {
        barrier(game_[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {in_.width, in_.height, 1};
        fn.vkCmdCopyImageToBuffer(cmd, game_[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dumpBuf[0], 1, &copy);
        barrier(game_[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    } else {
        barrier(game_[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass_;
    rp.framebuffer = framebuffer_[i];
    rp.renderArea = {{0, 0}, out_};
    fn.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_[i], 0, nullptr);
    const float viewportInfo[4] = {1.0f / in_.width, 1.0f / in_.height, static_cast<float>(in_.width),
                                   static_cast<float>(in_.height)};
    fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof viewportInfo, viewportInfo);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRenderPass(cmd);

    if (dump && dumpReady) {
        barrier(real_[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {out_.width, out_.height, 1};
        fn.vkCmdCopyImageToBuffer(cmd, real_[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dumpBuf[1], 1, &copy);
        barrier(real_[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    // Give the image back in the layout the game expects for its next frame.
    barrier(game_[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    if (timing) fn.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 2 * i + 1);
    fn.vkEndCommandBuffer(cmd);

    std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done_[i];
    r = fn.vkQueueSubmit(queue, 1, &si, fence_[i]);
    if (r != VK_SUCCESS) {
        XLOGE("upscale submit failed (%d)", static_cast<int>(r));
        // Leave the fence signalled so the next use of this slot does not stall.
        VkSubmitInfo none{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        fn.vkQueueSubmit(queue, 1, &none, fence_[i]);
    } else {
        timed_[i] = timing;
        *signal = done_[i];
    }

    if (dump) {
        if (dumpReady && r == VK_SUCCESS && fn.vkWaitForFences(dev, 1, &fence_[i], VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS) {
            writeDump("in", dumpMem[0], in_);
            writeDump("out", dumpMem[1], out_);
        }
        for (int k = 0; k < 2; ++k) {
            if (dumpBuf[k]) fn.vkDestroyBuffer(dev, dumpBuf[k], nullptr);
            if (dumpMem[k]) fn.vkFreeMemory(dev, dumpMem[k], nullptr);
        }
    }
    return r;
}

}  // namespace xhyn
