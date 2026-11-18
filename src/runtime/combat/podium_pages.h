#pragma once

// KNOWN-WRONG #2760: NOT DONE (H-POD-a, PLAN-v7 8.2 a). The six rows' stock bytes are Cat's recorded bytes
// (fix_rules_pages.py), not reconstructed from the game's disassembly: PLAN-v7 section 16 debt, to be reconstructed through
// nevr-giveyouup after the release. NOT shown by this change: that the shipped echovr.exe's terminal matches these bytes
// (no game run here); the stock path of the enable and match checks on a page other than 1 (the fake image runs only the
// stock MenuPage body and the RULES tail); the call rate of ENABLE and MATCH (ADR 0038 measurement not taken). Register()
// installs the rows when called; combat.cpp does not call it in this change (W-REG wires it), so nothing installs. Rows()
// is the pure plan.
//
// The podium terminal's RULES page 2 (LOADOUTS), the page state and the start-button gate (#2242, H-POD-a). Ported from
// he_is_the_cat's fix_rules_pages.py, read as text at 82ad7ba (never run). The terminal script d4653fc2065e8184 is the one
// the RULES button already hooks (runtime/combat/rules_button.h); this module hooks six other functions of the same script,
// each through the script hook table (runtime/hook/script_hooks.h), which checks each row's prologue before it installs
// anything, so a script whose bytes differ runs stock with a Rule 6 Warning per row.
//
// The six rows (his site, his form, ours):
//   RESTART  0x1A1C0 RestartButtonPressed    (fix_rules_pages.py:104, :417-435) E: on page 1 of the rules page, NEXT flips
//            the page and sets the thread's next node (page 2 hides the labels, page 1 shows them; a payload map keeps the
//            page and only hides). Otherwise the stock body.
//   MENU1    0x209C0 the node that sets MenuPage = 1 (:106, :476-482) E: the stock body (it sets MenuPage), then the page
//            starts at the payload map's value. Returns 0 as the stock body does.
//   RULES    0x11310 RulesButtonPressed's MenuPage check (:136, :459-473) E: sets the page and the press flag (BACK on page 1,
//            RULES otherwise), then the stock body.
//   ENABLE   0x23810 start_button enable check 1 (:171, :564-569) E: EXEC_TRUE while MenuPage is 1, else the stock body.
//   MATCH    0x209E0 start_button enable check 2 (:172, :570-575) E: the same.
//   ALIGN    0x35720 the podium's alignment check (:79, :590-592) N: returns 1 without the stock body (his `mov al,1; ret`).
//
// Why hooks and not Cat's `jmp rel32` into a new `.rules` section: each of his cave sites needs new executable memory,
// which this tree has no pattern for. A script hook row is the tree's own pattern (MinHook trampolines, prologue checked),
// so the six sites are rows and none is a cave. ALIGN_CHECK is a row, not his three-byte write: the row returns the same 1
// and needs no patch-row kind in the table.
//
// The page and the press flag are this module's own state (PageState below), not bytes of the game's mailbox: his caves
// keep them at mailbox +10h and +17h, and no reader on main owns those bytes, so they are podium_pages' own (PLAN-v7 8.2).
// The payload map (mailbox +30h, rules::kPayloadMap) is read from the mailbox.
//
// ADR 0038 (frequency): RESTART, MENU1 and RULES run per event (a press, a menu transition). ALIGN runs from the podium's
// layout update. ENABLE and MATCH run when the terminal evaluates its start button; that call rate is NOT MEASURED (no live
// run here). Their cost on a page other than 1 is the stock call through the trampoline plus one pointer load and one
// compare. Decision for this piece: a hook, the form the tree uses; a measurement (ADR 0038 rule 2) decides it later.
//
// Registration: Rows() is pure; Register() hands the rows to the script hook table and logs once with its consequence. It is
// called from combat.cpp by W-REG, not in this change. Nothing is logged per event.

#include <cstddef>
#include <cstdint>

#include "runtime/hook/script_hooks.h"

