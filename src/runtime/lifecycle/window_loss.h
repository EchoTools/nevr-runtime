#pragma once

// Notices that the game's main window is gone. Pure, so the decision is unit-tested without a
// window system.
//
// The game quits when its window gets WM_CLOSE (title-bar close). A window destroyed without a
// close request (XDestroyWindow, a window manager that is gone) never reaches that path: the
// process keeps running and logging with no window (#341). The runtime treats the loss of a window
// it was tracking as a quit.
//
// A window that was never found is not a loss: a client with no window yet must not exit.

namespace WindowLoss {

enum class State { kUntracked, kAlive, kLost };

class Watch {
 public:
  // `find` returns the main window handle, or nullptr when there is none yet; it is asked only
  // while no window is tracked. `isWindow(handle)` says whether the tracked window still exists.
  // kLost is sticky.
  template <typename Handle, typename Find, typename IsWindow>
  State Poll(Find&& find, IsWindow&& isWindow) {
    if (lost_) return State::kLost;
    if (!tracked_) {
      const Handle found = find();
      if (found == Handle{}) return State::kUntracked;
      tracked_ = true;
      handleBits_ = reinterpret_cast<unsigned long long>(found);
    }
    if (isWindow(reinterpret_cast<Handle>(handleBits_))) return State::kAlive;
    lost_ = true;
    return State::kLost;
  }

 private:
  bool tracked_ = false;
  bool lost_ = false;
  unsigned long long handleBits_ = 0;
};

}  // namespace WindowLoss
