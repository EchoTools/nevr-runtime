#pragma once

// #553: the game logs `The D3D device has been removed with reason 'DXGI_ERROR_DEVICE_HUNG'` and crashes when
// the GPU stops answering. Players read that as a nevr-runtime bug. Meta documents the cause (recent NVIDIA
// drivers freeze Meta Horizon Link) and keeps a driver page current, so the runtime does not track driver
// versions: it recognises the crash and points the player there.
//
// Pure policy, no I/O: builtin_filter.cpp feeds every game line to Observe and performs the effects the
// result names (the log line and crash-record line always; the dialog only when Action says so).
//
//   * only the HUNG reason matches: DEVICE_REMOVED / DEVICE_RESET / INVALID_CALL are different problems;
//   * once per run: the first match is handled, later ones return Action::None;
//   * never a dialog on a dedicated server or any headless run: nobody is at the screen and a modal box
//     would hold the process. Those get the log lines only.

#include <string>
#include <string_view>

namespace nevr_gpu_hung {

constexpr std::string_view kMetaDriverPage = "https://www.meta.com/help/quest/294035639100350/";
constexpr std::string_view kIssue = "https://github.com/EchoTools/nevr-runtime/issues/553";

inline bool IsDeviceHungLine(std::string_view line) {
  return line.find("The D3D device has been removed") != std::string_view::npos &&
         line.find("DXGI_ERROR_DEVICE_HUNG") != std::string_view::npos;
}

// The value after `label` on a game System Info line (`Video Card: NVIDIA ...`, `Driver Version: r610_85 (61088)`),
// trimmed; empty when the line does not carry the label.
inline std::string ValueAfterLabel(std::string_view line, std::string_view label) {
  const size_t at = line.find(label);
  if (at == std::string_view::npos) return std::string();
  size_t begin = at + label.size();
  while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
  size_t end = line.size();
  while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\r' || line[end - 1] == '\n' ||
                         line[end - 1] == '\t')) {
    --end;
  }
  return std::string(line.substr(begin, end - begin));
}

enum class Action {
  None,         // not the hung line, or already handled this run
  LogOnly,      // server / headless: the record, no dialog
  LogAndNotify  // a player at the screen: the record and the dialog
};

struct Result {
  Action action = Action::None;
  std::string log_line;  // for the nevr log and the crash record
  std::string dialog_text;
};

class Tracker {
 public:
  // Feed every game line. `isServer` and `isHeadless` are the runtime's mode flags at the moment of the line.
  Result Observe(std::string_view line, bool isServer, bool isHeadless) {
    // The System Info dump names the card and driver; remember them for the line below.
    const std::string card = ValueAfterLabel(line, "Video Card:");
    if (!card.empty()) video_card_ = card;
    const std::string driver = ValueAfterLabel(line, "Driver Version:");
    if (!driver.empty()) driver_version_ = driver;

    Result result;
    if (handled_ || !IsDeviceHungLine(line)) return result;
    handled_ = true;

    result.action = (isServer || isHeadless) ? Action::LogOnly : Action::LogAndNotify;
    result.log_line = std::string("[NEVR.GPU] the game's graphics device stopped responding (DXGI_ERROR_DEVICE_HUNG)") +
                      " gpu=\"" + (video_card_.empty() ? "unknown" : video_card_) + "\"" +
                      " driver=\"" + (driver_version_.empty() ? "unknown" : driver_version_) + "\"" +
                      "; this is a known GPU driver problem with Meta Horizon Link, update or roll back the "
                      "driver: " + std::string(kMetaDriverPage) + " (nevr-runtime #553)";
    result.dialog_text =
        std::string("Echo VR's graphics card stopped responding (DXGI_ERROR_DEVICE_HUNG), so the game is closing.\n\n") +
        "This is a known problem between recent NVIDIA graphics drivers and Meta Horizon Link, not a fault in your "
        "headset or in nevr-runtime. Update or reinstall your graphics driver, or roll back to an earlier one.\n\n" +
        "Meta's guidance:\n" + std::string(kMetaDriverPage) + "\n\n" + "More: " + std::string(kIssue);
    return result;
  }

  bool handled() const { return handled_; }

 private:
  bool handled_ = false;
  std::string video_card_;
  std::string driver_version_;
};

}  // namespace nevr_gpu_hung
