#pragma once
// The map probes' game-free half: the maps_probe.yaml reader, the level-table diff, the P-SUBLEVEL matcher, the P-SKEW client
// and the P-RULES store check. Every line it produces is written by probe_game.h's LogLines under the `NEVR.PROBE` tag and
// holds hex level ids, frame numbers, counts and states only: no text read from the file or the game (logging.md Rule 4).
//
// A probe is a measurement build's code: it is compiled only when CMake's NEVR_MAP_PROBES is ON (src/runtime/CMakeLists.txt),
// and tools/verify_scenario_control_absent.py checks that no release DLL carries the `[NEVR.PROBE]` tag.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_map_probe {

enum class SkewExpect { kNone, kMissing, kVersion };

struct ProbeConfig {
  bool rules = false;
  bool skew = false;
  bool sublevel = false;
  SkewExpect skew_expect = SkewExpect::kNone;
  std::uint32_t skew_version = 0;  // the n of `skew_expect: probe@n`
  std::uint32_t ignored_lines = 0;
};

// `key: value` lines; `#` starts a comment. Unknown keys and values count in ignored_lines and change nothing.
ProbeConfig ParseProbeConfig(std::string_view text);
const char* SkewExpectName(SkewExpect expect);

// Both sorted. Fills the levels only in `after` and the levels only in `before`.
void DiffLevels(const std::vector<std::uint64_t>& before, const std::vector<std::uint64_t>& after,
                std::vector<std::uint64_t>* added, std::vector<std::uint64_t>* removed);

std::string HexLevel(std::uint64_t level);

// P-RULES. The game stores the three zone point values it reads from mp_arena_rules with `mov [rip+rel32], reg`; a global is
// read only when the store at its site still encodes that global's address.
inline constexpr std::size_t kZoneCount = 3;
inline constexpr std::size_t kStoreOpcodeBytes = 3;
inline constexpr std::size_t kStoreSize = 7;  // opcode (3) + rel32 (4)
struct ZoneStore {
  std::uintptr_t site_rva;
  std::uint8_t opcode[kStoreOpcodeBytes];
  std::uintptr_t global_rva;
};
// True when the kStoreSize bytes at `site` are `store.opcode` and a rel32 that lands on store.global_rva.
bool StoreEncodes(const std::uint8_t* site, const ZoneStore& store);

// The bytes the store at `store.site_rva` has when it encodes `store.global_rva`: the opcode and the rel32. StoreEncodes is true
// exactly when the bytes at `site` equal them.
void ExpectedStoreBytes(const ZoneStore& store, std::uint8_t (&out)[kStoreSize]);

// Rule 6 (docs/standards/logging.md): the fields of a pin refusal, `va=0x<hex> expected=<hex> actual=<hex>`; `actual` is
// "unreadable" when `actual` is null. `size` bytes of each.
std::string NotPinnedFields(std::uint64_t va, const std::uint8_t* expected, const std::uint8_t* actual, std::size_t size);

// Pairs each successful Enable/Disable (source B) with the add/del it causes in the enabled-level table (source A).
// A frame is one probe tick on the game thread; B's frame is the last tick before the call, so A, read on the next
// tick, is on time at B's frame + 1.
class SublevelMatcher {
 public:
  static constexpr std::uint64_t kMissingAfterFrames = 250;  // about 2 s of game-thread ticks
  static constexpr std::uint64_t kOnTimeFrames = 1;          // A is read on the tick after B's

  void OnB(bool enable, std::uint64_t level, std::uint64_t frame);
  // Appends a line for a late or unpaired A event.
  void OnA(bool add, std::uint64_t level, std::uint64_t frame, std::vector<std::string>* lines);
  // B events that have waited longer than kMissingAfterFrames count as missing; `all` expires every one.
  void Expire(std::uint64_t frame, bool all, std::vector<std::string>* lines);
  std::string Summary() const;

  std::uint32_t b_events() const { return b_events_; }
  std::uint32_t matched() const { return matched_; }
  std::uint32_t late() const { return late_; }
  std::uint32_t missing() const { return missing_; }
  std::uint32_t extra() const { return extra_; }

 private:
  struct Pending {
    bool enable;
    std::uint64_t level;
    std::uint64_t frame;
  };
  std::vector<Pending> pending_;
  std::uint32_t b_events_ = 0;
  std::uint32_t matched_ = 0;
  std::uint32_t late_ = 0;
  std::uint32_t missing_ = 0;
  std::uint32_t extra_ = 0;
};

// P-SKEW. The probe server pins every session to ProbePin(kServerProbeVersion).
inline constexpr std::uint32_t kServerProbeVersion = 2;
inline constexpr std::uint32_t kMaxProbeVersion = 99;
inline constexpr std::uint64_t kLeaveRetryFrames = 1250;  // about 10 s of game-thread ticks

std::uint64_t ProbePin(std::uint32_t version);  // rad_hash("probe@<version>")
int ProbeVersionOf(std::uint64_t pin);           // -1 when the pin is no probe@0..kMaxProbeVersion

struct SkewInput {
  std::uint64_t lobby = 0;  // 0 when there is no lobby
  std::uint64_t level = 0;  // owner JSON "level"
  std::uint64_t pin = 0;    // owner JSON "nevr_map_pin"
  std::int32_t state = 0;   // EchoVR::NetGameState
};

struct SkewAction {
  bool leave = false;  // schedule the return to the lobby this tick, then call OnLeaveResult
  std::string notice;  // set on the first attempt only; at most 63 bytes (the error block's line)
};

class SkewClient {
 public:
  explicit SkewClient(const ProbeConfig& config) : config_(config) {}

  // A successful Enable of `level`, drained this tick before Tick.
  void OnEnable(std::uint64_t level, std::uint64_t frame, std::vector<std::string>* lines);
  SkewAction Tick(const SkewInput& in, std::uint64_t frame, std::vector<std::string>* lines);
  void OnLeaveResult(bool scheduled, std::uint64_t frame, std::vector<std::string>* lines);

 private:
  void ResetSession();

  ProbeConfig config_;
  bool have_seen_ = false;
  SkewInput seen_;
  SkewInput key_;  // the session decided: lobby, level and pin
  std::int32_t last_state_ = 0;
  bool loaded_ = false;
  bool decided_ = false;
  bool leave_pending_ = false;
  bool notice_written_ = false;
  std::uint64_t leave_deadline_ = 0;
  std::string notice_;
};

}  // namespace nevr_map_probe
