#include "runtime/patch/menu_friends_pack.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/patch/friends_panel_platform.h"
#include "runtime/patch/invite_choices.h"
#include "runtime/patch/menu_friends_profile.h"
#include "runtime/patch/ui_text_writer.h"

namespace nevr_menu_friends_pack {
namespace {

using nevr_friends_platform::Level;
using nevr_friends_platform::Log;

// The panel actor: the main menu's SEASON INFO actor, which draws the panel canvas.
constexpr std::uint64_t kPanelActor = 0x9E0BF5FE92ABA570ULL;
constexpr std::uint32_t kRows = 5;
constexpr std::size_t kNameChars = 32;    // the canvas reserves 33 bytes per name
constexpr std::size_t kStatusChars = 16;  // and 17 per status
constexpr std::uint64_t kCheckIntervalMs = 1000;

constexpr std::array<std::uint64_t, kRows> kNameElements = {
    EchoVR::CSymbol64Hash("friend0_name"), EchoVR::CSymbol64Hash("friend1_name"), EchoVR::CSymbol64Hash("friend2_name"),
    EchoVR::CSymbol64Hash("friend3_name"), EchoVR::CSymbol64Hash("friend4_name")};
constexpr std::array<std::uint64_t, kRows> kStatusElements = {
    EchoVR::CSymbol64Hash("friend0_status"), EchoVR::CSymbol64Hash("friend1_status"),
    EchoVR::CSymbol64Hash("friend2_status"), EchoVR::CSymbol64Hash("friend3_status"),
    EchoVR::CSymbol64Hash("friend4_status")};
constexpr std::array<std::uint64_t, kRows> kMarkElements = {
    EchoVR::CSymbol64Hash("friend0_mark"), EchoVR::CSymbol64Hash("friend1_mark"), EchoVR::CSymbol64Hash("friend2_mark"),
    EchoVR::CSymbol64Hash("friend3_mark"), EchoVR::CSymbol64Hash("friend4_mark")};
constexpr std::uint64_t kInviteText = EchoVR::CSymbol64Hash("play_text");

constexpr std::array<std::uint64_t, kRows> kRowButtons = {
    EchoVR::CSymbol64Hash("friend0_btn"), EchoVR::CSymbol64Hash("friend1_btn"), EchoVR::CSymbol64Hash("friend2_btn"),
    EchoVR::CSymbol64Hash("friend3_btn"), EchoVR::CSymbol64Hash("friend4_btn")};
// The panel's page element id (CR15UIPage2ElementCR, set by the level edit) and its INVITE FRIENDS button.
constexpr std::uint64_t kPageElementId = EchoVR::CSymbol64Hash("friends_panel");
constexpr std::uint64_t kInviteButton = EchoVR::CSymbol64Hash("play_btn");
// Each row and the INVITE FRIENDS button is its own ui_mnu_button_pointermesh instance in the menu's level, with its own
// click event (the instance's event setting), so the event names the row.
constexpr std::array<std::uint64_t, kRows> kRowClickEvents = {
    EchoVR::CSymbol64Hash("evt_friend0_btn_clicked"), EchoVR::CSymbol64Hash("evt_friend1_btn_clicked"),
    EchoVR::CSymbol64Hash("evt_friend2_btn_clicked"), EchoVR::CSymbol64Hash("evt_friend3_btn_clicked"),
    EchoVR::CSymbol64Hash("evt_friend4_btn_clicked")};
constexpr std::uint64_t kInviteClickEvent = EchoVR::CSymbol64Hash("evt_friends_invite_btn_clicked");
constexpr std::size_t kEventWords = nevr_invite_choices::kEventBytes / sizeof(std::uint64_t);
constexpr std::size_t kMaxLoggedKinds = 32;

// Everything the panel keeps between frames. Select mode: INVITE FRIENDS turns the list into a checklist; a row click
// ticks or unticks that friend (by account id, so a reordered roster keeps the ticks), and the button sends one invite to
// each ticked friend. Written by the click handler, read by OnFrame; both run on the game-loop thread, the mutex keeps
// that an assumption checked by nothing. A function-local static: the Quest sentinel may carry no dynamic initializer.
struct State {
  std::atomic<bool> added{false};
  std::array<std::string, kRows> shownNames;
  std::array<std::string, kRows> shownStatuses;
  std::array<std::string, kRows> shownMarks;
  std::string shownInviteText = "INVITE FRIENDS";
  std::mutex selectMutex;
  bool selectMode = false;
  std::vector<std::uint64_t> selected;
  std::atomic<bool> textsDirty{false};
  std::uint64_t lastCheckMs = 0;
  std::uint32_t loggedCount = ~0U;
  std::mutex eventMutex;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> loggedKinds;
};

State& S() {
  static State state;
  return state;
}

// What the game's font can draw: printable ASCII, at most `limit` characters (the writer refuses anything else).
std::string Printable(const char* text, std::size_t limit) {
  std::string out;
  for (const char* p = text; p != nullptr && *p != '\0' && out.size() < limit; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    out.push_back(c >= 0x20 && c <= 0x7E ? static_cast<char>(c) : '?');
  }
  return out;
}

std::string StatusFor(const nevr_social_roster::Roster& roster, std::uint32_t row) {
  if (!roster.OnlineAt(row)) return "OFFLINE";
  std::string text = Printable(roster.StatusTextAt(row), kStatusChars);
  if (text.empty()) return "ONLINE";
  for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

// Posts one text when it differs from what the row shows; the shown text changes only once the writer took it.
void PostIfChanged(std::uint64_t element, const std::string& text, std::string* shown) {
  if (*shown == text) return;
  nevr_ui_text::Request request;
  request.target = {kPanelActor, element};
  request.text = text;
  if (nevr_ui_text::Post(std::move(request)) == nevr_ui_text::Check::Ok) *shown = text;
}

// ---- clicks ----

// The sending actor's name: the sender's low 16 bits index the game space's actor table (invite_choices.h).
std::uint64_t SenderOf(std::uint64_t space, std::uint64_t sender) {
  using nevr_friends_platform::Read;
  const auto index = static_cast<std::uint16_t>(sender & 0xffff);
  std::uint64_t table = 0, names = 0, capacity = 0, owner = 0;
  if (index == nevr_invite_choices::kNoSender || space == 0) return 0;
  if (!Read(space + nevr_invite_choices::kSpaceActorTableOffset, &table, sizeof(table)) || table == 0) return 0;
  if (!Read(table + nevr_invite_choices::kTableNamesOffset, &names, sizeof(names)) || names == 0) return 0;
  if (!Read(table + nevr_invite_choices::kTableCapacityOffset, &capacity, sizeof(capacity)) || index >= capacity) return 0;
  if (!Read(names + std::uint64_t{index} * sizeof(std::uint64_t), &owner, sizeof(owner))) return 0;
  return owner;
}

// Sends one party invite; returns whether the requests were handed to the service.
bool InviteId(std::uint64_t id) {
  const std::vector<nevr_social_party::Message> messages = nevr_social_party::Global().SendInvite(id);
  if (messages.empty()) {
    Log(Level::Info, "[NEVR.MENU] friends panel invite: nothing to send in the current party state");
    return false;
  }
  const bool sent = nevr_social_party::Send(messages);
  Log(sent ? Level::Info : Level::Warning, "[NEVR.MENU] friends panel invite requests=%zu %s", messages.size(),
      sent ? "sent" : "NOT sent");
  return sent;
}

std::string InviteButtonText(const State& s) {
  if (!s.selectMode) return "INVITE FRIENDS";
  if (s.selected.empty()) return "CANCEL";
  return "SEND INVITES (" + std::to_string(s.selected.size()) + ")";
}

void OnInviteClicked() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.selectMutex);
  if (!s.selectMode) {
    s.selectMode = true;
    s.selected.clear();
    Log(Level::Info, "[NEVR.MENU] friends panel select mode on (rows now tick friends to invite)");
  } else {
    std::size_t sent = 0;
    for (const std::uint64_t id : s.selected) sent += InviteId(id) ? 1 : 0;
    Log(Level::Info, "[NEVR.MENU] friends panel select mode off selected=%zu invites_sent=%zu", s.selected.size(), sent);
    s.selectMode = false;
    s.selected.clear();
  }
  s.textsDirty.store(true);
}

void OnRowClicked(std::uint32_t row, std::uint64_t event) {
  State& s = S();
  std::uint64_t id = 0;
  if (!nevr_social_roster::Global().IdAt(row, &id)) {
    Log(Level::Info, "[NEVR.MENU] friends panel click row=%u: no friend in that row", row);
    return;
  }
  std::lock_guard<std::mutex> lock(s.selectMutex);
  if (!s.selectMode) {
    nevr_menu_friends_profile::OpenFromRowClick(row, event);
    return;
  }
  const auto at = std::find(s.selected.begin(), s.selected.end(), id);
  const bool ticked = at == s.selected.end();
  if (ticked) {
    s.selected.push_back(id);
  } else {
    s.selected.erase(at);
  }
  Log(Level::Info, "[NEVR.MENU] friends panel row %s row=%u selected=%zu", ticked ? "ticked" : "unticked", row,
      s.selected.size());
  s.textsDirty.store(true);
}

}  // namespace

