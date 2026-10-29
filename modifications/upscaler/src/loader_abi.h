// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
//
// Minimal copy of the Vulkan loader/layer interface structures that
// libgtav_vulkan.so builds in LayerCreateInstance/LayerCreateDevice (the
// layouts were checked against its disassembly). vk_layer.h is not shipped in
// every NDK, so only what this layer reads is declared here.
#pragma once
#include <vulkan/vulkan.h>

namespace xhyn {

constexpr int kStructureLoaderInstanceCreateInfo = 47;
constexpr int kStructureLoaderDeviceCreateInfo = 48;
constexpr int kLayerLinkInfo = 0;

struct LayerInstanceLink {
    LayerInstanceLink* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    void* pfnNextGetPhysicalDeviceProcAddr;
};

struct LayerInstanceCreateInfo {
    int sType;
    const void* pNext;
    int function;
    union {
        LayerInstanceLink* pLayerInfo;
        void* pfn;
    } u;
};

struct LayerDeviceLink {
    LayerDeviceLink* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
};

struct LayerDeviceCreateInfo {
    int sType;
    const void* pNext;
    int function;
    union {
        LayerDeviceLink* pLayerInfo;
        void* pfn;
    } u;
};

struct VkStructHeader {
    VkStructureType sType;
    const void* pNext;
};

// Returns the first loader link node of the requested kind in a pNext chain.
template <class T>
T* findLink(const void* chain, int sType) {
    for (auto* node = static_cast<const VkStructHeader*>(chain); node;
         node = static_cast<const VkStructHeader*>(node->pNext)) {
        if (static_cast<int>(node->sType) == sType) {
            auto* link = const_cast<T*>(reinterpret_cast<const T*>(node));
            if (link->function == kLayerLinkInfo) return link;
        }
    }
    return nullptr;
}

}  // namespace xhyn
