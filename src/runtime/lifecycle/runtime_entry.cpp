#include "runtime/lifecycle/runtime_entry.h"

#include "abi/echovr.h"
#include "core/logging.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/initialize.h"

namespace nevr_runtime_entry {
namespace {

// Called once by the host's wrapper detour, on the game's main thread after the CRT is up and
// outside the loader lock. A nonzero return makes the host run the original wrapper only.
std::int32_t Initialize(const NevrHostContext* host) {
  const nevr_runtime_abi::Status status = nevr_runtime_abi::ValidateHostContext(host);
  if (status != nevr_runtime_abi::Status::kOk) {
    return static_cast<std::int32_t>(status) + 1;  // never 0
  }
  InitializeGameModule(host->game_module);
  ArmGameMain(host);
  Log(EchoVR::LogLevel::Info, "[NEVR.BOOT] runtime initialized from the host's wrapper detour");
  return 0;
}

const NevrRuntimeApi g_api = [] {
  NevrRuntimeApi api{};
  api.header.struct_size = sizeof(NevrRuntimeApi);
  api.header.abi_major = nevr_runtime_abi::kMajor;
  api.header.abi_minor = nevr_runtime_abi::kMinor;
  api.initialize = &Initialize;
  api.game_main = &RuntimeGameMain;
  return api;
}();

}  // namespace

const NevrRuntimeApi* GetRuntimeApi() { return &g_api; }

}  // namespace nevr_runtime_entry
