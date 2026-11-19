#include "render/OverlayThread.h"

#include "Log.h"
#include "render/Overlay.h"

#include <shellscalingapi.h>

#include <bit>
#include <utility>

namespace traymirror {
namespace {

constexpr wchar_t kUiClass[] = L"TrayMirrorUiWnd";

// Overlay thread-local wake-ups (not part of the public protocol).
constexpr UINT kMsgRebuild = WM_APP + 2;

// How many icon slots the strip reserves at its right end (stage 2 has
// no overflow chevron — plan ruling R6). Twelve slots at 96 DPI is
// 216 px, comfortably inside the measured 490 px gap of the reference
// machine and never over the clock.
constexpr int kStripIconSlots = 12;

UINT DpiForMonitor(HMONITOR monitor) {
  UINT dpiX = 96;
  UINT dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) != 0) {
    return 96;
  }
  return dpiX;
}

}  // namespace

bool OverlayThread::EnsureUiClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OverlayThread::UiProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kUiClass;
    if (RegisterClassExW(&wc) != 0) {
      return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

LRESULT CALLBACK OverlayThread::UiProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = std::bit_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self =
      std::bit_cast<OverlayThread*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleUiMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT OverlayThread::HandleUiMessage(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  // `hwnd` is the parameter, not uiHwnd_: WM_NCCREATE runs before the
  // member is assigned, and DefWindowProc(nullptr, ...) refuses creation.
  switch (message) {
    case kMsgInbox:
      ApplyInbox();
      return 0;
    case kMsgRebuild:
      BuildOverlays();
      return 0;
    case WM_DISPLAYCHANGE:
      // Posted, not handled inline: rebuilding windows inside the
      // broadcast is legal but slow for every other window's sender.
      PostMessageW(hwnd, kMsgRebuild, 0, 0);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

OverlayThread::~OverlayThread() { Stop(); }

bool OverlayThread::Start(ModelInbox* inbox, TrayModel* model,
                          MonitorEnumerator enumerate) {
  if (thread_.joinable()) {
    return false;
  }
  inbox_ = inbox;
  model_ = model;
  enumerate_ = std::move(enumerate);
  ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (ready_ == nullptr) {
    return false;
  }
  thread_ = std::thread(&OverlayThread::Run, this);
  WaitForSingleObject(ready_, INFINITE);
  CloseHandle(ready_);
  ready_ = nullptr;
  return uiHwnd_.load() != nullptr;
}

void OverlayThread::Run() {
  auto log = InitHostLogging("");
  threadId_.store(GetCurrentThreadId());
  if (!EnsureUiClass()) {
    log->error("UI window class registration failed ({})", GetLastError());
    if (ready_ != nullptr) {
      SetEvent(ready_);
    }
    return;
  }
  const HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kUiClass,
                                    L"TrayMirrorUiWnd", WS_POPUP, 0, 0, 0, 0,
                                    nullptr, nullptr, GetModuleHandleW(nullptr),
                                    this);
  if (hwnd == nullptr) {
    log->error("UI window creation failed ({})", GetLastError());
    if (ready_ != nullptr) {
      SetEvent(ready_);
    }
    return;
  }
  uiHwnd_.store(hwnd);
  BuildOverlays();
  if (inbox_ != nullptr) {
    inbox_->SetNudge(hwnd, kMsgInbox);
  }
  if (ready_ != nullptr) {
    SetEvent(ready_);
  }

  MSG msg = {};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  overlays_.clear();
  uiHwnd_.store(nullptr);
}

void OverlayThread::Stop() {
  if (!thread_.joinable()) {
    return;
  }
  const HWND ui = uiHwnd_.load();
  if (ui != nullptr) {
    PostMessageW(ui, WM_CLOSE, 0, 0);
  } else {
    PostThreadMessageW(threadId_.load(), WM_QUIT, 0, 0);
  }
  thread_.join();
}

void OverlayThread::BuildOverlays() {
  overlays_.clear();
  if (!enumerate_) {
    return;
  }
  const HWND ui = uiHwnd_.load();
  for (const MonitorTaskbar& mt : enumerate_()) {
    if (!mt.hasTaskbar) {
      continue;
    }
    const UINT dpi = DpiForMonitor(mt.monitor);
    const int slot = IconPxForDpi(dpi) + IconGapPxForDpi(dpi);
    const RECT strip = ComputeOverlayStripRect(mt, slot * kStripIconSlots);
    if (strip.right <= strip.left) {
      continue;
    }
    auto overlay = Overlay::Create(mt.monitor, strip, [ui]() {
      if (ui != nullptr) {
        PostMessageW(ui, kMsgRebuild, 0, 0);
      }
    });
    if (overlay != nullptr) {
      overlays_.push_back(std::move(overlay));
    }
  }
  PushModelToOverlays();
}

void OverlayThread::ApplyInbox() {
  if (inbox_ == nullptr || model_ == nullptr) {
    return;
  }
  bool changed = false;
  for (const TrayMessage& msg : inbox_->Drain()) {
    if (model_->Apply(msg)) {
      changed = true;
    }
  }
  if (changed) {
    PushModelToOverlays();
  }
}

void OverlayThread::PushModelToOverlays() {
  if (model_ == nullptr || overlays_.empty()) {
    return;
  }
  std::vector<IconView> icons;
  icons.reserve(model_->icons().size());
  for (const TrayIcon& icon : model_->icons()) {
    if (icon.hidden()) {
      continue;
    }
    icons.push_back(IconView{.icon = icon.iconCopy,
                             .serial = icon.iconSerial,
                             .tip = icon.tip});
  }
  for (const std::unique_ptr<Overlay>& overlay : overlays_) {
    overlay->SetIcons(icons);
    overlay->Render();
  }
}

}  // namespace traymirror
