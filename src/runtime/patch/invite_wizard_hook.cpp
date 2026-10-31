#include "runtime/patch/invite_wizard_hook.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/invite_answer_apply.h"
#include "runtime/patch/invite_choice_hook.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/ui_texture_mips_rules.h"  // NameForHash, the search of the table sorted by hash

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the generated symbol cache, sorted by hash (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

// The wizard's click metric leaves the game through the remote-log submit (ADR 0017, "Runtime hook"). Its only caller is
// CR15NetJsonLog::vslot[1] (ReVault echovr.exe 0x1401ccf40), which passes the serialised JSON line and its length to the
// submit (0x140113960). Register layout, from that caller's decompilation: RCX = line bytes, RDX = length (UNVERIFIED
// until a live capture). The bytes are read only when they are committed, readable memory; a wrong layout is refused, not
// dereferenced.
namespace nevr_invite_hook {

const char* LevelName(std::uint64_t symbol) {
  return nevr_ui_texture_mips::NameForHash(evr::kSymCacheData, static_cast<std::size_t>(evr::kSymCacheCount), symbol);
}

namespace {

constexpr std::uint64_t kSubmitVA = 0x140113960ULL;
constexpr std::array<std::uint8_t, 16> kSubmitPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                          0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};

using SubmitFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

SubmitFn g_originalSubmit = nullptr;

// True when every page under [address, address + length) is committed and readable.
bool ReadableRange(std::uintptr_t address, std::size_t length) {
  const std::uintptr_t end = address + length;
  std::uintptr_t at = address;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (regionEnd <= at) return false;
    at = regionEnd;
  }
  return true;
}

void ObserveMetric(std::uint64_t data, std::uint64_t length) {
  if (data == 0 || length == 0 || length > nevr_invite::kMetricMaxBytes) return;
  const auto address = static_cast<std::uintptr_t>(data);
  const auto size = static_cast<std::size_t>(length);
  if (!ReadableRange(address, size)) return;
  const std::string_view line(reinterpret_cast<const char*>(address), size);
  if (!nevr_invite::MayBeWizardMetric(line)) return;

  if (nevr_invite::IsShareJoin(line)) {
    Log(EchoVR::LogLevel::Info, "%s", nevr_invite::ShareJoinClickLine().c_str());
    nevr_invite_apply::RequestShareJoin();  // the join runs on the game's thread, in the next frame
    return;
  }
  const nevr_invite::Metric metric = nevr_invite::ParseMetric(line);
  if (metric.kind == nevr_invite::Kind::NotWizard) return;
  Log(EchoVR::LogLevel::Info, "%s", nevr_invite::ClickLine(metric.kind).c_str());

  // The card choices (experience, mode, map) are what the component-event observer last saw (invite_choice_hook.cpp);
  // with none seen a create click is "incomplete".
  const nevr_invite::Choices choices = nevr_invite_choice_hook::Choices().Snapshot();
  const nevr_invite::Plan plan = nevr_invite::PlanRequest(metric, choices, LevelName);
  if (plan.skip != nevr_invite::Skip::None) {
    Log(EchoVR::LogLevel::Info, "%s", nevr_invite::NotSentLine(plan.skip, plan.code_len).c_str());
    if (metric.kind == nevr_invite::Kind::Join && (plan.skip == nevr_invite::Skip::Empty || plan.skip == nevr_invite::Skip::Malformed)) {
      // The field shows a typed code, so a join click that carries none says what it does carry (keys and kinds, never values).
      Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] join click carried no usable code, so no request was made; metric_fields=%s",
          nevr_invite::MetricShape(line).c_str());
    }
    return;
  }
  // A typed code or a create goes to the invite service (D1b). It writes the request line, or the line that says why nothing
  // was sent; the answer is written into the menu by nevr_invite_apply::OnFrame (D2).
  nevr_invite_service::Submit(plan);
}

std::uint64_t SubmitHook(std::uint64_t data, std::uint64_t length, std::uint64_t arg3, std::uint64_t arg4) {
  try {
    ObserveMetric(data, length);
  } catch (const std::exception& error) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.INVITE] observer failed reason=%s", error.what());
  }
  return g_originalSubmit(data, length, arg3, arg4);
}

}  // namespace

bool Install(std::uintptr_t gameBase) {
  void* target = nevr::ResolveVA_Checked(gameBase, kSubmitVA);
  if (!nevr::ValidatePrologue(target, kSubmitPrologue.data(), kSubmitPrologue.size())) {
    // Rule 6: the hook's name, its address, the pinned bytes and the bytes found there. An unreadable target is not read.
    const std::string expected = nevr_invite::HexBytes(kSubmitPrologue.data(), kSubmitPrologue.size());
    const std::string actual = ReadableRange(reinterpret_cast<std::uintptr_t>(target), kSubmitPrologue.size())
                                   ? nevr_invite::HexBytes(static_cast<const std::uint8_t*>(target), kSubmitPrologue.size())
                                   : std::string("unreadable");
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.INVITE] wizard observer off reason=prologue_mismatch name=RemoteLogSubmit va=0x%llx expected=%s actual=%s",
        static_cast<unsigned long long>(kSubmitVA), expected.c_str(), actual.c_str());
    return false;
  }
  g_originalSubmit = reinterpret_cast<SubmitFn>(target);
  PatchDetour(&g_originalSubmit, reinterpret_cast<PVOID>(&SubmitHook), "RemoteLogSubmit");
  Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] wizard observer installed va=0x%llx",
      static_cast<unsigned long long>(kSubmitVA));
  return true;
}

}  // namespace nevr_invite_hook
