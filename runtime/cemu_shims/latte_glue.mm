// BotW-Native: Metal-only frame capture (--capture). The rest of the Latte glue lives in
// latte_glue.cpp so Windows and Linux can build it as plain C++.
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"

void log_write(const char* fmt, ...);

namespace {
void write_png(const std::vector<uint8>& rgb, int w, int h, const std::string& path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(nullptr, rgb.data(), rgb.size());
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(w, h, 8, 24, w * 3, cs, kCGImageAlphaNone, prov, nullptr, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), path.size(), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGDataProviderRelease(prov);
    CFRelease(data);
    CGColorSpaceRelease(cs);
}
}  // namespace

void botw_capture_hook_metal(LatteTextureView* view, bool isPadView, const std::string& path) {
    auto* mr = (MetalRenderer*)g_renderer.get();
    MTL::Texture* tex = static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture();
    uint32 w = (uint32)tex->width(), h = (uint32)tex->height();
    MTL::PixelFormat fmt = tex->pixelFormat();
    uint32 bpp = (fmt == MTL::PixelFormatRGBA16Float) ? 8 : 4;
    MTL::Buffer* buf = mr->GetDevice()->newBuffer(w * h * bpp, MTL::ResourceStorageModeShared);
    auto* enc = mr->GetBlitCommandEncoder();
    enc->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1), buf, 0, w * bpp, 0);
    mr->Flush(true);
    std::vector<uint8> rgb(w * h * 3);
    const uint8* src = (const uint8*)buf->contents();
    const bool encode = !isPadView && LatteGPUState.tvBufferUsesSRGB && fmt != MTL::PixelFormatBGRA8Unorm_sRGB;
    static uint8 s_srgb[1024];
    static bool s_srgb_init = [] {
        for (int k = 0; k < 1024; k++) {
            float v = k / 1023.0f;
            v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
            s_srgb[k] = (uint8)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
        }
        return true;
    }();
    (void)s_srgb_init;
    for (uint32 i = 0; i < w * h; i++) {
        uint8 r, g, b;
        if (fmt == MTL::PixelFormatBGRA8Unorm || fmt == MTL::PixelFormatBGRA8Unorm_sRGB) {
            b = src[i * 4]; g = src[i * 4 + 1]; r = src[i * 4 + 2];
        } else if (fmt == MTL::PixelFormatRGB10A2Unorm) {
            uint32 v = ((const uint32*)src)[i];
            r = (uint8)(((v >> 0) & 1023) >> 2); g = (uint8)(((v >> 10) & 1023) >> 2); b = (uint8)(((v >> 20) & 1023) >> 2);
        } else if (bpp == 8) {
            const __fp16* hp = (const __fp16*)(src + i * 8);
            auto c = [](float x) { return (uint8)std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f); };
            r = c(hp[0]); g = c(hp[1]); b = c(hp[2]);
        } else {
            r = src[i * 4]; g = src[i * 4 + 1]; b = src[i * 4 + 2];
        }
        if (encode) {
            if (fmt == MTL::PixelFormatRGB10A2Unorm) {
                uint32 v = ((const uint32*)src)[i];
                r = s_srgb[v & 1023]; g = s_srgb[(v >> 10) & 1023]; b = s_srgb[(v >> 20) & 1023];
            } else {
                r = s_srgb[r * 1023 / 255]; g = s_srgb[g * 1023 / 255]; b = s_srgb[b * 1023 / 255];
            }
        }
        rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
    }
    buf->release();
    write_png(rgb, (int)w, (int)h, path);
    log_write("capture %s (%ux%u, format %u)", path.c_str(), w, h, (uint32)fmt);
}
