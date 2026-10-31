#pragma once
// CCanvasUICS::SetText(handle, CSymbol64 item, const char* text) (Quest 0x149111c in the disassembly): (this = CCanvasUICS*,
// RDX = SCompactPoolHandle (u32), R8 = item symbol, R9 = NUL-terminated text) -> non-zero when the item exists. It
// returns 0 at once for a null text. Called by every SetText2 script node (0x14126dce0) and by the credits page. The
// menu version label hooks it (menu_version_label.cpp) and the UI text writer calls it (ui_text.cpp), so the address
// and the bytes that pin it live in one place.
// PUSH RBX; SUB RSP,0x40; MOV RBX,R9; TEST R9,R9; JZ; MOV R9,R8; LEA RAX,[RSP+0x30]; MOV R8D,EDX; MOV [RSP+0x20],RAX;
// LEA RDX,[RSP+0x68].

#include <array>
#include <cstdint>

namespace nevr_canvas_set_text {

inline constexpr std::uint64_t kSetTextVA = 0x14037D470;
inline constexpr std::array<std::uint8_t, 32> kSetTextPrologue = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xD9, 0x4D, 0x85, 0xC9, 0x74, 0x3F, 0x4D, 0x8B,
    0xC8, 0x48, 0x8D, 0x44, 0x24, 0x30, 0x44, 0x8B, 0xC2, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8D};
using SetTextFn = std::uint64_t (*)(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text);

}  // namespace nevr_canvas_set_text
