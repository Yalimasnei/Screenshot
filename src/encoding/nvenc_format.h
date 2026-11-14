// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nv-codec-headers/nvEncodeAPI.h"
#include <algorithm>
#include <vector>

// Choose the actual input specification before configuring the encoder.
// A missing requested capability/format falls back as a pair to 8-bit 4:2:0.
inline bool SelectNvencInput(int& bits, bool& yuv444, bool supports10, bool supports444,
                             const std::vector<NV_ENC_BUFFER_FORMAT>& formats, NV_ENC_BUFFER_FORMAT& selected) {
    auto format = [&] {
        return yuv444 ? (bits == 10 ? NV_ENC_BUFFER_FORMAT_YUV444_10BIT : NV_ENC_BUFFER_FORMAT_YUV444)
                      : (bits == 10 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12);
    };
    if ((bits == 10 && !supports10) || (yuv444 && !supports444)) { bits = 8; yuv444 = false; }
    selected = format();
    if (std::find(formats.begin(), formats.end(), selected) == formats.end()) {
        bits = 8; yuv444 = false; selected = NV_ENC_BUFFER_FORMAT_NV12;
    }
    return std::find(formats.begin(), formats.end(), selected) != formats.end();
}
