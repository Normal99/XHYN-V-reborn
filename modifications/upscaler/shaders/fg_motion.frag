// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#version 450

// Block search: for each block of the current frame, where was it in the previous one?
// xy is that offset in UV units, z the remaining difference. A small cost on distance keeps
// still areas such as the HUD at exactly zero.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 motion;
layout(set = 0, binding = 0) uniform sampler2D previous;
layout(set = 0, binding = 1) uniform sampler2D current;

const int kCoarseRadius = 6;  // in luma texels; 4 input pixels each
const float kDistanceCost = 0.002;

float blockCost(vec2 texel, vec2 offset) {
    const vec2 taps[5] = vec2[](vec2(0, 0), vec2(-2, -2), vec2(2, -2), vec2(-2, 2), vec2(2, 2));
    float sum = 0.0;
    for (int i = 0; i < 5; ++i) {
        const vec2 p = uv + taps[i] * texel;
        sum += abs(textureLod(current, p, 0.0).r - textureLod(previous, p + offset * texel, 0.0).r);
    }
    return sum / 5.0;
}

void main() {
    const vec2 texel = 1.0 / vec2(textureSize(current, 0));
    vec2 best = vec2(0.0);
    float bestCost = blockCost(texel, best);
    for (int y = -kCoarseRadius; y <= kCoarseRadius; y += 2) {
        for (int x = -kCoarseRadius; x <= kCoarseRadius; x += 2) {
            const vec2 offset = vec2(x, y);
            const float cost = blockCost(texel, offset) + kDistanceCost * length(offset);
            if (cost < bestCost) {
                bestCost = cost;
                best = offset;
            }
        }
    }
    const vec2 centre = best;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 offset = centre + vec2(x, y);
            const float cost = blockCost(texel, offset) + kDistanceCost * length(offset);
            if (cost < bestCost) {
                bestCost = cost;
                best = offset;
            }
        }
    }
    motion = vec4(best * texel, bestCost, 1.0);
}
