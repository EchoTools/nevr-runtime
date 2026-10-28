#pragma once

// When the host stops the game because nevr-runtime.dll did not load (RuntimeRendezvous::StopGame), a player at the
// screen is told once, in plain words, that the install is broken and what to do about it. The wording and the
// verification of the file are #854's; this is the minimum that keeps the stop from looking like a crash.
//
// The policy is pure (no I/O) and follows the one #741 set for the plugin refusal dialog (ext/plugin_refusal_notice.h):
//   * never on a dedicated server or any headless run: nobody is at the screen and a modal box would hold the
//     process; those get the game_stop record only, and so does a run whose command line cannot be read. The host runs before the runtime, so it reads the game's
//     command line itself: a token that names -server or -headless, with any leading dashes, and by the game's own
//     tokenizer (cmdline_strip.h) plus the quoted spelling boot.cpp's CommandLineToArgvW reading also takes;
//   * never under the loader lock (the dialog pumps messages);
//   * one dialog per run: the first thread into StopGame shows it, the others wait behind it.

#include "runtime/ext/plugin_signature.h"
#include "runtime/host/runtime_load.h"
#include "runtime/lifecycle/cmdline_strip.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace nevr_host {

/// What the player reads. The brand is spelled as it is in prose (docs/standards/naming.md). Each cause of a refusal
/// names the file and says what is wrong in plain words (docs/adr/0012-static-install-root.md, "The host verifies
/// nevr-runtime.dll before it loads it"); the log line carries the verifier's own words and the path searched.
inline constexpr wchar_t kStopNoticeText[] =
    L"nEVR couldn't start: nevr-runtime.dll is missing or damaged. Please reinstall with the installer.";
inline constexpr wchar_t kStopNoticeMissingText[] =
    L"nEVR couldn't start: nevr-runtime.dll is missing. Please run the installer.";
inline constexpr wchar_t kStopNoticeUnsignedText[] =
    L"nEVR couldn't start: nevr-runtime.dll is not signed by the nEVR release process. Please reinstall with the installer.";
inline constexpr wchar_t kStopNoticeChangedText[] =
    L"nEVR couldn't start: nevr-runtime.dll was changed after it was signed. Please reinstall with the installer.";
inline constexpr wchar_t kStopNoticeUnreadableText[] =
    L"nEVR couldn't start: nevr-runtime.dll could not be read. Please reinstall with the installer.";
inline constexpr wchar_t kStopNoticeRefusedText[] =
    L"nEVR couldn't start: the signature on nevr-runtime.dll is not accepted. Please reinstall with the installer.";
inline constexpr wchar_t kStopNoticeTitle[] = L"nEVR";

/// The words for a runtime that did not load: the outcome decides, and for a refusal the verifier's reason.
inline const wchar_t* StopNoticeTextFor(RuntimeLoadOutcome outcome, nevr_plugin_signature::Reason reason) {
  using nevr_plugin_signature::Reason;
  switch (outcome) {
    case RuntimeLoadOutcome::kRuntimeAbsent: return kStopNoticeMissingText;
    case RuntimeLoadOutcome::kRuntimeRefused:
      switch (reason) {
        case Reason::Unsigned: return kStopNoticeUnsignedText;
        case Reason::DigestMismatch:
        case Reason::SignatureInvalid: return kStopNoticeChangedText;
        case Reason::Unreadable: return kStopNoticeUnreadableText;
        default: return kStopNoticeRefusedText;
      }
    default: return kStopNoticeText;
  }
}

enum class StopNotice {
  kShow,                         ///< A player is at the screen: tell them.
  kSuppressedServerOrHeadless,   ///< -server or -headless: the record only.
  kSuppressedLoaderLock,         ///< The loader lock is held by this thread: the record only.
};

/// The word the game_stop record carries for a decision. The record is written before the box opens, so "shown"
/// says the host is about to show it.
inline const char* StopNoticeName(StopNotice decision) {
  switch (decision) {
    case StopNotice::kShow: return "shown";
    case StopNotice::kSuppressedServerOrHeadless: return "suppressed_server_or_headless";
    case StopNotice::kSuppressedLoaderLock: return "suppressed_loader_lock";
  }
  return "suppressed_server_or_headless";
}

namespace stop_notice_detail {

inline bool IsDash(char c) { return c == '-' || c == '+' || static_cast<unsigned char>(c) == 0x96; }

inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ' ') : c; }

/// A token that starts with a dash and, with its leading dashes taken off, spells `lowerName` in any case.
inline bool IsDashedName(std::string_view token, std::string_view lowerName) {
  if (token.empty() || !IsDash(token.front())) return false;
  std::size_t i = 0;
  while (i < token.size() && IsDash(token[i])) ++i;
  token.remove_prefix(i);
  if (token.size() != lowerName.size()) return false;
  for (std::size_t k = 0; k < token.size(); ++k) {
    if (Lower(token[k]) != lowerName[k]) return false;
  }
  return true;
}

}  // namespace stop_notice_detail

/// The decision for one stop. `commandLine` is the process's command line (ANSI). A null one is a command line that
/// could not be read, and a dialog that might hold a server is worse than none, so it is suppressed.
inline StopNotice DecideStopNotice(const char* commandLine, bool loaderLockHeld) {
  if (loaderLockHeld) return StopNotice::kSuppressedLoaderLock;
  if (commandLine == nullptr) return StopNotice::kSuppressedServerOrHeadless;
  for (const nevr_cmdline::Token& token : nevr_cmdline::Tokenize(commandLine)) {
    if (stop_notice_detail::IsDashedName(token.text, "server") ||
        stop_notice_detail::IsDashedName(token.text, "headless")) {
      return StopNotice::kSuppressedServerOrHeadless;
    }
  }
  return StopNotice::kShow;
}

}  // namespace nevr_host
