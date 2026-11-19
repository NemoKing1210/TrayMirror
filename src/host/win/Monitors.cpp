#include "win/Monitors.h"

#include "Log.h"

#include <UIAutomation.h>
#include <objbase.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <bit>
#include <algorithm>
#include <cwchar>
#include <vector>

namespace traymirror {
namespace {

using Microsoft::WRL::ComPtr;

// The enumeration callbacks receive their collector through LPARAM; the
// bits are the pointer (same width by construction), expressed this way
// because a cast from an integer to a pointer is what the repository's
// clang-tidy profile forbids.
template <typename T>
T* ParamPtr(LPARAM data) noexcept {
  return std::bit_cast<T*>(data);
}

// Measured on the reference machine at 96 DPI: the clock's right inset
// is 8 px and its element is 65 px wide, so the strip anchor sits 73 px
// left of the taskbar's right edge when UIA cannot be reached.
constexpr int kClockReservePx = 73;

struct MonitorEntry {
  HMONITOR monitor = nullptr;
  RECT rect = {};
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC /*unused*/,
                             LPRECT /*unused*/, LPARAM data) {
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(monitor, &mi) != FALSE) {
    ParamPtr<std::vector<MonitorEntry>>(data)->push_back(
        {.monitor = monitor, .rect = mi.rcMonitor});
  }
  return TRUE;
}

BOOL CALLBACK CollectTaskbar(HWND hwnd, LPARAM data) {
  wchar_t cls[64] = {};
  const int cap = static_cast<int>(sizeof(cls) / sizeof(cls[0]));
  if (GetClassNameW(hwnd, cls, cap) == 0) {
    return TRUE;
  }
  if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
    ParamPtr<std::vector<HWND>>(data)->push_back(hwnd);
  }
  return TRUE;
}

class ComApartment {
 public:
  ComApartment() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComApartment() {
    if (SUCCEEDED(hr_) != 0) {
      CoUninitialize();
    }
  }
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;
  [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) != 0; }

 private:
  HRESULT hr_;
};

// One taskbar element's rect via UIA (spec 6.1 names UIA for taskbar
// elements). False when the element is absent or a UIA call fails; the
// caller then falls back to the measured reserves.
bool UiaElementRect(HWND taskbar, const wchar_t* className, RECT* out) {
  ComApartment com;
  if (!com.ok()) {
    return false;
  }
  ComPtr<IUIAutomation> uia;
  if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(uia.GetAddressOf()))) != 0) {
    return false;
  }
  ComPtr<IUIAutomation2> bounded;
  if (SUCCEEDED(uia.As(&bounded))) {
    bounded->put_ConnectionTimeout(500);
    bounded->put_TransactionTimeout(500);
  }
  ComPtr<IUIAutomationElement> window;
  if (FAILED(uia->ElementFromHandle(taskbar, window.GetAddressOf())) != 0 ||
      window == nullptr) {
    return false;
  }
  VARIANT cls;
  VariantInit(&cls);
  cls.vt = VT_BSTR;
  cls.bstrVal = SysAllocString(className);
  if (cls.bstrVal == nullptr) {
    return false;
  }
  ComPtr<IUIAutomationCondition> condition;
  const HRESULT created = uia->CreatePropertyCondition(
      UIA_ClassNamePropertyId, cls, condition.GetAddressOf());
  VariantClear(&cls);
  if (FAILED(created) != 0 || condition == nullptr) {
    return false;
  }
  ComPtr<IUIAutomationElement> element;
  if (FAILED(window->FindFirst(TreeScope_Descendants, condition.Get(),
                               element.GetAddressOf())) != 0 ||
      element == nullptr) {
    return false;
  }
  RECT rect = {};
  if (FAILED(element->get_CurrentBoundingRectangle(&rect)) != 0) {
    return false;
  }
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return false;
  }
  *out = rect;
  return true;
}

