// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
//
// Replacement for libgtav_bcn.so. libgtav_vulkan.so dlopens that name and looks up
// exactly three symbols; the original library is shipped next to this one as
// libxhyn_bcn.so and every call is forwarded to it (see layer.cpp).
#include <vulkan/vulkan.h>

#include "layer.h"

#define XHYN_EXPORT extern "C" __attribute__((visibility("default")))

XHYN_EXPORT PFN_vkVoidFunction BCnLayer_GetInstanceProcAddr(VkInstance instance, const char* name) {
    return xhyn::layerGetInstanceProcAddr(instance, name);
}

XHYN_EXPORT PFN_vkVoidFunction BCnLayer_GetDeviceProcAddr(VkDevice device, const char* name) {
    return xhyn::layerGetDeviceProcAddr(device, name);
}

// BCnLayer_Diagnostics has no public prototype, so it is forwarded with a tail jump
// that keeps every argument register and the return value untouched. It goes to the
// original layer only for instances created through it (layer.cpp), else to a stub.
// libgtav_vulkan.so appends the result to a std::string, so the stub must not return null.
extern "C" {
static const char* diagnosticsFallback() { return ""; }
__attribute__((visibility("hidden"))) void* xhyn_diagnostics_target = reinterpret_cast<void*>(&diagnosticsFallback);
}

#if defined(__aarch64__)
asm(".text\n"
    ".globl BCnLayer_Diagnostics\n"
    ".type BCnLayer_Diagnostics,%function\n"
    ".p2align 2\n"
    "BCnLayer_Diagnostics:\n"
    "    hint #34\n"  // bti c: a valid indirect-call target if the library is ever built BTI-guarded
    "    adrp x16, xhyn_diagnostics_target\n"
    "    ldr x16, [x16, :lo12:xhyn_diagnostics_target]\n"
    "    br x16\n"
    ".size BCnLayer_Diagnostics, .-BCnLayer_Diagnostics\n");
#elif defined(__x86_64__)
asm(".text\n"
    ".globl BCnLayer_Diagnostics\n"
    ".type BCnLayer_Diagnostics,@function\n"
    "BCnLayer_Diagnostics:\n"
    "    jmp *xhyn_diagnostics_target(%rip)\n"
    ".size BCnLayer_Diagnostics, .-BCnLayer_Diagnostics\n");
#else
#error "unsupported architecture"
#endif

namespace xhyn {
void useInnerDiagnostics(bool inner) {
    const InnerLayer& layer = innerLayer();
    xhyn_diagnostics_target = inner && layer.diagnostics ? layer.diagnostics : reinterpret_cast<void*>(&diagnosticsFallback);
}
}  // namespace xhyn
