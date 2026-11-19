#include "core/InitialSync.h"

namespace traymirror {

BroadcastFn DefaultTaskbarCreatedBroadcast() {
  return []() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    if (message != 0) {
      SendNotifyMessageW(HWND_BROADCAST, message, 0, 0);
    }
  };
}

bool SignalResync() {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kResyncEventName);
  if (event == nullptr) {
    return false;
  }
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}

}  // namespace traymirror
