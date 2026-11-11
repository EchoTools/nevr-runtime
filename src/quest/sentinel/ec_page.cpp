#include "ec_page.h"

#include <dlfcn.h>

// The page lives in a hidden object; `re33_ec_page` is an exported alias of it (ec_page.h says why).
extern "C" {
alignas(4096) __attribute__((visibility("hidden"))) std::uint8_t nevr_ec_page_storage[sentinel::kEcPageSize] = {};
extern __attribute__((visibility("default"), alias("nevr_ec_page_storage"))) std::uint8_t re33_ec_page[sentinel::kEcPageSize];
}

namespace sentinel {

std::uint8_t* EcPage() noexcept { return nevr_ec_page_storage; }

EcPageState PublishEcPage(bool* reopened, const void** seen) noexcept {
  Dl_info di{};
  const char* const path = dladdr(nevr_ec_page_storage, &di) != 0 && di.dli_fname != nullptr ? di.dli_fname : nullptr;
  // The extra reference is kept for the process's life: the sentinel is never unmapped.
  const bool got = path != nullptr && dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL) != nullptr;
  if (reopened != nullptr) *reopened = got;
  const void* const found = dlsym(RTLD_DEFAULT, "re33_ec_page");
  if (seen != nullptr) *seen = found;
  if (found == nullptr) return EcPageState::kInvisible;
  return found == static_cast<const void*>(nevr_ec_page_storage) ? EcPageState::kVisible : EcPageState::kOther;
}

}  // namespace sentinel
