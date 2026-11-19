#include "render/OverlayThread.h"

#include "Log.h"
#include "render/Overlay.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <bit>
#include <cwchar>
#include <iterator>
#include <utility>

namespace traymirror {
namespace {

constexpr wchar_t kUiClass[] = L"TrayMirrorUiWnd";

// Overlay thread-local wake-ups (not part of the public protocol).
constexpr UINT kMsgRebuild = WM_APP + 2;
constexpr UINT kMsgRaise = WM_APP + 5;
constexpr UINT kMsgPruneOwner = WM_APP + 6;
constexpr UINT kMsgGeometryReady = WM_APP + 7;
thread_local HWND g_geometryWindow = nullptr;
thread_local bool g_geometryQueued = false;
thread_local bool g_raiseQueued = false;

void CALLBACK TaskbarEvent(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
                           LONG object, LONG child, DWORD /*thread*/, DWORD /*time*/) {
  if (g_geometryWindow == nullptr || hwnd == nullptr) {
    return;
  }
  if (event == EVENT_SYSTEM_FOREGROUND) {
    if (!g_raiseQueued) {
      g_raiseQueued = PostMessageW(g_geometryWindow, kMsgRaise, 0, 0) != FALSE;
    }
    if (!g_geometryQueued) {
      g_geometryQueued = PostMessageW(g_geometryWindow, kMsgRebuild, 0, 0) != FALSE;
    }
    return;
  }
  (void)object;
  (void)child;
  const HWND root = GetAncestor(hwnd, GA_ROOT);
  if (root == GetForegroundWindow() && !g_raiseQueued) {
    g_raiseQueued = PostMessageW(g_geometryWindow, kMsgRaise, 0, 0) != FALSE;
  }
  wchar_t name[64] = {};
  GetClassNameW(root, name, static_cast<int>(std::size(name)));
  if ((wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0) &&
      !g_geometryQueued) {
    g_geometryQueued = PostMessageW(g_geometryWindow, kMsgRebuild, 0, 0) != FALSE;
  }
}

UINT DpiForMonitor(HMONITOR monitor) {
  UINT dpiX = 96;
  UINT dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) != 0) {
    return 96;
  }
  return dpiX;
}

// RAII around the promotion store key: the read must not leak it when
// the model is rebuilt in between.
class ScopedRegKey {
 public:
  explicit ScopedRegKey(HKEY key) : key_(key) {}
  ~ScopedRegKey() {
    if (key_ != nullptr) {
      RegCloseKey(key_);
    }
  }
  ScopedRegKey(const ScopedRegKey&) = delete;
  ScopedRegKey& operator=(const ScopedRegKey&) = delete;
  [[nodiscard]] HKEY key() const noexcept { return key_; }

 private:
  HKEY key_;
};

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
      g_geometryQueued = false;
      RequestGeometry();
      return 0;
    case kMsgGeometryReady:
      ApplyGeometrySnapshot();
      return 0;
    case kMsgRaise:
      g_raiseQueued = false;
      for (const auto& overlay : overlays_) {
        overlay->RefreshVisibility();
      }
      return 0;
    case kMsgPruneOwner:
      if (model_ != nullptr && model_->RemoveIfOwnerGone(std::bit_cast<HWND>(wparam))) {
        PushModelToOverlays();
      }
      return 0;
    case kMsgPromotion:
      if (ApplyPromotionFromStore()) {
        PushModelToOverlays();
      }
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
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
                          MonitorEnumerator enumerate, DispatchSender sender,
                          PromotionWatcher::KeyOpener promotion) {
  if (thread_.joinable()) {
    return false;
  }
  inbox_ = inbox;
  model_ = model;
  enumerate_ = std::move(enumerate);
  sender_ = std::move(sender);
  promotion_ = std::move(promotion);
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
  g_geometryWindow = hwnd;
  g_geometryQueued = false;
  g_raiseQueued = false;
  geometryHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                  nullptr, TaskbarEvent, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                    nullptr, TaskbarEvent, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  visibilityHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
                                    nullptr, TaskbarEvent, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  dispatcher_ = std::make_unique<Dispatcher>(
      sender_ != nullptr ? std::move(sender_) : DefaultDispatchSender());
  if (promotion_ != nullptr) {
    watcher_ = std::make_unique<PromotionWatcher>(promotion_);
    if (!watcher_->Start(hwnd, kMsgPromotion)) {
      log->info("promotion store unavailable; every icon stays on the strip");
      watcher_.reset();
    }
  }
  // Dead owner windows (spec 4.4): an icon whose window is gone can
  // never be clicked again, so it leaves the model and the strip.
  deadIcons_ = std::make_unique<DeadIconWatcher>([this](HWND owner) {
    PostMessageW(uiHwnd_.load(), kMsgPruneOwner, reinterpret_cast<WPARAM>(owner), 0);
  });
  if (!deadIcons_->Start()) {
    log->warn("dead-icon watcher unavailable: stale icons stay on the strip");
    deadIcons_.reset();
  }
  BuildOverlays();
  // Once the startup snapshot exists, every later geometry refresh runs
  // on this worker: UIA can take hundreds of milliseconds and the UI
  // thread must keep rendering and dispatching input meanwhile.
  geometryWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  geometryStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (geometryWake_ != nullptr && geometryStop_ != nullptr) {
    geometryThread_ = std::thread(&OverlayThread::GeometryLoop, this);
  } else {
    log->warn("geometry worker unavailable; refreshes run on the UI thread");
  }
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
  g_geometryWindow = nullptr;
  if (geometryHook_ != nullptr) {
    UnhookWinEvent(geometryHook_);
    geometryHook_ = nullptr;
  }
  if (foregroundHook_ != nullptr) {
    UnhookWinEvent(foregroundHook_);
    foregroundHook_ = nullptr;
  }
  if (visibilityHook_ != nullptr) {
    UnhookWinEvent(visibilityHook_);
    visibilityHook_ = nullptr;
  }
  // The worker may be inside a UIA query; join it before the overlays it
  // reports about are destroyed.
  if (geometryStop_ != nullptr) {
    SetEvent(geometryStop_);
  }
  if (geometryThread_.joinable()) {
    geometryThread_.join();
  }
  for (HANDLE* handle : {&geometryWake_, &geometryStop_}) {
    if (*handle != nullptr) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  }
  overlays_.clear();
  // Stop the registry thread before the window it posts to disappears.
  watcher_.reset();
  deadIcons_.reset();
  dispatcher_.reset();
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
  if (!enumerate_) {
    return;
  }
  BuildOverlaysFrom(enumerate_());
}

