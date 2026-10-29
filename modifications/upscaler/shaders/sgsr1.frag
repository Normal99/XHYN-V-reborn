// Snapdragon(TM) Game Super Resolution 1, adapted for Vulkan GLSL 450.
//
// Derived from sgsr1_shader_mobile_edge_direction.frag in
// https://github.com/SnapdragonGameStudios/snapdragon-gsr
//   Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//   SPDX-License-Identifier: BSD-3-Clause
// The upstream license text is in third_party/snapdragon-gsr/LICENSE.
//
// Changes from upstream: Vulkan 450 syntax, push constant instead of the uniform
// block, operation mode fixed to RGBA (gather channel 1), the edge-direction
// variant selected by a specialization constant, and an optional sRGB encode for
// SRGB swapchain formats (the input is sampled as raw encoded values).
#version 450

precision mediump float;
precision highp int;

layout(constant_id = 0) const bool UseEdgeDirection = false;
layout(constant_id = 1) const bool DstSrgb = false;
layout(constant_id = 2) const float EdgeThreshold = 8.0 / 255.0;
layout(constant_id = 3) const float EdgeSharpness = 2.0;

layout(push_constant) uniform Push {
    highp vec4 viewportInfo;  // 1/width, 1/height, width, height of the input image
} pc;

layout(set = 0, binding = 0) uniform mediump sampler2D ps0;

layout(location = 0) in highp vec2 uv;
layout(location = 0) out vec4 outTarget;

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

vec2 weightY(float dx, float dy, float c, vec3 data) {
    float std = data.x;
    float x;
    if (UseEdgeDirection) {
        vec2 dir = data.yz;
        float edgeDis = (dx * dir.y) + (dy * dir.x);
        x = ((dx * dx) + (dy * dy)) + ((edgeDis * edgeDis) * ((clamp((c * c) * std, 0.0, 1.0) * 0.7) + -1.0));
    } else {
        x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    }
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}

vec2 edgeDirection(vec4 left, vec4 right) {
    float RxLz = right.x + (-left.z);
    float RwLy = right.w + (-left.y);
    vec2 delta;
    delta.x = RxLz + RwLy;
    delta.y = RxLz + (-RwLy);
    float lengthInv = inversesqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    return vec2(delta.x * lengthInv, delta.y * lengthInv);
}

// Full precision: the half-precision default above would shift sRGB output by a code or two.
highp vec3 srgbToLinear(highp vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec4 color;
    color.xyz = textureLod(ps0, uv, 0.0).xyz;

    highp vec2 imgCoord = (uv * pc.viewportInfo.zw) + vec2(-0.5, 0.5);
    highp vec2 imgCoordPixel = floor(imgCoord);
    highp vec2 coord = imgCoordPixel * pc.viewportInfo.xy;
    vec2 pl = imgCoord + (-imgCoordPixel);
    vec4 left = textureGather(ps0, coord, 1);

    float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);
    if (edgeVote > EdgeThreshold) {
        coord.x += pc.viewportInfo.x;

        vec4 right = textureGather(ps0, coord + vec2(pc.viewportInfo.x, 0.0), 1);
        vec4 upDown;
        upDown.xy = textureGather(ps0, coord + vec2(0.0, -pc.viewportInfo.y), 1).wz;
        upDown.zw = textureGather(ps0, coord + vec2(0.0, pc.viewportInfo.y), 1).yx;

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color.y - mean;

        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                      (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +
                     (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        float sumMean = 1.014185e+01 / sum;
        float std = sumMean * sumMean;

        vec3 data = vec3(std, 0.0, 0.0);
        if (UseEdgeDirection) data.yz = edgeDirection(left, right);

        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, data);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, data);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, data);
        aWY += weightY(pl.x, pl.y, left.z, data);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, data);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, data);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, data);

        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        float deltaY = clamp(EdgeSharpness * finalY, minY, maxY) - color.w;

        // smooth high contrast input
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

        color.x = clamp(color.x + deltaY, 0.0, 1.0);
        color.y = clamp(color.y + deltaY, 0.0, 1.0);
        color.z = clamp(color.z + deltaY, 0.0, 1.0);
    }

    outTarget = vec4(DstSrgb ? srgbToLinear(color.xyz) : color.xyz, 1.0);
}
