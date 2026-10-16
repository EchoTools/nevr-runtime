// nevr.dll fixture for the crash harness: implements the v1 descriptor from
// core/runtime_abi.h. Compile-definition variants:
//   HOST_CRASH_BAD_VERSION   descriptor reports abi_major + 1 (ABI-incompatible)
//   HOST_CRASH_INIT_FAILURE  initialize() returns nonzero (init failure)

#include "core/runtime_abi.h"

#include <windows.h>

#include <cstdint>

namespace {

std::int32_t Initialize(const NevrHostContext*) {
#if defined(HOST_CRASH_INIT_FAILURE)
  return 7;
#else
  return 0;
#endif
}

void Before(void*) {}
void After(void*, std::uint64_t) {}

const NevrRuntimeApi kApi = {
    {
        sizeof(NevrRuntimeApi),
#if defined(HOST_CRASH_BAD_VERSION)
        nevr_runtime_abi::kMajor + 1,
#else
        nevr_runtime_abi::kMajor,
#endif
        nevr_runtime_abi::kMinor,
        0,
    },
    &Initialize,
    &Before,
    &After,
};

}  // namespace

extern "C" __declspec(dllexport) const NevrRuntimeApi* NEVR_GetRuntimeApi() {
  return &kApi;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  return TRUE;
}
