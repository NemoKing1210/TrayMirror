#include "render/Tooltip.h"

#include "Log.h"
#include "render/Theme.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace traymirror {
namespace {

constexpr wchar_t kTooltipClass[] = L"TrayMirrorTooltip";

// Used only if Windows cannot supply NONCLIENTMETRICS for the popup.
constexpr wchar_t kFontFamily[] = L"Segoe UI";

int Scaled(int dip, int dpi) { return MulDiv(dip, dpi, 96); }

// GDI has no emergency break: a word wider than the box would run past
// the edge. This inserts a hard break at the first character that no
// longer fits, so an unbroken 600-character tip wraps instead of
// overflowing the monitor.
std::wstring WrapToWidth(HDC dc, const std::wstring& text, int width) {
  if (width <= 0) {
    return text;
  }
  std::wstring out;
  out.reserve(text.size() + (text.size() / 8));
  std::wstring line;
  for (const wchar_t ch : text) {
    if (ch == L'\n') {
      out += line;
      out += L'\n';
      line.clear();
      continue;
    }
    line.push_back(ch);
    SIZE extent = {};
    if (GetTextExtentPoint32W(dc, line.c_str(),
                              static_cast<int>(line.size()), &extent) != FALSE &&
        extent.cx > width && line.size() > 1) {
      line.pop_back();
      out += line;
      out += L'\n';
      line.assign(1, ch);
    }
  }
  out += line;
  return out;
}

BYTE Channel(float value) {
  return static_cast<BYTE>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

COLORREF ToColorRef(const D2D1_COLOR_F& color) {
  return RGB(Channel(color.r), Channel(color.g), Channel(color.b));
}

}  // namespace

bool Tooltip::EnsureClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
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
  if (message == WM_NCCREATE) {
    const auto* create = std::bit_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = std::bit_cast<Tooltip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCHITTEST) {
    return HTTRANSPARENT;
  }
  if (message == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  }
  if (self != nullptr && message == WM_NCDESTROY) {
    self->hwnd_ = nullptr;
    self->visible_ = false;
  }
  if (self != nullptr && message == WM_PAINT) {
    PAINTSTRUCT paint = {};
    const HDC dc = BeginPaint(hwnd, &paint);
    self->Paint(dc);
    EndPaint(hwnd, &paint);
    return 0;
  }
  if (self != nullptr && message == WM_PRINTCLIENT) {
    // Same pixels as WM_PAINT, into a caller-supplied DC: used by tests
    // and by window-capture tools.
    self->Paint(std::bit_cast<HDC>(static_cast<std::uintptr_t>(wparam)));
    return 0;
  }
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  if (self != nullptr && self->visible_ &&
      (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED)) {
    self->Show(self->text_, self->anchor_);
    return 0;
  }
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
  tip->hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
      kTooltipClass, L"TrayMirrorTooltip", WS_POPUP, 0, 0, 0, 0, nullptr,
      nullptr, GetModuleHandleW(nullptr), tip.get());
  if (tip->hwnd_ == nullptr) {
    InitHostLogging("")->error("tooltip window creation failed ({})",
                               GetLastError());
    return nullptr;
  }
  return tip;
}

