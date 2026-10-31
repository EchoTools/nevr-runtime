#pragma once
// #896 (the first accessor of the UI seam), #906, docs/adr/0017: write a text into the game's UI.
//
// A request names an actor of the menu gamespace, one of the canvas text elements of that actor, and the text. The game
// keeps its UI as actors with components: the canvas component of an actor holds named text elements, and the game's own
// setter writes one. This header is the pure part: what may be written, how an actor is found in the gamespaces, the one
// write expressed against a platform, and the list of requests that wait for the game to be ready. ui_text.cpp is the
// platform (the game's functions) and the glue to the per-frame dispatcher; src/runtime/tests/test_ui_text.cpp pins all
// of this on the host with a fake game.
//
// How #896 reuses it: an accessor declaration is a name, typed arguments and read or write. This one is
// `ui.text.set(actor: symbol, element: symbol, text: string)`, a write; a plugin or a script posts a Request through
// `nevr_ui_text::Post`, never touches a game address, and the runtime owns every offset below and checks the build.
//
// A text is never logged, only its size: the invite code goes through here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace nevr_ui_text {

// ---- what may be written ----

/// The game's text element is a 64-byte buffer including the terminator (the modkit author's figure, not verified here: the
/// cap keeps a text inside it).
inline constexpr size_t kMaxTextBytes = 63;
inline constexpr size_t kMaxPending = 16;
inline constexpr uint64_t kRetryIntervalMs = 100;
inline constexpr uint64_t kGiveUpMs = 30000;
inline constexpr size_t kMaxGameSpaces = 64;

/// A request names its actor and element by CSymbol64.
struct Target {
  uint64_t actor = 0;
  uint64_t element = 0;
  bool operator==(const Target& o) const { return actor == o.actor && element == o.element; }
};

struct Request {
  Target target;
  std::string text;
};

enum class Check { Ok, NoTarget, TooLong, NotPrintable, Full };

inline const char* CheckName(Check c) {
  switch (c) {
    case Check::Ok: return "ok";
    case Check::NoTarget: return "no_target";
    case Check::TooLong: return "too_long";
    case Check::NotPrintable: return "not_printable";
    case Check::Full: return "full";
  }
  return "unknown";
}

/// Printable ASCII only, at most kMaxTextBytes; an empty text clears the element. The game's font draws no more.
inline Check CheckRequest(const Request& request) {
  if (request.target.actor == 0 || request.target.element == 0) return Check::NoTarget;
  if (request.text.size() > kMaxTextBytes) return Check::TooLong;
  for (const char c : request.text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7e) return Check::NotPrintable;
  }
  return Check::Ok;
}

// ---- finding the actor ----

// The gamespace's actor table (ReVault echovr.exe 0x14050fb60 and the modkit author's findings, confirmed by a live run
// in the PR that introduced patch/invite_choices.h): gs+0x370 -> table; table+0x80 -> an array of 8-byte actor names;
// table+0xa8 -> how many.
inline constexpr size_t kSpaceActorTableOffset = 0x370;
inline constexpr size_t kTableNamesOffset = 0x80;
inline constexpr size_t kTableCountOffset = 0xa8;
/// An actor's index is 16 bits and 0xffff means none, so a table of 0xffff or more is not one.
inline constexpr uint64_t kMaxActors = 0xfffe;
/// The packed identity the component lookup takes for the actor at `index` (as the game's script nodes build it).
inline constexpr uint64_t kActorIdentBase = 0x0000ffffffff0000ULL;

/// The index of `actor` in the names array of the gamespace `gs`; false when not there or when memory cannot be read.
/// `p.Read(address, out, length)` is a reader that can refuse.
template <typename Platform>
inline bool FindActorIndex(Platform& p, uintptr_t gs, uint64_t actor, uint64_t* index) {
  uint64_t table = 0, names = 0, count = 0;
  if (!p.Read(gs + kSpaceActorTableOffset, &table, sizeof(table)) || table == 0) return false;
  if (!p.Read(static_cast<uintptr_t>(table) + kTableNamesOffset, &names, sizeof(names)) || names == 0) return false;
  if (!p.Read(static_cast<uintptr_t>(table) + kTableCountOffset, &count, sizeof(count))) return false;
  if (count == 0 || count > kMaxActors) return false;
  std::vector<uint64_t> all(static_cast<size_t>(count));
  if (!p.Read(static_cast<uintptr_t>(names), all.data(), all.size() * sizeof(uint64_t))) return false;
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i] == actor) {
      *index = i;
      return true;
    }
  }
  return false;
}

// ---- one write ----

enum class Outcome { Written, NoGame, NoActor, NoCanvas, NotWritten, Refused };

inline const char* OutcomeName(Outcome o) {
  switch (o) {
    case Outcome::Written: return "written";
    case Outcome::NoGame: return "no_game";
    case Outcome::NoActor: return "no_actor";
    case Outcome::NoCanvas: return "no_canvas";
    case Outcome::NotWritten: return "not_written";
    case Outcome::Refused: return "refused";
  }
  return "unknown";
}

/// Whether a platform looks a canvas up by the actor's name (the Quest's CComponentHandle<CCanvasUICS>::Find, which finds
/// the actor in the gamespace or the ones below it): `bool FindCanvasByName(uintptr_t gs, uint64_t actor, uintptr_t* cs,
/// uint32_t* handle)`, true when the actor has a canvas.
template <typename P, typename = void>
struct FindsCanvasByName : std::false_type {};
template <typename P>
struct FindsCanvasByName<P, std::void_t<decltype(std::declval<P&>().FindCanvasByName(
                                uintptr_t{}, uint64_t{}, static_cast<uintptr_t*>(nullptr), static_cast<uint32_t*>(nullptr)))>>
    : std::true_type {};

