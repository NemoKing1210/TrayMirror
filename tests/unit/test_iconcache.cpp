// IconCache (spec 6.2): bitmap reuse keyed by icon identity and size.
// Real D2D bitmaps on a WARP device — no mocks; reuse is observed by
// pointer identity and factory call counts.
#include "render/IconCache.h"
#include "render/IconRasterizer.h"

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <windows.h>

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <set>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using traymirror::IconCache;
using traymirror::IconCacheKey;

HICON IconA() { return LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); }
HICON IconB() { return LoadIconW(nullptr, MAKEINTRESOURCEW(32513)); }

// A minimal D2D context for making 1x1 solid bitmaps.
class Device {
 public:
  bool Init() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, d3d_.GetAddressOf(),
                                 nullptr, nullptr)) != 0) {
      return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d_.As(&dxgi)) != 0) {
      return false;
    }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 factory_.GetAddressOf())) != 0) {
      return false;
    }
    if (FAILED(factory_->CreateDevice(dxgi.Get(), device_.GetAddressOf())) !=
        0) {
      return false;
    }
    return FAILED(device_->CreateDeviceContext(
                      D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                      context_.GetAddressOf())) == 0;
  }

  ID2D1Bitmap* MakeBitmap(uint32_t bgra) {
    const uint32_t pixel = bgra;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(context_->CreateBitmap(D2D1::SizeU(1, 1), &pixel, 4, &props,
                                      bitmap.GetAddressOf())) != 0) {
      return nullptr;
    }
    return bitmap.Detach();
  }

 private:
  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID2D1Factory1> factory_;
  ComPtr<ID2D1Device> device_;
  ComPtr<ID2D1DeviceContext> context_;
};

class IconCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(device_.Init()); }

  IconCache::BitmapFactory CountingFactory(int* calls) {
    return [this, calls](HICON, int) {
      ++*calls;
      return device_.MakeBitmap(0xFF000000U | static_cast<uint32_t>(*calls));
    };
  }

  Device device_;
};

}  // namespace

TEST_F(IconCacheTest, SameSerialAndPxReturnsSameBitmap) {
  int calls = 0;
  IconCache cache(CountingFactory(&calls));
  ID2D1Bitmap* first = cache.Get(IconA(), 1, 16);
  ID2D1Bitmap* second = cache.Get(IconA(), 1, 16);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(cache.size(), 1U);
}

TEST_F(IconCacheTest, SerialChangeReplacesBitmap) {
  int calls = 0;
  IconCache cache(CountingFactory(&calls));
  ID2D1Bitmap* first = cache.Get(IconA(), 1, 16);
  ID2D1Bitmap* second = cache.Get(IconA(), 2, 16);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(calls, 2);
}

TEST_F(IconCacheTest, PruneReleasesStaleEntries) {
  int calls = 0;
  IconCache cache(CountingFactory(&calls));
  cache.Get(IconA(), 1, 16);
  cache.Get(IconA(), 2, 16);
  ASSERT_EQ(cache.size(), 2U);

  cache.Prune({IconCacheKey{.icon = IconA(), .serial = 2, .px = 16}});
  EXPECT_EQ(cache.size(), 1U);

  // The pruned key is gone: the next Get builds a fresh bitmap.
  ID2D1Bitmap* recreated = cache.Get(IconA(), 1, 16);
  ASSERT_NE(recreated, nullptr);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(cache.size(), 2U);
}

TEST_F(IconCacheTest, NullIconIsNotCached) {
  int calls = 0;
  IconCache cache(CountingFactory(&calls));
  EXPECT_EQ(cache.Get(nullptr, 1, 16), nullptr);
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_EQ(calls, 0);
}

TEST_F(IconCacheTest, DistinctIconsWithTheSameSerialDoNotCollide) {
  // Model serials are per icon, so (serial, px) alone is not an
  // identity: two icons can both report serial 1.
  int calls = 0;
  IconCache cache(CountingFactory(&calls));
  ID2D1Bitmap* a = cache.Get(IconA(), 1, 16);
  ID2D1Bitmap* b = cache.Get(IconB(), 1, 16);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_EQ(calls, 2);
}

