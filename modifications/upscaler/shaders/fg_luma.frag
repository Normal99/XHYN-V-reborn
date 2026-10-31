// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#version 450

// Quarter-size luma of the game's image, for motion search.
layout(location = 0) in vec2 uv;
layout(location = 0) out float luma;
layout(set = 0, binding = 0) uniform sampler2D source;

void main() {
    luma = dot(texture(source, uv).rgb, vec3(0.299, 0.587, 0.114));
}