void OverlayThread::BuildOverlaysFrom(
    const std::vector<MonitorTaskbar>& snapshot) {
  stripRoomExhausted_ = true;
  // Exact content width keeps the last button next to the native clock.
  const size_t slots = std::max<size_t>(1, CountStripSlots());
  builtSlots_ = slots;
  const HWND ui = uiHwnd_.load();
  std::vector<std::unique_ptr<Overlay>> updated;
  for (const MonitorTaskbar& mt : snapshot) {
    if (!mt.hasTaskbar) {
      continue;
    }
    const auto existing = std::ranges::find_if(overlays_, [&mt](const auto& overlay) {
      return overlay != nullptr && overlay->monitor() == mt.monitor &&
             GetWindow(overlay->hwnd(), GW_OWNER) == mt.taskbarWindow;
    });
    const UINT dpi = DpiForMonitor(mt.monitor);
    const int slot = TrayButtonSizeForDpi(dpi, mt.trayMetrics).cx;
    const int desired = slot * static_cast<int>(slots);
    const RECT strip = ComputeOverlayStripRect(mt, desired);
    if (strip.right <= strip.left) {
      if (existing != overlays_.end() && !mt.hasTaskListBounds) {
        (*existing)->SetGeometrySafe(false);
        updated.push_back(std::move(*existing));
      }
      continue;
    }
    if ((strip.right - strip.left) >= desired) {
      stripRoomExhausted_ = false;  // at least one monitor can still grow
    }
    if (existing != overlays_.end()) {
      if (!(*existing)->Reposition(strip, mt.trayMetrics)) {
        (*existing)->SetGeometrySafe(false);
        InitHostLogging("")->warn("overlay geometry update failed; hiding the unsafe strip");
      }
      updated.push_back(std::move(*existing));
      continue;
    }
    auto overlay = Overlay::Create(
        mt.monitor, strip,
        [ui]() {
          if (ui != nullptr) {
            PostMessageW(ui, kMsgRebuild, 0, 0);
          }
        },
        [this](const DispatchCall& call) { PostGesture(call); }, mt.trayMetrics, mt.taskbarWindow);
    if (overlay != nullptr) {
      updated.push_back(std::move(overlay));
    }
  }
  overlays_ = std::move(updated);
  PushModelToOverlays();
}

void OverlayThread::RequestGeometry() {
  if (!geometryThread_.joinable()) {
    BuildOverlays();  // no worker (no enumerator, or creation failed)
    return;
  }
  geometryPending_ = true;  // coalesces a burst into one more cycle
  SetEvent(geometryWake_);
}

void OverlayThread::ApplyGeometrySnapshot() {
  std::vector<MonitorTaskbar> snapshot;
  {
    std::scoped_lock lock(geometryMutex_);
    snapshot = geometrySnapshot_;
  }
  BuildOverlaysFrom(snapshot);
}

