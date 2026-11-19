// Single instance guard (spec 7.4): a named session mutex.
#pragma once

#include <windows.h>

class SingleInstance {
 public:
  SingleInstance() = default;
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // False: another host is already running.
  bool Acquire();

 private:
  HANDLE mutex_ = nullptr;
};
