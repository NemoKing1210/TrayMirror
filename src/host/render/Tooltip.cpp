#include "render/Tooltip.h"

#include "Log.h"

namespace traymirror {

std::unique_ptr<Tooltip> Tooltip::Create(HWND owner) {
  if (owner == nullptr) {
    return nullptr;
  }
  std::unique_ptr<Tooltip> tip(new Tooltip());
  tip->hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, GetModuleHandleW(nullptr),
      nullptr);
  if (tip->hwnd_ == nullptr) {
    return nullptr;
  }
  tip->tool_.cbSize = sizeof(TOOLINFOW);
  tip->tool_.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
  tip->tool_.hwnd = owner;
  tip->tool_.uId = reinterpret_cast<UINT_PTR>(owner);
  // The control copies the text on TTM_ADDTOOL and on every update.
  tip->tool_.lpszText = const_cast<wchar_t*>(L"");
  tip->registered_ =
      SendMessageW(tip->hwnd_, TTM_ADDTOOLW, 0,
                   reinterpret_cast<LPARAM>(&tip->tool_)) != FALSE;
  if (!tip->registered_) {
    InitHostLogging("")->warn("tooltip TTM_ADDTOOL failed ({})", GetLastError());
  }
  return tip;
}

Tooltip::~Tooltip() {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void Tooltip::Show(const std::wstring& text, POINT anchor) {
  if (hwnd_ == nullptr) {
    return;
  }
  if (text_ != text) {
    // Re-register rather than TTM_UPDATETIPTEXT: the update path leaves
    // the tool's text empty on this system, while a fresh AddTool with
    // the string is applied (measured; see the tooltip smoke test).
    if (registered_) {
      SendMessageW(hwnd_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool_));
      registered_ = false;
    }
    text_ = text;
    tool_.lpszText = text_.data();
    registered_ = SendMessageW(hwnd_, TTM_ADDTOOLW, 0,
                               reinterpret_cast<LPARAM>(&tool_)) != FALSE;
  }
  SendMessageW(hwnd_, TTM_TRACKPOSITION, 0, MAKELPARAM(anchor.x, anchor.y));
  SendMessageW(hwnd_, TTM_TRACKACTIVATE, TRUE,
               reinterpret_cast<LPARAM>(&tool_));
}

void Tooltip::Hide() {
  if (hwnd_ == nullptr) {
    return;
  }
  SendMessageW(hwnd_, TTM_TRACKACTIVATE, FALSE,
               reinterpret_cast<LPARAM>(&tool_));
}

}  // namespace traymirror
