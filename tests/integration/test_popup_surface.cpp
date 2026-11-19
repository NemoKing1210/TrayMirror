// The shared popup surface (plan task 5): DirectComposition content over
// the measured acrylic material, with a capability report and a defined
// opaque fallback. These tests exercise the real composition stack on
// the test machine; they do not establish visual parity with Explorer,
// which remains owner-verified.
#include "render/NativePopupSurface.h"
#include "render/Theme.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <iterator>
#include <memory>

namespace {

using traymirror::NativePopupSurface;
using traymirror::PopupMaterial;
using traymirror::ResolvePopupMaterial;

constexpr wchar_t kPopupSurfaceClass[] = L"TrayMirrorPopupSurfaceTest";

LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

class PopupSurfaceIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SurfaceProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kPopupSurfaceClass;
    if (RegisterClassExW(&wc) == 0) {
      ASSERT_EQ(GetLastError(), ERROR_CLASS_ALREADY_EXISTS);
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupSurfaceClass,
                            L"popup surface test", WS_POPUP, 100, 100, 200,
                            150, nullptr, nullptr, GetModuleHandleW(nullptr),
                            nullptr);
    ASSERT_NE(hwnd_, nullptr);
  }

  void TearDown() override {
    surface_.reset();
    if (hwnd_ != nullptr) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  HWND hwnd_ = nullptr;
  std::unique_ptr<NativePopupSurface> surface_;
};

TEST_F(PopupSurfaceIntegration, AttachBringsUpComposition) {
  surface_ = NativePopupSurface::Attach(hwnd_);
  ASSERT_NE(surface_, nullptr);
  EXPECT_TRUE(surface_->compositionAvailable());
  EXPECT_EQ(surface_->environment().compositionAvailable, true);
}

TEST_F(PopupSurfaceIntegration, MaterialMatchesTheReportedEnvironment) {
  surface_ = NativePopupSurface::Attach(hwnd_);
  ASSERT_NE(surface_, nullptr);
  surface_->ApplyTheme(true);
  EXPECT_EQ(surface_->material(),
            ResolvePopupMaterial(surface_->environment()));
  EXPECT_EQ(surface_->materialActive(),
            surface_->material() == PopupMaterial::kAcrylic);
}

TEST_F(PopupSurfaceIntegration, RenderRunsTheContentCallback) {
  surface_ = NativePopupSurface::Attach(hwnd_);
  ASSERT_NE(surface_, nullptr);
  ASSERT_TRUE(surface_->Resize(200, 150));

  int calls = 0;
  int reportedWidth = 0;
  int reportedHeight = 0;
  EXPECT_TRUE(surface_->Render(
      [&](ID2D1DeviceContext* dc, int width, int height) {
        ++calls;
        reportedWidth = width;
        reportedHeight = height;
        EXPECT_NE(dc, nullptr);
      }));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(reportedWidth, 200);
  EXPECT_EQ(reportedHeight, 150);
}

TEST_F(PopupSurfaceIntegration, ResizeRecreatesTheSurfaceAndKeepsRendering) {
  surface_ = NativePopupSurface::Attach(hwnd_);
  ASSERT_NE(surface_, nullptr);
  ASSERT_TRUE(surface_->Resize(200, 150));

  ASSERT_TRUE(surface_->Resize(320, 240));
  int calls = 0;
  EXPECT_TRUE(surface_->Render(
      [&](ID2D1DeviceContext*, int width, int height) {
        ++calls;
        EXPECT_EQ(width, 320);
        EXPECT_EQ(height, 240);
      }));
  EXPECT_EQ(calls, 1);

  ASSERT_TRUE(surface_->Resize(120, 90));
  EXPECT_TRUE(surface_->Render(
      [&](ID2D1DeviceContext*, int width, int height) {
        ++calls;
        EXPECT_EQ(width, 120);
        EXPECT_EQ(height, 90);
      }));
  EXPECT_EQ(calls, 2);
}

TEST_F(PopupSurfaceIntegration, DegenerateSizesAreRejected) {
  surface_ = NativePopupSurface::Attach(hwnd_);
  ASSERT_NE(surface_, nullptr);
  EXPECT_FALSE(surface_->Resize(0, 150));
  EXPECT_FALSE(surface_->Resize(200, 0));
  EXPECT_FALSE(surface_->Resize(-1, 100));
}

TEST_F(PopupSurfaceIntegration, AttachCanUseACallerOwnedDevice) {
  // The strip already owns a D3D/D2D device; the flyout must be able to
  // render on it instead of paying device creation on the click path.
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  Microsoft::WRL::ComPtr<ID3D11Device> d3d;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
      d3d.GetAddressOf(), nullptr, nullptr)));
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  ASSERT_TRUE(SUCCEEDED(d3d.As(&dxgi)));
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
  ASSERT_TRUE(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                          factory.GetAddressOf())));
  Microsoft::WRL::ComPtr<ID2D1Device> device;
  ASSERT_TRUE(SUCCEEDED(factory->CreateDevice(dxgi.Get(),
                                              device.GetAddressOf())));

  surface_ = NativePopupSurface::Attach(hwnd_, device.Get());
  ASSERT_NE(surface_, nullptr);
  EXPECT_TRUE(surface_->compositionAvailable());
  ASSERT_TRUE(surface_->Resize(200, 150));

  int calls = 0;
  EXPECT_TRUE(surface_->Render(
      [&](ID2D1DeviceContext*, int width, int height) {
        ++calls;
        EXPECT_EQ(width, 200);
        EXPECT_EQ(height, 150);
      }));
  EXPECT_EQ(calls, 1);
}

TEST(PopupSurface, AttachRejectsAnInvalidWindow) {
  EXPECT_EQ(NativePopupSurface::Attach(nullptr), nullptr);
}

}  // namespace
