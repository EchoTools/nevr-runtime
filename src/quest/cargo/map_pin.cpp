#include "quest/cargo/map_pin.h"

#include <algorithm>
#include <mutex>

#include "nevr/symbol_hash.h"

namespace nevr_quest::maps::pin {
namespace {

std::mutex g_mutex;
View g_view;
std::vector<Pin> g_mounted;

// The client tick's state: game thread only (TickWith), and ResetForTest.
struct TickState {
  bool decided = false;
  std::uint64_t lobby = 0, level = 0, pin = 0;  // the key decided last
  bool leave_pending = false;
  std::uint64_t leave_since_ms = 0;
  std::string leave_id;
};
TickState g_tick;
void (*g_fetch_request)(const std::string& id) = nullptr;

const Pin* PinById(const std::vector<Pin>& pins, const std::string& id) {
  for (const Pin& p : pins)
    if (p.id == id) return &p;
  return nullptr;
}

const std::string* SecondOf(const std::vector<std::pair<std::string, std::string>>& rows, const std::string& id) {
  for (const auto& row : rows)
    if (row.first == id) return &row.second;
  return nullptr;
}

}  // namespace

std::uint64_t PinValue(const Pin& pin) {
  const std::string identity = pin.id + "@" + pin.version + "@" + pin.source_sha256.substr(0, 16);
  return EchoVR::CSymbol64Hash(identity.c_str());
}

std::uint64_t SessionLevelOf(const std::string& id) { return EchoVR::CSymbol64Hash(id.c_str()); }

std::uint64_t LowspecLevelOf(const std::string& id) { return EchoVR::CSymbol64Hash((id + "_lowspec").c_str()); }

View ViewFromBoot(const nevr_maps::BootReport& boot, const std::vector<std::string>& subscribed) {
  View view;
  for (const auto& row : boot.listed) {
    view.listed.emplace_back(SessionLevelOf(row.second), row.second);
    view.listed.emplace_back(LowspecLevelOf(row.second), row.second);
  }
  view.subscribed = subscribed;
  view.not_mounted = boot.not_mounted;
  for (const nevr_maps::MountedMap& m : boot.staged)
    view.staged.push_back({m.id, m.version, m.source_sha256, m.index_sha256, m.platform, m.level});
  return view;
}

void SetView(View view) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_view = std::move(view);
}

void SetMounted(std::vector<Pin> pins) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mounted = std::move(pins);
}

std::vector<Pin> Mounted() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mounted;
}

Decision Decide(std::uint64_t level, std::uint64_t pin) {
  Decision decision;
  if (level == 0) return decision;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto listed = std::find_if(g_view.listed.begin(), g_view.listed.end(),
                                   [level](const std::pair<std::uint64_t, std::string>& row) { return row.first == level; });
  if (listed == g_view.listed.end()) return decision;
  decision.id = listed->second;
  if (pin == 0) {
    decision.action = Action::kUnpinned;
    decision.reason = "map_unpinned";
    return decision;
  }
  // A staged map has not mounted yet (the lobby can come before the first resource request) but will.
  const Pin* mounted = PinById(g_mounted, decision.id);
  const Pin* staged = PinById(g_view.staged, decision.id);
  if ((mounted != nullptr && PinValue(*mounted) == pin) || (staged != nullptr && PinValue(*staged) == pin)) return decision;

  decision.action = Action::kLeave;
  if (const std::string* token = SecondOf(g_view.not_mounted, decision.id)) {
    decision.reason = "not_mounted";
    decision.request_fetch = *token == "stock_changed";
    return decision;
  }
  if (std::find(g_view.subscribed.begin(), g_view.subscribed.end(), decision.id) != g_view.subscribed.end()) {
    decision.reason = "version";
    decision.request_fetch = true;
    return decision;
  }
  decision.reason = "not_subscribed";
  return decision;
}

