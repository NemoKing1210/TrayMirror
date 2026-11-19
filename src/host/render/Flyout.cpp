#include "render/Flyout.h"

#include "Log.h"
#include "render/CellLayout.h"
#include "win/Monitors.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <bit>
#include <iterator>

namespace traymirror {
namespace {

constexpr wchar_t kFlyoutClass[] = L"TrayMirrorFlyout";

// The gap between the strip and the flyout, in DIPs.
constexpr int kAnchorGapDip = 4;

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

UINT DpiForMonitor(HMONITOR monitor) {
  UINT dpiX = 96;
  UINT dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) != 0) {
    return 96;
  }
  return dpiX;
}

}  // namespace

bool Flyout::EnsureClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &Flyout::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kFlyoutClass;
    if (RegisterClassExW(&wc) != 0) {
      return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

LRESULT CALLBACK Flyout::WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = std::bit_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = std::bit_cast<Flyout*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Flyout::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                              LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;  // Paint() fills every pixel
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      const HDC dc = BeginPaint(hwnd, &ps);
      Paint(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TrackLeave(hwnd);
      SetHover(HitTest(POINT{.x = GET_X_LPARAM(lparam),
                             .y = GET_Y_LPARAM(lparam)}));
      return 0;
    }
    case WM_MOUSELEAVE:
      SetHover(kNoHover);
      return 0;
    case WM_LBUTTONDOWN:
      SetHover(HitTest(POINT{.x = GET_X_LPARAM(lparam),
                             .y = GET_Y_LPARAM(lparam)}));
      DispatchAt(TrayEvent::kLeftDown,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_LBUTTONUP:
      DispatchAt(TrayEvent::kLeftUp,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      Close();  // the click was the user's whole interaction
      return 0;
    case WM_LBUTTONDBLCLK:
      DispatchAt(TrayEvent::kLeftDblClk,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONDOWN:
      DispatchAt(TrayEvent::kRightDown,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONUP:
      DispatchAt(TrayEvent::kRightUp,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      Close();  // the owner's menu takes over from here
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Close();
      }
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) {
        if (activated_) {
          Close();  // an outside click deactivates the flyout
        }
      } else {
        activated_ = true;
      }
      return 0;
    case WM_CLOSE:
      Close();
      return 0;
    case WM_NCDESTROY:
      hwnd_ = nullptr;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::unique_ptr<Flyout> Flyout::Create(HMONITOR monitor, const RECT& anchor,
                                       std::vector<IconView> icons,
                                       DispatchClick onClick,
                                       std::function<void()> onClose) {
  auto log = InitHostLogging("");
  if (icons.empty()) {
    return nullptr;  // nothing to reveal: the chevron would not exist
  }
  if (!EnsureClass()) {
    log->error("flyout window class registration failed ({})", GetLastError());
    return nullptr;
  }
  std::unique_ptr<Flyout> flyout(new Flyout());
  const UINT dpi = DpiForMonitor(monitor);
  flyout->iconPx_ = IconPxForDpi(dpi);
  flyout->dark_ = SystemUsesDarkTheme();
  flyout->icons_ = std::move(icons);
  flyout->onClick_ = std::move(onClick);
  flyout->onClose_ = std::move(onClose);

  const GridLayout grid = LayoutGrid(flyout->iconPx_, flyout->icons_.size());
  flyout->cells_ = grid.cells;
  if (grid.width <= 0 || grid.height <= 0) {
    return nullptr;
  }

  // Above the strip, right edges aligned; clamped into the work area so
  // a strip at the top edge puts the flyout below itself instead.
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  const bool haveMonitor = GetMonitorInfoW(monitor, &mi) != FALSE;
  const int gap = MulDiv(kAnchorGapDip, static_cast<int>(dpi), 96);
  int x = anchor.right - grid.width;
  int y = anchor.top - grid.height - gap;
  if (haveMonitor) {
    if (y < mi.rcWork.top) {
      y = anchor.bottom + gap;
    }
    if (x < mi.rcWork.left) {
      x = mi.rcWork.left;
    }
    if (x + grid.width > mi.rcWork.right) {
      x = mi.rcWork.right - grid.width;
    }
    if (y + grid.height > mi.rcWork.bottom) {
      y = mi.rcWork.bottom - grid.height;
    }
  }

  const HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kFlyoutClass, L"TrayMirrorFlyout",
      WS_POPUP, x, y, grid.width, grid.height, nullptr, nullptr,
      GetModuleHandleW(nullptr), flyout.get());
  if (hwnd == nullptr) {
    log->error("flyout window creation failed ({})", GetLastError());
    return nullptr;
  }
  flyout->hwnd_ = hwnd;

  const int radius = MulDiv(8, static_cast<int>(dpi), 96);
  HRGN region = CreateRoundRectRgn(0, 0, grid.width + 1, grid.height + 1,
                                   radius, radius);
  if (region != nullptr) {
    SetWindowRgn(hwnd, region, FALSE);  // the window owns the region now
  }

  ShowWindow(hwnd, SW_SHOW);
  // The user just clicked the chevron, so the process received the last
  // input event and is allowed to take the foreground; without it the
  // keyboard (and therefore Esc) never reaches this thread.
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  flyout->activated_ = GetForegroundWindow() == hwnd;
  if (!flyout->activated_) {
    log->warn("flyout could not take the foreground: Esc and outside-click "
              "will not close it");
  }
  UpdateWindow(hwnd);
  return flyout;
}

Flyout::~Flyout() { Close(); }

void Flyout::Close() {
  auto close = std::move(onClose_);
  onClick_ = {};
  if (hwnd_ != nullptr) {
    const HWND hwnd = hwnd_;
    hwnd_ = nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
  }
  if (close) {
    close();  // may destroy this object: nothing below may use a member
  }
}

void Flyout::Paint(HDC dc) {
  RECT client = {};
  GetClientRect(hwnd_, &client);
  const COLORREF background = dark_ ? RGB(32, 32, 32) : RGB(243, 243, 243);
  HBRUSH brush = CreateSolidBrush(background);
  if (brush != nullptr) {
    FillRect(dc, &client, brush);
    DeleteObject(brush);
  }
  if (hovered_ != kNoHover && hovered_ < cells_.size()) {
    RECT hover = cells_[hovered_];
    InflateRect(&hover, -2, -2);
    HBRUSH hoverBrush = CreateSolidBrush(dark_ ? RGB(60, 60, 60)
                                               : RGB(225, 225, 225));
    if (hoverBrush != nullptr) {
      FillRect(dc, &hover, hoverBrush);
      DeleteObject(hoverBrush);
    }
  }
  for (size_t i = 0; i < cells_.size() && i < icons_.size(); ++i) {
    const RECT& cell = cells_[i];
    DrawIconEx(dc, cell.left, cell.top, icons_[i].icon, iconPx_, iconPx_, 0,
               nullptr, DI_NORMAL);
  }
}

void Flyout::SetHover(size_t index) {
  if (index == hovered_) {
    return;
  }
  hovered_ = index;
  if (hwnd_ != nullptr) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

size_t Flyout::HitTest(POINT pt) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& cell = cells_[i];
    if (pt.x >= cell.left && pt.x < cell.right && pt.y >= cell.top &&
        pt.y < cell.bottom) {
      return i;
    }
  }
  return kNoHover;
}

void Flyout::DispatchAt(TrayEvent event, POINT pt) {
  const size_t index = HitTest(pt);
  if (index == kNoHover || index >= icons_.size() || !onClick_) {
    return;
  }
  const DispatchTarget& target = icons_[index].target;
  if (target.owner == nullptr) {
    return;
  }
  RECT window = {};
  if (GetWindowRect(hwnd_, &window) == FALSE) {
    return;
  }
  onClick_(DispatchCall{
      .target = target,
      .event = event,
      .screenPos = POINT{.x = window.left + pt.x, .y = window.top + pt.y}});
}

void Flyout::TrackLeave(HWND hwnd) {
  if (tracking_) {
    return;
  }
  TRACKMOUSEEVENT track = {};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd;
  TrackMouseEvent(&track);
  tracking_ = true;
}

}  // namespace traymirror
