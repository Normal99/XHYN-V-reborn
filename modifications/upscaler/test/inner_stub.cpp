// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
//
// Stand-in for the original BCn layer in host tests. It follows the same contract as
// the real one: it is entered through BCnLayer_Get*ProcAddr, takes the next-layer
// pointers from the loader link chain (and advances the chain), and forwards.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "../src/loader_abi.h"

using namespace xhyn;

namespace {
std::mutex g_lock;
std::map<void*, PFN_vkGetInstanceProcAddr> g_instanceNext;
std::map<void*, PFN_vkGetDeviceProcAddr> g_deviceNext;
PFN_vkGetInstanceProcAddr g_lastGipa = nullptr;
int g_deviceFromInstance = 0;

VkResult stubCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    auto* link = findLink<LayerInstanceCreateInfo>(ci->pNext, kStructureLoaderInstanceCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // advance, as real layers do
    auto create = reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"));
    VkResult r = create(ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_instanceNext[*out] = next;
        g_lastGipa = next;
        fprintf(stderr, "STUB: vkCreateInstance forwarded\n");
    }
    return r;
}

VkResult stubCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    auto* link = findLink<LayerDeviceCreateInfo>(ci->pNext, kStructureLoaderDeviceCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    // The device create function belongs to the instance; the stub only knows the driver's pointer.
    auto create = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create) {
        VkInstance inst = VK_NULL_HANDLE;
        { std::lock_guard<std::mutex> lock(g_lock); inst = g_instanceNext.empty() ? VK_NULL_HANDLE : reinterpret_cast<VkInstance>(g_instanceNext.begin()->first); }
        create = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(inst, "vkCreateDevice"));
    }
    VkResult r = create(pd, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_deviceNext[*out] = nextGdpa;
        ++g_deviceFromInstance;
        fprintf(stderr, "STUB: vkCreateDevice forwarded\n");
    }
    return r;
}
}  // namespace

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT PFN_vkVoidFunction BCnLayer_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(stubCreateInstance);
    if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(stubCreateDevice);
    PFN_vkGetInstanceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_instanceNext.find(instance);
        next = it != g_instanceNext.end() ? it->second : g_lastGipa;
    }
    return next ? next(instance, name) : nullptr;
}

EXPORT PFN_vkVoidFunction BCnLayer_GetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_deviceNext.find(device);
        if (it != g_deviceNext.end()) next = it->second;
    }
    return next ? next(device, name) : nullptr;
}

// Used by the harness to prove the shim's tail-jump forwards the result.
EXPORT const char* BCnLayer_Diagnostics() { return "inner stub diagnostics"; }
