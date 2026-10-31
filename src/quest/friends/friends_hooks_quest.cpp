// The FRIENDS panel's two libr15 slots on the Quest (friends_quest.h). Built without exceptions (callback_thunk.h).
//
// NRadEngine::CncaGameSpace::SendComponentEvent(SComponentEvent const&, unsigned int), libr15 0x1807264, JUMP_SLOT
// 0x36dbf40 (`readelf -rW` on the pinned file): `this` the game space (x0), the event (x1), the flags (w2). The other
// overload (CSymbol64, SActor, CSymbol64, unsigned int, 0x17fb0f8) builds the event on its stack ({event, *(space + 0x58),
// actor, key, ~0}: the PC's layout, invite_choices.h) and calls this one through its own PLT stub, so the slot sees every
// event. The panel sees it before the game, as on PC (invite_choice_hook.cpp).
//
// NRadEngine::CResourceManager::AddPackage(CSymbol64 set, char const* data root), libr15 0x2543238, JUMP_SLOT 0x36c1d30:
// the game adds its stock sets through it at start; right after the first that succeeds, the panel's pictures set is
// added the same way, once. A set that fails to add leaves the game as it ships.
//
// The Quest's AddPackage does not use its root argument (the game passes null): ReadManifestFile (0x2541404) and the
// package paths AddPackage builds are "<output directory>/<project>/<platform>/{manifests,packages}/..." with the output
// directory from NRadEngine::CFileTypes::GetOutputDirectory() const (0x11ebfd0, JUMP_SLOT 0x36ed5b8, called through the
// PLT). So the panel's set is added with that function answering the panel's own folder, on the thread that adds it and
// only while it does: nothing is written into the game's data folder, and every other caller gets the game's answer.

#include <atomic>
#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "libr15_identity.h"
#include "quest/friends/friends_quest.h"
#include "quest/friends/friends_quest_internal.h"
#include "runtime/patch/friends_package.h"
#include "runtime/patch/menu_friends_pack.h"

namespace nevr_quest_friends {
namespace {

inline constexpr const char* kLibR15 = "libr15.so";
inline constexpr const char* kSendComponentEventSymbol =
    "_ZN10NRadEngine13CncaGameSpace18SendComponentEventERKNS_15SComponentEventEj";
inline constexpr const char* kAddPackageSymbol = "_ZN10NRadEngine16CResourceManager10AddPackageENS_9CSymbol64EPKc";
inline constexpr const char* kOutputDirectorySymbol = "_ZNK10NRadEngine10CFileTypes18GetOutputDirectoryEv";

using SendEventSig = void(void* space, const void* event, std::uint32_t flags);
struct SendEventTag {};
using SendEventThunk = sentinel::CallbackThunk<SendEventTag, SendEventSig>;

using AddPackageSig = std::uint32_t(void* manager, std::uint64_t set, const char* dataRoot);
struct AddPackageTag {};
using AddPackageThunk = sentinel::CallbackThunk<AddPackageTag, AddPackageSig>;

using OutputDirectorySig = const char*(const void* fileTypes);
struct OutputDirectoryTag {};
using OutputDirectoryThunk = sentinel::CallbackThunk<OutputDirectoryTag, OutputDirectorySig>;

sentinel::GotHook g_sendHook;
sentinel::GotHook g_addHook;
sentinel::GotHook g_outputHook;
thread_local bool t_addingOurs = false;  // this thread is inside the AddPackage call for the panel's set
std::atomic<const char*> g_dataRoot{nullptr};
std::atomic<bool> g_addTried{false};

void OnSendEvent(SendEventThunk::Fn original, void* space, const void* event, std::uint32_t flags) noexcept {
  nevr_menu_friends_pack::OnComponentEvent(reinterpret_cast<std::uint64_t>(space), reinterpret_cast<std::uint64_t>(event));
  original(space, event, flags);
}

NEVR_HOOK_RECORD(kSendEventRecord, SendEventThunk, &OnSendEvent);

std::uint32_t OnAddPackage(AddPackageThunk::Fn original, void* manager, std::uint64_t set, const char* dataRoot) noexcept {
  const std::uint32_t result = original(manager, set, dataRoot);
  const char* ours = g_dataRoot.load(std::memory_order_acquire);
  if (result != 0 || ours == nullptr || set == nevr_friends_package::kSetId) return result;
  bool expected = false;
  if (!g_addTried.compare_exchange_strong(expected, true)) return result;
  t_addingOurs = true;
  const std::uint32_t added = original(manager, nevr_friends_package::kSetId, ours);
  t_addingOurs = false;
  if (added == 0) nevr_menu_friends_pack::MarkPackageAdded();
  sentinel::LogFields(added == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "friends_panel",
                      {{"status", added == 0 ? "package_set_added" : "package_set_not_added"}, {"result", added}});
  return result;
}

NEVR_HOOK_RECORD(kAddPackageRecord, AddPackageThunk, &OnAddPackage);

const char* OnOutputDirectory(OutputDirectoryThunk::Fn original, const void* fileTypes) noexcept {
  if (t_addingOurs) {
    const char* ours = g_dataRoot.load(std::memory_order_acquire);
    if (ours != nullptr) return ours;
  }
  return original(fileTypes);
}

NEVR_HOOK_RECORD(kOutputDirectoryRecord, OutputDirectoryThunk, &OnOutputDirectory);

}  // namespace

std::size_t InstallFriendsPanel() noexcept {
  const char* root = PrepareFriendsPanel();
  if (root == nullptr) return 0;
  g_dataRoot.store(root, std::memory_order_release);
  SendEventThunk::Arm(kSendEventRecord);
  AddPackageThunk::Arm(kAddPackageRecord);
  OutputDirectoryThunk::Arm(kOutputDirectoryRecord);
  const char* const buildId = sentinel::pinned::LibR15PatchedBuildId();
  const sentinel::GotStatus send = sentinel::InstallThunk<SendEventThunk>(
      g_sendHook, {kLibR15, kSendComponentEventSymbol, sentinel::RelocKind::kJumpSlot, buildId, 0x36dbf40ULL});
  const sentinel::GotStatus add = sentinel::InstallThunk<AddPackageThunk>(
      g_addHook, {kLibR15, kAddPackageSymbol, sentinel::RelocKind::kJumpSlot, buildId, 0x36c1d30ULL});
  const sentinel::GotStatus output = sentinel::InstallThunk<OutputDirectoryThunk>(
      g_outputHook, {kLibR15, kOutputDirectorySymbol, sentinel::RelocKind::kJumpSlot, buildId, 0x36ed5b8ULL});
  const bool ok = send == sentinel::GotStatus::kOk && add == sentinel::GotStatus::kOk && output == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "friends_panel",
                      {{"status", ok ? "installed" : "not_installed"},
                       {"send_component_event", sentinel::GotStatusName(send)},
                       {"add_package", sentinel::GotStatusName(add)},
                       {"output_directory", sentinel::GotStatusName(output)}});
  return (send == sentinel::GotStatus::kOk ? 1u : 0u) + (add == sentinel::GotStatus::kOk ? 1u : 0u) +
         (output == sentinel::GotStatus::kOk ? 1u : 0u);
}

}  // namespace nevr_quest_friends
