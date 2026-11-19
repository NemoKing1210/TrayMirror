// Overlay smoke tests (spec 6.1/6.2): real windows and a real
// D2D/DComp surface, synthetic geometry — no Explorer contact. A
// composition failure must degrade (window alive, no render), never
// crash, so the assertions are about the window, not the GPU.
#include "render/Overlay.h"
#include "render/OverlayThread.h"

#include "core/ModelInbox.h"
#include "core/TrayModel.h"
#include "dispatch/Dispatch.h"
#include "win/Monitors.h"

#include <windows.h>

#include <commctrl.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

namespace {

using traymirror::ComputeOverlayStripRect;
using traymirror::DispatchCall;
using traymirror::Dispatcher;
using traymirror::DispatchTarget;
using traymirror::IconView;
using traymirror::ModelInbox;
using traymirror::MonitorTaskbar;
using traymirror::Overlay;
using traymirror::OverlayThread;
using traymirror::TrayEvent;
using traymirror::TrayModel;

// The system tooltip window is a top-level window of class
// tooltips_class32 owned by the overlay. Found by enumeration so the
// test never depends on the tooltip's own internals.
HWND g_tooltipOwner = nullptr;
HWND g_tooltipFound = nullptr;

BOOL CALLBACK FindTooltipProc(HWND hwnd, LPARAM /*unused*/) {
  wchar_t cls[64] = {};
  if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0 ||
      wcscmp(cls, L"tooltips_class32") != 0) {
    return TRUE;
  }
  if (GetWindow(hwnd, GW_OWNER) != g_tooltipOwner) {
    return TRUE;
  }
  g_tooltipFound = hwnd;
  return FALSE;
}

HWND FindOwnedTooltip(HWND owner) {
  g_tooltipOwner = owner;
  g_tooltipFound = nullptr;
  EnumWindows(FindTooltipProc, 0);
  return g_tooltipFound;
}

// A taskbar row on the monitor the test process starts on: the real
// work-area edge, a measured-width clock at its right edge.
MonitorTaskbar SyntheticTaskbar() {
  const POINT origin = {0, 0};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  const BOOL ok = GetMonitorInfoW(monitor, &mi);
  EXPECT_NE(ok, FALSE);

  MonitorTaskbar mt;
  mt.monitor = monitor;
  mt.monitorRect = mi.rcMonitor;
  mt.taskbarRect = {mi.rcMonitor.left, mi.rcWork.bottom, mi.rcMonitor.right,
                    mi.rcMonitor.bottom};
  mt.clockRect = {mt.taskbarRect.right - 73, mt.taskbarRect.top,
                  mt.taskbarRect.right, mt.taskbarRect.bottom};
  mt.hasTaskbar = true;
  return mt;
}

// A tray icon owner (spec 4.4): its own thread and window, able to stop
// pumping to simulate a hung application.
constexpr UINT kClickCallback = WM_APP + 200;
constexpr UINT kHangMessage = WM_APP + 201;
constexpr wchar_t kOwnerClass[] = L"TrayMirrorOverlayTestOwner";
std::atomic<bool> g_ownerHung{false};

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wparam,
                           LPARAM lparam) {
  if (message == kHangMessage) {
    g_ownerHung = true;
    Sleep(static_cast<DWORD>(wparam));
    return 0;
  }
  if (message == WM_CLOSE) {
    DestroyWindow(hwnd);
    return 0;
  }
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

class Owner {
 public:
  bool Start() {
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready_ == nullptr) {
      return false;
    }
    thread_ = std::thread([this]() {
      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &OwnerProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = kOwnerClass;
      RegisterClassExW(&wc);
      hwnd_ = CreateWindowExW(0, kOwnerClass, L"", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, wc.hInstance, nullptr);
      SetEvent(ready_);
      if (hwnd_ == nullptr) {
        return;
      }
      MSG msg = {};
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        DispatchMessageW(&msg);
      }
    });
    WaitForSingleObject(ready_, 5000);
    CloseHandle(ready_);
    ready_ = nullptr;
    return hwnd_ != nullptr;
  }

  void Stop() {
    if (!thread_.joinable()) {
      return;
    }
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    thread_.join();
  }

  ~Owner() { Stop(); }

  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

  void Hang(DWORD ms) {
    g_ownerHung = false;
    PostMessageW(hwnd_, kHangMessage, ms, 0);
    for (int waited = 0; waited < 2000 && !g_ownerHung; waited += 5) {
      Sleep(5);
    }
  }

 private:
  std::thread thread_;
  HANDLE ready_ = nullptr;
  HWND hwnd_ = nullptr;
};

DispatchTarget MakeTarget(HWND owner, UINT version = 0) {
  return DispatchTarget{.owner = owner,
                        .callbackMessage = kClickCallback,
                        .uid = 9,
                        .version = version,
                        .ownerPid = 4242};
}

IconView FirstIcon(DispatchTarget target) {
  return IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
                  .serial = 1,
                  .tip = L"first",
                  .target = target};
}

}  // namespace

TEST(OverlaySmoke, CreatesAndDestroysStrip) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);
  EXPECT_NE(overlay->hwnd(), nullptr);
  overlay->SetIcons({});
  overlay->Render();
  overlay.reset();  // destruction must not hang or crash
}

TEST(OverlaySmoke, StripMatchesGeometry) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);

  RECT actual = {};
  ASSERT_NE(GetWindowRect(overlay->hwnd(), &actual), FALSE);
  EXPECT_EQ(actual.left, strip.left);
  EXPECT_EQ(actual.top, strip.top);
  EXPECT_EQ(actual.right, strip.right);
  EXPECT_EQ(actual.bottom, strip.bottom);
}

