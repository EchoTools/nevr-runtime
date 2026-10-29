#include "runtime/patch/server_team_size_install.h"

#include <cstdint>
#include <string>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/process_memory.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/server_team_size.h"

namespace nevr_server_team_size {

static_assert(nevr_patch_addresses::SERVER_ASSIGN_TEAM_CAP_SIZE == sizeof(kCapExpected), "cap site size");
static_assert(nevr_patch_addresses::SERVER_ASSIGN_TEAM_SLOT_SIZE == sizeof(kSlotExpected), "slot site size");

namespace {

bool ProcessWrite(void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, src, n); }

}  // namespace

void Install(bool isServer) {
  if (!isServer) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] server team size not applied: this is a client (the server assigns teams)");
    return;
  }
  const char* text = NevrCfgGetFlat("nevr_max_team_size");
  std::string problem;
  const uint32_t size = ParseMaxTeamSize(text, &problem);
  if (!problem.empty()) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] network.max_team_size=%s is %s (allowed %u..%u); using %u", text,
        problem.c_str(), static_cast<unsigned>(kMinTeamSize), static_cast<unsigned>(kMaxTeamSize),
        static_cast<unsigned>(size));
  }
  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  uint8_t* const capSite = base + nevr_patch_addresses::SERVER_ASSIGN_TEAM_CAP;
  uint8_t* const slotSite = base + nevr_patch_addresses::SERVER_ASSIGN_TEAM_SLOT;
  const Outcome outcome = Apply(capSite, slotSite, size, ProcessWrite);
  char record[512];
  FormatRecord(record, sizeof(record), outcome, reinterpret_cast<uintptr_t>(capSite),
               reinterpret_cast<uintptr_t>(slotSite));
  Log(IsFailure(outcome) ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s source=%s", record,
      text != nullptr && text[0] != '\0' ? "config.yaml" : "default");
}

}  // namespace nevr_server_team_size
