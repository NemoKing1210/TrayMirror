// Rendering regressions with independent synthetic fixtures. These verify
// scale, placement, alpha and input state, not screenshot parity with Explorer.
#include "render/Overlay.h"
#include "render/Theme.h"
#include "win/Monitors.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <vector>
#include <chrono>

namespace {

using traymirror::IconView;
using traymirror::Overlay;


// A solid icon: every pixel is the same colour, so the square the strip
// draws is unambiguous in the read-back bitmap whatever the cache does
// with the alpha channel.
HICON SolidIcon(int px, COLORREF color, uint32_t alpha = 255) {
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
  const HBITMAP dib =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                       DIB_RGB_COLORS, &bits, nullptr, 0);
  // A real 1-bpp mask, all zeroes (nothing masked out): an uninitialised
  // mask makes DrawIconEx drop pixels.
  BITMAPINFO maskInfo = {};
  maskInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  maskInfo.bmiHeader.biWidth = px;
  maskInfo.bmiHeader.biHeight = -px;
  maskInfo.bmiHeader.biPlanes = 1;
  maskInfo.bmiHeader.biBitCount = 1;
  void* maskBits = nullptr;
  const HBITMAP mask =
      CreateDIBSection(screen, &maskInfo, DIB_RGB_COLORS, &maskBits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (dib == nullptr || bits == nullptr || mask == nullptr) {
    if (dib != nullptr) {
      DeleteObject(dib);
    }
    if (mask != nullptr) {
      DeleteObject(mask);
    }
    return nullptr;
  }
  std::memset(maskBits, 0, static_cast<size_t>(((px + 31) / 32) * 4) *
                             static_cast<size_t>(px));
  auto* const pixels = static_cast<uint32_t*>(bits);
  // Real 32bpp icons carry straight alpha (measured system icons contain
  // RGB above alpha); the renderer must premultiply exactly once.
  const uint32_t value = (alpha << 24) |
      (static_cast<uint32_t>(GetRValue(color)) << 16) |
      (static_cast<uint32_t>(GetGValue(color)) << 8) |
      static_cast<uint32_t>(GetBValue(color));
  for (int i = 0; i < px * px; ++i) {
    pixels[i] = value;
  }
  ICONINFO info = {};
  info.fIcon = TRUE;
  info.hbmColor = dib;
  info.hbmMask = mask;
  const HICON icon = CreateIconIndirect(&info);
  DeleteObject(dib);
  DeleteObject(mask);
  return icon;
}


struct Harness {
  std::unique_ptr<Overlay> overlay;
  explicit Harness(int dpi) {
    const HMONITOR monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    // Off the taskbar: these windows never install a hook or contact Explorer.
    RECT strip{100, 100, 100 + MulDiv(160, dpi, 96),
               100 + MulDiv(48, dpi, 96)};
    overlay = Overlay::Create(monitor, strip);
  }
};

bool IsNear(uint32_t pixel, COLORREF color, int tolerance) {
  return std::abs(static_cast<int>((pixel >> 16) & 255U) - GetRValue(color)) <= tolerance &&
         std::abs(static_cast<int>((pixel >> 8) & 255U) - GetGValue(color)) <= tolerance &&
         std::abs(static_cast<int>(pixel & 255U) - GetBValue(color)) <= tolerance;
}

class RenderScale : public testing::TestWithParam<int> {};

TEST_P(RenderScale, GlyphsUsePhysicalPixelsAndFullButtonPitch) {
  const int dpi = GetParam();
  Harness h(dpi);
  ASSERT_NE(h.overlay, nullptr);
  const int glyph = MulDiv(16, dpi, 96);
  const HICON icon = SolidIcon(glyph, RGB(255, 0, 0));
  ASSERT_NE(icon, nullptr);
  h.overlay->SetIcons({IconView{.icon = icon, .serial = 1},
                      IconView{.icon = icon, .serial = 2},
                      IconView{.icon = icon, .serial = 3}});
  const auto bitmap = h.overlay->RenderToBitmap(dpi, true);
  ASSERT_TRUE(bitmap.has_value());
  const int y = MulDiv(24, dpi, 96);
  const int pad = MulDiv(8, dpi, 96);
  const int pitch = MulDiv(32, dpi, 96);
  for (int x = 0; x < bitmap->width; ++x) {
    const bool expected = x >= pad && x < 3 * pitch &&
                          (x % pitch) >= pad && (x % pitch) < pad + glyph;
    EXPECT_EQ(IsNear(bitmap->At(x, y), RGB(255, 0, 0), 2), expected) << x;
  }
  EXPECT_EQ(bitmap->At(bitmap->width - 1, 0), 0U)
      << "empty pixels must reveal the actual taskbar material";
  DestroyIcon(icon);
}

TEST_P(RenderScale, GlyphIsCenteredVertically) {
  const int dpi = GetParam();
  Harness h(dpi);
  ASSERT_NE(h.overlay, nullptr);
  const int glyph = MulDiv(16, dpi, 96);
  const HICON icon = SolidIcon(glyph, RGB(0, 0, 255));
  ASSERT_NE(icon, nullptr);
  h.overlay->SetIcons({IconView{.icon = icon, .serial = 1}});
  const auto bitmap = h.overlay->RenderToBitmap(dpi, false);
  ASSERT_TRUE(bitmap.has_value());
  const int left = MulDiv(8, dpi, 96);
  const int top = (bitmap->height - glyph) / 2;
  for (int y = 0; y < bitmap->height; ++y) {
    EXPECT_EQ(IsNear(bitmap->At(left, y), RGB(0, 0, 255), 2),
              y >= top && y < top + glyph) << y;
  }
  DestroyIcon(icon);
}

INSTANTIATE_TEST_SUITE_P(Dpi, RenderScale, testing::Values(96, 120, 144, 192));

TEST(VisualRender, ModelReplacementClearsTheOldHover) {
  Harness h(96);
  ASSERT_NE(h.overlay, nullptr);
  h.overlay->SetIcons({IconView{.serial = 1}});
  h.overlay->Render();
  SendMessageW(h.overlay->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(4, 24));
  const auto hovered = h.overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(hovered.has_value());
  EXPECT_GT(hovered->At(4, 24) >> 24, 0U);
  h.overlay->SetIcons({IconView{.serial = 2}});
  const auto replaced = h.overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(replaced.has_value());
  EXPECT_EQ(replaced->At(4, 24), 0U);
}

TEST(VisualRender, TranslucentIconsArePremultipliedExactlyOnce) {
  Harness h(96);
  ASSERT_NE(h.overlay, nullptr);
  const HICON icon = SolidIcon(16, RGB(255, 0, 0), 128);
  ASSERT_NE(icon, nullptr);
  h.overlay->SetIcons({IconView{.icon = icon, .serial = 1}});
  const auto bitmap = h.overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(bitmap.has_value());
  const uint32_t pixel = bitmap->At(16, 24);
  EXPECT_NEAR(pixel >> 24, 128U, 1);
  EXPECT_NEAR((pixel >> 16) & 255U, 128U, 1);
  DestroyIcon(icon);
}

TEST(VisualRender, UnrelatedModelUpdatesPreserveHoverOnTheSameOwner) {
  Harness h(96);
  ASSERT_NE(h.overlay, nullptr);
  IconView first{.serial = 1, .target = {
      .owner = reinterpret_cast<HWND>(0x1234), .uid = 1}};
  h.overlay->SetIcons({first, IconView{.serial = 1}});
  h.overlay->Render();
  SendMessageW(h.overlay->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(4, 24));
  first.serial = 2;  // an animated icon must retain its hover
  h.overlay->SetIcons({first, IconView{.serial = 2}});
  const auto bitmap = h.overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_GT(bitmap->At(4, 24) >> 24, 0U);
}

TEST(VisualRender, HoverUsesAnInsetVerticalRectangle) {
  Harness h(96);
  ASSERT_NE(h.overlay, nullptr);
  h.overlay->SetIcons({IconView{.serial = 1}});
  h.overlay->Render();
  SendMessageW(h.overlay->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(16, 24));
  const auto bitmap = h.overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_GT(bitmap->At(16, 6) >> 24, 0U);  // tall, beyond the old square
  EXPECT_EQ(bitmap->At(0, 24), 0U);       // horizontal breathing room
  EXPECT_EQ(bitmap->At(16, 47), 0U);      // vertical breathing room
}

TEST(VisualRender, NativeFortyPixelButtonKeepsItsTallHighlight) {
  const RECT strip{100, 100, 260, 148};
  auto overlay = Overlay::Create(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY),
                                  strip, {}, {}, {32, 40, 96});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({IconView{.serial = 1}});
  overlay->Render();
  SendMessageW(overlay->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(16, 24));
  const auto bitmap = overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_GT(bitmap->At(16, 6) >> 24, 0U);
  EXPECT_EQ(bitmap->At(0, 24), 0U);
}