std::string NoticeFor(const Decision& decision, const std::string& local_version) {
  std::string head, middle, tail, second;
  if (decision.reason == "version" && !local_version.empty()) {
    head = "Needs ", middle = " (not your ", tail = "); updating for next launch.", second = local_version;
  } else if (decision.reason == "version" || (decision.reason == "not_mounted" && decision.request_fetch)) {
    head = "Needs map ", tail = "; updating for your next launch.";
  } else if (decision.reason == "not_subscribed") {
    head = "Needs map ", tail = ". Add it to maps.subscribed.";
  } else {
    head = "Map ", middle = " did not load: ", tail = ". See the nevr log.";
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const std::string* token = SecondOf(g_view.not_mounted, decision.id)) second = *token;
  }
  const std::size_t fixed = head.size() + middle.size() + tail.size();
  const std::size_t budget = kNoticeBytes > fixed ? kNoticeBytes - fixed : 0;
  std::size_t first_len = decision.id.size(), second_len = second.size();
  while (first_len + second_len > budget && first_len + second_len > 0) {
    if (first_len >= second_len) --first_len; else --second_len;
  }
  const auto cut = [](const std::string& field, std::size_t len) {
    return len >= field.size() ? field : len == 0 ? std::string() : field.substr(0, len - 1) + "~";
  };
  return head + cut(decision.id, first_len) + middle + cut(second, second_len) + tail;
}

std::vector<LoginEntry> LoginEntries() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<LoginEntry> entries;
  for (const Pin& p : g_mounted) entries.push_back({p, true});
  for (const Pin& p : g_view.staged)
    if (PinById(g_mounted, p.id) == nullptr) entries.push_back({p, false});
  return entries;
}

std::uint64_t SessionLevelWith(GameAccess& game, std::uint64_t lobby) {
  const std::uint64_t owner = lobby == 0 ? 0 : game.OwnerSymbol(lobby, kOwnerLevelKey);
  return owner != 0 ? owner : game.MainLevel();
}

void TickWith(GameAccess& game) {
  if (!game.Callable()) return;
  const std::uint64_t lobby = game.Lobby();
  const std::int32_t state = game.NetState();
  if (lobby == 0 || state < kFirstSessionState || state > kLastSessionState) {
    g_tick = TickState{};
    return;
  }
  if (g_tick.leave_pending) {
    if (game.ScheduleLeave()) {
      g_tick.leave_pending = false;
    } else if (game.NowMs() - g_tick.leave_since_ms >= kLeaveRetryMs) {
      g_tick.leave_pending = false;
      game.LogSkew("leave_failed", g_tick.leave_id, "");
    }
    return;
  }
  const std::uint64_t level = SessionLevelWith(game, lobby);
  const std::uint64_t pin = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (g_tick.decided && g_tick.lobby == lobby && g_tick.level == level && g_tick.pin == pin) return;
  g_tick.decided = true;
  g_tick.lobby = lobby, g_tick.level = level, g_tick.pin = pin;

  const Decision decision = Decide(level, pin);
  if (decision.action == Action::kNone) return;
  if (decision.action == Action::kUnpinned) {
    game.LogSkew(decision.reason.c_str(), decision.id, "");
    return;
  }
  std::string local_version;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Pin* local = PinById(g_mounted, decision.id);
    if (local == nullptr) local = PinById(g_view.staged, decision.id);
    if (local != nullptr) local_version = local->version;
  }
  const std::string notice = NoticeFor(decision, local_version);
  game.LogSkew(decision.reason.c_str(), decision.id, notice);
  game.WriteNotice(kNoticeTitle, notice.c_str());
  if (decision.request_fetch && g_fetch_request != nullptr) g_fetch_request(decision.id);
  if (!game.ScheduleLeave()) {
    g_tick.leave_pending = true;
    g_tick.leave_since_ms = game.NowMs();
    g_tick.leave_id = decision.id;
  }
}

void SetFetchRequest(void (*request)(const std::string& id)) { g_fetch_request = request; }

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_view = View{};
  g_mounted.clear();
  g_tick = TickState{};
  g_fetch_request = nullptr;
}

}  // namespace nevr_quest::maps::pin
