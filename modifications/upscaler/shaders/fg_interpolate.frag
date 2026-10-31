// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#version 450

// The frame halfway between previous and current. Where the two warped images disagree the
// motion is wrong, so the current frame is used alone: a sharp error is less visible than a ghost.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
layout(set = 0, binding = 0) uniform sampler2D previous;
layout(set = 0, binding = 1) uniform sampler2D current;
layout(set = 0, binding = 2) uniform sampler2D motion;

void main() {
    const vec2 offset = texture(motion, uv).xy;
    const vec3 a = texture(previous, uv + 0.5 * offset).rgb;
    const vec3 b = texture(current, uv - 0.5 * offset).rgb;
    const vec3 difference = abs(a - b);
    const float mismatch = smoothstep(0.08, 0.3, max(difference.r, max(difference.g, difference.b)));
    colour = vec4(mix(0.5 * (a + b), b, mismatch), 1.0);
}
