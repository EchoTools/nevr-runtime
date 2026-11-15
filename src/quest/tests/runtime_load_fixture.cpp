// A fixture runtime for runtime_load_test (src/quest/shim/runtime_load.h). One shared object per mode, chosen by the macro
// its build gives it (host_tests/groups/integration.cmake):
//   NEVR_FIXTURE_OK               a valid descriptor; initialize returns 0
//   NEVR_FIXTURE_NO_ENTRY         no NEVR_GetRuntimeApi export
//   NEVR_FIXTURE_NULL_DESCRIPTOR  NEVR_GetRuntimeApi returns null
//   NEVR_FIXTURE_BAD_MAJOR        a descriptor whose abi_major is one the shim does not speak
//   NEVR_FIXTURE_INIT_FAIL        a valid descriptor; initialize returns a nonzero status
// Every mode records the context its initialize was given and counts its calls, so the test reads what the shim handed over.
// Built without exceptions, like the rest of the shim's host tests.

#include "core/quest_runtime_abi.h"

// The NEVR-BUILD literal the update start reads a fixture's version from (core/build_literal_scan.h): the NUL that starts it, the
// prefix, the version, a space, a 40-digit commit and the closing NUL. Every mode carries it, so a fixture in the root names a version.
extern "C" __attribute__((used, visibility("default"))) const char NEVR_FixtureBuildLiteral[] =
    "\0NEVR-BUILD 6.18.0 0123456789abcdef0123456789abcdef01234567";

namespace {

/// The status initialize returns in the init-failed mode, which the test expects in the record.
constexpr std::int32_t kFixtureInitFailStatus = 7;
/// An abi_major the shim does not speak (the shim speaks nevr_quest_runtime_abi::kMajor).
constexpr std::uint32_t kFixtureBadMajor = 2;

#if defined(NEVR_FIXTURE_BAD_MAJOR)
constexpr std::uint32_t kFixtureMajor = kFixtureBadMajor;
#else
constexpr std::uint32_t kFixtureMajor = nevr_quest_runtime_abi::kMajor;
#endif

NevrQuestHostContext g_lastContext = {};
std::uint32_t g_initCalls = 0;

// Unused in the no-entry and null-descriptor modes, which is why it is marked maybe_unused.
[[maybe_unused]] std::int32_t Initialize(const NevrQuestHostContext* host) noexcept {
  ++g_initCalls;
  if (host != nullptr) g_lastContext = *host;
#if defined(NEVR_FIXTURE_INIT_FAIL)
  return kFixtureInitFailStatus;
#else
  return 0;
#endif
}

[[maybe_unused]] NevrQuestRuntimeApi g_api = {
    {nevr_quest_runtime_abi::kRuntimeApiV1Size, kFixtureMajor, 0, 0},
    &Initialize,
};

}  // namespace

extern "C" {

__attribute__((visibility("default"))) const NevrQuestHostContext* NEVR_FixtureLastContext() noexcept {
  return &g_lastContext;
}

__attribute__((visibility("default"))) std::uint32_t NEVR_FixtureInitCalls() noexcept { return g_initCalls; }

#if !defined(NEVR_FIXTURE_NO_ENTRY)
__attribute__((visibility("default"))) const NevrQuestRuntimeApi* NEVR_GetRuntimeApi() noexcept {
#if defined(NEVR_FIXTURE_NULL_DESCRIPTOR)
  return nullptr;
#else
  return &g_api;
#endif
}
#endif

}  // extern "C"