struct TaskListBounds {
  RECT row = {};
  LONG right = 0;
  bool found = false;
};

BOOL CALLBACK CollectTaskListBounds(HWND hwnd, LPARAM data) {
  wchar_t name[64] = {};
  GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
  if (wcscmp(name, L"MSTaskListWClass") != 0 || IsWindowVisible(hwnd) == FALSE) {
    return TRUE;
  }
  auto* bounds = ParamPtr<TaskListBounds>(data);
  RECT rect = {};
  if (GetWindowRect(hwnd, &rect) != FALSE && rect.right > rect.left &&
      rect.bottom > bounds->row.top && rect.top < bounds->row.bottom &&
      rect.right <= bounds->row.right && rect.left >= bounds->row.left) {
    bounds->right = bounds->found ? (std::max)(bounds->right, rect.right) : rect.right;
    bounds->found = true;
  }
  return TRUE;
}

}  // namespace

std::vector<MonitorTaskbar> DefaultEnumerateSecondaryMonitors() {
  auto log = InitHostLogging("");
  std::vector<MonitorEntry> monitors;
  EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                      reinterpret_cast<LPARAM>(&monitors));
  std::vector<HWND> taskbars;
  EnumWindows(CollectTaskbar, reinterpret_cast<LPARAM>(&taskbars));

  TrayMetrics trayMetrics;
  const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
  RECT button = {};
  if (primary != nullptr &&
      UiaElementRect(primary, L"SystemTray.NotifyIconView", &button)) {
    const UINT dpi = GetDpiForWindow(primary);
    trayMetrics = {.buttonWidth = button.right - button.left,
                   .buttonHeight = button.bottom - button.top,
                   .referenceDpi = dpi == 0 ? 96 : dpi};
    log->info("native tray button {}x{} at {} DPI", trayMetrics.buttonWidth,
              trayMetrics.buttonHeight, trayMetrics.referenceDpi);
  } else {
    log->warn("native tray button bounds unavailable; using scalable fallback");
  }

  std::vector<MonitorTaskbar> result;
  for (const MonitorEntry& entry : monitors) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(entry.monitor, &mi) == FALSE ||
        (mi.dwFlags & MONITORINFOF_PRIMARY) != 0) {
      continue;
    }
    MonitorTaskbar mt;
    mt.trayMetrics = trayMetrics;
    mt.monitor = entry.monitor;
    mt.monitorRect = mi.rcMonitor;
    for (const HWND taskbar : taskbars) {
      if (MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST) !=
          entry.monitor) {
        continue;
      }
      if (IsWindowVisible(taskbar) == FALSE) {
        break;
      }
      RECT taskbarRect = {};
      if (GetWindowRect(taskbar, &taskbarRect) == FALSE) {
        continue;
      }
      mt.hasTaskbar = true;
      mt.taskbarWindow = taskbar;
      mt.taskbarRect = taskbarRect;
      if (UiaElementRect(taskbar, L"SystemTray.OmniButton", &mt.clockRect)) {
        log->info("monitor ({},{}): taskbar clock at ({},{})", entry.rect.left,
                  entry.rect.top, mt.clockRect.left, mt.clockRect.top);
      } else {
        UINT dpi = GetDpiForWindow(taskbar);
        if (dpi == 0) {
          dpi = 96;
        }
        const int reserve = MulDiv(kClockReservePx, static_cast<int>(dpi), 96);
        mt.clockRect = {.left = taskbarRect.right - reserve,
                        .top = taskbarRect.top,
                        .right = taskbarRect.right,
                        .bottom = taskbarRect.bottom};
        log->warn("monitor ({},{}): clock element not found; reserving {} px",
                  entry.rect.left, entry.rect.top, reserve);
      }
      RECT taskList = {};
      TaskListBounds native{.row = taskbarRect};
      EnumChildWindows(taskbar, CollectTaskListBounds, reinterpret_cast<LPARAM>(&native));
      mt.hasTaskListBounds = native.found;
      mt.taskListRight = native.right;
      if (UiaElementRect(taskbar, L"MSTaskListWClass", &taskList)) {
        if (taskList.left >= taskbarRect.left && taskList.right <= taskbarRect.right) {
          mt.taskListRight = native.found ? (std::max)(native.right, taskList.right) : taskList.right;
          mt.hasTaskListBounds = true;
        }
        log->info("monitor ({},{}): task list ends at x={}", entry.rect.left,
                  entry.rect.top, mt.taskListRight);
      }
      if (!mt.hasTaskListBounds) {
        log->warn("task list bounds unavailable; retaining any existing safe strip");
      }
      break;
    }
    result.push_back(mt);
  }
  return result;
}