Tooltip::~Tooltip() {
  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool Tooltip::UsesDarkTheme() const {
  return dark_ ? dark_() : SystemUsesDarkTheme();
}

bool Tooltip::Measure(const std::wstring& text, int dpi, int maxWidth,
                      int maxHeight, int* width, int* height) {
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  const bool systemFont = SystemParametersInfoForDpi(
      SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
      static_cast<UINT>(dpi)) != FALSE;
  HFONT font = systemFont ? CreateFontIndirectW(&metrics.lfStatusFont) : nullptr;
  if (font == nullptr) {
    font = CreateFontW(-Scaled(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, kFontFamily);
  }
  if (font == nullptr) {
    return false;
  }
  HDC dc = GetDC(hwnd_);
  if (dc == nullptr) {
    DeleteObject(font);
    return false;
  }
  const HGDIOBJ previousFont = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  const int horizontal = Scaled(metrics::kTooltipPadLeft +
                                metrics::kTooltipPadRight, dpi);
  const int vertical = Scaled(metrics::kTooltipPadTop +
                              metrics::kTooltipPadBottom, dpi);
  const int contentWidth = (std::max)(1, maxWidth - horizontal);
  const int contentHeight = (std::max)(1, maxHeight - vertical);
  std::wstring wrapped = WrapToWidth(dc, text, contentWidth);
  RECT calc = {.left = 0, .top = 0, .right = contentWidth,
               .bottom = contentHeight};
  DrawTextW(dc, wrapped.c_str(), static_cast<int>(wrapped.size()), &calc,
            DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK);
  SelectObject(dc, previousFont);
  ReleaseDC(hwnd_, dc);
  if (font_ != nullptr) {
    DeleteObject(font_);
  }
  font_ = font;
  wrapped_ = std::move(wrapped);
  *width = (std::min)(maxWidth, static_cast<int>(calc.right) + horizontal);
  *height = (std::min)(maxHeight, static_cast<int>(calc.bottom) + vertical);
  return *width > 0 && *height > 0;
}

void Tooltip::Show(const std::wstring& text, const RECT& iconRect) {
  if (hwnd_ == nullptr || text.empty()) {
    Hide();
    return;
  }
  const UINT dpi = GetDpiForWindow(owner_);
  dpi_ = static_cast<int>(dpi == 0 ? 96 : dpi);
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  RECT probe = iconRect;
  if (GetMonitorInfoW(MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST),
                       &monitor) == FALSE) {
    Hide();
    return;
  }
  const RECT bounds = monitor.rcWork;
  int width = 0;
  int height = 0;
  if (!Measure(text, dpi_, (std::min)(Scaled(480, dpi_), static_cast<int>(bounds.right - bounds.left)),
               bounds.bottom - bounds.top, &width, &height)) {
    Hide();
    return;
  }
  const int gap = Scaled(metrics::kTooltipGap, dpi_);
  int x = ((iconRect.left + iconRect.right) / 2) - (width / 2);
  int y = iconRect.top - gap - height;
  if (y < bounds.top) {
    y = iconRect.bottom + gap;
  }
  x = std::clamp(x, static_cast<int>(bounds.left),
                   static_cast<int>(bounds.right) - width);
  y = std::clamp(y, static_cast<int>(bounds.top),
                   static_cast<int>(bounds.bottom) - height);
  text_ = text;
  anchor_ = iconRect;
  ApplyPopupAppearance(hwnd_, UsesDarkTheme(), true);
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  SetWindowTextW(hwnd_, text.c_str());
  visible_ = true;
  InvalidateRect(hwnd_, nullptr, FALSE);
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd_);
}

void Tooltip::Paint(HDC dc) {
  RECT client = {};
  GetClientRect(hwnd_, &client);
  const ThemeTokens tokens = ThemeTokens::Current(UsesDarkTheme());
  auto* const brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
  SetDCBrushColor(dc, tokens.tooltipSurface);
  FillRect(dc, &client, brush);
  if (font_ == nullptr) {
    return;
  }
  const HGDIOBJ previous = SelectObject(dc, font_);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, ToColorRef(tokens.tooltipText));
  RECT area = {.left = Scaled(metrics::kTooltipPadLeft, dpi_),
               .top = Scaled(metrics::kTooltipPadTop, dpi_),
               .right = client.right - Scaled(metrics::kTooltipPadRight, dpi_),
               .bottom = client.bottom - Scaled(metrics::kTooltipPadBottom, dpi_)};
  DrawTextW(dc, wrapped_.c_str(), static_cast<int>(wrapped_.size()), &area,
            DT_NOPREFIX | DT_WORDBREAK | DT_LEFT | DT_TOP);
  SelectObject(dc, previous);
}

void Tooltip::Hide() {
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  visible_ = false;
}

}  // namespace traymirror
