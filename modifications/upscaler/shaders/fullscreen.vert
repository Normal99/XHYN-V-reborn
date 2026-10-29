// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#version 450

layout(location = 0) out vec2 uv;

void main() {
    // One oversized triangle covering the viewport; uv is 0..1 over the visible part.
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
