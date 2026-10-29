// Host test for the hardware dump sharing libr15's vrapi_GetSystemPropertyInt slot with the texture-quality hook (#943).
// GotHook allows one owner per slot, so when the texture-quality hook owns it the dump installs nothing there and is told
// each query's native answer instead (sentinel/sysprop_int_share.h). Real texture_quality_hook.cpp, hwdump_install.cpp,
// hwdump_handlers.cpp and hwdump_records.cpp; no GOT slot is touched (a host has no libr15, so every GotHook install is
// refused, which the unshared case relies on).
//
// Built and run by `just test-quest-hooks`.

#include <cstdint>
#include <cstdio>
#include <string_view>

#include "quest/diag/hwdump_handlers.h"
#include "quest/diag/hwdump_install.h"
#include "quest/diag/hwdump_records.h"
#include "quest/tests/test_check.h"
#include "render_config_share.h"
#include "sysprop_int_share.h"
#include "texture_quality_hook.h"

namespace {

namespace hw = nevr_quest::hwdump;
namespace tq = nevr_quest::texture_quality;
using sentinel::pinned::OvrJavaOpaque;

constexpr std::size_t kSysPropIntIndex = 7;
constexpr int kNativeWidth = 2064;
constexpr int kNativeHeight = 2208;

int FakeRuntime(const OvrJavaOpaque*, int id) {
  if (id == 5) return kNativeWidth;
  if (id == 6) return kNativeHeight;
  return id * 10 + 7;
}

tq::SystemPropertyIntThunk::Fn Entry() {
  return reinterpret_cast<tq::SystemPropertyIntThunk::Fn>(tq::SystemPropertyIntThunk::EntryAddress());
}

// A real ovrJava: the dump's observer reads the VM and activity from it, as libr15's callers pass it.
const hw::OvrJava kRealJava = {reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222),
                               reinterpret_cast<void*>(0x3333)};
const OvrJavaOpaque* const kJava = reinterpret_cast<const OvrJavaOpaque*>(&kRealJava);

const hw::RecordSnapshot* Find(const hw::RecordSnapshot* snaps, std::size_t n, std::int64_t id) {
  for (std::size_t i = 0; i < n; ++i) {
    if (snaps[i].fn == hw::QueryFn::kVrapiGetSystemPropertyInt && snaps[i].id == id) return &snaps[i];
  }
  return nullptr;
}

hw::HookState StateOfTheSlot() {
  hw::HookState states[hw::kHookCount];
  hw::HookStates(states);
  return states[kSysPropIntIndex];
}

void WithTheSlotUnownedTheDumpInstallsItsOwnHookAndObservesNothing() {
  sentinel::MarkSysPropIntSlotOwned(false);
  sentinel::SetSysPropIntObserver(nullptr);
  hw::InstallHooks();  // a host has no libr15: GotHook refuses every slot
  const hw::HookState s = StateOfTheSlot();
  QCHECK(std::string_view(s.symbol) == "vrapi_GetSystemPropertyInt");
  QCHECK(std::string_view(s.status) != "shared");
  QCHECK(!s.installed);
  QCHECK(!hw::SysPropIntShared());
  QCHECK(sentinel::GetSysPropIntObserver() == nullptr);
}

