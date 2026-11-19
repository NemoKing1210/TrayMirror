// A non-owning view of one model icon as the overlay renders it, and the
// sink every gesture on it goes to. Shared by the strip and the flyout:
// a click on a hidden icon is the same call as a click on a strip icon.
#pragma once

#include "dispatch/Dispatch.h"

#include <windows.h>

#include <functional>
#include <string>

namespace traymirror {

// The icon handle belongs to the model and outlives the view.
struct IconView {
  HICON icon = nullptr;
  UINT serial = 0;
  std::wstring tip;
  DispatchTarget target;  // where clicks on this icon are replayed
};

// Where a click on a mirrored icon goes; the overlay never contacts the
// owner itself (spec 4.4).
using DispatchClick = std::function<void(const DispatchCall&)>;

}  // namespace traymirror
