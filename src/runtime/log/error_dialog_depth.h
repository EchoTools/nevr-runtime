#pragma once

// How deep the current thread is inside the game's error dialog (echovr.exe 0x1400EA730), #2245. The hook
// (error_dialog_level.cpp) runs the original through Run(); the built-in filter asks Inside() for every line, and a
// line logged while it is true is recorded at error (core/log_level.h, Effective).

namespace nevr_error_dialog {

// A count, not a flag: the dialog does not call itself, but a count stays right if it ever does.
inline thread_local int t_depth = 0;

/// Marks the thread for its lifetime. A guard, so an exception that leaves the dialog unmarks the thread too.
class Scope {
 public:
  Scope() { ++t_depth; }
  ~Scope() { --t_depth; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

/// Runs `body` (the original dialog) with the thread marked as inside the dialog.
template <typename Body>
void Run(Body&& body) {
  const Scope scope;
  body();
}

/// The runtime's one longjmp (lifecycle/crash_recovery.cpp, N125) leaves the game loop from a vectored exception
/// handler, which runs no destructor (N70 item 3), so a Scope it jumps over stays counted. Its landing calls this.
inline void ResetAfterLongjmp() { t_depth = 0; }

/// True on a thread that is inside the game's error dialog.
inline bool Inside() { return t_depth > 0; }

}  // namespace nevr_error_dialog
