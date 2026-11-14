// SPDX-License-Identifier: GPL-3.0-only
#include "core/common.h"
#include <algorithm>
#include <cstring>

bool DecodeCapturePixels(const BYTE* src, size_t pitch, int w, int h, CapturePixels format, int outputBits,
                         bool hdr, float white, Image& image, Image& exact) {
    const size_t bytes = format == CapturePixels::ScRgb16 ? 8 : 4;
    if (!src || w <= 0 || h <= 0 || pitch < size_t(w) * bytes) return false;
    image = Image{};
    exact = Image{};
    image.w = w; image.h = h;
    const bool highSource = format == CapturePixels::ScRgb16 || format == CapturePixels::Rgb10;
    image.bits = highSource ? std::clamp(outputBits, 8, format == CapturePixels::Rgb10 ? 10 : 16) : 8;
    image.px.resize(size_t(w) * h);
    if (image.bits > 8) image.rgba.resize(image.px.size());
    if (format == CapturePixels::ScRgb16) {
        ToneMapper tm(white);
        if (image.bits > 8) {
            tm.Map16(src, pitch, w, h, image.rgba.data(), size_t(w), hdr);
            UpdatePreview(image);
        } else if (hdr) tm.Map(src, pitch, w, h, image.px.data(), size_t(w));
        else tm.MapExact(src, pitch, w, h, image.px.data(), size_t(w));
        exact.w = w; exact.h = h;
        exact.px.resize(image.px.size());
        tm.MapExact(src, pitch, w, h, exact.px.data(), size_t(w));
    } else for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint32_t p;
        memcpy(&p, src + size_t(y) * pitch + size_t(x) * 4, 4);
        const size_t i = size_t(y) * w + x;
        if (format == CapturePixels::Rgb10) {
            auto value = [](uint32_t v) { return uint16_t((v * 65535u + 511) / 1023); };
            const Rgba16 q{value(p & 1023), value((p >> 10) & 1023), value((p >> 20) & 1023), 65535};
            if (image.bits > 8) image.rgba[i] = q;
            auto byte = [](uint16_t v) { return uint32_t((uint32_t(v) + 128) / 257); };
            image.px[i] = 0xff000000u | byte(q.r) << 16 | byte(q.g) << 8 | byte(q.b);
        } else if (format == CapturePixels::Rgba8) {
            image.px[i] = 0xff000000u | (p & 255) << 16 | (p & 0xff00) | ((p >> 16) & 255);
        } else image.px[i] = p | 0xff000000u;
    }
    image.depths.push_back({{0, 0, w, h}, image.bits});
    return true;
}
