// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#pragma once
#include <cstdint>
#include <string>

namespace xhyn {

enum class Mode { Off, Probe, Sgsr1 };

// Who handles BC texture decoding. The original BCn layer is only needed on GPUs without
// native BC support; Auto keeps the hardware path when every GPU has it.
enum class BcnPolicy { Auto, Inner, Native };

struct Config {
    Mode mode = Mode::Probe;
    BcnPolicy bcn = BcnPolicy::Auto;
    // The game is told to draw at surface size / scale, or at render_width x render_height when
    // both are set. On a surface with no fixed size the output is game size * scale instead.
    float scale = 1.5f;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    // Output is the surface's own size unless both out_width and out_height are set.
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    // Surface size to assume, for host tests: their headless surfaces have no fixed size.
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    // Snapdragon GSR 1 shader options (see sgsr1_shader_mobile_edge_direction.frag).
    float edgeSharpness = 2.0f;   // 1.0 .. 2.0
    float edgeThreshold = 8.0f;   // in 1/255 steps; 4 suggested for VR, 8 for phones
    bool edgeDirection = false;
    // Presents one interpolated frame before each game frame. Needs mode sgsr1.
    bool frameGen = false;
    bool frameGenPacing = true;   // show the two frames evenly spaced instead of back to back
    bool gpuTiming = true;
    uint32_t logInterval = 300;   // presents between statistics lines; 0 disables
    uint32_t dumpFrame = 0;       // write input/output PPM files at this present; 0 disables
    std::string dumpDir;
    std::string source;           // where the settings were read from, for the log
};

// Reads xhyn-upscale.conf (key=value lines) and XHYN_UPSCALE_* overrides.
Config loadConfig();
const char* modeName(Mode mode);
const char* bcnName(BcnPolicy policy);

}  // namespace xhyn
