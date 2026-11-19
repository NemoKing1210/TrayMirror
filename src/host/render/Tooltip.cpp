#include "render/Tooltip.h"

#include "Log.h"

#include <d2d1helper.h>
#include <shellscalingapi.h>

#include <cmath>

namespace traymirror {
namespace {

constexpr wchar_t kTooltipClass[] = L"TrayMirrorTooltip";

// The measured cells (docs/visual-spec.md, report 09), in DIPs.
constexpr int kPadXDip = 10;      // text to border, horizontal
constexpr int kPadYDip = 8;       // line box to border, vertical
constexpr int kRadiusDip = 4;     // corner radius
constexpr int kGapDip = 9;        // tooltip bottom to the taskbar row
constexpr int kFontDip = 12;      // Segoe UI, 9 pt at 96 DPI
constexpr wchar_t kFontFamily[] = L"Segoe UI";

D2D1_COLOR_F ToColorF(COLORREF color, float alpha = 1.0F) {
  return D2D1::ColorF(GetRValue(color) / 255.0F, GetGValue(color) / 255.0F,
                      GetBValue(color) / 255.0F, alpha);
}

int Scaled(int dip, int dpi) { return MulDiv(dip, dpi, 96); }

bool SystemUsesDarkTheme() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return true;  // the Win11 default taskbar is dark
  }
  DWORD light = 1;
  DWORD size = sizeof(light);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(
      key, L"AppsUseLightTheme", nullptr, &type,
      reinterpret_cast<LPBYTE>(&light), &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_DWORD) {
    return true;
  }
  return light == 0;
}

}  // namespace

TooltipPalette TooltipPalette::For(bool dark) {
  if (dark) {
    return TooltipPalette{.background = RGB(41, 45, 50),   // #292D32, measured
                          .text = RGB(247, 247, 247)};    // #F7F7F7, measured
  }
  return TooltipPalette{.background = RGB(249, 249, 249),  // derived
                        .text = RGB(26, 26, 26)};         // derived
}

bool Tooltip::EnsureClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Tooltip::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTooltipClass;
    if (RegisterClassExW(&wc) != 0) {
      return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

LRESULT CALLBACK Tooltip::WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::unique_ptr<Tooltip> Tooltip::Create(HWND owner, ThemeProbe dark) {
  if (owner == nullptr) {
    return nullptr;
  }
  if (!EnsureClass()) {
    InitHostLogging("")->error("tooltip window class registration failed ({})",
                               GetLastError());
    return nullptr;
  }
  std::unique_ptr<Tooltip> tip(new Tooltip());
  tip->owner_ = owner;
  tip->dark_ = std::move(dark);
  if (!tip->InitFactories()) {
    return nullptr;
  }
  tip->hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      kTooltipClass, L"TrayMirrorTooltip", WS_POPUP, 0, 0, 0, 0, nullptr,
      nullptr, GetModuleHandleW(nullptr), nullptr);
  if (tip->hwnd_ == nullptr) {
    InitHostLogging("")->error("tooltip window creation failed ({})",
                               GetLastError());
    return nullptr;
  }
  return tip;
}

Tooltip::~Tooltip() {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool Tooltip::InitFactories() {
  auto log = InitHostLogging("");
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               d2d_.GetAddressOf())) != 0) {
    log->warn("tooltip: D2D1CreateFactory failed");
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(
                                     dwrite_.GetAddressOf()))) != 0) {
    log->warn("tooltip: DWriteCreateFactory failed");
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0F, 96.0F);  // 1 DIP = 1 px: the scaling is done with MulDiv
  if (FAILED(d2d_->CreateDCRenderTarget(&props, target_.GetAddressOf())) != 0) {
    log->warn("tooltip: CreateDCRenderTarget failed");
    return false;
  }
  return true;
}

bool Tooltip::UsesDarkTheme() const {
  return dark_ ? dark_() : SystemUsesDarkTheme();
}

bool Tooltip::Measure(const std::wstring& text, int dpi, int* width,
                      int* height) {
  // The format is rebuilt per tip: the font size follows the monitor DPI,
  // and a tooltip is shown once per hover, not per frame.
  format_.Reset();
  const auto fontSize = static_cast<float>(Scaled(kFontDip, dpi));
  if (FAILED(dwrite_->CreateTextFormat(kFontFamily, nullptr,
                                       DWRITE_FONT_WEIGHT_REGULAR,
                                       DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, fontSize,
                                       L"", format_.GetAddressOf())) != 0) {
    return false;
  }
  format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite_->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format_.Get(),
          4096.0F, 4096.0F, layout.GetAddressOf())) != 0) {
    return false;
  }
  DWRITE_TEXT_METRICS metrics = {};
  if (FAILED(layout->GetMetrics(&metrics)) != 0) {
    return false;
  }
  *width = static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) +
           (2 * Scaled(kPadXDip, dpi));
  *height = static_cast<int>(std::ceil(metrics.height)) +
            (2 * Scaled(kPadYDip, dpi));
  return *width > 0 && *height > 0;
}

