#include "render/IconRasterizer.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstring>

namespace traymirror {
namespace {

using Microsoft::WRL::ComPtr;

// WIC's HICON decoder yields straight (non-premultiplied) BGRA: measured
// system icons contain pixels whose RGB exceeds their alpha, which is
// impossible once premultiplied. Direct2D's PREMULTIPLIED bitmaps need
// RGB = color * alpha, so the converter premultiplies exactly once, before
// scaling — the same order GDI uses. Scaling straight alpha and
// premultiplying afterwards would multiply transparent texels' color into
// dark halos.
bool CopySource(IWICImagingFactory* wic, HICON icon,
                ComPtr<IWICBitmapSource>* out, UINT* width, UINT* height) {
  if (wic == nullptr || icon == nullptr || out == nullptr) {
    return false;
  }
  ComPtr<IWICBitmap> decoded;
  if (FAILED(wic->CreateBitmapFromHICON(icon, decoded.GetAddressOf())) ||
      decoded == nullptr) {
    return false;
  }
  UINT sourceWidth = 0;
  UINT sourceHeight = 0;
  if (FAILED(decoded->GetSize(&sourceWidth, &sourceHeight)) ||
      sourceWidth == 0 || sourceHeight == 0) {
    return false;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
      converter == nullptr ||
      FAILED(converter->Initialize(decoded.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  *out = std::move(converter);
  if (width != nullptr) {
    *width = sourceWidth;
  }
  if (height != nullptr) {
    *height = sourceHeight;
  }
  return true;
}

}  // namespace

bool ProbeIconSize(IWICImagingFactory* wic, HICON icon, int* width,
                   int* height) {
  if (width == nullptr || height == nullptr) {
    return false;
  }
  ComPtr<IWICBitmapSource> source;
  UINT sourceWidth = 0;
  UINT sourceHeight = 0;
  if (!CopySource(wic, icon, &source, &sourceWidth, &sourceHeight)) {
    return false;
  }
  *width = static_cast<int>(sourceWidth);
  *height = static_cast<int>(sourceHeight);
  return true;
}

bool RasterizeIcon(IWICImagingFactory* wic, HICON icon, int targetPx,
                   RasterIcon* out) {
  if (out == nullptr || wic == nullptr || icon == nullptr || targetPx <= 0) {
    return false;
  }
  ComPtr<IWICBitmapSource> source;
  UINT sourceWidth = 0;
  UINT sourceHeight = 0;
  if (!CopySource(wic, icon, &source, &sourceWidth, &sourceHeight)) {
    return false;
  }
  const UINT size = static_cast<UINT>(targetPx);

  ComPtr<IWICBitmapSource> pixels = source;
  ComPtr<IWICBitmapScaler> scaler;
  if (sourceWidth != size || sourceHeight != size) {
    if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) ||
        scaler == nullptr) {
      return false;
    }
    // Up-scaling a small tray glyph: linear is what GDI's DrawIconEx does
    // and keeps the native stroke weight. Down-scaling a large source
    // benefits from the cubic filter's sharp edges.
    const WICBitmapInterpolationMode mode =
        size >= (std::max)(sourceWidth, sourceHeight)
            ? WICBitmapInterpolationModeLinear
            : WICBitmapInterpolationModeHighQualityCubic;
    if (FAILED(scaler->Initialize(source.Get(), size, size, mode))) {
      return false;
    }
    pixels = scaler;
  }

  const auto target = static_cast<size_t>(targetPx);
  const auto stride =
      static_cast<UINT>(target * static_cast<size_t>(sizeof(uint32_t)));
  const auto bytes = static_cast<UINT>(target * static_cast<size_t>(stride));
  out->width = targetPx;
  out->height = targetPx;
  out->pixels.assign(target * target, 0);
  if (FAILED(pixels->CopyPixels(nullptr, stride, bytes,
                                reinterpret_cast<BYTE*>(out->pixels.data())))) {
    out->pixels.clear();
    out->width = 0;
    out->height = 0;
    return false;
  }
  return true;
}

void BitmapDeleter::operator()(HBITMAP bitmap) const noexcept {
  if (bitmap != nullptr) {
    DeleteObject(bitmap);
  }
}

UniqueBitmap CreatePremultipliedBitmap(IWICImagingFactory* wic, HICON icon,
                                       int targetPx) {
  RasterIcon raster;
  if (!RasterizeIcon(wic, icon, targetPx, &raster)) {
    return {};
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = raster.width;
  info.bmiHeader.biHeight = -raster.height;  // top-down, WIC's row order
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP dib =
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    if (dib != nullptr) {
      DeleteObject(dib);
    }
    return {};
  }
  std::memcpy(bits, raster.pixels.data(),
              raster.pixels.size() * sizeof(uint32_t));
  return UniqueBitmap(dib);
}

bool BlendBitmap(HDC dc, HBITMAP bitmap, int x, int y, int size) {
  if (dc == nullptr || bitmap == nullptr || size <= 0) {
    return false;
  }
  const HDC source = CreateCompatibleDC(dc);
  if (source == nullptr) {
    return false;
  }
  const HGDIOBJ previous = SelectObject(source, bitmap);
  const BLENDFUNCTION blend = {.BlendOp = AC_SRC_OVER,
                               .SourceConstantAlpha = 255,
                               .AlphaFormat = AC_SRC_ALPHA};
  const BOOL blended = AlphaBlend(dc, x, y, size, size, source, 0, 0, size,
                                  size, blend);
  SelectObject(source, previous);
  DeleteDC(source);
  return blended != FALSE;
}

}  // namespace traymirror
