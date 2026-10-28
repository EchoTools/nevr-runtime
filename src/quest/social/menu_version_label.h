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

inline constexpr std::uint64_t kSetTextVaddr = 0x148111cULL;  // CCanvasUICS::SetText(this, handle, item, text)

// Installs the hook on the loaded libr15.so. Returns 1 when installed, 0 when refused or libr15 is not usable. Logs one
// line either way.
std::size_t InstallMenuVersionLabelHook() noexcept;

}  // namespace quest_social::menu_version
