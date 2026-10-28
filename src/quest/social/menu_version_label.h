#pragma once

#include <cstddef>
#include <cstdint>

// The runtime version on the Quest main menu (#747), the twin of the PC hook (src/runtime/patch/menu_version_label.cpp).
// The stock versions panel sets its lobby_version text item through CCanvasUICS::SetText; this hooks that call on the
// pinned libr15.so and draws "nEVR <project version>" in place of the lobby build time. The decisions are the PC rules
// header (src/runtime/patch/menu_version_label_rules.h), reused unchanged.
//
// Link-time address: ReVault names SetText at 0x149111c in libr15 (the ReVault address is the pinned one plus 0x100000,
// memory: reference-revault-libr15-offset). Its first four words are the prologue the install checks. The first word is
// `cbz x3`, which is PC-relative: the inline backend rewrites it in the trampoline (sentinel/arm64_reloc.h), so the hook
// installs on the pinned build.
namespace quest_social::menu_version {

inline constexpr std::uint64_t kSetTextVaddr = 0x139111cULL;  // CCanvasUICS::SetText(this, handle, item, text)

// The first four instruction words at kSetTextVaddr in the pinned libr15.so; the install refuses any other bytes, and
// got_pinned_test.cpp compares them with the real library.
inline constexpr std::uint32_t kSetTextPrologueWords[4] = {0xb4000243u, 0xd10083ffu, 0xa9017bf3u, 0xaa0303f3u};

// Installs the hook on the loaded libr15.so. Returns 1 when installed, 0 when refused or libr15 is not usable. Logs one
// line either way.
std::size_t InstallMenuVersionLabelHook() noexcept;

// Host-test support (src/quest/tests/menu_version_label_test.cpp): sets the label the handler draws, without touching
// libr15, and arms the handler. SetTextEntryForTest is the address the game's calls would reach; the test sets
// SetTextOriginalForTest to a stand-in for the game's SetText. ResetForTest disarms the handler and clears both.
void ArmForTest(const char* label) noexcept;
void* SetTextEntryForTest() noexcept;
void SetTextOriginalForTest(void* original) noexcept;
void ResetForTest() noexcept;

}  // namespace quest_social::menu_version
