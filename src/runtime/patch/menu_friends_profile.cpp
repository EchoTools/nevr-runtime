#include "runtime/patch/menu_friends_profile.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "nevr/symbol_hash.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/patch/friends_panel_platform.h"
#include "runtime/patch/invite_choices.h"
#include "runtime/patch/menu_friends_avatars.h"
#include "runtime/patch/ui_text_writer.h"

namespace nevr_menu_friends_profile {
namespace {

using nevr_friends_platform::Level;
using nevr_friends_platform::Log;

// The SUPPORT & SOCIAL card's click: its ui_mnu_button_pointermesh event and its page element id (CR15UIPage2ElementCR,
// last word). The home page script opens the pop-up page on it.
constexpr std::uint64_t kOpenEvent = EchoVR::CSymbol64Hash("evt_discord_btn_clicked");
constexpr std::uint64_t kSocialCardElement = 0x2FD3868C5D7B458EULL;
// The pop-up's buttons (ui_mnu_popup_panel_image_social): left SOCIAL (the Discord link), middle CONTINUE (close),
// right HELP (the Help page link). The package's ARENA / COMBAT switch sends its own two events.
constexpr std::uint64_t kLeftClicked = EchoVR::CSymbol64Hash("evt_left_btn_clicked");
constexpr std::uint64_t kMiddleClicked = EchoVR::CSymbol64Hash("evt_middle_btn_clicked");
constexpr std::uint64_t kRightClicked = EchoVR::CSymbol64Hash("evt_right_btn_clicked");
constexpr std::uint64_t kArenaClicked = EchoVR::CSymbol64Hash("evt_nevr_profile_tab_arena");
constexpr std::uint64_t kCombatClicked = EchoVR::CSymbol64Hash("evt_nevr_profile_tab_combat");
// What a left or right click becomes: a symbol no script handles, so the stock link never opens.
constexpr std::uint64_t kSwallowed = EchoVR::CSymbol64Hash("evt_nevr_friend_profile_handled");

constexpr std::size_t kStats = 8;
constexpr std::size_t kNameChars = 40;   // headertext holds 41 bytes
constexpr std::size_t kButtonChars = 32; // the button labels hold 33
constexpr std::size_t kLabelChars = 24;  // stat<i>_label holds 25
constexpr std::size_t kValueChars = 16;  // stat<i>_value and the tab labels hold 17

// The texts the runtime writes, in one table: element name and length limit.
struct Element {
  std::uint64_t id;
  std::size_t limit;
};
enum Field { kFHeader, kFStatus, kFParty, kFBody2, kFLeft, kFMiddle, kFRight, kFTabArena, kFTabCombat, kFStat0 };
constexpr int kFields = kFStat0 + 2 * kStats;  // a label and a value per stat line

std::array<Element, kFields> MakeElements() {
  std::array<Element, kFields> e{};
  e[kFHeader] = {EchoVR::CSymbol64Hash("headertext"), kNameChars};
  e[kFStatus] = {EchoVR::CSymbol64Hash("bodytext"), 60};
  e[kFParty] = {EchoVR::CSymbol64Hash("linktext"), 60};
  e[kFBody2] = {EchoVR::CSymbol64Hash("bodytext2"), 60};
  e[kFLeft] = {EchoVR::CSymbol64Hash("lefttext"), kButtonChars};
  e[kFMiddle] = {EchoVR::CSymbol64Hash("middletext"), kButtonChars};
  e[kFRight] = {EchoVR::CSymbol64Hash("righttext"), kButtonChars};
  e[kFTabArena] = {EchoVR::CSymbol64Hash("tab_arena_txt"), kValueChars};
  e[kFTabCombat] = {EchoVR::CSymbol64Hash("tab_combat_txt"), kValueChars};
  for (std::size_t i = 0; i < kStats; ++i) {
    e[kFStat0 + 2 * i] = {EchoVR::CSymbol64Hash(("stat" + std::to_string(i) + "_label").c_str()), kLabelChars};
    e[kFStat0 + 2 * i + 1] = {EchoVR::CSymbol64Hash(("stat" + std::to_string(i) + "_value").c_str()), kValueChars};
  }
  return e;
}
const std::array<Element, kFields>& Elements() {
  static const std::array<Element, kFields> elements = MakeElements();
  return elements;
}

enum class Mode { kArena, kCombat };
using StatLines = std::array<std::pair<std::string, std::string>, kStats>;

struct State {
  std::uint64_t generation = 0;   // one per opening; a fetch for an older one is dropped
  std::uint64_t rosterId = 0;
  std::string name;
  std::string userId;             // the friend's service user id, once the fetch found it
  Mode mode = Mode::kArena;
  bool confirming = false;        // UNFRIEND clicked once: the stats column asks, the buttons are CANCEL / YES
  bool removed = false;
  std::string statsNote = "LOADING STATS...";  // shown instead of the lines until they arrive
  StatLines arena{};
  StatLines combat{};
  std::string leftLabel = "INVITE";
  std::string rightLabel = "UNFRIEND";
  std::array<std::string, kFields> texts;
};

// The pop-up's state, behind one mutex; a function-local static (the Quest sentinel may carry no dynamic initializer).
struct Shared {
  std::mutex mutex;
  State state;
  std::array<std::string, kFields> shown;  // what each element shows; empty, so the first write replaces the defaults
  std::atomic<bool> dirty{false};
};

Shared& G() {
  static Shared shared;
  return shared;
}

std::string Printable(const std::string& text, std::size_t limit) {
  std::string out;
  for (const char c : text) {
    if (out.size() >= limit) break;
    const auto u = static_cast<unsigned char>(c);
    out.push_back(u >= 0x20 && u <= 0x7E ? c : '?');
  }
  return out;
}

std::string Upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// 48210 -> "48,210".
std::string Grouped(long long value) {
  std::string digits = std::to_string(value < 0 ? -value : value);
  for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
  return value < 0 ? "-" + digits : digits;
}

// Recomputes every text from the state. G().mutex held.
void Compose() {
  State& s = G().state;
  s.texts[kFHeader] = Printable(s.name, kNameChars);
  s.texts[kFBody2] = "";
  s.texts[kFMiddle] = "CLOSE";
  // The switch has no background, so emptying its labels hides it while UNFRIEND asks to confirm.
  s.texts[kFTabArena] = s.confirming ? "" : s.mode == Mode::kArena ? "> ARENA <" : "ARENA";
  s.texts[kFTabCombat] = s.confirming ? "" : s.mode == Mode::kCombat ? "> COMBAT <" : "COMBAT";
  StatLines lines{};
  if (s.confirming) {
    lines[0].first = "ARE YOU SURE YOU WANT";
    lines[1].first = "TO UNFRIEND";
    lines[2].first = Printable(Upper(s.name), kLabelChars - 1) + "?";
    lines[4].first = "THEY WILL BE REMOVED";
    lines[5].first = "FROM YOUR FRIENDS LIST.";
    s.texts[kFLeft] = "CANCEL";
    s.texts[kFRight] = "YES, UNFRIEND";
  } else {
    if (!s.statsNote.empty()) {
      lines[0].first = s.statsNote;
    } else {
      lines = s.mode == Mode::kArena ? s.arena : s.combat;
    }
    s.texts[kFLeft] = s.leftLabel;
    s.texts[kFRight] = s.rightLabel;
  }
  for (std::size_t i = 0; i < kStats; ++i) {
    s.texts[kFStat0 + 2 * i] = lines[i].first;
    s.texts[kFStat0 + 2 * i + 1] = lines[i].second;
  }
  G().dirty.store(true);
}

// ---- service ----

// One request to the community service with the player's token. `method` GET, POST or DELETE.
bool Http(const char* method, const std::string& url, const std::string& bearer, const std::string& payload,
          std::string* body, long* status) {
  nevr_friends_platform::HttpReply reply = nevr_friends_platform::Http(method, url, bearer, payload);
  *body = std::move(reply.body);
  *status = reply.status;
  return reply.ok;
}

// A stat's value: the service writes each one as {"cnt": n, "op": "...", "val": x}.
bool StatValue(const nlohmann::json& group, const char* key, double* out) {
  if (!group.is_object() || !group.contains(key)) return false;
  const nlohmann::json& stat = group[key];
  const nlohmann::json& value = stat.is_object() ? stat.value("val", nlohmann::json()) : stat;
  if (!value.is_number()) return false;
  *out = value.get<double>();
  return true;
}

// The service leaves a zero stat out of the reply (omitzero), so a missing stat in a group that exists is 0; a missing
// group is "-".
std::string Count(const nlohmann::json& group, const char* key) {
  double v = 0;
  if (!group.is_object() || group.empty()) return "-";
  return StatValue(group, key, &v) ? Grouped(std::llround(v)) : "0";
}

std::string Percent(const nlohmann::json& group, const char* key) {
  double v = 0;
  if (!group.is_object() || group.empty()) return "-";
  return StatValue(group, key, &v) ? std::to_string(std::lround(v)) + "%" : "0%";
}

std::string Pair(const nlohmann::json& group, const char* a, const char* b) {
  return Count(group, a) + " / " + Count(group, b);
}

// The eight lines of each mode, from the player/profile reply (the game's server profile, stats by group name).
void ReadStats(const nlohmann::json& profile, StatLines* arenaOut, StatLines* combatOut) {
  const nlohmann::json stats = profile.value("stats", nlohmann::json::object());
  const nlohmann::json arena = stats.value("arena", nlohmann::json::object());
  const nlohmann::json combat = stats.value("combat", nlohmann::json::object());
  *arenaOut = {{{"LEVEL", Count(arena, "Level")},
                {"WINS / LOSSES", Pair(arena, "ArenaWins", "ArenaLosses")},
                {"WIN RATE", Percent(arena, "ArenaWinPercentage")},
                {"GOALS", Count(arena, "Goals")},
                {"ASSISTS", Count(arena, "Assists")},
                {"SAVES", Count(arena, "Saves")},
                {"STUNS", Count(arena, "Stuns")},
                {"MVPS", Count(arena, "ArenaMVPs")}}};
  *combatOut = {{{"LEVEL", Count(combat, "Level")},
                 {"WINS / LOSSES", Pair(combat, "CombatWins", "CombatLosses")},
                 {"WIN RATE", Percent(combat, "CombatWinPercentage")},
                 {"KILLS", Count(combat, "CombatKills")},
                 {"DEATHS", Count(combat, "CombatDeaths")},
                 {"ASSISTS", Count(combat, "CombatAssists")},
                 {"DAMAGE", Count(combat, "CombatDamage")},
                 {"MVPS", Count(combat, "CombatMVPs")}}};
  for (StatLines* lines : {arenaOut, combatOut}) {
    for (auto& line : *lines) line.second = Printable(line.second, kValueChars);
  }
}

// The player's own active guild group (account metadata active_group_id), so the stats are the ones the game's own
// profile view shows: the service otherwise uses the friend's active group.
std::string ActiveGroup(const std::string& base, const std::string& token) {
  std::string body;
  long status = 0;
  if (!Http("GET", base + "/v2/account", token, "", &body, &status) || status != 200) return "";
  try {
    const nlohmann::json account = nlohmann::json::parse(body);
    const std::string metadata = account.value("user", nlohmann::json::object()).value("metadata", "");
    if (metadata.empty()) return "";
    return nlohmann::json::parse(metadata).value("active_group_id", "");
  } catch (const nlohmann::json::exception&) {
    return "";
  }
}

// Worker: the friend's service user id (from the friend list, matched by name as the avatars are), then their profile.
void FetchProfile(std::uint64_t generation, std::string name) {
  const std::string base = nevr_friends_platform::ServiceBase();
  const std::string token = nevr_friends_platform::Token();
  const auto finish = [generation](const std::string& userId, const std::string& note, const StatLines& arena,
                                   const StatLines& combat) {
    std::lock_guard<std::mutex> lock(G().mutex);
    if (G().state.generation != generation) return;
    G().state.userId = userId;
    G().state.statsNote = note;
    G().state.arena = arena;
    G().state.combat = combat;
    Compose();
  };
  if (base.empty() || token.empty()) {
    Log(Level::Info, "[NEVR.MENU] friend profile stats skipped reason=%s", token.empty() ? "no_token" : "no_http_uri");
    finish("", "STATS UNAVAILABLE", {}, {});
    return;
  }
  std::string body, userId;
  long status = 0;
  if (!Http("GET", std::string(base) + "/v2/friend?limit=100", token, "", &body, &status) || status != 200) {
    Log(Level::Warning, "[NEVR.MENU] friend profile: friend list request failed status=%ld", status);
    finish("", "STATS UNAVAILABLE", {}, {});
    return;
  }
  try {
    const nlohmann::json doc = nlohmann::json::parse(body);
    for (const auto& f : doc.value("friends", nlohmann::json::array())) {
      const auto& u = f.value("user", nlohmann::json::object());
      if (Lower(u.value("display_name", "")) == Lower(name) || Lower(u.value("username", "")) == Lower(name)) {
        userId = u.value("id", "");
        break;
      }
    }
  } catch (const nlohmann::json::exception&) {
    Log(Level::Warning, "[NEVR.MENU] friend profile: friend list unreadable bytes=%zu", body.size());
  }
  if (userId.empty()) {
    Log(Level::Info, "[NEVR.MENU] friend profile: no service account matched the row's name");
    finish("", "STATS UNAVAILABLE", {}, {});
    return;
  }
  const std::string group = ActiveGroup(base, token);
  nlohmann::json request{{"user_id", userId}};
  if (!group.empty()) request["group_id"] = group;
  body.clear();
  // unwrap: the request body is the RPC payload itself and the reply is the RPC's own JSON.
  if (!Http("POST", std::string(base) + "/v2/rpc/player/profile?unwrap", token, request.dump(), &body, &status) ||
      status != 200) {
    Log(Level::Warning, "[NEVR.MENU] friend profile stats request failed status=%ld", status);
    finish(userId, "STATS UNAVAILABLE", {}, {});
    return;
  }
  StatLines arena{}, combat{};
  try {
    ReadStats(nlohmann::json::parse(body), &arena, &combat);
  } catch (const nlohmann::json::exception&) {
    Log(Level::Warning, "[NEVR.MENU] friend profile stats unreadable bytes=%zu", body.size());
    finish(userId, "STATS UNAVAILABLE", {}, {});
    return;
  }
  Log(Level::Info, "[NEVR.MENU] friend profile stats loaded bytes=%zu group=%s", body.size(),
      group.empty() ? "friend_default" : "own_active");
  finish(userId, "", arena, combat);
}

// Worker: removes the friend on the service, then drops them from the roster and asks the game's list to refresh.
void Unfriend(std::uint64_t rosterId, std::string userId) {
  const std::string base = nevr_friends_platform::ServiceBase();
  const std::string token = nevr_friends_platform::Token();
  std::string body;
  long status = 0;
  const bool ok = !base.empty() && !token.empty() &&
                  Http("DELETE", std::string(base) + "/v2/friend?ids=" + userId, token, "", &body, &status) &&
                  status == 200;
  Log(ok ? Level::Info : Level::Warning, "[NEVR.MENU] friend profile unfriend account=%llu status=%ld %s",
      static_cast<unsigned long long>(rosterId), status, ok ? "removed" : "NOT removed");
  if (ok) {
    nevr_social_roster::Global().Remove(rosterId);
    nevr_social_party::Send(nevr_social_party::Global().RefreshFriends());
  }
  std::lock_guard<std::mutex> lock(G().mutex);
  if (G().state.rosterId != rosterId) return;
  G().state.removed = ok;
  G().state.rightLabel = ok ? "REMOVED" : "UNFRIEND FAILED";
  Compose();
}

void Rewrite(std::uint64_t event, std::uint64_t symbol, const std::uint64_t* component) {
  nevr_friends_platform::WriteWord(event + nevr_invite_choices::kEventSymbolOffset, symbol);
  if (component != nullptr) nevr_friends_platform::WriteWord(event + nevr_invite_choices::kEventComponentOffset, *component);
}

}  // namespace

bool OpenFromRowClick(std::uint32_t row, std::uint64_t event) {
  const nevr_social_roster::Roster& roster = nevr_social_roster::Global();
  std::uint64_t id = 0;
  if (!roster.IdAt(row, &id)) return false;
  const std::string name = roster.NameAt(row) != nullptr ? roster.NameAt(row) : "";
  const bool online = roster.OnlineAt(row);
  std::string status = online ? Upper(Printable(roster.StatusTextAt(row), 60)) : "OFFLINE";
  if (status.empty()) status = "ONLINE";
  const std::string party = !online ? "" : roster.PartyIdAt(row) != 0 ? "IN AN OPEN PARTY" : "NOT IN AN OPEN PARTY";
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(G().mutex);
    generation = ++G().state.generation;
    G().state.rosterId = id;
    G().state.name = name;
    G().state.userId.clear();
    G().state.mode = Mode::kArena;
    G().state.confirming = false;
    G().state.removed = false;
    G().state.statsNote = "LOADING STATS...";
    G().state.leftLabel = "INVITE";
    G().state.rightLabel = "UNFRIEND";
    G().state.texts[kFStatus] = status;
    G().state.texts[kFParty] = party;
    Compose();
  }
  nevr_menu_friends_avatars::ShowProfile(id);
  Rewrite(event, kOpenEvent, &kSocialCardElement);
  Log(Level::Info, "[NEVR.MENU] friend profile opened row=%u account=%llu online=%d", row,
      static_cast<unsigned long long>(id), online ? 1 : 0);
  std::thread(FetchProfile, generation, name).detach();
  return true;
}