// --- shared rasterizer -----------------------------------------------------
// The strip draws through this module; the flyout will too. Its reference
// is native DrawIconEx at the same target size, rendered over black so the
// composited RGB equals the icon's premultiplied RGB.
namespace {

using traymirror::RasterIcon;
using traymirror::RasterizeIcon;
using traymirror::ProbeIconSize;

HICON RasterSolidIcon(int px, COLORREF color, uint32_t alpha = 255) {
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = px;
  header.bV5Height = -px;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000U;
  header.bV5GreenMask = 0x0000FF00U;
  header.bV5BlueMask = 0x000000FFU;
  header.bV5AlphaMask = 0xFF000000U;
  void* bits = nullptr;
  const HDC screen = GetDC(nullptr);
  const HBITMAP dib = CreateDIBSection(
      screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits,
      nullptr, 0);
  BITMAPINFO maskInfo = {};
  maskInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  maskInfo.bmiHeader.biWidth = px;
  maskInfo.bmiHeader.biHeight = -px;
  maskInfo.bmiHeader.biPlanes = 1;
  maskInfo.bmiHeader.biBitCount = 1;
  void* maskBits = nullptr;
  const HBITMAP mask = CreateDIBSection(screen, &maskInfo, DIB_RGB_COLORS,
                                        &maskBits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (dib == nullptr || bits == nullptr || mask == nullptr) {
    if (dib != nullptr) DeleteObject(dib);
    if (mask != nullptr) DeleteObject(mask);
    return nullptr;
  }
  std::memset(maskBits, 0,
              static_cast<size_t>(((px + 31) / 32) * 4) * static_cast<size_t>(px));
  auto* const pixels = static_cast<uint32_t*>(bits);
  const uint32_t value =
      (alpha << 24) |
      (((static_cast<uint32_t>(GetRValue(color)) * alpha) / 255U) << 16) |
      (((static_cast<uint32_t>(GetGValue(color)) * alpha) / 255U) << 8) |
      ((static_cast<uint32_t>(GetBValue(color)) * alpha) / 255U);
  for (int i = 0; i < px * px; ++i) pixels[i] = value;
  ICONINFO info = {};
  info.fIcon = TRUE;
  info.hbmColor = dib;
  info.hbmMask = mask;
  const HICON icon = CreateIconIndirect(&info);
  DeleteObject(dib);
  DeleteObject(mask);
  return icon;
}

std::vector<uint32_t> NativeReference(HICON icon, int px,
                                     uint32_t background = 0) {
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = px;
  info.bmiHeader.biHeight = -px;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP dib =
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    if (dib != nullptr) DeleteObject(dib);
    return {};
  }
  auto* const target = static_cast<uint32_t*>(bits);
  for (int i = 0; i < px * px; ++i) target[i] = background;
  const HDC dc = CreateCompatibleDC(nullptr);
  const HGDIOBJ old = SelectObject(dc, dib);
  DrawIconEx(dc, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
  SelectObject(dc, old);
  DeleteDC(dc);
  std::vector<uint32_t> pixels(static_cast<size_t>(px) * px);
  std::memcpy(pixels.data(), bits, pixels.size() * sizeof(uint32_t));
  DeleteObject(dib);
  return pixels;
}

Microsoft::WRL::ComPtr<IWICImagingFactory> MakeWic() {
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(wic.GetAddressOf()));
  return wic;
}

int MaxChannelDiff(uint32_t a, uint32_t b) {
  int worst = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int da = static_cast<int>((a >> shift) & 0xFFU);
    const int db = static_cast<int>((b >> shift) & 0xFFU);
    worst = (std::max)(worst, std::abs(da - db));
  }
  return worst;
}

class IconRasterizerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized_ = com == S_OK || com == S_FALSE;
    wic_ = MakeWic();
    ASSERT_NE(wic_, nullptr);
  }
  void TearDown() override {
    wic_.Reset();
    if (comInitialized_) CoUninitialize();
  }

  Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
  bool comInitialized_ = false;
};

}  // namespace