void OverlayThread::GeometryLoop() {
  const HANDLE waits[2] = {geometryStop_, geometryWake_};
  for (;;) {
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) !=
        WAIT_OBJECT_0 + 1) {
      break;  // stop requested (or the event failed: fail safe, no spin)
    }
    for (;;) {
      geometryPending_ = false;
      std::vector<MonitorTaskbar> snapshot;
      if (enumerate_) {
        snapshot = enumerate_();
      }
      {
        std::scoped_lock lock(geometryMutex_);
        geometrySnapshot_ = std::move(snapshot);
      }
      const HWND ui = uiHwnd_.load();
      if (ui != nullptr) {
        PostMessageW(ui, kMsgGeometryReady, 0, 0);
      }
      if (!geometryPending_.load()) {
        break;  // no notification arrived while this cycle ran
      }
    }
  }
}

void OverlayThread::ApplyInbox() {
  if (inbox_ == nullptr || model_ == nullptr) {
    return;
  }
  bool changed = false;
  bool placementChanged = false;
  for (const TrayMessage& msg : inbox_->Drain()) {
    placementChanged = placementChanged || msg.nim == 0 || msg.nim == 2;
    if (model_->Apply(msg)) {
      changed = true;
    }
  }
  // A new icon needs its registry entry read; a modify of an existing
  // one does not (only the watcher can change it). Reading the whole
  // store per record would re-enumerate the key on every tooltip update.
  if (placementChanged || model_->icons().size() != promotionIconCount_) {
    changed = ApplyPromotionFromStore() || changed;
  }
  if (changed) {
    PushModelToOverlays();
  }
}

bool OverlayThread::ApplyPromotionFromStore() {
  if (model_ == nullptr) {
    return false;
  }
  promotionIconCount_ = model_->icons().size();
  if (promotion_ == nullptr) {
    return false;  // no store wired (tests): every icon stays promoted
  }
  const ScopedRegKey settings(promotion_());
  if (settings.key() == nullptr) {
    return false;  // no store: every icon stays promoted (spec 4.5)
  }
  return ApplyPromotion(model_, ReadNotifyIconSettings(settings.key()));
}

void OverlayThread::PostGesture(const DispatchCall& call) {
  if (dispatcher_ == nullptr) {
    return;
  }
  // Lazy check (spec 4.4): the owner may have died since the last
  // message, and a gesture replayed to a destroyed window is lost.
  if (call.target.owner != nullptr && IsWindow(call.target.owner) == FALSE) {
    PostMessageW(uiHwnd_.load(), kMsgPruneOwner,
                   reinterpret_cast<WPARAM>(call.target.owner), 0);
    return;
  }
  dispatcher_->Post(call);
}

size_t OverlayThread::CountStripSlots() const {
  if (model_ == nullptr) {
    return 0;
  }
  size_t count = 0;
  bool overflow = false;
  for (const TrayIcon& icon : model_->icons()) {
    if (!icon.hidden() && icon.promoted) {
      ++count;
    } else if (!icon.hidden()) {
      overflow = true;
    }
  }
  return count + (overflow ? 1U : 0U);
}

void OverlayThread::PushModelToOverlays() {
  if (model_ == nullptr) {
    return;
  }
  std::vector<IconView> strip;
  std::vector<IconView> overflow;
  strip.reserve(model_->icons().size());
  overflow.reserve(model_->icons().size());
  std::vector<const TrayIcon*> ordered;
  for (const TrayIcon& icon : model_->icons()) {
    ordered.push_back(&icon);
  }
  std::ranges::stable_sort(ordered, {}, [](const TrayIcon* icon) { return icon->shellOrder; });
  for (const TrayIcon* entry : ordered) {
    const TrayIcon& icon = *entry;
    if (icon.hidden()) {
      continue;
    }
    IconView view{
        .icon = icon.iconCopy,
        .serial = icon.iconSerial,
        .tip = icon.tip,
        .target = DispatchTarget{.owner = icon.ownerHwnd,
                                 .callbackMessage = icon.callbackMessage,
                                 .uid = icon.key.uid,
                                 .version = icon.version,
                                 .ownerPid = icon.ownerPid}};
    // Stage 3 (spec 4.5/6.4): the registry promotion decides strip cell
    // versus overflow; the strip draws a chevron for whatever it hides.
    if (icon.promoted) {
      strip.push_back(std::move(view));
    } else {
      overflow.push_back(std::move(view));
    }
  }
  for (const std::unique_ptr<Overlay>& overlay : overlays_) {
    overlay->SetIcons(strip, overflow);
    overlay->Render();
  }
  // The model outgrew the strip: rebuild it wider so no icon is hidden.
  // Not reposted when the taskbar has no more room, so a saturated strip
  // cannot cause a rebuild loop.
  const size_t needed = std::max<size_t>(1, strip.size() + (overflow.empty() ? 0U : 1U));
  const HWND ui = uiHwnd_.load();
  if ((needed < builtSlots_ || (!stripRoomExhausted_ && needed > builtSlots_)) &&
      ui != nullptr) {
    PostMessageW(ui, kMsgRebuild, 0, 0);
  }
}

}  // namespace traymirror