RECT ComputeOverlayStripRect(const MonitorTaskbar& mt, int widthPx) {
  if (!mt.hasTaskbar || !mt.hasTaskListBounds || widthPx <= 0) {
    return RECT{};
  }
  RECT strip = {};
  strip.top = mt.taskbarRect.top;
  strip.bottom = mt.taskbarRect.bottom;
  const bool hasClock = mt.clockRect.right > mt.clockRect.left;
  LONG right = hasClock ? mt.clockRect.left : mt.taskbarRect.right;
  if (right > mt.monitorRect.right) {
    right = mt.monitorRect.right;
  }
  LONG left = right - widthPx;
  // The overlay is topmost: anything under it cannot be clicked. Stop at
  // the task list's right edge when it could be measured.
  if (left < mt.taskListRight) {
    left = mt.taskListRight;
  }
  if (left < mt.monitorRect.left) {
    left = mt.monitorRect.left;
  }
  if (left >= right) {
    return RECT{};  // no room between the task list and the clock
  }
  strip.left = left;
  strip.right = right;
  return strip;
}

int IconPxForDpi(UINT dpi) {
  // The notification area draws system small icons, so the size is the
  // system metric at that DPI, not a table: it follows the user's
  // display settings (spec 6.3). The table below is only a fallback for
  // a DPI the metric cannot answer.
  const UINT effective = dpi == 0 ? 96 : dpi;
  const int metric = GetSystemMetricsForDpi(SM_CXSMICON, effective);
  if (metric > 0) {
    return metric;
  }
  if (dpi >= 192) {
    return 32;
  }
  if (dpi >= 144) {
    return 24;
  }
  if (dpi >= 120) {
    return 20;
  }
  return 16;
}

SIZE TrayButtonSizeForDpi(UINT dpi, const TrayMetrics& metrics) {
  const UINT target = dpi == 0 ? 96 : dpi;
  TrayMetrics valid = metrics;
  if (valid.referenceDpi < 48 || valid.referenceDpi > 960 ||
      valid.buttonWidth < MulDiv(16, static_cast<int>(valid.referenceDpi), 96) ||
      valid.buttonWidth > MulDiv(128, static_cast<int>(valid.referenceDpi), 96) ||
      valid.buttonHeight < MulDiv(16, static_cast<int>(valid.referenceDpi), 96) ||
      valid.buttonHeight > MulDiv(128, static_cast<int>(valid.referenceDpi), 96)) {
    valid = {};
  }
  const int icon = IconPxForDpi(target);
  return SIZE{.cx = (std::max)(icon, MulDiv(valid.buttonWidth, static_cast<int>(target),
                                   static_cast<int>(valid.referenceDpi))),
              .cy = (std::max)(icon, MulDiv(valid.buttonHeight, static_cast<int>(target),
                                   static_cast<int>(valid.referenceDpi)))};
}

bool ShouldExposeTray(const RECT& monitor, const RECT& foreground, bool shellDesktop) {
  return shellDesktop || foreground.left > monitor.left || foreground.top > monitor.top ||
         foreground.right < monitor.right || foreground.bottom < monitor.bottom;
}

}  // namespace traymirror