void WithTheSlotOwnedTheDumpSharesItAndSeesTheRuntimesAnswerNotTheOverride() {
  tq::ResetForTest();
  hw::ResetCaptureForTest();
  QCHECK(tq::InstallVia(tq::Level::kHigh, false, []() noexcept { return sentinel::GotStatus::kOk; }));
  QCHECK(sentinel::SysPropIntSlotOwned());
  hw::InstallHooks();
  const hw::HookState shared = StateOfTheSlot();
  QCHECK(std::string_view(shared.status) == "shared");
  QCHECK(shared.installed);
  QCHECK(hw::SysPropIntShared());
  QCHECK(sentinel::GetSysPropIntObserver() != nullptr);

  tq::SystemPropertyIntThunk::Reset();
  *tq::SystemPropertyIntThunk::OriginalOut() = reinterpret_cast<void*>(&FakeRuntime);
  QCHECK(tq::ArmForTest(tq::Level::kHigh));  // the same level again: re-arms the thunk after Reset
  QCHECK(Entry()(kJava, 5) == 3072);  // the game gets the override
  QCHECK(Entry()(kJava, 6) == 3380);
  QCHECK(Entry()(kJava, 9) == 97);

  hw::RecordSnapshot snaps[hw::kMaxRecords];
  const std::size_t n = hw::Records().Snapshot(snaps, hw::kMaxRecords);
  const hw::RecordSnapshot* width = Find(snaps, n, 5);
  const hw::RecordSnapshot* height = Find(snaps, n, 6);
  const hw::RecordSnapshot* other = Find(snaps, n, 9);
  QCHECK(width != nullptr && width->last.num_ints == 1 && width->last.ints[0] == kNativeWidth);  // what the runtime said
  QCHECK(height != nullptr && height->last.num_ints == 1 && height->last.ints[0] == kNativeHeight);
  QCHECK(other != nullptr && other->last.ints[0] == 97);
  QCHECK(hw::ObservedSysPropIntCalls() == 3);
  QCHECK(hw::Captured().vm == kRealJava.vm && hw::Captured().activity == kRealJava.activity);  // the dump still gets the VM
  QCHECK(StateOfTheSlot().calls == 3);
  QCHECK(StateOfTheSlot().faults == 0);
  sentinel::SetSysPropIntObserver(nullptr);
  tq::ResetForTest();
}

// The render-configuration log (#1139) owns four of the dump's slots when it installed first: the dump installs nothing on them
// and reports them owned_by_render_config; its other fourteen hooks are untouched.
void WithTheRenderConfigSlotsOwnedTheDumpLeavesThemAlone() {
  struct Owned {
    std::size_t index;
    const char* symbol;
    sentinel::RenderConfigSlot slot;
  };
  const Owned owned[] = {
      {8, "vrapi_GetSystemPropertyFloat", sentinel::RenderConfigSlot::kGetSystemPropertyFloat},
      {12, "vrapi_SetPropertyInt", sentinel::RenderConfigSlot::kSetPropertyInt},
      {13, "vrapi_SetDisplayRefreshRate", sentinel::RenderConfigSlot::kSetDisplayRefreshRate},
      {14, "vrapi_SetClockLevels", sentinel::RenderConfigSlot::kSetClockLevels},
  };
  tq::ResetForTest();
  hw::ResetCaptureForTest();
  for (const Owned& o : owned) sentinel::MarkRenderConfigSlotOwned(o.slot, true);
  hw::InstallHooks();
  hw::HookState states[hw::kHookCount];
  hw::HookStates(states);
  for (const Owned& o : owned) {
    QCHECK(std::string_view(states[o.index].symbol) == o.symbol);
    QCHECK(std::string_view(states[o.index].status) == "owned_by_render_config");
    QCHECK(states[o.index].installed);
  }
  // Every other hook is the dump's own: a host has no libr15, so each of its installs is refused and says so.
  std::size_t own = 0;
  for (std::size_t i = 0; i < hw::kHookCount; ++i) {
    bool isOwned = false;
    for (const Owned& o : owned) isOwned = isOwned || o.index == i;
    if (isOwned) continue;
    QCHECK(std::string_view(states[i].status) != "owned_by_render_config");
    QCHECK(!states[i].installed);
    ++own;
  }
  QCHECK(own == hw::kHookCount - 4);
  for (const Owned& o : owned) sentinel::MarkRenderConfigSlotOwned(o.slot, false);

  // Nothing owned: the dump tries all four itself.
  hw::InstallHooks();
  hw::HookStates(states);
  for (const Owned& o : owned) QCHECK(std::string_view(states[o.index].status) != "owned_by_render_config");
}

}  // namespace

int main() {
  WithTheSlotUnownedTheDumpInstallsItsOwnHookAndObservesNothing();
  WithTheSlotOwnedTheDumpSharesItAndSeesTheRuntimesAnswerNotTheOverride();
  WithTheRenderConfigSlotsOwnedTheDumpLeavesThemAlone();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "hwdump_share_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("hwdump_share_test: OK\n");
  return 0;
}