TEST_F(IconRasterizerTest, ExactSizeMatchesNativeDrawIconEx) {
  const HICON icon = RasterSolidIcon(16, RGB(255, 0, 0));
  ASSERT_NE(icon, nullptr);
  RasterIcon raster;
  ASSERT_TRUE(RasterizeIcon(wic_.Get(), icon, 16, &raster));
  const std::vector<uint32_t> reference = NativeReference(icon, 16);
  ASSERT_EQ(reference.size(), raster.pixels.size());
  int worst = 0;
  for (size_t i = 0; i < reference.size(); ++i) {
    worst = (std::max)(worst, MaxChannelDiff(reference[i], raster.pixels[i]));
  }
  EXPECT_LE(worst, 2) << "exact-size conversion must not resample the glyph";
  DestroyIcon(icon);
}

TEST_F(IconRasterizerTest, SemitransparentEdgesMatchNativeOverBlack) {
  const HICON icon = RasterSolidIcon(16, RGB(0, 128, 255), 128);
  ASSERT_NE(icon, nullptr);
  RasterIcon raster;
  ASSERT_TRUE(RasterizeIcon(wic_.Get(), icon, 16, &raster));
  const std::vector<uint32_t> reference = NativeReference(icon, 16);
  ASSERT_EQ(reference.size(), raster.pixels.size());
  int worst = 0;
  for (size_t i = 0; i < reference.size(); ++i) {
    worst = (std::max)(worst, MaxChannelDiff(reference[i], raster.pixels[i]));
  }
  EXPECT_LE(worst, 2) << "premultiplied pixels must not be multiplied twice";
  DestroyIcon(icon);
}

TEST_F(IconRasterizerTest, MonochromeMaskMatchesNative) {
  BYTE andMask[32] = {};
  BYTE xorMask[32];
  std::memset(xorMask, 255, sizeof(xorMask));
  const HICON icon = CreateIcon(GetModuleHandleW(nullptr), 16, 16, 1, 1,
                                andMask, xorMask);
  ASSERT_NE(icon, nullptr);
  RasterIcon raster;
  ASSERT_TRUE(RasterizeIcon(wic_.Get(), icon, 16, &raster));
  const std::vector<uint32_t> reference = NativeReference(icon, 16);
  ASSERT_EQ(reference.size(), raster.pixels.size());
  int worst = 0;
  for (size_t i = 0; i < reference.size(); ++i) {
    worst = (std::max)(worst, MaxChannelDiff(reference[i], raster.pixels[i]));
  }
  EXPECT_LE(worst, 2) << "the AND/XOR mask must survive the conversion";
  DestroyIcon(icon);
}

TEST_F(IconRasterizerTest, UpScalingKeepsTheGlyphColor) {
  const HICON icon = RasterSolidIcon(16, RGB(0, 255, 0));
  ASSERT_NE(icon, nullptr);
  RasterIcon raster;
  ASSERT_TRUE(RasterizeIcon(wic_.Get(), icon, 24, &raster));
  ASSERT_EQ(raster.width, 24);
  const uint32_t centre = raster.pixels[12U * 24U + 12U];
  EXPECT_NEAR(static_cast<int>((centre >> 8) & 0xFFU), 255, 2);
  EXPECT_NEAR(static_cast<int>((centre >> 16) & 0xFFU), 0, 2);
  EXPECT_NEAR(static_cast<int>(centre & 0xFFU), 0, 2);
  DestroyIcon(icon);
}

TEST_F(IconRasterizerTest, ProbeReportsTheNativeSourceSize) {
  const HICON icon = RasterSolidIcon(16, RGB(1, 2, 3));
  ASSERT_NE(icon, nullptr);
  int width = 0;
  int height = 0;
  ASSERT_TRUE(ProbeIconSize(wic_.Get(), icon, &width, &height));
  EXPECT_EQ(width, 16);
  EXPECT_EQ(height, 16);
  DestroyIcon(icon);
}

