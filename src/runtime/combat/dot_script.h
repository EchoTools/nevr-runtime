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
// 3. The hide node (69412c20, ARC_SCENE off) goes on to the field sound's stop (5fefe254) instead of 0x169412C20
//    (fix_dot_field.py LINKS[2]; next_node arm at VA 0x180011b5a of 97d0806e6ea89130.dll).
// 4. The field's scene sets (fix_dot_field.py, the port of its .rdata write and flag stores). The three scene-set nodes
//    that load the DOT's (FIELD, 678a..) pair (show 9507dc91 at FUN_18000CFE0, hide 69412c20 at FUN_18000DA00, and
//    e94b7b9c at FUN_18000AC90) are rebuilt to store (FIELD, ARC_SCENE) instead, with the flag each one stored: show 1,
//    hide 0, and e94b7b9c 0 (so ARC_SCENE is off there too, as in his build; X2). The stock constant in .rdata is not
//    written. Each body is the same 0x40-byte argument block passed to read_context_variables (RVA 0x4270), which the
//    script's helpers fill from the node's context; the rebuilt node calls that helper itself and never the stock node.
// 5. The field's sound (node f2a71917 returns the sound's start, 0x1F2A71917 at 0x12B4F in fix_dot_field.py LINKS[0],
//    which cmp's rcx against 0xF2A71917 at 0x180012B01) goes on to the show node (9507dc91) instead; the show node's end
//    (0xCD089EC5 at 0x11E33, LINKS[1]) ends the chain (-1).
//    Both are keyed on the node and the game's own result, so no other node's next changes.
//
// Which path each serves: the setup chain (node 393f4ada) skips the three stock scene-set nodes (item 2); the field's
// show path reaches 9507dc91 only through item 5, after the sound starts, and its hide path through item 3.

#include <cstddef>
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
inline constexpr std::uint64_t kFirstSceneSetNode = 0x9507dc91;   // the setup chain's first scene-set node (skipped)
inline constexpr std::uint64_t kAfterSceneSetNodes = 0x013166c6;

// The field's hide chain (fix_dot_field.py LINKS[2]), keyed on the node and the game's own next for ports out and 0 of
// next_node: the hide node's next in the game is 0x169412C20.
inline constexpr std::uint64_t kHideNode = 0x69412c20;             // the hide node (ARC_SCENE off)
inline constexpr std::uint64_t kHideStockNext = 0x169412c20;
inline constexpr std::uint64_t kSoundStop = 0x5FEFE254;            // the field sound's stop

// `mov [rsp+8],rbx; push rdi; sub rsp,0x20; cmp qword [rdx+0x18],0`
inline constexpr std::uint8_t kPlaySoundPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                      0xec, 0x20, 0x48, 0x83, 0x7a, 0x18, 0x00};
// `mov eax, 0x8354b4cb; cmp rcx, rax; jg ...`
inline constexpr std::uint8_t kNextNodePrologue[] = {0xb8, 0xcb, 0xb4, 0x54, 0x83, 0x48, 0x3b, 0xc8, 0x0f, 0x8f};

// The field's scene sets (item 4). FIELD and ARC_SCENE are the CSymbol64 hashes of the model component and scene set
// fix_dot_field.py names (ARC_SCENE: the arc mine's arming-bubble scene set, which the field shows with its bubble).
inline constexpr std::uint64_t kFieldComponent = 0x51b9f7653ecac7cbULL;
inline constexpr std::uint64_t kArcScene = 0x0c8dd23bd40ba0c4ULL;
inline constexpr std::uint32_t kShowSceneSetRva = 0xCFE0;   // FUN_18000CFE0, node 9507dc91: ARC_SCENE on
inline constexpr std::uint32_t kHideSceneSetRva = 0xDA00;   // FUN_18000DA00, node 69412c20: ARC_SCENE off
inline constexpr std::uint32_t kBubbleSceneSetRva = 0xAC90; // FUN_18000AC90, node e94b7b9c: stores ARC_SCENE with flag 0
inline constexpr std::uint32_t kReadContextVariablesRva = 0x4270;  // the script's helper the three nodes call
inline constexpr std::uint64_t kShowNode = 0x9507dc91;
inline constexpr std::uint64_t kSoundNode = 0xf2a71917;         // the field sound's node (fix_dot_field.py LINKS[0])
inline constexpr std::uint64_t kSoundStartNext = 0x1f2a71917;   // the field sound's start (stock result of kSoundNode)
inline constexpr std::uint64_t kShowStockNext = 0xcd089ec5;     // the show node's stock result: its end
inline constexpr std::uint64_t kChainEnd = ~std::uint64_t{0};   // next_node's -1

