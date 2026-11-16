#pragma once

// Announcer callouts for the gear that has none (#2242; he_is_the_cat's fix_vo.py at 82ad7ba, read as text, reconstructed
// here as three function-entry hooks on the announcer script, 7c10e713aec2f12b, stock sha256 952fa974b37cdb35...).
//
// When an item is equipped, the equip script fires the event CSymbol64("evt_vo_hra_equip_<kind>_<gear>"). The announcer
// has a listener for twenty of those events. Each starts the PULSAR thread (node 0x72657a17), which plays the line's
// Wwise event from its own play node. Six lines have no listener of their own: translocator, dot field, drone, umbral
// veil, overcharge and point. The seventh, blazar, is the stock chain listener (its event is listed by the game, its node
// 0x9b7c22fb, busy count at +0x20, and it plays Wwise 0x5e486409, the id of vo_hra_gun_equip_blazar). This module routes
// blazar to PULSAR instead, as fix_vo.py does: its handler returns before the stock listeners, "so chain's own listener
// does not also run" (fix_vo.py, the handler's comment). The stock chain path is therefore not taken for a blazar event. A dedicated server's content sets are never fed to this module (PLAN-v5 R6-a-2b; cdn_start.cpp PublishServerSet).
//
// Pieces of the stock script these hooks use:
//
//  * The event handler, FUN_180004b30 (.pdata 0x4b30..0x51d2). Before the stock listeners run, each of the seven events
//    is checked. A match stops there: when the PULSAR node is idle (its busy counter at the script's counters +0x0c is
//    below 1), the line's Wwise event becomes the override and the PULSAR thread starts with the thread data its own
//    listener writes; when PULSAR is busy the event is taken and nothing starts. Any other event goes to the stock handler.
//  * The event-list query at 0x3230 (a leaf, no .pdata). The game's key (0x41d2c22b1f2d130d) gets its 20-entry name
//    table; this module returns that table followed by six names of ours (blazar's name is in the game's list already).
//  * read_sound_cc_context, 0x17E0 (.pdata 0x17e0..0x196f): every node's sound is read through it, with the node's Wwise
//    event in the output state at +0x28. The PULSAR node's event is 0x18A0F124. When the override is set, that event
//    becomes the override and the override is cleared, as fix_vo.py does at PULSAR's play node. The nineteen other node
//    events, and PULSAR's with no override, reach the stock read unchanged.
//
// The stock release of a node thread (release_node_entry, 0x5fd0) decrements the busy counter at node+0x70, which the
// handler points at PULSAR's counter, so a thread that ends releases the counter whatever it played.
//
// What ends a node thread (7c10e713aec2f12b.dll): execute_node_block (0x2bf0) runs the node function (0x32a0) for each of
// the script's twenty thread slots and releases the slot (release_node_entry, 0x5fd0: the counter at node+0x70 goes down)
// unless the node function returned -1. The PULSAR node (0x3a70) sets the Wwise event in its output state and tail-calls
// read_sound_cc_context; that returns the engine callback's result (the call through [0xA0E8] with (node, 0)). The script
// never looks the Wwise id up, so whether a thread playing an id with no sound object returns -1 is the engine
// callback's, not the script's: the disassembly of the script does not show that an unknown id completes the thread.
// The hooks therefore install only where the id's record is mounted (below), and the stock chain listener for blazar
// (0x4dd7: its own node 0x9b7c22fb and counter +0x20, Wwise 0x5e486409 = vo_hra_gun_equip_blazar) is replaced by PULSAR's
// path only then; with the records absent the stock chain path runs as before.
//
// KNOWN-WRONG #2242: the Quest has no announcer callouts. The Quest sentinel has no script-hook table for the announcer
// script (src/quest/combat holds the other EchoCombat pieces), so the six lines stay silent there until the Quest twin lands.
//
// The hooks install only where the game holds the patched sound bank when it reads its sound resources: the bank record
// itself (type kBankType, name kBankName, the Wwise bank 03f1464db1fe49b5 under the combat_content label, in
// cdn_manifest as windows-x86_64) must be in the set the CDN published at the boot mount, which is built from the store
// before the game reads its resources (Shadow::Find, tagged with the label's index). A label that mounted without that
// record does not count, and a set that first carries the bank from the mid-run fetch registers nothing this run: the game
// has already read the stock bank, so the callouts start at the next launch (one Info line says so). OnPublished is that
// seam (cdn_start.cpp). A dedicated server never runs the CDN (boot.cpp starts it on a client only), so it never registers
// these hooks; it plays no announcer audio. Until the hooks register, the stock announcer runs unchanged. The override is
// cleared when the announcer script is freed.
//
// Rows check the stock bytes at each entry before anything is installed (script_hooks.h), and the table is the only way in.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/resource_shadow.h"
#include "nevr/symbol_hash.h"