TEST(VisualRender, MonochromeIconsKeepTheirMaskAndRemainVisible) {
  Harness h(96);
  ASSERT_NE(h.overlay, nullptr);
  BYTE andMask[32] = {};
  BYTE xorMask[32];
  std::memset(xorMask, 255, sizeof(xorMask));
  const HICON icon = CreateIcon(GetModuleHandleW(nullptr), 16, 16, 1, 1,
                                andMask, xorMask);
  ASSERT_NE(icon, nullptr);
  h.overlay->SetIcons({IconView{.icon = icon, .serial = 1}});
  const auto bitmap = h.overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_EQ(bitmap->At(16, 24), 0xFFFFFFFFU);
  DestroyIcon(icon);
}

TEST(VisualRender, SystemIconsProduceVisiblePixels) {
  wchar_t interactive[2] = {};
  const bool review = GetEnvironmentVariableW(L"TRAYMIRROR_VISUAL_REVIEW", interactive, 2) > 0;
  std::unique_ptr<HWND__, decltype(&DestroyWindow)> inspector(nullptr, DestroyWindow);
  if (review) {
    inspector.reset(CreateWindowExW(0, L"STATIC", L"TrayMirror rendering preview (no hook)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120, 640, 400,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
    ASSERT_NE(inspector, nullptr);
  }
  const HMONITOR monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
  const RECT strip{200, 400, 360, 448};
  auto overlay = Overlay::Create(monitor, strip);
  ASSERT_NE(overlay, nullptr);
  std::vector<IconView> icons;
  for (const WORD id : {WORD{32512}, WORD{32513}, WORD{32515}, WORD{32516}}) {
    icons.push_back(IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(id)),
                             .serial = 1});
  }
  overlay->SetIcons(icons, {icons.front()});
  overlay->Render();
  const auto bitmap = overlay->RenderToBitmap(96, true);
  ASSERT_TRUE(bitmap.has_value());
  size_t painted = 0;
  for (const uint32_t pixel : bitmap->pixels) {
    painted += (pixel >> 24) != 0 ? 1U : 0U;
  }
  EXPECT_GT(painted, 100U);
  if (review) {
    auto tip = traymirror::Tooltip::Create(overlay->hwnd());
    ASSERT_NE(tip, nullptr);
    tip->Show(L"Native system font and popup material", strip);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (std::chrono::steady_clock::now() < until) {
      MSG msg = {};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
  }
}

}  // namespace
