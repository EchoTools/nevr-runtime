#pragma once
// When the Windows sign-in window may be closed (#397). The window is created on its own thread and the
// caller waits a bounded time for it; the wait can end first, and the window then appears with nobody
// left to close it. This is the decision, pure, so a test can drive the orderings; the caller holds the
// lock that guards both events.

namespace nevr::auth {

class SignInDialogLifecycle {
 public:
  // The window now exists. True: nobody wants it any more, close it at once.
  bool OnCreated() {
    created_ = true;
    return abandoned_;
  }
  // The caller stopped waiting for the window (the wait timed out, or the flow is over). True: the window
  // does not exist yet and will have to close itself when it does; false: it exists, close it directly.
  bool Abandon() {
    abandoned_ = true;
    return !created_;
  }
  bool created() const { return created_; }
  bool abandoned() const { return abandoned_; }

 private:
  bool created_ = false;
  bool abandoned_ = false;
};

}  // namespace nevr::auth
