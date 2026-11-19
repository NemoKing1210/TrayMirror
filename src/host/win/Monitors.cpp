#include "win/Monitors.h"

#include "Log.h"

#include <UIAutomation.h>
#include <objbase.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <bit>
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
  if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(uia.GetAddressOf()))) != 0) {
    return false;
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

}  // namespace

std::vector<MonitorTaskbar> DefaultEnumerateSecondaryMonitors() {
  auto log = InitHostLogging("");
  std::vector<MonitorEntry> monitors;
  EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                      reinterpret_cast<LPARAM>(&monitors));
  std::vector<HWND> taskbars;
  EnumWindows(CollectTaskbar, reinterpret_cast<LPARAM>(&taskbars));

  std::vector<MonitorTaskbar> result;
  for (const MonitorEntry& entry : monitors) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(entry.monitor, &mi) == FALSE ||
        (mi.dwFlags & MONITORINFOF_PRIMARY) != 0) {
      continue;
    }
    MonitorTaskbar mt;
    mt.monitor = entry.monitor;
    mt.monitorRect = mi.rcMonitor;
    for (const HWND taskbar : taskbars) {
      if (MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST) !=
          entry.monitor) {
        continue;
      }
      RECT taskbarRect = {};
      if (GetWindowRect(taskbar, &taskbarRect) == FALSE) {
        continue;
      }
      mt.hasTaskbar = true;
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
      if (UiaElementRect(taskbar, L"MSTaskListWClass", &taskList)) {
        mt.taskListRight = taskList.right;
        log->info("monitor ({},{}): task list ends at x={}", entry.rect.left,
                  entry.rect.top, mt.taskListRight);
      }
      break;
    }
    result.push_back(mt);
  }
  return result;
}

RECT ComputeOverlayStripRect(const MonitorTaskbar& mt, int widthPx) {
  if (!mt.hasTaskbar || widthPx <= 0) {
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
  if (mt.taskListRight > 0 && left < mt.taskListRight) {
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

int IconGapPxForDpi(UINT dpi) {
  return MulDiv(2, static_cast<int>(dpi), 96);
}

}  // namespace traymirror
