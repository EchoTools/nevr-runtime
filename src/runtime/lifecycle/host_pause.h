#pragma once
// What a dedicated server does between printing why it is stopping and stopping (#1532).
//
// A host who started the server from a console window reads the reason before the window closes: with an
// interactive console (stdin is a console) the server waits for a key. Under a service manager or a panel with
// no input there is nobody to press one, so it waits a short, fixed time and exits: never at once, so a restart
// loop does not hammer the community service, and never for ever, so it does not hang the loop.
//
// Platform neutral and pure: every side effect is an injected operation (crash_recovery.cpp supplies the
// console ones), so both branches run under test.

#include <functional>
#include <string>

namespace nevr_host_pause {

inline constexpr unsigned kNonInteractiveWaitMs = 2000;

struct Io {
  std::function<void(const std::string&)> write_line;  // the console (standard error)
  std::function<bool()> stdin_is_console;              // an interactive console is attached
  std::function<void()> wait_for_key;                  // returns after one key press
  std::function<void(unsigned)> wait_ms;               // a timed wait with nothing to wake it
};

// Prints `reason`, then holds: for a key press (interactive) or kNonInteractiveWaitMs (not). Returns which.
enum class Held { kForKey, kForTime };

inline Held HoldBeforeExit(const std::string& reason, const Io& io) {
  io.write_line("server stopped: " + reason);
  if (io.stdin_is_console()) {
    io.write_line("Press any key to close.");
    io.wait_for_key();
    return Held::kForKey;
  }
  io.wait_ms(kNonInteractiveWaitMs);
  return Held::kForTime;
}

}  // namespace nevr_host_pause
