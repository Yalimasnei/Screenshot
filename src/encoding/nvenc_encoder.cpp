// SPDX-License-Identifier: GPL-3.0-only
// HEIC encoding through the NVIDIA video encoder (NVENC) in the graphics
// driver: RGB is converted to Y'CbCr 4:2:0 or 4:4:4 (BT.709, full range) at
// 8 or 10 bits, encoded with lossless tuning, then wrapped in HEIF.
// nvcuda.dll / nvEncodeAPI64.dll come with the NVIDIA driver; nothing is bundled.
#include "core/common.h"
#include "nv-codec-headers/nvEncodeAPI.h"
#include "encoding/nvenc_format.h"
#include <algorithm>
#include <cstring>

namespace {
// --- the few CUDA driver calls needed (from nvcuda.dll) ---
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
struct Cuda {
    CUresult(WINAPI* Init)(unsigned) = nullptr;
    CUresult(WINAPI* DeviceGetCount)(int*) = nullptr;
    CUresult(WINAPI* DeviceGet)(CUdevice*, int) = nullptr;
    CUresult(WINAPI* CtxCreate)(CUcontext*, unsigned, CUdevice) = nullptr;
    CUresult(WINAPI* CtxDestroy)(CUcontext) = nullptr;
};

struct Module {
    HMODULE handle = nullptr;
    Module() = default;
    explicit Module(HMODULE value) : handle(value) {}
    ~Module() { if (handle) FreeLibrary(handle); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    Module& operator=(Module&& other) noexcept {
        if (this != &other) {
            if (handle) FreeLibrary(handle);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
};

struct Driver {
    Module cuda;
    Module encoder;
    bool ok = false;
    int deviceOrdinal = -1;
    NvencCapabilities caps;
    Cuda cu;
    NV_ENCODE_API_FUNCTION_LIST nv{};
    Driver() = default;
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = default;
    Driver& operator=(Driver&&) = default;
};

struct EncoderSupport {
    NvencCapabilities caps;
    std::vector<NV_ENC_BUFFER_FORMAT> formats;
    int minW = 0, minH = 0, maxW = 0, maxH = 0;
};

bool QuerySupport(Driver& d, void* enc, EncoderSupport& support) {
    auto& nv = d.nv;
    uint32_t count = 0, got = 0;
    if (nv.nvEncGetEncodeGUIDCount(enc, &count) != NV_ENC_SUCCESS || !count || count > 256) return false;
    std::vector<GUID> codecs(count);
    if (nv.nvEncGetEncodeGUIDs(enc, codecs.data(), count, &got) != NV_ENC_SUCCESS || got > count) return false;
    if (std::none_of(codecs.begin(), codecs.begin() + got, [](const GUID& guid) {
        return IsEqualGUID(guid, NV_ENC_CODEC_HEVC_GUID);
    })) return false;
    auto cap = [&](NV_ENC_CAPS c) {
        NV_ENC_CAPS_PARAM p = {};
        p.version = NV_ENC_CAPS_PARAM_VER;
        p.capsToQuery = c;
        int value = 0;
        return nv.nvEncGetEncodeCaps(enc, NV_ENC_CODEC_HEVC_GUID, &p, &value) == NV_ENC_SUCCESS ? value : 0;
    };
    if (!cap(NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE)) return false;
    count = got = 0;
    if (nv.nvEncGetInputFormatCount(enc, NV_ENC_CODEC_HEVC_GUID, &count) != NV_ENC_SUCCESS || !count || count > 256)
        return false;
    support.formats.resize(count);
    if (nv.nvEncGetInputFormats(enc, NV_ENC_CODEC_HEVC_GUID, support.formats.data(), count, &got) != NV_ENC_SUCCESS || got > count)
        return false;
    support.formats.resize(got);
    auto accepts = [&](NV_ENC_BUFFER_FORMAT format) {
        return std::find(support.formats.begin(), support.formats.end(), format) != support.formats.end();
    };
    if (!accepts(NV_ENC_BUFFER_FORMAT_NV12)) return false;
    support.minW = cap(NV_ENC_CAPS_WIDTH_MIN); support.minH = cap(NV_ENC_CAPS_HEIGHT_MIN);
    support.maxW = cap(NV_ENC_CAPS_WIDTH_MAX); support.maxH = cap(NV_ENC_CAPS_HEIGHT_MAX);
    if (support.minW <= 0 || support.minH <= 0 || support.maxW < support.minW || support.maxH < support.minH)
        return false;
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P7_GUID,
                                      NV_ENC_TUNING_INFO_LOSSLESS, &preset) != NV_ENC_SUCCESS) return false;
    const bool ten = cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) != 0;
    const bool chroma444 = cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE) != 0;
    support.caps = {true, ten && accepts(NV_ENC_BUFFER_FORMAT_YUV420_10BIT),
                    chroma444 && accepts(NV_ENC_BUFFER_FORMAT_YUV444),
                    ten && chroma444 && accepts(NV_ENC_BUFFER_FORMAT_YUV444_10BIT)};
    return true;
}