bool IsProfileEvent(std::uint64_t symbol) { return symbol == kArenaClicked || symbol == kCombatClicked; }

void OnPopupEvent(std::uint64_t event, std::uint64_t symbol) noexcept {
  try {
    std::lock_guard<std::mutex> lock(G().mutex);
    if (symbol == kMiddleClicked) {
      G().state.confirming = false;
      Compose();
      Log(Level::Info, "[NEVR.MENU] friend profile closed");
      return;
    }
    if (symbol == kArenaClicked || symbol == kCombatClicked) {
      if (G().state.confirming) return;
      G().state.mode = symbol == kArenaClicked ? Mode::kArena : Mode::kCombat;
      Compose();
      Log(Level::Info, "[NEVR.MENU] friend profile stats mode=%s", symbol == kArenaClicked ? "arena" : "combat");
      return;
    }
    if (symbol != kLeftClicked && symbol != kRightClicked) {
      Log(Level::Info, "[NEVR.MENU] friend profile pop-up event symbol=0x%016llx (passed to the game)",
          static_cast<unsigned long long>(symbol));
      return;
    }
    Rewrite(event, kSwallowed, nullptr);
    if (G().state.rosterId == 0 || G().state.removed) return;
    if (symbol == kLeftClicked) {
      if (G().state.confirming) {  // CANCEL
        G().state.confirming = false;
        Compose();
        Log(Level::Info, "[NEVR.MENU] friend profile unfriend cancelled account=%llu",
            static_cast<unsigned long long>(G().state.rosterId));
        return;
      }
      const std::vector<nevr_social_party::Message> messages = nevr_social_party::Global().SendInvite(G().state.rosterId);
      const bool sent = !messages.empty() && nevr_social_party::Send(messages);
      Log(sent ? Level::Info : Level::Warning,
          "[NEVR.MENU] friend profile invite account=%llu requests=%zu %s",
          static_cast<unsigned long long>(G().state.rosterId), messages.size(), sent ? "sent" : "NOT sent");
      G().state.leftLabel = sent ? "INVITED" : "INVITE FAILED";
      Compose();
      return;
    }
    // UNFRIEND: the first click asks in the pop-up, YES removes.
    if (!G().state.confirming) {
      G().state.confirming = true;
      Compose();
      Log(Level::Info, "[NEVR.MENU] friend profile unfriend asked to confirm account=%llu",
          static_cast<unsigned long long>(G().state.rosterId));
      return;
    }
    G().state.confirming = false;
    if (G().state.userId.empty()) {
      G().state.rightLabel = "TRY AGAIN SHORTLY";
      Compose();
      Log(Level::Info, "[NEVR.MENU] friend profile unfriend waiting: service account not found yet");
      return;
    }
    G().state.rightLabel = "REMOVING...";
    Compose();
    std::thread(Unfriend, G().state.rosterId, G().state.userId).detach();
  } catch (const std::exception& error) {
    Log(Level::Warning, "[NEVR.MENU] friend profile event failed reason=%s", error.what());
  }
}

void OnFrame() {
  if (!G().dirty.exchange(false) || !nevr_ui_text::Ready()) return;
  std::lock_guard<std::mutex> lock(G().mutex);
  for (int f = 0; f < kFields; ++f) {
    const std::string text = Printable(G().state.texts[f], Elements()[f].limit);
    if (G().shown[f] == text) continue;
    nevr_ui_text::Request request;
    request.target = {kPopupActor, Elements()[f].id};
    request.text = text;
    if (nevr_ui_text::Post(std::move(request)) == nevr_ui_text::Check::Ok) {
      G().shown[f] = text;
    } else {
      G().dirty.store(true);  // try again next frame
    }
  }
}

}  // namespace nevr_menu_friends_profile