TEST(OverlaySmoke, DrawsIconsWithRealD2D) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);

  // Real icons plus one null handle (the placeholder path). A device
  // failure inside drawing must degrade to a log line, never a crash.
  overlay->SetIcons(
      {traymirror::IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
                            .serial = 1,
                            .tip = L"first"},
       traymirror::IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32513)),
                            .serial = 2,
                            .tip = L"second"},
       traymirror::IconView{.icon = nullptr, .serial = 3, .tip = L"missing"}});
  overlay->Render();
  overlay->Render();  // clean: no dirty flag, no redraw
}

TEST(OverlaySmoke, TooltipShowsAndHides) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({traymirror::IconView{
      .icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
      .serial = 1,
      .tip = L"first tip"}});
  overlay->Render();

  // The first cell starts at the strip's left edge; the icon is
  // vertically centered, so the cell's middle is the strip's middle.
  const int height = strip.bottom - strip.top;
  const POINT inside = {4, height / 2};
  SendMessageW(overlay->hwnd(), WM_MOUSEMOVE, 0,
               MAKELPARAM(inside.x, inside.y));

  const HWND tip = FindOwnedTooltip(overlay->hwnd());
  ASSERT_NE(tip, nullptr);
  EXPECT_NE(IsWindowVisible(tip), FALSE);
  wchar_t text[128] = {};
  TOOLINFOW info = {};
  info.cbSize = sizeof(info);
  info.hwnd = overlay->hwnd();
  info.uId = reinterpret_cast<UINT_PTR>(overlay->hwnd());
  info.lpszText = text;
  SendMessageW(tip, TTM_GETTEXTW, 128, reinterpret_cast<LPARAM>(&info));
  EXPECT_STREQ(text, L"first tip");

  SendMessageW(overlay->hwnd(), WM_MOUSELEAVE, 0, 0);
  EXPECT_EQ(IsWindowVisible(tip), FALSE);
}

TEST(OverlaySmoke, UiWindowExistsWhileRunning) {  auto enumerate = []() {
    return std::vector<MonitorTaskbar>{SyntheticTaskbar()};
  };
  ModelInbox inbox;
  TrayModel model;
  OverlayThread thread;
  ASSERT_TRUE(thread.Start(&inbox, &model, enumerate));

  const HWND ui = thread.uiHwnd();
  ASSERT_NE(ui, nullptr);
  EXPECT_NE(IsWindow(ui), FALSE);

  thread.Stop();
  EXPECT_EQ(IsWindow(ui), FALSE);
}

TEST(OverlayClick, DispatchesLeftClickToOwner) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  std::vector<DispatchCall> calls;
  auto overlay = Overlay::Create(
      mt.monitor, strip, {}, [&](const DispatchCall& call) {
        calls.push_back(call);
      });
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1234)))});
  overlay->Render();

  const int height = strip.bottom - strip.top;
  SendMessageW(overlay->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(4, height / 2));
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(4, height / 2));

  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].event, TrayEvent::kLeftDown);
  EXPECT_EQ(calls[1].event, TrayEvent::kLeftUp);
  EXPECT_EQ(calls[0].screenPos.x, strip.left + 4);
  EXPECT_EQ(calls[0].screenPos.y, strip.top + (height / 2));
}

TEST(OverlayClick, IgnoresClicksOutsideIcons) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  std::vector<DispatchCall> calls;
  auto overlay = Overlay::Create(
      mt.monitor, strip, {}, [&](const DispatchCall& call) {
        calls.push_back(call);
      });
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1234)))});
  overlay->Render();

  // Right of the single 16 px icon at 96 DPI: inside the strip, outside
  // every cell. A click there belongs to nobody.
  const int height = strip.bottom - strip.top;
  const int width = strip.right - strip.left;
  SendMessageW(overlay->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(width - 4, height / 2));
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(width - 4, height / 2));

  EXPECT_TRUE(calls.empty());
}

TEST(OverlayClick, UsesModelTargetFields) {
  Owner owner;
  ASSERT_TRUE(owner.Start());
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  std::vector<DispatchCall> calls;
  auto overlay = Overlay::Create(
      mt.monitor, strip, {}, [&](const DispatchCall& call) {
        calls.push_back(call);
      });
  ASSERT_NE(overlay, nullptr);
  const DispatchTarget target = MakeTarget(owner.hwnd(), 4);
  overlay->SetIcons({FirstIcon(target)});
  overlay->Render();

  const int height = strip.bottom - strip.top;
  SendMessageW(overlay->hwnd(), WM_RBUTTONUP, 0, MAKELPARAM(4, height / 2));

  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls[0].event, TrayEvent::kRightUp);
  EXPECT_EQ(calls[0].target.owner, target.owner);
  EXPECT_EQ(calls[0].target.callbackMessage, kClickCallback);
  EXPECT_EQ(calls[0].target.uid, 9U);
  EXPECT_EQ(calls[0].target.version, 4U);
  EXPECT_EQ(calls[0].target.ownerPid, 4242U);
}

TEST(OverlayClick, UiStaysResponsiveWhileOwnerHung) {
  Owner owner;
  ASSERT_TRUE(owner.Start());
  owner.Hang(1000);  // the owner is not pumping

  Dispatcher dispatcher;  // production sender: posts, never sends
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(
      mt.monitor, strip, {}, [&](const DispatchCall& call) {
        dispatcher.Post(call);
      });
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(owner.hwnd()))});
  overlay->Render();

  const int height = strip.bottom - strip.top;
  const auto start = std::chrono::steady_clock::now();
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(4, height / 2));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            200);
}