void ProbeDevices(Driver& d, int count) {
    for (int ordinal = 0; ordinal < count; ++ordinal) {
        CUdevice device = 0;
        CUcontext ctx = nullptr;
        if (d.cu.DeviceGet(&device, ordinal) != 0 || d.cu.CtxCreate(&ctx, 0, device) != 0) continue;
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp = {};
        sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        sp.device = ctx;
        sp.apiVersion = NVENCAPI_VERSION;
        void* enc = nullptr;
        EncoderSupport support;
        const bool opened = d.nv.nvEncOpenEncodeSessionEx(&sp, &enc) == NV_ENC_SUCCESS;
        const bool supported = opened && QuerySupport(d, enc, support);
        if (opened) d.nv.nvEncDestroyEncoder(enc);
        d.cu.CtxDestroy(ctx);
        if (!supported) continue;
        // Encoding uses this same adapter; an unrelated CUDA device is not proof of NVENC support.
        d.deviceOrdinal = ordinal;
        d.caps = support.caps;
        d.ok = true;
        break;
    }
}

Driver LoadDriver() {
    Driver d;
    d.cuda = Module(LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    d.encoder = Module(LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!d.cuda.handle || !d.encoder.handle) return d;
    d.cu.Init = reinterpret_cast<decltype(d.cu.Init)>(GetProcAddress(d.cuda.handle, "cuInit"));
    d.cu.DeviceGetCount = reinterpret_cast<decltype(d.cu.DeviceGetCount)>(GetProcAddress(d.cuda.handle, "cuDeviceGetCount"));
    d.cu.DeviceGet = reinterpret_cast<decltype(d.cu.DeviceGet)>(GetProcAddress(d.cuda.handle, "cuDeviceGet"));
    d.cu.CtxCreate = reinterpret_cast<decltype(d.cu.CtxCreate)>(GetProcAddress(d.cuda.handle, "cuCtxCreate_v2"));
    d.cu.CtxDestroy = reinterpret_cast<decltype(d.cu.CtxDestroy)>(GetProcAddress(d.cuda.handle, "cuCtxDestroy_v2"));
    using MaxVer = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using Create = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto maxVer = reinterpret_cast<MaxVer>(GetProcAddress(d.encoder.handle, "NvEncodeAPIGetMaxSupportedVersion"));
    auto create = reinterpret_cast<Create>(GetProcAddress(d.encoder.handle, "NvEncodeAPICreateInstance"));
    if (!d.cu.Init || !d.cu.DeviceGetCount || !d.cu.DeviceGet || !d.cu.CtxCreate || !d.cu.CtxDestroy || !maxVer || !create)
        return d;
    uint32_t v = 0;
    const uint32_t need = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (maxVer(&v) != NV_ENC_SUCCESS || v < need) {
        Log(L"NVENC: driver supports API %u.%u, need %u.%u", v >> 4, v & 15, need >> 4, need & 15);
        return d;
    }
    d.nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    int count = 0;
    if (create(&d.nv) == NV_ENC_SUCCESS && d.cu.Init(0) == 0 && d.cu.DeviceGetCount(&count) == 0 && count > 0)
        ProbeDevices(d, count);
    return d;
}

Driver& Load() {
    static Driver driver = LoadDriver();
    return driver;
}

using Bytes = std::vector<BYTE>;
void Put32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(BYTE(v >> s)); }
void Put16(Bytes& b, uint16_t v) { b.push_back(BYTE(v >> 8)); b.push_back(BYTE(v)); }
void Tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }
size_t Begin(Bytes& b, const char* type, int version = -1, uint32_t flags = 0) {
    const size_t at = b.size();
    Put32(b, 0);
    Tag(b, type);
    if (version >= 0) Put32(b, uint32_t(version) << 24 | flags);
    return at;
}
bool End(Bytes& b, size_t at) {
    if (at > b.size() || b.size() - at > UINT32_MAX) return false;
    const uint32_t n = uint32_t(b.size() - at);
    for (int i = 0; i < 4; ++i) b[at + i] = BYTE(n >> (24 - 8 * i));
    return true;
}

