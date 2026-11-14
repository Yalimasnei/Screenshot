// SPDX-License-Identifier: GPL-3.0-only
// Linear scRGB -> sRGB SDR. Normalise by the display's SDR white first, so
// Windows' SDR brightness setting does not alter the screenshot's midtones.
// Keep the lower 80% of linear RGB unchanged. When the frame contains HDR
// highlights, use a smooth shoulder above that point to fit its peak into SDR.
// Scale RGB together by its largest component to avoid clipping one colour
// channel before the others. The colour picker bypasses the shoulder.
#include "core/common.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace {
constexpr int kOetfSteps = 16384;
constexpr double kShoulder = 0.8;

float HalfToFloat(uint16_t h) {
    const uint32_t s = uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    uint32_t f;
    if (e == 0) {
        const float v = std::ldexp(float(m), -24);
        return s ? -v : v;
    }
    f = e == 31 ? (s | 0x7f800000u | (m << 13)) : (s | ((e + 112) << 23) | (m << 13));
    float r;
    memcpy(&r, &f, 4);
    return r;
}

// Nonnegative finite half patterns sort in the same order as their values.
// Ignore negative channels and invalid FP16 values when measuring highlights.
inline uint16_t PositiveHalf(uint16_t h) { return h < 0x7c00 ? h : 0; }
}  // namespace

ToneMapper::ToneMapper(float sdrWhite) : half_(65536) {
    white_ = std::isfinite(sdrWhite) && sdrWhite > 0 ? sdrWhite : 2.5f;
    srgb_.resize(kOetfSteps + 1);
    srgbFloat_.resize(kOetfSteps + 1);
    for (int i = 0; i <= kOetfSteps; ++i) {
        const double x = double(i) / kOetfSteps;
        const double encoded = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
        srgb_[i] = uint8_t(std::lround(255 * encoded));
        srgbFloat_[i] = float(encoded);
    }
    for (uint32_t i = 0; i < 65536; ++i) {
        const float v = HalfToFloat(uint16_t(i));
        half_[i] = std::isfinite(v) ? v : 0.f;
    }
}

void ToneMapper::Rows(const BYTE* src, size_t srcPitch, int w, int y0, int y1, uint32_t* dst, size_t dstPitch,
                      const float* scale) const {
    auto code = [this](float v) -> uint32_t {
        v = v > 0 ? (v < 1 ? v : 1) : 0;
        return srgb_[int(v * kOetfSteps + 0.5f)];
    };
    for (int y = y0; y < y1; ++y) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(src + size_t(y) * srcPitch);
        uint32_t* d = dst + size_t(y) * dstPitch;
        for (int x = 0; x < w; ++x, s += 4) {
            const float r = half_[s[0]], g = half_[s[1]], b = half_[s[2]];
            const uint16_t peak = std::max({PositiveHalf(s[0]), PositiveHalf(s[1]), PositiveHalf(s[2])});
            const float k = scale[peak];
            d[x] = 0xff000000u | code(r * k) << 16 | code(g * k) << 8 | code(b * k);
        }
    }
}

void ToneMapper::MapExact(const BYTE* src, size_t srcPitch, int w, int h, uint32_t* dst, size_t dstPitch) const {
    const float k = 1 / white_;
    auto code = [this, k](float v) -> uint32_t {
        v *= k;
        v = v > 0 ? (v < 1 ? v : 1) : 0;
        return srgb_[int(v * kOetfSteps + 0.5f)];
    };
    for (int y = 0; y < h; ++y) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(src + size_t(y) * srcPitch);
        uint32_t* d = dst + size_t(y) * dstPitch;
        for (int x = 0; x < w; ++x, s += 4)
            d[x] = 0xff000000u | code(half_[s[0]]) << 16 | code(half_[s[1]]) << 8 | code(half_[s[2]]);
    }
}

