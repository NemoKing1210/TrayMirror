#include "core/ModelInbox.h"

namespace traymirror {

ModelInbox::ModelInbox(HWND nudge, UINT message)
    : nudge_(nudge), message_(message) {}

void ModelInbox::SetNudge(HWND nudge, UINT message) {
  nudge_ = nudge;
  message_ = message;
}

void ModelInbox::Push(const TrayMessage& msg) {
  {
    const std::scoped_lock lock(mutex_);
    queue_.push_back(msg);
  }
  if (nudge_ != nullptr) {
    PostMessageW(nudge_, message_, 0, 0);
  }
}

std::vector<TrayMessage> ModelInbox::Drain() {
  std::vector<TrayMessage> out;
  const std::scoped_lock lock(mutex_);
  out.swap(queue_);
  return out;
}

}  // namespace traymirror
