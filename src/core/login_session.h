#pragma once

// The server's login session GUID, captured from the game's login response (state_machine.cpp,
// NetGameSwitchStateHook) and read by the ServerDB registration. The writer is the game thread; a
// reader is also ixwebsocket's thread (the ServerDB reconnect handler re-registers), so a plain
// 16-byte GUID could be read half-written. Every access goes through this lock.

#include <windows.h>

#include <mutex>

namespace LoginSession {

inline std::mutex& Mutex() {
  static std::mutex mutex;
  return mutex;
}

inline GUID& Storage() {
  static GUID id = {};
  return id;
}

inline GUID Get() {
  std::lock_guard<std::mutex> lock(Mutex());
  return Storage();
}

inline void Set(const GUID& id) {
  std::lock_guard<std::mutex> lock(Mutex());
  Storage() = id;
}

}  // namespace LoginSession