/// Writes `request` through the platform `p`, which provides:
///   uintptr_t Game();                                          the game object, 0 before there is one
///   uintptr_t GameSpace(uintptr_t game, uint64_t index);       the index-th gamespace, 0 past the last
///   bool Read(uintptr_t address, void* out, size_t length);    a read that can refuse
///   bool FindCanvas(uintptr_t gs, uint64_t ident, uintptr_t* cs, uint32_t* handle);   the actor's canvas component
///   bool SetText(uintptr_t cs, uint32_t handle, uint64_t element, const char* text);  true when the element exists
/// or, in place of Read and FindCanvas, FindCanvasByName (above). Game thread only: the canvas component is the UI's own
/// state.
template <typename Platform>
inline Outcome Apply(Platform& p, const Request& request) {
  if (CheckRequest(request) != Check::Ok) return Outcome::Refused;
  const uintptr_t game = p.Game();
  if (game == 0) return Outcome::NoGame;
  for (size_t i = 0; i < kMaxGameSpaces; ++i) {
    const uintptr_t gs = p.GameSpace(game, i);
    if (gs == 0) break;
    if constexpr (FindsCanvasByName<Platform>::value) {
      uintptr_t cs = 0;
      uint32_t handle = 0;
      if (!p.FindCanvasByName(gs, request.target.actor, &cs, &handle) || cs == 0) continue;
      return p.SetText(cs, handle, request.target.element, request.text.c_str()) ? Outcome::Written
                                                                                : Outcome::NotWritten;
    } else {
      uint64_t index = 0;
      if (!FindActorIndex(p, gs, request.target.actor, &index)) continue;
      uintptr_t cs = 0;
      uint32_t handle = 0;
      if (!p.FindCanvas(gs, kActorIdentBase | index, &cs, &handle) || cs == 0) return Outcome::NoCanvas;
      return p.SetText(cs, handle, request.target.element, request.text.c_str()) ? Outcome::Written
                                                                                : Outcome::NotWritten;
    }
  }
  return Outcome::NoActor;
}

// ---- the requests that wait ----

/// What one drain did with one request. `done` means it leaves the list: written, or dropped after kGiveUpMs.
struct Report {
  Request request;
  Outcome outcome = Outcome::Written;
  int attempts = 0;
  bool done = false;
  bool dropped = false;
};

inline std::string Hex64(uint64_t v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "0x0000000000000000";
  for (int i = 17; i >= 2; --i) {
    out[static_cast<size_t>(i)] = kHex[v & 0xfu];
    v >>= 4;
  }
  return out;
}

/// The log line for a report that is done: never the text, only its size (docs/standards/logging.md, `[NEVR.UI]`).
inline std::string ReportLine(const Report& r) {
  std::string line = std::string("[NEVR.UI] text ") + (r.dropped ? "dropped" : "written") +
                     " actor=" + Hex64(r.request.target.actor) + " element=" + Hex64(r.request.target.element) +
                     " bytes=" + std::to_string(r.request.text.size()) + " attempts=" + std::to_string(r.attempts);
  if (r.dropped) line += std::string(" reason=") + OutcomeName(r.outcome);
  return line;
}

/// Requests posted from any thread and written on the game thread. A request the game is not ready for (the page's canvas
/// does not exist yet) is tried again every kRetryIntervalMs and dropped after kGiveUpMs. One request per target: a newer
/// text replaces an older one, also when the older is in the game at that moment.
class Pending {
 public:
  Check Post(Request request, uint64_t nowMs) {
    const Check check = CheckRequest(request);
    if (check != Check::Ok) return check;
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& e : entries_) {
      if (e.request.target == request.target) {
        e = Entry{std::move(request), nowMs, nowMs, 0};
        return Check::Ok;
      }
    }
    if (entries_.size() >= kMaxPending) return Check::Full;
    entries_.push_back(Entry{std::move(request), nowMs, nowMs, 0});
    return Check::Ok;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  /// Tries every request that is due. Game thread. Nothing is locked while the game runs.
  template <typename Platform>
  std::vector<Report> Drain(Platform& p, uint64_t nowMs) {
    std::vector<Entry> due;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (entries_.empty()) return {};
      std::vector<Entry> keep;
      for (Entry& e : entries_) (e.nextTryMs <= nowMs ? due : keep).push_back(std::move(e));
      entries_ = std::move(keep);
    }
    std::vector<Report> reports;
    std::vector<Entry> retry;
    for (Entry& e : due) {
      ++e.attempts;
      Report r{e.request, Apply(p, e.request), e.attempts, false, false};
      if (r.outcome == Outcome::Written) {
        r.done = true;
      } else if (nowMs - e.postedMs >= kGiveUpMs) {
        r.done = true;
        r.dropped = true;
      } else {
        e.nextTryMs = nowMs + kRetryIntervalMs;
        retry.push_back(std::move(e));
      }
      reports.push_back(std::move(r));
    }
    if (!retry.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (Entry& e : retry) {
        bool replaced = false;
        for (const Entry& now : entries_) replaced = replaced || now.request.target == e.request.target;
        if (!replaced) entries_.push_back(std::move(e));  // a newer post for the target wins
      }
    }
    return reports;
  }

 private:
  struct Entry {
    Request request;
    uint64_t postedMs = 0;
    uint64_t nextTryMs = 0;
    int attempts = 0;
  };
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

}  // namespace nevr_ui_text
