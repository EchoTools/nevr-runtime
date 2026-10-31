#pragma once
// #966 S4: the frame path's view of the invite service, with no worker type and no exception path, so the Quest's hook-side
// translation unit can include it. Implemented in invite_service.cpp (the exception TU), on top of the service's answers.
#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_invite_match {

/// The longest match id the frame path takes. Longer ids are refused (taken as empty, so the join logs them as an invalid id).
inline constexpr std::size_t kMaxMatchIdBytes = 63;

/// True while the invite service is started (the network.invite_codes feature, nevr_invite_service::Active).
bool ServiceActive() noexcept;

/// Takes the next resolved match id the service answered with (a Resolve that succeeded and parsed). The text is copied to `out`
/// (not NUL-terminated) and its length set in `length`; an empty id is returned as length 0 and still counts as one answer.
/// False when no resolved answer is waiting. Called on the game thread once per frame.
bool TakeResolvedMatch(char* out, std::size_t capacity, std::size_t* length) noexcept;

/// True when TakeResolvedMatch would return a match: one is already taken from the service and queued, or an answer waits in the
/// service. Takes no service lock (an atomic read), so a frame with nothing waiting costs one load (#1415, as the PC applier).
bool ResolvedMatchPending() noexcept;

/// The longest text the UI text writer takes (nevr_ui_text::kMaxTextBytes).
inline constexpr std::size_t kMaxShareTextBytes = 63;

/// One text the service's answers ask the wizard's pages to show (patch/invite_answer_text.h: the code, the expiry and the link of
/// the share page, the title of the entry page), as plain bytes for the frame path: the actor and element it is written
/// into, the field's log name and the text (NUL-terminated, `length` bytes).
struct ShareText {
  std::uint64_t actor = 0;
  std::uint64_t element = 0;
  char field[16] = {};
  char text[kMaxShareTextBytes + 1] = {};
  std::size_t length = 0;
};

/// The log line for a text handed to the writer, and for one it did not take (docs/standards/logging.md, [NEVR.INVITE]): the field
/// and the size only, never the text, because a code is a bearer secret. The PC applier (invite_answer_text.h) and the Quest's
/// (quest/invite/join_game_quest.cpp) write these same lines.
inline std::string PostedText(const char* field, std::size_t bytes) {
  return std::string("[NEVR.INVITE] text posted field=") + field + " bytes=" + std::to_string(bytes);
}
inline std::string NotPostedText(const char* field, std::size_t bytes, const char* reason) {
  return std::string("[NEVR.INVITE] text not posted field=") + field + " bytes=" + std::to_string(bytes) + " reason=" + reason;
}

/// Takes the next text an answer asked for, oldest first. The answers reach this queue through TakeResolvedMatch (it files every
/// answer it takes: a resolved match as a match, a created code or a refusal as texts), so a frame calls TakeResolvedMatch until
/// it returns false and then this until it returns false. False when no text waits.
bool TakeShareText(ShareText* out) noexcept;

}  // namespace nevr_invite_match
