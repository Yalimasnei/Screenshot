// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <wincodec.h>

// WIC HEIF auxiliary-frame interfaces from the Windows SDK wincodec.idl.
// llvm-mingw's headers do not yet declare them. QueryInterface keeps the
// application compatible with codecs that do not implement frame chains.
namespace HeifWic {
constexpr int AlphaMap = 5;  // WICBitmapChainType_AlphaMap
inline constexpr GUID ReaderId = {0x0c599495, 0xa120, 0x4222, {0x91, 0x30, 0xa8, 0xc2, 0x94, 0x10, 0xbd, 0x0b}};
inline constexpr GUID WriterId = {0x40d9ea28, 0x4768, 0x47b3, {0x8c, 0x12, 0x55, 0x8a, 0x48, 0xe9, 0x8e, 0x38}};
struct Reader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetChainedFrameCount(int type, UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChainedFrame(int type, UINT index, IWICBitmapFrameDecode** frame) = 0;
};
struct Writer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AppendFrameToChain(int type, IWICBitmapFrameEncode** frame,
                                                         IPropertyBag2** options) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoesSupportChainType(int type, BOOL* supported) = 0;
};
}  // namespace HeifWic