// Splits an Annex B stream into NAL units (start codes removed).
std::vector<Bytes> SplitNals(const BYTE* p, size_t n) {
    std::vector<Bytes> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && p[end - 1] == 0) --end;  // trailing_zero_8bits / 4-byte start code
                out.emplace_back(p + start, p + end);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) out.emplace_back(p + start, p + n);
    return out;
}
int NalType(const Bytes& nal) { return nal.empty() ? -1 : (nal[0] >> 1) & 0x3F; }

// HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 8.3.3).
bool MakeHvcC(const Bytes& vps, const Bytes& sps, const Bytes& pps, int bits, bool yuv444, Bytes& out) {
    if ((bits != 8 && bits != 10) || vps.empty() || sps.empty() || pps.empty() ||
        vps.size() > UINT16_MAX || sps.size() > UINT16_MAX || pps.size() > UINT16_MAX)
        return false;
    Bytes r;  // SPS RBSP (emulation prevention removed), first 15 bytes are enough
    for (size_t i = 0; i < sps.size() && r.size() < 16; ++i) {
        if (i >= 2 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue;
        r.push_back(sps[i]);
    }
    if (r.size() < 15) return false;
    out.clear();
    out.push_back(1);                                // configurationVersion
    out.insert(out.end(), r.begin() + 3, r.begin() + 15);  // profile byte, compat flags, constraint flags, level
    Put16(out, 0xF000);                              // min_spatial_segmentation_idc = 0
    out.push_back(0xFC);                             // parallelismType = 0
    out.push_back(BYTE(0xFC | (yuv444 ? 3 : 1)));      // chroma_format_idc
    out.push_back(BYTE(0xF8 | (bits - 8)));          // bit_depth_luma_minus8
    out.push_back(BYTE(0xF8 | (bits - 8)));          // bit_depth_chroma_minus8
    Put16(out, 0);                                   // avgFrameRate
    out.push_back(0x0F);                             // 1 temporal layer, nested, 4-byte NAL lengths
    out.push_back(3);
    for (const Bytes* ps : {&vps, &sps, &pps}) {
        out.push_back(BYTE(0x80 | NalType(*ps)));    // array_completeness = 1
        Put16(out, 1);
        Put16(out, uint16_t(ps->size()));
        out.insert(out.end(), ps->begin(), ps->end());
    }
    return true;
}

struct HevcItem { Bytes sample, config; };
bool ParseHevcItem(const Bytes& annexB, int bits, bool yuv444, HevcItem& item) {
    item.sample.clear();
    item.config.clear();
    const std::vector<Bytes> nals = SplitNals(annexB.data(), annexB.size());
    const Bytes *vps = nullptr, *sps = nullptr, *pps = nullptr;
    Bytes& sample = item.sample;  // length-prefixed NAL units except parameter sets / AUD
    for (const Bytes& n : nals) {
        const int t = NalType(n);
        if (t == 32) vps = &n;
        else if (t == 33) sps = &n;
        else if (t == 34) pps = &n;
        else if (t != 35) {
            if (n.size() > UINT32_MAX - 4 || sample.size() > UINT32_MAX - 4 - n.size()) return false;
            Put32(sample, uint32_t(n.size()));
            sample.insert(sample.end(), n.begin(), n.end());
        }
    }
    return vps && sps && pps && !sample.empty() && MakeHvcC(*vps, *sps, *pps, bits, yuv444, item.config);
}

// HEIF colour item plus an optional, independently coded alpha auxiliary item.
// Separate HEVC items preserve compatibility with HEIF decoders; a raw NVENC
// multi-layer video stream is not itself a HEIF alpha auxiliary image.
bool WrapHeic(const Bytes& annexB, int w, int h, int bits, bool yuv444, const Bytes& alpha, Bytes& o) {
    HevcItem color, mask;
    if (!ParseHevcItem(annexB, bits, yuv444, color) ||
        (!alpha.empty() && !ParseHevcItem(alpha, 8, false, mask))) return false;
    if (w <= 0 || h <= 0 || color.sample.size() > UINT32_MAX ||
        (!alpha.empty() && mask.sample.size() > UINT32_MAX)) return false;
    const int count = alpha.empty() ? 1 : 2;

    o.clear();
    size_t box = Begin(o, "ftyp");
    const char* brand = yuv444 ? "heix" : "heic";
    Tag(o, brand); Put32(o, 0); Tag(o, "mif1"); Tag(o, brand);
    if (!End(o, box)) return false;
    const size_t meta = Begin(o, "meta", 0);
    box = Begin(o, "hdlr", 0); Put32(o, 0); Tag(o, "pict"); Put32(o, 0); Put32(o, 0); Put32(o, 0); o.push_back(0); if (!End(o, box)) return false;
    box = Begin(o, "pitm", 0); Put16(o, 1); if (!End(o, box)) return false;
    box = Begin(o, "iinf", 0); Put16(o, uint16_t(count));
    for (int id = 1; id <= count; ++id) {
        const size_t infe = Begin(o, "infe", 2, id == 2 ? 1 : 0);  // auxiliary item is hidden
        Put16(o, uint16_t(id)); Put16(o, 0); Tag(o, "hvc1"); o.push_back(0); if (!End(o, infe)) return false;
    }
    if (!End(o, box)) return false;
    box = Begin(o, "iloc", 0);
    o.push_back(0x44); o.push_back(0x00);                 // 4-byte offsets and lengths
    Put16(o, uint16_t(count));
    size_t offsetAt[2]{};
    for (int id = 1; id <= count; ++id) {
        Put16(o, uint16_t(id)); Put16(o, 0); Put16(o, 1);  // one absolute extent per item
        offsetAt[id - 1] = o.size();
        Put32(o, 0); Put32(o, uint32_t(id == 1 ? color.sample.size() : mask.sample.size()));
    }
    if (!End(o, box)) return false;
    box = Begin(o, "iprp");
    {
        const size_t ipco = Begin(o, "ipco");
        { const size_t p = Begin(o, "hvcC"); o.insert(o.end(), color.config.begin(), color.config.end()); if (!End(o, p)) return false; }  // 1
        { const size_t p = Begin(o, "ispe", 0); Put32(o, uint32_t(w)); Put32(o, uint32_t(h)); if (!End(o, p)) return false; }  // 2
        { const size_t p = Begin(o, "colr"); Tag(o, "nclx"); Put16(o, 1); Put16(o, 13); Put16(o, 1); o.push_back(0x80); if (!End(o, p)) return false; }  // 3: BT.709 / sRGB / BT.709 matrix, full range
        { const size_t p = Begin(o, "pixi", 0); o.push_back(3); o.push_back(BYTE(bits)); o.push_back(BYTE(bits)); o.push_back(BYTE(bits)); if (!End(o, p)) return false; }  // 4
        if (count == 2) {
            { const size_t p = Begin(o, "hvcC"); o.insert(o.end(), mask.config.begin(), mask.config.end()); if (!End(o, p)) return false; }  // 5
            { const size_t p = Begin(o, "pixi", 0); o.push_back(1); o.push_back(8); if (!End(o, p)) return false; }  // 6: luma is alpha
            {
                const size_t p = Begin(o, "auxC", 0);
                constexpr char type[] = "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
                o.insert(o.end(), type, type + sizeof(type)); if (!End(o, p)) return false;
            }  // 7
        }
        if (!End(o, ipco)) return false;
        const size_t ipma = Begin(o, "ipma", 0);
        Put32(o, uint32_t(count)); Put16(o, 1); o.push_back(4);
        o.push_back(0x81); o.push_back(0x02); o.push_back(0x03); o.push_back(0x04);  // hvcC is essential
        if (count == 2) {
            Put16(o, 2); o.push_back(4);
            o.push_back(0x85); o.push_back(0x02); o.push_back(0x06); o.push_back(0x87);
        }
        if (!End(o, ipma)) return false;
    }
    if (!End(o, box)) return false;
    if (count == 2) {
        box = Begin(o, "iref", 0);
        const size_t ref = Begin(o, "auxl"); Put16(o, 2); Put16(o, 1); Put16(o, 1); if (!End(o, ref)) return false;
        if (!End(o, box)) return false;
    }
    if (!End(o, meta)) return false;
    box = Begin(o, "mdat");
    if (o.size() > UINT32_MAX) return false;
    const uint32_t dataAt = uint32_t(o.size());
    o.insert(o.end(), color.sample.begin(), color.sample.end());
    o.insert(o.end(), mask.sample.begin(), mask.sample.end());
    if (o.size() > UINT32_MAX || !End(o, box)) return false;
    for (int id = 0; id < count; ++id) {
        const size_t at = size_t(dataAt) + (id ? color.sample.size() : 0);
        if (at > UINT32_MAX) return false;
        for (int i = 0; i < 4; ++i) o[offsetAt[id] + i] = BYTE(at >> (24 - 8 * i));
    }
    return true;
}

// One-frame YCbCr encode (BT.709, full range) at 8 or 10 bits. Lossless
// tuning preserves the quantised YCbCr samples, not the original RGB.
// `img` must have even size. Unsupported specifications fall back to 8-bit
// 4:2:0. For the alpha item, write alpha directly to luma without an OETF or
// RGB conversion; neutral chroma makes it a grayscale auxiliary image.
bool EncodeFrame(Driver& d, const Image& img, int& bits, bool& yuv444, Bytes& out, std::wstring& why,
                 bool alpha = false) {
    CUdevice dev = 0;
    CUcontext ctx = nullptr;
    if (d.cu.DeviceGet(&dev, d.deviceOrdinal) != 0 || d.cu.CtxCreate(&ctx, 0, dev) != 0) { why = Tr(L"无法初始化 NVIDIA 显卡", L"Could not initialize the NVIDIA GPU"); return false; }
    auto& nv = d.nv;
    void* enc = nullptr;
    NV_ENC_INPUT_PTR in = nullptr;
    NV_ENC_OUTPUT_PTR bs = nullptr;
    bool ok = false;
    do {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp = {};
        sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        sp.device = ctx;
        sp.apiVersion = NVENCAPI_VERSION;
        if (nv.nvEncOpenEncodeSessionEx(&sp, &enc) != NV_ENC_SUCCESS) { why = Tr(L"显卡编码器忙或不可用", L"The GPU encoder is busy or unavailable"); enc = nullptr; break; }
        EncoderSupport support;
        if (!QuerySupport(d, enc, support)) {
            why = Tr(L"显卡不支持所需的 HEVC 编码", L"The GPU does not support the required HEVC encoding"); break;
        }
        NV_ENC_BUFFER_FORMAT fmt = NV_ENC_BUFFER_FORMAT_UNDEFINED;
        if (!SelectNvencInput(bits, yuv444, support.caps.tenBit || support.caps.yuv444TenBit,
                              support.caps.yuv444 || support.caps.yuv444TenBit, support.formats, fmt)) {
            why = Tr(L"显卡编码器不接受 4:2:0 输入", L"The GPU encoder does not accept 4:2:0 input"); break;
        }
        const bool ten = bits == 10;
        const int minW = support.minW, minH = support.minH;
        if (img.w < minW || img.h < minH || img.w > support.maxW || img.h > support.maxH) {
            wchar_t t[96];
            swprintf(t, 96, Tr(L"尺寸超出显卡编码范围（最小 %d×%d）", L"Size is outside the GPU encoding range (minimum %d×%d)"), minW, minH);
            why = t;
            break;
        }
        NV_ENC_PRESET_CONFIG preset = {};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P7_GUID,
                                            NV_ENC_TUNING_INFO_LOSSLESS, &preset) != NV_ENC_SUCCESS) {
            why = Tr(L"显卡编码器设置失败", L"Could not configure the GPU encoder");
            break;
        }
        NV_ENC_CONFIG cfg = preset.presetCfg;
        cfg.version = NV_ENC_CONFIG_VER;
        cfg.profileGUID = yuv444 ? NV_ENC_HEVC_PROFILE_FREXT_GUID
                                : (ten ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID);
        cfg.gopLength = 1;
        cfg.frameIntervalP = 1;
        auto& hevc = cfg.encodeCodecConfig.hevcConfig;
        hevc.chromaFormatIDC = yuv444 ? 3 : 1;
        hevc.inputBitDepth = ten ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        hevc.outputBitDepth = hevc.inputBitDepth;
        hevc.idrPeriod = 1;
        hevc.repeatSPSPPS = 1;
        auto& vui = hevc.hevcVUIParameters;
        vui.videoSignalTypePresentFlag = 1;
        vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        vui.videoFullRangeFlag = 1;
        vui.colourDescriptionPresentFlag = 1;
        vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
        vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        NV_ENC_INITIALIZE_PARAMS ip = {};
        ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
        ip.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
        ip.presetGUID = NV_ENC_PRESET_P7_GUID;
        ip.tuningInfo = NV_ENC_TUNING_INFO_LOSSLESS;
        ip.encodeWidth = ip.darWidth = uint32_t(img.w);
        ip.encodeHeight = ip.darHeight = uint32_t(img.h);
        ip.frameRateNum = 1;
        ip.frameRateDen = 1;
        ip.enablePTD = 1;
        ip.encodeConfig = &cfg;
        if (nv.nvEncInitializeEncoder(enc, &ip) != NV_ENC_SUCCESS) { why = Tr(L"显卡编码器初始化失败", L"Could not initialize the GPU encoder"); break; }

        NV_ENC_CREATE_INPUT_BUFFER cib = {};
        cib.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        cib.width = uint32_t(img.w);
        cib.height = uint32_t(img.h);
        cib.bufferFmt = fmt;
        NV_ENC_CREATE_BITSTREAM_BUFFER cbb = {};
        cbb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv.nvEncCreateInputBuffer(enc, &cib) != NV_ENC_SUCCESS) { why = Tr(L"显卡显存不足", L"Not enough GPU memory"); break; }
        in = cib.inputBuffer;
        if (nv.nvEncCreateBitstreamBuffer(enc, &cbb) != NV_ENC_SUCCESS) { why = Tr(L"显卡显存不足", L"Not enough GPU memory"); break; }
        bs = cbb.bitstreamBuffer;

        NV_ENC_LOCK_INPUT_BUFFER lk = {};
        lk.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lk.inputBuffer = in;
        if (nv.nvEncLockInputBuffer(enc, &lk) != NV_ENC_SUCCESS) { why = Tr(L"显卡编码失败", L"GPU encoding failed"); break; }
        BYTE* base = static_cast<BYTE*>(lk.bufferDataPtr);
        const size_t pitch = lk.pitch;
        BYTE* uv = base + pitch * size_t(img.h);
        // RGB -> Y'CbCr, BT.709 full range: Y, Cb, Cr scaled to the code range.
        const float maxv = ten ? 1023.f : 255.f, mid = ten ? 512.f : 128.f;
        auto put = [&](BYTE* row, int i, float v) {
            const int q = int(std::min(std::max(v + 0.5f, 0.f), maxv));
            if (ten) { const uint16_t s = uint16_t(q << 6); memcpy(row + 2 * i, &s, 2); }  // P010: value in the high 10 bits
            else row[i] = BYTE(q);
        };
        if (alpha) {
            for (int y = 0; y < img.h; ++y) {
                for (int x = 0; x < img.w; ++x)
                    base[size_t(y) * pitch + x] = BYTE(img.px[size_t(y) * img.w + x] >> 24);
            }
            for (int y = 0; y < img.h / 2; ++y) std::memset(uv + size_t(y) * pitch, 128, size_t(img.w));
        } else if (yuv444) {
            BYTE* crPlane = uv + pitch * size_t(img.h);
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x) {
                    const Rgba16 p = img.Pixel16(size_t(y) * img.w + x);
                    const float r = p.r / 65535.f, g = p.g / 65535.f, b = p.b / 65535.f;
                    const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    put(base + size_t(y) * pitch, x, Y * maxv);
                    put(uv + size_t(y) * pitch, x, (b - Y) / 1.8556f * maxv + mid);
                    put(crPlane + size_t(y) * pitch, x, (r - Y) / 1.5748f * maxv + mid);
                }
        } else for (int y = 0; y < img.h; y += 2)
            for (int x = 0; x < img.w; x += 2) {
                float cb = 0, cr = 0;
                for (int k = 0; k < 4; ++k) {
                    const int xx = x + (k & 1), yy = y + (k >> 1);
                    const Rgba16 p = img.Pixel16(size_t(yy) * img.w + xx);
                    const float r = p.r / 65535.f, g = p.g / 65535.f, b = p.b / 65535.f;
                    const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    put(base + size_t(yy) * pitch, xx, Y * maxv);
                    cb += (b - Y) / 1.8556f;
                    cr += (r - Y) / 1.5748f;
                }
                BYTE* row = uv + size_t(y / 2) * pitch;
                put(row, x, cb / 4 * maxv + mid);      // Cb at even index, Cr next to it
                put(row, x + 1, cr / 4 * maxv + mid);
            }
        nv.nvEncUnlockInputBuffer(enc, in);

        NV_ENC_PIC_PARAMS pic = {};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = uint32_t(img.w);
        pic.inputHeight = uint32_t(img.h);
        pic.inputPitch = uint32_t(pitch);
        pic.inputBuffer = in;
        pic.outputBitstream = bs;
        pic.bufferFmt = fmt;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        NVENCSTATUS st = nv.nvEncEncodePicture(enc, &pic);
        if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
            NV_ENC_PIC_PARAMS eos = {};
            eos.version = NV_ENC_PIC_PARAMS_VER;
            eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
            st = nv.nvEncEncodePicture(enc, &eos);
        }
        if (st != NV_ENC_SUCCESS) { why = Tr(L"显卡编码失败", L"GPU encoding failed"); break; }
        NV_ENC_LOCK_BITSTREAM lb = {};
        lb.version = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = bs;
        if (nv.nvEncLockBitstream(enc, &lb) != NV_ENC_SUCCESS) { why = Tr(L"显卡编码失败", L"GPU encoding failed"); break; }
        const BYTE* data = static_cast<const BYTE*>(lb.bitstreamBufferPtr);
        out.assign(data, data + lb.bitstreamSizeInBytes);
        nv.nvEncUnlockBitstream(enc, bs);
        ok = !out.empty();
    } while (false);
    if (enc) {
        if (in) nv.nvEncDestroyInputBuffer(enc, in);
        if (bs) nv.nvEncDestroyBitstreamBuffer(enc, bs);
        nv.nvEncDestroyEncoder(enc);
    }
    d.cu.CtxDestroy(ctx);
    return ok;
}
}  // namespace

