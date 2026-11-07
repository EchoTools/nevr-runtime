#pragma once

// TEAM READY in Combat private matches (#600, EchoCombat slice 7 PR 3; he_is_the_cat's fix_team_ready.py, read as text,
// reconstructed here as hooks on Combat's game-mode script, mp_combat_server_state 6fa8cc9cd721a563):
//
// 1. Each delivery of nevt_blue_team_ready / nevt_red_team_ready runs R15NetSetFlag blue_team_ready / red_team_ready with
//    the value of expression 7b6a5f5f / 79460fd8 (RVA 0x26480 / 0x265D0), which is `!flag`: every delivery flips the
//    flag. With more than one client the event reaches the server again and flips it back, and the match waits in
//    pre_match_wait_for_ready. Both values become a constant true (the round reset still clears the flags).
// 2. The handlers run only when their shouldexecute (blue 0x25730, red 0x2B150) holds, and it requires a sender that is
//    not a demo account (a -noovr client): its demo field is read through the engine accessor the script calls at slot
//    RVA 0x33BC0 (`call [rip+...]` at 0x2599b / 0x2b3bb), for the field whose id setup_bindings stores at RVA 0x33968.
//    So a demo account can never ready its team. The shouldexecutes are wrapped to mark the thread, and the slot is
//    replaced by one that calls the engine and then reports "not a demo" for that field only while a shouldexecute runs.
//    The accessor's other callers in the script are untouched.

#include <cstdint>

namespace nevr_combat::team_ready {

inline constexpr std::uint64_t kScript = 0x6fa8cc9cd721a563ULL;
inline constexpr std::uint32_t kBlueValueRva = 0x26480;
inline constexpr std::uint32_t kRedValueRva = 0x265D0;
inline constexpr std::uint32_t kBlueShouldExecuteRva = 0x25730;
inline constexpr std::uint32_t kRedShouldExecuteRva = 0x2B150;
inline constexpr std::uint32_t kAccessorSlotRva = 0x33BC0;
inline constexpr std::uint32_t kDemoFieldIdRva = 0x33968;

// `sub rsp, 0x358; mov rax, imm64`: the start of both value expressions.
inline constexpr std::uint8_t kValuePrologue[] = {0x48, 0x81, 0xec, 0x58, 0x03, 0x00, 0x00, 0x48, 0xb8};
// `mov [rsp+8],rbx; mov [rsp+0x10],rsi; mov [rsp+0x18],rdi; push rbp`: the start of both shouldexecutes.
inline constexpr std::uint8_t kShouldExecutePrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                          0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x55};

using AccessorFn = std::uint64_t (*)(void* ctx, std::uint64_t field, void* out);

/// What the replaced accessor does around the engine's: inside a shouldexecute and for the demo field, the value read is
/// "not a demo" (0). The field id is a dword: the script loads it with `mov edx, dword [rip+0x33968]` (8b 15, at
/// 0x25989 and 0x2b3a9) and binds the next dword, 0x3396c, as another field; so it is compared with the low dword of
/// `field`. Exposed for tests.
std::uint64_t ReadField(AccessorFn engine, std::uint32_t demo_field, bool in_should_execute, void* ctx,
                        std::uint64_t field, void* out);

/// Registers the rows with the script hook table. Once.
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::team_ready
