/* `re33_ec_page`: the EC script libraries' spare page (#2187, docs/quest/combat/ec.md). 4096 zeroed bytes, 4096-aligned, exported from the
 * sentinel as a default-visibility data symbol, because the EC script libraries find it with `dlsym(RTLD_DEFAULT, "re33_ec_page")` and take
 * `page - 0x2104000` as echovr.exe's image base. A page nobody writes is "a PC without the EC network client" to every EC script.
 *
 * Why the definition is here and not in src/quest/combat: the combat archive is linked into the sentinel with `-Wl,--exclude-libs,ALL`
 * (sentinel/CMakeLists.txt), which hides every symbol of an archive member. This object is a source of the shared library itself, so its
 * default-visibility alias stays in the dynamic symbol table (tests/quest TestExportAllowlist lists it).
 *
 * The storage is hidden and the export is an alias of it. A default-visibility data symbol is preemptible: if another object already exports
 * `re33_ec_page` in the global scope, every reference to a default-visibility definition inside the sentinel would bind to THAT page through the
 * GOT, and the sentinel would write someone else's memory. The hidden storage binds locally, always; the sentinel's own code reaches the page
 * through `EcPage()` only (src/quest/tests/ec_page_test.cpp runs both cases on a shared object built from this file).
 *
 * Built with -fno-exceptions and hidden visibility, like every sentinel translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sentinel {

inline constexpr std::size_t kEcPageSize = 4096;

// The page the sentinel writes: the hidden storage, never the preemptible symbol.
std::uint8_t* EcPage() noexcept;

enum class EcPageState {
  kVisible,    // dlsym(RTLD_DEFAULT, "re33_ec_page") is this page: the script libraries find it
  kInvisible,  // not in the global lookup: the script libraries decide as on a PC without the EC network client
  kOther,      // another object already exports a re33_ec_page: the scripts use that one
};

// Asks the dynamic linker to put the object that holds the page into the global lookup (dlopen of its own path with
// RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL; the extra reference is kept for the process) and reports what dlsym(RTLD_DEFAULT) then finds.
// `reopened` gets whether that dlopen answered a handle. Writes no line: the caller does.
EcPageState PublishEcPage(bool* reopened, const void** seen) noexcept;

}  // namespace sentinel