void MarkPackageAdded() { S().added.store(true); }

bool PackageAdded() { return S().added.load(); }

void OnComponentEvent(std::uint64_t space, std::uint64_t event) noexcept {
  if (event == 0 || !PackageAdded()) return;
  std::array<std::uint64_t, kEventWords> words{};
  if (!nevr_friends_platform::Read(event, words.data(), sizeof(words))) return;
  const std::uint64_t symbol = words[nevr_invite_choices::kEventSymbolOffset / 8];
  const std::uint64_t component = words[nevr_invite_choices::kEventComponentOffset / 8];
  try {
    // Ours when the panel actor sent it, or when it names one of the panel's ids (an event can be sent on the page's
    // behalf, naming the element clicked).
    const std::uint64_t owner = SenderOf(space, words[nevr_invite_choices::kEventSenderOffset / 8]);
    if (owner == nevr_menu_friends_profile::kPopupActor || nevr_menu_friends_profile::IsProfileEvent(symbol)) {
      nevr_menu_friends_profile::OnPopupEvent(event, symbol);
      return;
    }
    const bool ourComponent = component == kPageElementId || component == kInviteButton ||
                              std::find(kRowButtons.begin(), kRowButtons.end(), component) != kRowButtons.end();
    if (!ourComponent && owner != kPanelActor) return;
    State& s = S();
    {
      std::lock_guard<std::mutex> lock(s.eventMutex);
      const std::pair<std::uint64_t, std::uint64_t> kind{symbol, component};
      if (s.loggedKinds.size() < kMaxLoggedKinds &&
          std::find(s.loggedKinds.begin(), s.loggedKinds.end(), kind) == s.loggedKinds.end()) {
        s.loggedKinds.push_back(kind);
        Log(Level::Info,
            "[NEVR.MENU] friends panel event symbol=0x%016llx component=0x%016llx words=%016llx %016llx %016llx %016llx %016llx",
            static_cast<unsigned long long>(symbol), static_cast<unsigned long long>(component),
            static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]),
            static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]),
            static_cast<unsigned long long>(words[4]));
      }
    }
    if (symbol == kInviteClickEvent) {
      OnInviteClicked();
      return;
    }
    for (std::uint32_t row = 0; row < kRows; ++row) {
      if (symbol == kRowClickEvents[row]) OnRowClicked(row, event);
    }
  } catch (const std::exception& error) {
    Log(Level::Warning, "[NEVR.MENU] friends panel event failed reason=%s", error.what());
  }
}

