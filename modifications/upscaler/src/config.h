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
    // Output size is input * scale unless both out_width and out_height are set.
    float scale = 1.5f;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    // Snapdragon GSR 1 shader options (see sgsr1_shader_mobile_edge_direction.frag).
    float edgeSharpness = 2.0f;   // 1.0 .. 2.0
    float edgeThreshold = 8.0f;   // in 1/255 steps; 4 suggested for VR, 8 for phones
    bool edgeDirection = false;
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
