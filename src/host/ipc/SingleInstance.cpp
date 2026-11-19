#include "ipc/SingleInstance.h"

#include "Proto.h"

SingleInstance::~SingleInstance() {
  if (mutex_ != nullptr) {
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
  }
}

bool SingleInstance::Acquire() {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
  if (mutex == nullptr) {
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return false;
  }
  mutex_ = mutex;
  return true;
}
