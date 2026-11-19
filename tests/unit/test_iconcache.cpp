// IconCache (spec 6.2): bitmap reuse keyed by icon identity and size.
// Real D2D bitmaps on a WARP device — no mocks; reuse is observed by
// pointer identity and factory call counts.
#include "render/IconCache.h"

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <windows.h>

#include <gtest/gtest.h>

#include <functional>
#include <set>

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
