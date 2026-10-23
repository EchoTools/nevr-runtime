#pragma once

// ADR 0010 section 6 (#741): when the signature gate refuses a plugin, a player is told which file, once. The
// policy is the one #553 set for the graphics-driver dialog (gpu_hung_notice.h): pure, no I/O; the loader writes
// the log line and the manifest entry first and performs the dialog this returns.
//
//   * one dialog per run, however many files were refused, and none after it;
//   * never on a dedicated server or any headless run: nobody is at the screen and a modal box would hold the
//     process; those get the log line and the manifest entry only;
//   * the text names each refused file; the exact reason is in the log line, not the dialog.

#include <string>
#include <vector>

namespace nevr_plugin_refusal {

// The player's text for one file. The brand is spelled as it is in prose (docs/standards/naming.md).
inline std::string TextFor(const std::string& file) {
  return file + " wasn't loaded because it isn't signed by nEVR. Plugins from the launcher are signed; ask the "
                "plugin's author for a signed release.";
}

class Tracker {
 public:
  // A file the gate refused (a plugin, or a DLL it depends on). Each name is kept once.
  void Record(const std::string& file) {
    for (const std::string& f : files_) {
      if (f == file) return;
    }
    files_.push_back(file);
  }

  // The dialog text when one is due: files were refused, nothing was shown yet, and a player is at the screen.
  // Empty otherwise. A server or headless run never shows one and never will (it is not marked shown, so a
  // later call from a client state cannot happen in the same process).
  std::string TakeDialog(bool isServer, bool isHeadless) {
    if (shown_ || files_.empty() || isServer || isHeadless) return std::string();
    shown_ = true;
    if (files_.size() == 1) return TextFor(files_[0]);
    std::string text;
    for (const std::string& f : files_) text += f + " wasn't loaded because it isn't signed by nEVR.\n";
    return text + "Plugins from the launcher are signed; ask each plugin's author for a signed release.";
  }

  size_t refused() const { return files_.size(); }
  bool shown() const { return shown_; }

 private:
  std::vector<std::string> files_;
  bool shown_ = false;
};

}  // namespace nevr_plugin_refusal