void OnFrame() {
  if (!PackageAdded() || !nevr_ui_text::Ready()) return;
  nevr_menu_friends_profile::OnFrame();
  State& s = S();
  const std::uint64_t now = nevr_friends_platform::NowMs();
  const bool dirty = s.textsDirty.exchange(false);
  if (!dirty && now - s.lastCheckMs < kCheckIntervalMs) return;
  s.lastCheckMs = now;
  const nevr_social_roster::Roster& roster = nevr_social_roster::Global();
  const std::uint32_t count = roster.Count();
  for (std::uint32_t row = 0; row < kRows; ++row) {
    const bool used = row < count;
    PostIfChanged(kNameElements[row], used ? Printable(roster.NameAt(row), kNameChars) : std::string(), &s.shownNames[row]);
    PostIfChanged(kStatusElements[row], used ? StatusFor(roster, row) : std::string(), &s.shownStatuses[row]);
  }
  {
    std::lock_guard<std::mutex> lock(s.selectMutex);
    for (std::uint32_t row = 0; row < kRows; ++row) {
      std::uint64_t id = 0;
      std::string mark;
      if (s.selectMode && roster.IdAt(row, &id)) {
        mark = std::find(s.selected.begin(), s.selected.end(), id) != s.selected.end() ? "SELECTED" : "SELECT";
      }
      PostIfChanged(kMarkElements[row], mark, &s.shownMarks[row]);
    }
    PostIfChanged(kInviteText, InviteButtonText(s), &s.shownInviteText);
  }
  // One line whenever the number of friends changes (the first check included); names are never logged.
  if (count != s.loggedCount) {
    Log(Level::Info, "[NEVR.MENU] friends panel rows friends=%u online=%u shown=%u", count, roster.Online(),
        count < kRows ? count : kRows);
    s.loggedCount = count;
  }
}

}  // namespace nevr_menu_friends_pack
