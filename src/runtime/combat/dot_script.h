#pragma once

// The DOT FIELD's own script, mp_combat_grenade_dot (97d0806e6ea89130) (#600, EchoCombat slice 7 PR 5; he_is_the_cat's
// fix_dot_script.py, read as text, reconstructed here as two hooks):
//
// 1. Sound. Its two PlaySound nodes build the node on the stack and call the class's Execute (RVA 0x3D10,
//    Execute(node, ctx)) with the Wwise event a dword at node+0x28 (`mov dword [rbp-0x11], imm32` with the node at
//    rbp-0x39: field activates at 0xAA54, magazine deposited at 0xDDC4). The events, 0xF7C92B80
//    play_grenade_stun_active and 0xC9CD3996 stop_grenade_stun_active, are in no sound bank, so the field is silent; the
//    stun grenade's play_grenade_stun_activate (0x5B47A1CD) and stop_grenade_stun (0xBDE3209D) are. Execute is wrapped:
//    a node carrying one of the two is given the stun's before the original runs. Both ids occur only at those two
//    sites in the script.
// 2. Scene sets. The setup chain runs three EnableModelSceneSet nodes (9507dc91, cd089ec5, 878f54cb) on a model
//    component the DOT does not have, logging "Scriptnode can't find actor/component" three times a spawn. next_node
//    (RVA 0x10F30, next_node(node, port) -> the next node id) returns 0x9507DC91 after node 0x393F4ADA (ports `out` and
//    0, at 0x116AF); the wrap returns 0x013166C6 there instead, the node after the three.

#include <cstdint>

namespace nevr_combat::dot_script {

inline constexpr std::uint64_t kScript = 0x97d0806e6ea89130ULL;
inline constexpr std::uint32_t kPlaySoundExecuteRva = 0x3D10;
inline constexpr std::uint32_t kNextNodeRva = 0x10F30;
inline constexpr std::uintptr_t kEventOffset = 0x28;  // a dword in the PlaySound node

inline constexpr std::uint32_t kFieldOnEvent = 0xF7C92B80;     // play_grenade_stun_active
inline constexpr std::uint32_t kStunActivate = 0x5B47A1CD;     // play_grenade_stun_activate
inline constexpr std::uint32_t kFieldStopEvent = 0xC9CD3996;   // stop_grenade_stun_active
inline constexpr std::uint32_t kStunStop = 0xBDE3209D;         // stop_grenade_stun

inline constexpr std::uint64_t kSetupNode = 0x393f4ada;
inline constexpr std::uint64_t kFirstSceneSetNode = 0x9507dc91;
inline constexpr std::uint64_t kAfterSceneSetNodes = 0x013166c6;

// `mov [rsp+8],rbx; push rdi; sub rsp,0x20; cmp qword [rdx+0x18],0`
inline constexpr std::uint8_t kPlaySoundPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                      0xec, 0x20, 0x48, 0x83, 0x7a, 0x18, 0x00};
// `mov eax, 0x8354b4cb; cmp rcx, rax; jg ...`
inline constexpr std::uint8_t kNextNodePrologue[] = {0xb8, 0xcb, 0xb4, 0x54, 0x83, 0x48, 0x3b, 0xc8, 0x0f, 0x8f};

/// The event a PlaySound node plays: the stun's for the DOT's two, else unchanged.
std::uint32_t Event(std::uint32_t stock);

/// The next node: after the setup node, past the three scene-set nodes; else the game's.
std::uint64_t NextNode(std::uint64_t node, std::uint64_t stock);

/// Registers the two hooks with the script hook table. Once.
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::dot_script
