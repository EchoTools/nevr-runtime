#include "os_build_gate_hook.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::os_build_gate {

namespace {

sentinel::GotHook g_hook;
std::atomic<std::uint64_t> g_floor{0};
std::atomic<unsigned long long> g_calls{0};
std::atomic<bool> g_read{false};
std::atomic<std::uint64_t> g_osBuild{0};

// CSysInfo::OSBuildVersion. The game's only caller is CR15NetGame::Initialize, which refuses the headset when this returns
// 1 with a number below the floor. Answer 0 ("no number") every time, so Initialize goes on to its next check. The
// original runs into a local, so what this headset reports is in the log and the game's variable is not written.
int Handler(Thunk::Fn original, std::uint64_t* /*version*/) noexcept {
  std::uint64_t osBuild = 0;
  const bool read = original(&osBuild) != 0;
  if (g_calls.fetch_add(1, std::memory_order_acq_rel) != 0) return 0;  // the line is written once
  if (!read) osBuild = 0;
  g_read.store(read, std::memory_order_relaxed);
  g_osBuild.store(osBuild, std::memory_order_relaxed);
  const Seen seen = CurrentSeen();
  sentinel::LogFields(sentinel::LogLevel::kInfo, "os_build_gate",
                      {{"answer", "no_number"},
                       {"os_build_read", seen.read},
                       {"os_build", seen.os_build},
                       {"floor", seen.floor},
                       {"game_would_refuse", seen.would_refuse}});
  return 0;
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

}  // namespace

// The floor from a libr15 image, read only after its build ID is the pinned one. `*source` names the outcome.
std::uint64_t ReadPinnedFloorFrom(const sentinel::ElfImage& image, const char** source) noexcept {
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    *source = "no_build_id";
    return 0;
  }
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) {
    *source = "build_id_mismatch";
    return 0;
  }
  std::uint64_t floor = 0;
  std::memcpy(&floor, reinterpret_cast<const unsigned char*>(image.base) + sentinel::pinned::kMinOSBuildVersionVaddr,
              sizeof(floor));
  *source = "pinned";
  return floor;
}

namespace {

// The floor from the loaded libr15. `*source` names the outcome.
std::uint64_t ReadPinnedFloor(const char** source) noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    *source = "module_not_loaded";
    return 0;
  }
  return ReadPinnedFloorFrom(image, source);
}

void Arm(std::uint64_t floor) noexcept {
  g_floor.store(floor, std::memory_order_release);
  g_calls.store(0, std::memory_order_relaxed);
  g_read.store(false, std::memory_order_relaxed);
  g_osBuild.store(0, std::memory_order_relaxed);
  Thunk::Arm(kHook);
}

}  // namespace

Seen CurrentSeen() noexcept {
  const bool read = g_read.load(std::memory_order_relaxed);
  const std::uint64_t osBuild = g_osBuild.load(std::memory_order_relaxed);
  const std::uint64_t floor = g_floor.load(std::memory_order_acquire);
  return {g_calls.load(std::memory_order_acquire), read, osBuild, floor, read && floor != 0 && osBuild < floor};
}

bool InstallAt(const sentinel::GotTarget& target, std::uint64_t floor, const char* floorSource,
               sentinel::ImageLookup lookup) noexcept {
  Arm(floor);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target, lookup);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "os_build_gate_install",
                      {{"result", ok ? "installed" : "refused"},
                       {"status", sentinel::GotStatusName(status)},
                       {"slot", sentinel::pinned::kOSBuildVersionSymbol},
                       {"floor", floor},
                       {"floor_source", floorSource}});
  return ok;
}

bool Install() noexcept {
  const char* source = "unknown";
  const std::uint64_t floor = ReadPinnedFloor(&source);
  return InstallAt(sentinel::pinned::LibR15OSBuildVersion(), floor, source);
}

void ArmForTest(std::uint64_t floor) noexcept { Arm(floor); }

void ResetForTest() noexcept {
  g_floor.store(0, std::memory_order_relaxed);
  g_calls.store(0, std::memory_order_relaxed);
  g_read.store(false, std::memory_order_relaxed);
  g_osBuild.store(0, std::memory_order_relaxed);
}

}  // namespace nevr_quest::os_build_gate