namespace nevr_combat::podium_pages {

inline constexpr std::uint64_t kScript = 0xd4653fc2065e8184ULL;  // the podium terminal, d4653fc2065e8184.dll

// The stock RVAs and the bytes each row is written against (fix_rules_pages.py, quoted at each row above). A row whose bytes
// differ refuses the whole script.
inline constexpr std::uint32_t kRestartRva = 0x1A1C0;
inline constexpr std::uint32_t kMenu1Rva = 0x209C0;
inline constexpr std::uint32_t kRulesCheckRva = 0x11310;
inline constexpr std::uint32_t kEnableCheckRva = 0x23810;
inline constexpr std::uint32_t kMatchCheckRva = 0x209E0;
inline constexpr std::uint32_t kAlignCheckRva = 0x35720;

inline constexpr std::uint8_t kRestartBytes[] = {0x48, 0x81, 0xec, 0xa8, 0x04, 0x00, 0x00};  // sub rsp, 4A8h
inline constexpr std::uint8_t kMenu1Bytes[] = {0x48, 0x8b, 0x01, 0x48, 0x8b, 0x48, 0x68, 0x33, 0xc0, 0xc7, 0x81,
                                                0xa8, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3};  // the whole body:
// mov rax,[rcx]; mov rcx,[rax+68h]; xor eax,eax; mov [rcx+1A8h],1; ret (fix_rules_pages.py MENU1_BODY, :107)
inline constexpr std::uint8_t kRulesCheckBytes[] = {0x48, 0x83, 0xec, 0x58, 0x33, 0xd2};  // sub rsp, 58h; xor edx, edx
inline constexpr std::uint8_t kEnableCheckBytes[] = {0x48, 0x83, 0xec, 0x28, 0xe8};       // sub rsp, 28h; call cond
inline constexpr std::uint8_t kMatchCheckBytes[] = {0x48, 0x83, 0xec, 0x58, 0x33, 0xd2};  // sub rsp, 58h; xor edx, edx
inline constexpr std::uint8_t kAlignCheckBytes[] = {0x40, 0x53, 0x48, 0x81, 0xec, 0x70, 0x06};  // push rbx; sub rsp, 670h

// The script's MenuPage: the node context's first quadword is the node pointer, the node's data pointer is at [node + 68h],
// and the int MenuPage is at data + 1A8h (menu_check, fix_rules_pages.py:214-219). The stock MENU1 body (:106-107) loads
// rcx = data and stores MenuPage there, so the reader and the writer name the same field, data + 1A8h.
inline constexpr std::uint32_t kDataPointer = 0x68;
inline constexpr std::uint32_t kMenuPage = 0x1A8;
inline constexpr std::int32_t kMenuPageRules = 1;  // MenuPage 1 is the rules page

// The thread's next-node slot ([context + 10h] = the next node, then the node returns 0).
inline constexpr std::uint32_t kNodeNext = 0x10;

// The next nodes NEXT writes: HideMenuSettingsLabels' exit (page 2) and ShowMenuSettingsLabels' exit (page 1).
inline constexpr std::uint64_t kHideFrom = 0xF8953074ULL;
inline constexpr std::uint64_t kShowFrom = 0x9A09FD90ULL;

// The value the enable checks return while MenuPage is 1 (fix_rules_pages.py EXEC_TRUE: a node id of the script's graph).
inline constexpr std::uint64_t kExecTrue = 0x56CCD1BD99A59D3BULL;
// The alignment check returns 1 (his `mov al, 1; ret`).
inline constexpr std::uint64_t kAlignResult = 1;

// The page values (the page byte, the press flag). 0 is page 1; 1 is page 2.
inline constexpr std::uint8_t kPageOne = 0;
inline constexpr std::uint8_t kPageTwo = 1;
inline constexpr std::uint8_t kPressBack = 1;
inline constexpr std::uint8_t kPressRules = 2;

/// The page state this module keeps: the page (0 page 1, 1 page 2) and the press flag last set by RULES or BACK. Process
/// state, not the game's: nothing outside this module reads it (a later piece's press chain reads the press flag).
struct PageState {
  std::uint8_t page = kPageOne;
  std::uint8_t press = 0;
};

/// The MenuPage of the script's node context `ctx`: the int at ((*ctx) + kDataPointer) + kMenuPage, read as fix_rules_pages.py's
/// menu_check reads it. A null pointer on the way reads as 0 (not the rules page), so the stock body runs.
std::int32_t MenuPageOf(const void* ctx);

/// RESTART on the rules page (MenuPage is 1): NEXT. A payload map keeps the page and only hides the labels; otherwise the
/// page flips, and the labels are hidden on page 2 and shown on page 1. Returns the next node the thread takes.
std::uint64_t RestartNext(PageState& state, std::uint8_t payloadMap);

/// The node that sets MenuPage = 1 has run: the page starts on page 1, or on page 2 for a payload map (the payload byte).
void MenuOneStarts(PageState& state, std::uint8_t payloadMap);

/// RULES or BACK was pressed (RulesButtonPressed's check). BACK (`backNow`: MenuPage was 1) goes to page 1 with the press
/// flag BACK; RULES starts the page the payload map gives, with the flag RULES.
void RulesPress(PageState& state, bool backNow, std::uint8_t payloadMap);

/// Whether an enable check returns EXEC_TRUE: the script's MenuPage is the rules page.
bool EnableShowsTrue(std::int32_t menuPage);

inline constexpr std::size_t kRowCount = 6;

/// The registration plan: the six rows, in the order Register hands them to the script hook table. Pure: it names the
/// detours and the bytes, and installs nothing.
void Rows(nevr_script_hooks::Hook (&out)[kRowCount]);

/// Registers the six rows with the script hook table. Once. Called from combat.cpp by W-REG (not in this change).
void Register();

#ifdef NEVR_TEST_HOOKS
/// Forgets the page state and the registration. Tests only.
void ResetForTest();
/// Points the detours at a mailbox buffer (tests only: Register points them at echovr.exe's mailbox).
void SetMailboxForTest(const volatile std::uint8_t* mailbox);
#endif

}  // namespace nevr_combat::podium_pages