namespace nevr_combat::announcer {

inline constexpr std::uint64_t kScript = 0x7c10e713aec2f12bULL;

inline constexpr std::uint32_t kHandlerRva = 0x4B30;        // FUN_180004b30: the event handler (.pdata 0x4b30..0x51d2)
inline constexpr std::uint32_t kStartThreadRva = 0x5BC0;    // init_node_entry (.pdata 0x5bc0..0x5d26): (script, node)
inline constexpr std::uint32_t kEventsQueryRva = 0x3230;    // the event-list query (a leaf, no .pdata)
inline constexpr std::uint32_t kReadContextRva = 0x17E0;    // read_sound_cc_context (.pdata 0x17e0..0x196f)
inline constexpr std::uint32_t kPulsarNode = 0x72657A17;    // PULSAR's listener node (its busy counter is at +0x0c)
inline constexpr std::uint32_t kPulsarEvent = 0x18A0F124;   // PULSAR's Wwise event (vo_hra_gun_equip_pulsar)
inline constexpr std::uint32_t kPulsarCounter = 0x0C;       // the counters' dword that PULSAR's busy count lives in
inline constexpr std::uint64_t kEventsKey = 0x41d2c22b1f2d130dULL;  // the query's key for its event list
inline constexpr std::size_t kGameEventCount = 20;          // the game's list
inline constexpr std::size_t kAddedNames = 6;               // ours: the lines' names the game's list does not hold
inline constexpr std::size_t kTableSize = kGameEventCount + kAddedNames;
inline constexpr std::size_t kLineCount = 7;
inline constexpr std::size_t kHookRows = 3;                 // the handler, the query and the read
inline constexpr std::uint32_t kQueryAnswered = 1;          // the query's answer when its key is known
inline constexpr std::int32_t kBusy = 1;                    // PULSAR's counter at or past this is busy

/// The label that holds the callouts' sound bank (cdn_plan.cpp: on by default).
inline constexpr const char* kRecordsLabel = "combat_content";
/// The bank's resource record in the CDN manifest (combat_content, platform windows-x86_64): its type and its name.
inline constexpr std::uint64_t kBankType = 0x6d358eef7bb85a98ULL;
inline constexpr std::uint64_t kBankName = 0x03f1464db1fe49b5ULL;

// FNV-1 (32-bit) constants, the Wwise id hash (fix_vo.py's fnv1: the lower-case name).
inline constexpr std::uint32_t kFnvOffsetBasis = 2166136261u;
inline constexpr std::uint32_t kFnvPrime = 16777619u;
inline constexpr char kAsciiUpperFirst = 'A';
inline constexpr char kAsciiUpperLast = 'Z';
inline constexpr char kAsciiCaseOffset = 'a' - 'A';

/// FNV-1 (32-bit) of the lower-case name: Wwise's id for a name.
constexpr std::uint32_t Fnv1(const char* name) {
  std::uint32_t h = kFnvOffsetBasis;
  for (const char* p = name; *p != '\0'; ++p) {
    char c = *p;
    if (c >= kAsciiUpperFirst && c <= kAsciiUpperLast) c = static_cast<char>(c + kAsciiCaseOffset);
    h = h * kFnvPrime;
    h ^= static_cast<std::uint8_t>(c);
  }
  return h;
}

/// One line. `game_name` is the event the equip script fires, evt_vo_hra_equip_<kind>_<gear>, and `game_event` its
/// CSymbol64. `wwise_name` is the Wwise event the line plays, vo_hra_<gun|grenade|ability>_equip_<name>, and `wwise`
/// its id.
struct Line {
  const char* name;
  const char* game_name;
  const char* wwise_name;
  std::uint64_t game_event;
  std::uint32_t wwise;
};

constexpr Line MakeLine(const char* name, const char* game_name, const char* wwise_name) {
  return Line{name, game_name, wwise_name, EchoVR::CSymbol64Hash(game_name), Fnv1(wwise_name)};
}

/// The lines, in fix_vo.py's order (translocator, dot_field, drone, umbral_veil, overcharge, point, blazar).
inline constexpr Line kLines[kLineCount] = {
    MakeLine("translocator", "evt_vo_hra_equip_grenade_loc", "vo_hra_grenade_equip_translocator"),
    MakeLine("dot_field", "evt_vo_hra_equip_grenade_dot", "vo_hra_grenade_equip_dot_field"),
    MakeLine("drone", "evt_vo_hra_equip_grenade_drone", "vo_hra_grenade_equip_drone"),
    MakeLine("umbral_veil", "evt_vo_hra_equip_ability_smoke", "vo_hra_ability_equip_umbral_veil"),
    MakeLine("overcharge", "evt_vo_hra_equip_ability_buff", "vo_hra_ability_equip_overcharge"),
    MakeLine("point", "evt_vo_hra_equip_ability_point", "vo_hra_ability_equip_point"),
    MakeLine("blazar", "evt_vo_hra_equip_weapon_chain", "vo_hra_gun_equip_blazar"),
};

// The stock bytes at each entry the hooks replace (checked before any install, script_hooks.h).
inline constexpr std::uint8_t kHandlerPin[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0xb8,
                                               0x95, 0x83, 0xe7, 0xf5};
inline constexpr std::uint8_t kQueryPin[] = {0x48, 0xb8, 0x0d, 0x13, 0x2d, 0x1f, 0x2b, 0xc2, 0xd2, 0x41, 0x48, 0x3b,
                                             0xc8, 0x75, 0x17, 0x48, 0x8d, 0x05, 0xba, 0x6d, 0x00, 0x00, 0x48, 0x89,
                                             0x02, 0x41, 0xc7, 0x00, 0x14, 0x00, 0x00, 0x00};
inline constexpr std::uint8_t kReadPin[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83,
                                            0x7a, 0x18, 0x00, 0x48};

/// The index of the line whose game event is `symbol`, or -1.
int FindLine(std::uint64_t symbol);

/// The thread start the handler calls: (script, node) -> the started thread's node entry.
using StartFn = std::uint8_t* (*)(void* script, std::uint32_t node);

/// The stock functions the hooks call through (the hooked entries' originals).
using StockHandler = void (*)(void* script, const std::uint64_t* event);
using StockQuery = std::uint32_t (*)(std::uint64_t key, const void** table, std::uint32_t* count);
using StockRead = std::uint64_t (*)(std::uint8_t* output, void* node);

/// The handler's work for one event. Returns true when it took the event (the stock listeners must not run for it).
bool HandleEvent(void* script, const std::uint64_t* event, StartFn start, std::atomic<std::uint32_t>& override_event);

/// The event-list query's answer. When `key` is the events key and the query answered kQueryAnswered with the game's
/// count, the game's table (`*table`, `*count` entries) is copied into `scratch` followed by our names, and `*table` and
/// `*count` are pointed at it. Returns whether it did. `scratch` holds kTableSize entries.
bool ExtendEvents(std::uint64_t key, std::uint32_t answer, const void** table, std::uint32_t* count, const void** scratch);

/// The read's override: when the output state's event (+0x28) is PULSAR's and the override is set, the event becomes the
/// override and the override is cleared.
void PlayEvent(std::uint8_t* output, std::atomic<std::uint32_t>& override_event);

/// The table's entry points: the three detours it installs at the hooked entries, and the free listener.
void HandlerDetour(void* script, const std::uint64_t* event);
std::uint32_t QueryDetour(std::uint64_t key, const void** table, std::uint32_t* count);
std::uint64_t ReadDetour(std::uint8_t* output, void* node);
void OnFreed(std::uint64_t script);

/// The three hooked entries' bodies: the handler (a taken event never reaches `stock`), the query (its answer is extended
/// by ExtendEvents), and the read (the override goes into the output state before `stock` reads it).
void RunHandler(void* script, const std::uint64_t* event, StartFn start, std::atomic<std::uint32_t>& override_event,
                StockHandler stock);
std::uint32_t RunQuery(std::uint64_t key, const void** table, std::uint32_t* count, StockQuery stock,
                       const void** scratch);
std::uint64_t RunRead(std::uint8_t* output, void* node, std::atomic<std::uint32_t>& override_event, StockRead stock);

/// Whether a published set holds the bank record, tagged with the combat_content label (`labels` is the set's label names,
/// indexed by the record's user tag).
bool HoldsBank(const nevr_shadow::Shadow* shadow, const std::vector<std::string>& labels);

/// The seam the CDN calls with each set it publishes. `boot` is true for the set the boot mount published, before the game
/// reads its resources. Registers the hooks when a boot set holds the bank; a later set that first holds it registers
/// nothing and logs once that the callouts start at the next launch; a set without it does nothing.
void OnPublished(const nevr_shadow::Shadow* shadow, const std::vector<std::string>& labels, bool boot);

/// Registers the three rows with the script hook table, and the free listener that clears the override. Once.
void Register();

/// The override the hooks use (test and diagnostic access).
std::atomic<std::uint32_t>& CurrentOverride();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
/// Whether Register has run in this process (test access).
bool RegisteredForTest();
/// The three stock-function pointers the detours call through (the trampolines the table made; test access).
struct OriginalsForTest {
  const void* handler;
  const void* query;
  const void* read;
};
OriginalsForTest Originals();
/// Whether the deferred-to-next-launch line has been written (test access).
bool DeferredForTest();
#endif

}  // namespace nevr_combat::announcer
