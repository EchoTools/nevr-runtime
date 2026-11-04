#include "irradiance_default_hook.h"

#include <atomic>
#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::irradiance_default {
namespace {

// The texel the reference build uploads: R, G and B 0x6b, alpha 0xff. Static storage, so the pointer the helper is given is valid
// for the call (the helper copies the four bytes into its staging buffer before it returns).
alignas(4) constexpr std::uint8_t kGreyTexelBytes[4] = {0x6b, 0x6b, 0x6b, 0xff};
static_assert(sizeof(kGreyTexelBytes) == 4, "format 9 is a four-byte texel");

// The CGEngineResources being initialized, or null. Written by the Initialize wrapper on the thread that runs Initialize, read by
// the upload handler on every thread that calls the helper.
std::atomic<const void*> g_resources{nullptr};
// Substitutions made since the current Initialize began. Reset by the wrapper, counted by the handler.
std::atomic<std::uint32_t> g_substituted{0};
// Whether the `substituted` line and the `not_reached` warning have been written (once per process each).
std::atomic<bool> g_loggedSubstituted{false};
std::atomic<bool> g_loggedNotReached{false};

sentinel::GotHook g_initializeHook;

// One line per outcome, written after Initialize has returned: nothing is logged while the helper runs.
void Report(const void* resources, std::int32_t status, std::uint32_t substituted) noexcept {
  char resourcesText[19];
  sentinel::HexString(resourcesText, reinterpret_cast<std::uintptr_t>(resources));
  if (substituted != 0) {
    if (g_loggedSubstituted.exchange(true, std::memory_order_relaxed)) return;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "irradiance_default",
                        {{"status", "substituted"},
                         {"texture", "Default 3D Texture"},
                         {"resources", resourcesText},
                         {"texel", "0xff6b6b6b"},
                         {"substitutions", substituted},
                         {"initialize_status", status},
                         {"consequence", "the default 3D texture holds the grey texel instead of black (the reference build's light for areas no irradiance volume covers)"}});
    return;
  }
  if (g_loggedNotReached.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "irradiance_default",
                      {{"status", "not_reached"},
                       {"texture", "Default 3D Texture"},
                       {"resources", resourcesText},
                       {"substitutions", substituted},
                       {"initialize_status", status},
                       {"consequence", "this Initialize did not upload the default 3D texture through the hook; it keeps the game's black texel"}});
}

// CGEngineResources::Initialize. The mark is set before the original runs and cleared after it, so the upload handler can tell the
// one call that matters from every other use of the helper. The status is the game's, returned as it came.
std::int32_t OnInitialize(InitializeThunk::Fn original, void* resources) noexcept {
  g_substituted.store(0, std::memory_order_relaxed);
  g_resources.store(resources, std::memory_order_release);
  const std::int32_t status = original(resources);
  g_resources.store(nullptr, std::memory_order_release);
  Report(resources, status, g_substituted.load(std::memory_order_relaxed));
  return status;
}

// The image upload helper. During Initialize, for the SGTexture at `this + 0x1458`, in the four-byte format and with a texel, the
// texel pointer becomes the grey one. Every other call is the original with the arguments it came with. Never logs, allocates or
// throws: it is live across the call into the game (callback_thunk.h rule 1) and on every use of the helper.
void OnUpload(UploadThunk::Fn original, void* texture, const void* image, std::uint32_t format, std::int32_t arg3, std::int32_t arg4,
              const void* texel, const char* name) noexcept {
  const void* const resources = g_resources.load(std::memory_order_acquire);
  if (resources != nullptr && texel != nullptr && format == kTexelFormat &&
      texture == static_cast<const std::uint8_t*>(resources) + kDefault3dTextureOffset) {
    g_substituted.fetch_add(1, std::memory_order_relaxed);
    texel = kGreyTexelBytes;
  }
  original(texture, image, format, arg3, arg4, texel, name);
}

NEVR_HOOK_RECORD(kInitializeRecord, InitializeThunk, &OnInitialize);
NEVR_HOOK_RECORD(kUploadRecord, UploadThunk, &OnUpload);

}  // namespace

sentinel::GotStatus InstallInitializeHook(const sentinel::GotTarget& initialize, sentinel::ImageLookup lookup) noexcept {
  InitializeThunk::Arm(kInitializeRecord);
  return sentinel::InstallThunk<InitializeThunk>(g_initializeHook, initialize, lookup);
}

void ArmUploadHandler() noexcept { UploadThunk::Arm(kUploadRecord); }

void ArmInitializeForTest() noexcept { InitializeThunk::Arm(kInitializeRecord); }

void ArmForTest() noexcept {
  InitializeThunk::Arm(kInitializeRecord);
  UploadThunk::Arm(kUploadRecord);
}

void ResetForTest() noexcept {
  g_resources.store(nullptr, std::memory_order_relaxed);
  g_substituted.store(0, std::memory_order_relaxed);
  g_loggedSubstituted.store(false, std::memory_order_relaxed);
  g_loggedNotReached.store(false, std::memory_order_relaxed);
}

}  // namespace nevr_quest::irradiance_default
