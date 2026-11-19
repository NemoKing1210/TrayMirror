// One HICON conversion path shared by the strip and the overflow flyout
// (plan task 4): premultiplied BGRA at an exact target size. No scaling
// happens when the source already matches the target size.
#pragma once

#include <windows.h>

#include <wincodec.h>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace traymirror {

struct RasterIcon {
  int width = 0;
  int height = 0;
  // Row-major premultiplied BGRA, width * height entries.
  std::vector<uint32_t> pixels;
};

// Owns a GDI bitmap; the flyout caches one per icon.
struct BitmapDeleter {
  void operator()(HBITMAP bitmap) const noexcept;
};
using UniqueBitmap =
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapDeleter>;

// The icon's native pixel size, before any scaling.
bool ProbeIconSize(IWICImagingFactory* wic, HICON icon, int* width, int* height);

// Converts `icon` to a `targetPx` x `targetPx` premultiplied BGRA image.
// The source is copied byte-for-byte when it already has that size; only
// a size difference creates a scaler.
bool RasterizeIcon(IWICImagingFactory* wic, HICON icon, int targetPx,
                   RasterIcon* out);

// The same conversion as a top-down 32bpp DIB section, for the GDI-drawn
// flyout: AlphaBlend with AC_SRC_ALPHA expects premultiplied bytes.
UniqueBitmap CreatePremultipliedBitmap(IWICImagingFactory* wic, HICON icon,
                                       int targetPx);

// Composites a bitmap from CreatePremultipliedBitmap at `x`, `y` with
// SrcOver. Returns false when the DC or bitmap is unusable.
bool BlendBitmap(HDC dc, HBITMAP bitmap, int x, int y, int size);

}  // namespace traymirror