void ToneMapper::Map(const BYTE* src, size_t srcPitch, int w, int h, uint32_t* dst, size_t dstPitch) const {
    if (w <= 0 || h <= 0) return;
    uint16_t peak = 0;
    for (int y = 0; y < h; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(src + size_t(y) * srcPitch);
        for (int x = 0; x < w; ++x, row += 4)
            peak = std::max({peak, PositiveHalf(row[0]), PositiveHalf(row[1]), PositiveHalf(row[2])});
    }
    const double maxRgb = double(half_[peak]) / white_;
    std::vector<float> scale(0x7c00, 1.f / white_);
    if (maxRgb > 1) {
        // T(x) = knee + d / (1 + b*d), d=x-knee.
        // b makes T(framePeak)=1, with value and slope continuous at the knee.
        // As the frame peak approaches SDR white, b approaches 0 (identity).
        const double b = (maxRgb - 1) / ((1 - kShoulder) * (maxRgb - kShoulder));
        for (uint32_t i = 1; i <= peak; ++i) {
            const double x = double(half_[i]) / white_;
            if (x <= kShoulder) continue;
            const double d = x - kShoulder;
            const double mapped = kShoulder + d / (1 + b * d);
            scale[i] = float(mapped / half_[i]);
        }
    }
    const int n = std::min(h, int(std::clamp(std::thread::hardware_concurrency(), 1u, 16u)));
    std::vector<std::thread> pool;
    for (int i = 1; i < n; ++i)
        pool.emplace_back(&ToneMapper::Rows, this, src, srcPitch, w, h * i / n, h * (i + 1) / n, dst, dstPitch, scale.data());
    Rows(src, srcPitch, w, 0, h / n, dst, dstPitch, scale.data());
    for (auto& t : pool) t.join();
}

void ToneMapper::Map16(const BYTE* src, size_t srcPitch, int w, int h, Rgba16* dst, size_t dstPitch,
                       bool highlights) const {
    if (w <= 0 || h <= 0) return;
    uint16_t peak = 0;
    if (highlights) for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(src + size_t(y) * srcPitch);
        for (int x = 0; x < w; ++x, row += 4)
            peak = std::max({peak, PositiveHalf(row[0]), PositiveHalf(row[1]), PositiveHalf(row[2])});
    }
    const double framePeak = double(half_[peak]) / white_;
    const double b = framePeak > 1 ? (framePeak - 1) / ((1 - kShoulder) * (framePeak - kShoulder)) : 0;
    auto code = [this](float value) {
        const float v = std::clamp(value, 0.f, 1.f) * kOetfSteps;
        const int i = std::min(int(v), kOetfSteps - 1);
        const float f = v - i;
        return uint16_t(std::lround(65535 * (srgbFloat_[i] + f * (srgbFloat_[i + 1] - srgbFloat_[i]))));
    };
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const auto* s = reinterpret_cast<const uint16_t*>(src + size_t(y) * srcPitch);
            Rgba16* d = dst + size_t(y) * dstPitch;
            for (int x = 0; x < w; ++x, s += 4) {
                const float r = std::max(0.f, half_[s[0]]), g = std::max(0.f, half_[s[1]]), z = std::max(0.f, half_[s[2]]);
                const double p = std::max({r, g, z}) / white_;
                double scale = 1.0 / white_;
                if (b && p > kShoulder) {
                    const double delta = p - kShoulder;
                    scale *= (kShoulder + delta / (1 + b * delta)) / p;
                }
                d[x] = {code(float(r * scale)), code(float(g * scale)), code(float(z * scale)), 65535};
            }
        }
    };
    const int n = std::min(h, int(std::clamp(std::thread::hardware_concurrency(), 1u, 16u)));
    std::vector<std::thread> pool;
    for (int i = 1; i < n; ++i) pool.emplace_back(rows, h * i / n, h * (i + 1) / n);
    rows(0, h / n);
    for (auto& thread : pool) thread.join();
}
