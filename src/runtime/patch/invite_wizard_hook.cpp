#include "runtime/patch/invite_wizard_hook.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/invite_choice_hook.h"
#include "runtime/patch/invite_wizard.h"

// The wizard's click metric leaves the game through the remote-log submit (ADR 0017, "Runtime hook"). Its only caller is
// CR15NetJsonLog::vslot[1] (ReVault echovr.exe 0x1401ccf40), which passes the serialised JSON line and its length to the
// submit (0x140113960). Register layout, from that caller's decompilation: RCX = line bytes, RDX = length (UNVERIFIED
// until a live capture). The bytes are read only when they are committed, readable memory; a wrong layout is refused, not
// dereferenced.
namespace nevr_invite_hook {
namespace {

constexpr std::uint64_t kSubmitVA = 0x140113960ULL;
constexpr std::array<std::uint8_t, 16> kSubmitPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                          0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};

using SubmitFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

SubmitFn g_originalSubmit = nullptr;
std::string g_salt;  // per run: a fingerprint in one run's log cannot be matched against another's

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

  const nevr_invite::Metric metric = nevr_invite::ParseMetric(line);
  if (metric.kind == nevr_invite::Kind::NotWizard) return;
  Log(EchoVR::LogLevel::Info, "%s", nevr_invite::ClickLine(metric.kind).c_str());

  // The card choices (experience, mode, map) are what the component-event observer last saw (invite_choice_hook.cpp);
  // with none seen a create click is "incomplete".
  const nevr_invite::Choices choices = nevr_invite_choice_hook::Choices().Snapshot();
  const nevr_invite::Plan plan = nevr_invite::PlanRequest(metric, choices);
  if (plan.skip != nevr_invite::Skip::None) {
    Log(EchoVR::LogLevel::Info, "%s", nevr_invite::NotSentLine(plan.skip, plan.code_len).c_str());
    return;
  }
  const std::string fingerprint =
      plan.op == nevr_invite::Op::Resolve ? nevr_invite::CodeFingerprint(plan.code, g_salt) : std::string();
  Log(EchoVR::LogLevel::Info, "%s", nevr_invite::PlannedLine(plan, fingerprint).c_str());
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

void Install(std::uintptr_t gameBase) {
  void* target = nevr::ResolveVA_Checked(gameBase, kSubmitVA);
  if (!nevr::ValidatePrologue(target, kSubmitPrologue.data(), kSubmitPrologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.INVITE] feature off reason=prologue_mismatch va=0x%llx",
        static_cast<unsigned long long>(kSubmitVA));
    return;
  }
  g_salt = std::to_string(GetTickCount64());
  g_originalSubmit = reinterpret_cast<SubmitFn>(target);
  PatchDetour(&g_originalSubmit, reinterpret_cast<PVOID>(&SubmitHook), "RemoteLogSubmit");
  Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] wizard observer installed va=0x%llx",
      static_cast<unsigned long long>(kSubmitVA));
}

}  // namespace nevr_invite_hook