void Tooltip::Show(const std::wstring& text, const RECT& iconRect) {
  if (hwnd_ == nullptr) {
    return;
  }
  if (text.empty()) {
    Hide();
    return;
  }
  const UINT dpi = owner_ != nullptr ? GetDpiForWindow(owner_) : 96;
  const int scaledDpi = static_cast<int>(dpi == 0 ? 96 : dpi);
  int width = 0;
  int height = 0;
  if (!Measure(text, scaledDpi, &width, &height)) {
    Hide();
    return;
  }

  // Centred on the icon, clamped into the icon's monitor; above it when
  // there is room, below it otherwise (measured: box centre 2359.5 for an
  // icon at 2360, 9 px above the taskbar row).
  RECT monitorRect = {};
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  RECT probe = iconRect;
  const HMONITOR monitor = MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST);
  if (GetMonitorInfoW(monitor, &mi) != FALSE) {
    monitorRect = mi.rcMonitor;
  } else {
    monitorRect = RECT{.left = 0,
                       .top = 0,
                       .right = GetSystemMetrics(SM_CXSCREEN),
                       .bottom = GetSystemMetrics(SM_CYSCREEN)};
  }
  int x = ((iconRect.left + iconRect.right) / 2) - (width / 2);
  int y = iconRect.top - Scaled(kGapDip, scaledDpi) - height;
  if (y < monitorRect.top) {
    y = iconRect.bottom + Scaled(kGapDip, scaledDpi);
  }
  if (x < monitorRect.left) {
    x = monitorRect.left;
  }
  if (x + width > monitorRect.right) {
    x = monitorRect.right - width;
  }

  if (!Paint(text, width, height, scaledDpi, POINT{.x = x, .y = y})) {
    Hide();
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  // The window carries the text as its title: a layered popup draws
  // nothing but its pixels, and the title is how a caller (and the
  // overlay's tests) reads back what the last hover produced. It is not
  // cleared when the window hides, so the record outlives the hover.
  SetWindowTextW(hwnd_, text.c_str());
  visible_ = true;
}

bool Tooltip::Paint(const std::wstring& text, int width, int height, int dpi,
                    POINT destination) {
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = width;
  header.bV5Height = -height;  // top-down
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000U;
  header.bV5GreenMask = 0x0000FF00U;
  header.bV5BlueMask = 0x000000FFU;
  header.bV5AlphaMask = 0xFF000000U;

  const HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return false;
  }
  void* bits = nullptr;
  const HBITMAP dib =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                       DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    ReleaseDC(nullptr, screen);
    return false;
  }
  const HDC memory = CreateCompatibleDC(screen);
  if (memory == nullptr) {
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
    return false;
  }
  auto* const previous = SelectObject(memory, dib);

  RECT bounds = {.left = 0, .top = 0, .right = width, .bottom = height};
  bool painted = FAILED(target_->BindDC(memory, &bounds)) == 0;
  if (painted) {
    target_->SetDpi(96.0F, 96.0F);  // everything here is already in pixels
    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));  // transparent
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> foreground;
    const TooltipPalette palette = TooltipPalette::For(UsesDarkTheme());
    painted =
        SUCCEEDED(target_->CreateSolidColorBrush(
            ToColorF(palette.background), background.GetAddressOf())) &&
        SUCCEEDED(target_->CreateSolidColorBrush(ToColorF(palette.text),
                                                 foreground.GetAddressOf()));
    if (painted) {
      const auto radius = static_cast<float>(Scaled(kRadiusDip, dpi));
      const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
          D2D1::RectF(0.5F, 0.5F, static_cast<float>(width) - 0.5F,
                      static_cast<float>(height) - 0.5F),
          radius, radius);
      target_->FillRoundedRectangle(box, background.Get());
      Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
      if (SUCCEEDED(dwrite_->CreateTextLayout(
              text.c_str(), static_cast<UINT32>(text.size()), format_.Get(),
              static_cast<float>(width), static_cast<float>(height),
              layout.GetAddressOf()))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        target_->DrawTextLayout(
            D2D1::Point2F(static_cast<float>(Scaled(kPadXDip, dpi)),
                          static_cast<float>(Scaled(kPadYDip, dpi))),
            layout.Get(), foreground.Get());
      } else {
        painted = false;
      }
    }
    painted = target_->EndDraw() == S_OK && painted;
  }

  BLENDFUNCTION blend = {};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  SIZE size = {.cx = width, .cy = height};
  POINT source = {.x = 0, .y = 0};
  if (painted && UpdateLayeredWindow(hwnd_, screen, &destination, &size, memory,
                                     &source, 0, &blend, ULW_ALPHA) == FALSE) {
    InitHostLogging("")->warn("tooltip: UpdateLayeredWindow failed ({})",
                              GetLastError());
    painted = false;
  }

  SelectObject(memory, previous);
  DeleteDC(memory);
  DeleteObject(dib);
  ReleaseDC(nullptr, screen);
  return painted;
}

void Tooltip::Hide() {
  if (hwnd_ == nullptr || !visible_) {
    return;
  }
  ShowWindow(hwnd_, SW_HIDE);
  visible_ = false;
}

}  // namespace traymirror
