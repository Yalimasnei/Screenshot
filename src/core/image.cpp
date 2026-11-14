// SPDX-License-Identifier: GPL-3.0-only
#include "core/common.h"

void UpdatePreview(Image& img) {
    if (!img.HighPrecision()) return;
    img.px.resize(img.rgba.size());
    for (size_t i = 0; i < img.rgba.size(); ++i) {
        const Rgba16 p = img.rgba[i];
        auto byte = [](uint16_t v) { return uint32_t((uint32_t(v) + 128) / 257); };
        img.px[i] = byte(p.a) << 24 | byte(p.r) << 16 | byte(p.g) << 8 | byte(p.b);
    }
}

void CompositeAnnotations(Image& img, const Image& layer) {
    if (!img.HighPrecision() || img.w != layer.w || img.h != layer.h || layer.px.size() != img.rgba.size()) return;
    for (size_t i = 0; i < img.rgba.size(); ++i) {
        const uint32_t q = layer.px[i];
        const uint32_t a = q >> 24;
        if (!a) continue;  // untouched pixels keep every original bit
        Rgba16& p = img.rgba[i];
        const uint64_t alpha = uint64_t(a) * 65535 + uint64_t(p.a) * (255 - a);
        auto blend = [&](uint16_t base, uint32_t top) {
            const uint64_t n = uint64_t(top * 257) * a * 65535 + uint64_t(base) * p.a * (255 - a);
            return uint16_t((n + alpha / 2) / alpha);
        };
        p.r = blend(p.r, (q >> 16) & 255);
        p.g = blend(p.g, (q >> 8) & 255);
        p.b = blend(p.b, q & 255);
        p.a = uint16_t((alpha + 127) / 255);
    }
    UpdatePreview(img);
}