const NvencCapabilities& GetNvencCapabilities() { return Load().caps; }
bool NvencAvailable() { return GetNvencCapabilities().available; }

// `img` must have even width and height. Transparent captures and unavailable
// input specifications use 8-bit 4:2:0, without changing the saved preference.
bool EncodeHeicNvenc(const Image& img, int& bits, std::vector<BYTE>& out, std::wstring& why,
                     bool yuv444, bool* yuv444Used) {
    if (img.bits > 8 && !img.HighPrecision()) {
        why = Tr(L"缺少原始高精度像素", L"High-precision pixels are missing"); return false;
    }
    Driver& d = Load();
    if (!d.ok) { why = Tr(L"未检测到 NVIDIA 显卡编码器", L"No NVIDIA GPU encoder detected"); return false; }
    const bool transparent = std::any_of(img.px.begin(), img.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
    if (transparent) { bits = 8; yuv444 = false; }
    Bytes stream, alpha;
    if (!EncodeFrame(d, img, bits, yuv444, stream, why)) return false;
    if (transparent) {
        int alphaBits = 8;
        bool alpha444 = false;
        if (!EncodeFrame(d, img, alphaBits, alpha444, alpha, why, true)) return false;
    }
    if (!WrapHeic(stream, img.w, img.h, bits, yuv444, alpha, out)) { why = Tr(L"HEIC 封装失败", L"Could not create the HEIC container"); return false; }
    if (yuv444Used) *yuv444Used = yuv444;
    return true;
}