// The 16-byte prologues of the three nodes, read at the RVAs above: `mov rax,rsp; sub rsp,0x68;
// movdqa xmm0,[rip+disp32]` (the scene-set constant's load) and the first byte of the next store (c7: `mov dword`).
inline constexpr std::uint8_t kShowSceneSetPrologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x83, 0xec, 0x68, 0x66,
                                                         0x0f, 0x6f, 0x05, 0xa1, 0xe9, 0x00, 0x00, 0xc7};
inline constexpr std::uint8_t kHideSceneSetPrologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x83, 0xec, 0x68, 0x66,
                                                         0x0f, 0x6f, 0x05, 0x81, 0xdf, 0x00, 0x00, 0xc7};
inline constexpr std::uint8_t kBubbleSceneSetPrologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x83, 0xec, 0x68, 0x66,
                                                           0x0f, 0x6f, 0x05, 0xf1, 0x0c, 0x01, 0x00, 0xc7};
// `mov [rsp+8],rbx; push rdi; sub rsp,0x20; cmp qword [rdx+0x18],0`: the helper's first bytes (its body is the engine's
// read; the row pins it so a different build refuses the script). WEAK PIN: these 15 bytes are identical to
// kPlaySoundPrologue (Execute at 0x3D10), so the pin does not tell the helper from Execute. Not lengthened: the bytes
// past these are not read in this tree (see the commit's Not-established section).
inline constexpr std::uint8_t kReadContextPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                        0xec, 0x20, 0x48, 0x83, 0x7a, 0x18, 0x00};

/// The event a PlaySound node plays: the stun's for the DOT's two, else unchanged.
std::uint32_t Event(std::uint32_t stock);

/// The next node: after the setup node, past the three scene-set nodes; the field's sound leads to the show node and the
/// show node ends the chain; else the game's.
std::uint64_t NextNode(std::uint64_t node, std::uint64_t stock);

/// The argument block a scene-set node passes to the helper (its stack block in the game: the three context values at
/// +0x08, +0x10, +0x18, the component and the scene at +0x20 and +0x28, a zero dword at +0x30, the flag word at +0x34 and
/// 0xFF at +0x38; the layout is the decompiler's local_* order, so +0x38 is read off the names, not measured).
inline constexpr std::size_t kBlockSize = 0x40;
inline constexpr std::size_t kBlockComponent = 0x20;
inline constexpr std::size_t kBlockScene = 0x28;
inline constexpr std::size_t kBlockFlag = 0x34;
inline constexpr std::size_t kBlockTail = 0x38;
inline constexpr std::uint32_t kBlockTailValue = 0xFF;
inline constexpr std::size_t kContextPointer = 0x68;   // the node's object holds its context at +0x68
inline constexpr std::size_t kContextFirst = 0xB0;     // the context's three values, from +0xB0 in 8-byte steps
inline constexpr std::size_t kBlockContextValues[] = {0x08, 0x10, 0x18};  // where the three land in the block

/// Fills the block a scene-set node passes: the DOT's (FIELD, scene) pair and the flag. Exposed for tests.
void FillSceneSetBlock(std::uint8_t* block, const std::uint8_t* context, std::uint64_t scene, std::uint16_t flag);

/// The body the three rebuilt scene-set nodes share: fills the block from the node's context, then calls the helper on
/// the block and the node and returns the helper's result, as the stock node does (its tail is `call helper; ret`, and
/// the dispatcher reads rax). With no module base (the rows are not installed) it calls nothing and returns 0, the
/// game's success value. Exposed for tests.
std::uint64_t SceneSetNode(std::uint64_t* node, std::uint16_t flag);

/// Registers the hooks with the script hook table: the two stock E rows, the three scene-set N rows, and the helper's
/// pin. Once.
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::dot_script
