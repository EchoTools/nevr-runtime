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
  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  uint8_t* const capSite = base + nevr_patch_addresses::SERVER_ASSIGN_TEAM_CAP;
  uint8_t* const slotSite = base + nevr_patch_addresses::SERVER_ASSIGN_TEAM_SLOT;
  const char* const text = isServer ? NevrCfgGetFlat("nevr_max_team_size") : nullptr;
  for (const Line& line : InstallLines(isServer, text, capSite, slotSite, ProcessWrite,
                                       reinterpret_cast<uintptr_t>(capSite), reinterpret_cast<uintptr_t>(slotSite))) {
    Log(line.warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.text.c_str());
  }
}

}  // namespace nevr_server_team_size