TEST_F(IconRasterizerTest, RealSystemIconsMatchNativeAtTraySizes) {
  // Not a solid square: the shell's own antialiased icons are straight
  // alpha with colour above alpha. At the source size the bytes must be
  // identical to GDI within the documented tolerance; at the physical
  // sizes of 96/120/150/200% DPI the shape must agree. GDI downscales
  // with its default (aliasing) stretch mode, so exact per-pixel parity
  // there would mean reproducing the aliasing, not the glyph.
  for (const int id : {32512, 32513, 32515}) {
    const HICON icon = LoadIconW(nullptr, MAKEINTRESOURCEW(id));
    ASSERT_NE(icon, nullptr);
    int source = 0;
    int sourceHeight = 0;
    ASSERT_TRUE(ProbeIconSize(wic_.Get(), icon, &source, &sourceHeight));
    EXPECT_EQ(source, sourceHeight);
    for (const int px : {16, 20, 24, 32}) {
      RasterIcon raster;
      ASSERT_TRUE(RasterizeIcon(wic_.Get(), icon, px, &raster)) << px;
      const std::vector<uint32_t> reference = NativeReference(icon, px);
      ASSERT_EQ(reference.size(), raster.pixels.size());
      if (px == source) {
        int worst = 0;
        for (size_t i = 0; i < reference.size(); ++i) {
          worst = (std::max)(worst,
                             MaxChannelDiff(reference[i], raster.pixels[i]));
        }
        EXPECT_LE(worst, 2) << "exact-size icon " << id << " px " << px;
        continue;
      }
      int within = 0;
      int intersection = 0;
      int unionCount = 0;
      for (size_t i = 0; i < reference.size(); ++i) {
        const int refAlpha = static_cast<int>(reference[i] >> 24);
        const int outAlpha = static_cast<int>(raster.pixels[i] >> 24);
        if (std::abs(refAlpha - outAlpha) <= 32) {
          ++within;
        }
        const bool refInk = refAlpha >= 64;
        const bool outInk = outAlpha >= 64;
        intersection += refInk && outInk ? 1 : 0;
        unionCount += refInk || outInk ? 1 : 0;
      }
      const double size = static_cast<double>(reference.size());
      EXPECT_GE(static_cast<double>(within) / size, 0.80)
          << "alpha must follow the native glyph, icon " << id << " px " << px;
      EXPECT_GE(static_cast<double>(intersection) / unionCount, 0.80)
          << "ink shape must match the native glyph, icon " << id << " px "
          << px;
    }
  }
}

TEST_F(IconRasterizerTest, FlyoutGdiBlendMatchesNativeOverColour) {
  // The flyout is a GDI window: it composites the shared raster with
  // AlphaBlend instead of D2D. Over an opaque background the result must
  // equal DrawIconEx, or the strip and the flyout would disagree about
  // the same icon.
  const HICON icon = RasterSolidIcon(16, RGB(0, 128, 255), 128);
  ASSERT_NE(icon, nullptr);
  const traymirror::UniqueBitmap bitmap =
      traymirror::CreatePremultipliedBitmap(wic_.Get(), icon, 16);
  ASSERT_NE(bitmap, nullptr);

  constexpr uint32_t kBackground = 0xFF20140AU;  // R 32, G 20, B 10
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = 16;
  info.bmiHeader.biHeight = -16;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP target =
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ASSERT_NE(target, nullptr);
  auto* const pixels = static_cast<uint32_t*>(bits);
  for (int i = 0; i < 16 * 16; ++i) pixels[i] = kBackground;
  const HDC dc = CreateCompatibleDC(nullptr);
  const HGDIOBJ old = SelectObject(dc, target);
  EXPECT_TRUE(traymirror::BlendBitmap(dc, bitmap.get(), 0, 0, 16));
  SelectObject(dc, old);
  DeleteDC(dc);

  std::vector<uint32_t> blended(pixels, pixels + 16 * 16);
  DeleteObject(target);
  const std::vector<uint32_t> reference = NativeReference(icon, 16, kBackground);
  ASSERT_EQ(reference.size(), blended.size());
  int worst = 0;
  for (size_t i = 0; i < reference.size(); ++i) {
    worst = (std::max)(worst, MaxChannelDiff(reference[i], blended[i]));
  }
  EXPECT_LE(worst, 3) << "AlphaBlend must use the premultiplied bytes as-is";
  DestroyIcon(icon);
}

