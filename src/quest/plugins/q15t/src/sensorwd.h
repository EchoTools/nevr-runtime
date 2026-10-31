// sensorwd (source build FX-B T-07, the sensorwd.on half; the source build's fx "fxsensor.on / sensorwd.on"): the threat-scanner sensor
// sphere (model record ac2d3f9f080ba45f, one on every player actor) is enabled by the game's TriggeredPing and disabled again
// by FinishedPing 4.0 s later. On the Quest a sphere stayed drawn for minutes. This module times each sphere enable and turns a
// sphere that is still on 6 s after its enable off again, on the game thread; a sphere it turned off comes back on at the next
// TriggeredPing of its own actor (the engine's script thinks the component is still enabled, so its own EnableModel(on) would be
// a no-op).
//
// Three data words are repointed (RELRO / data pages through got_swap.h, no code word, no PROT_EXEC): the CModelCS
// OnComponentEnabled and OnComponentDisabled vtable words, and libr15's JUMP_SLOT of CncaGameSpace::SendComponentEventGlobal
// (the event that carries TriggeredPing). Only while they hold the stock functions; the code words the wrappers' reads were
// derived from are pinned first. Details, evidence and prediction: docs/quest/q15t/modules/sensorwd.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

// ELF vaddrs of the pinned build (b243509c08ce677aeb95fa348016949b3fc45230, disassembly VA = ELF + 0x100000).
struct SensorWdAddrs {
  std::uintptr_t en_slot;     // CModelCS vtable word: OnComponentEnabled   (R_AARCH64_ABS64, RELRO)
  std::uintptr_t en_fn;       // CModelCS::OnComponentEnabled
  std::uintptr_t dis_slot;    // CModelCS vtable word: OnComponentDisabled  (R_AARCH64_ABS64, RELRO)
  std::uintptr_t dis_fn;      // CModelCS::OnComponentDisabled
  std::uintptr_t event_slot;  // JUMP_SLOT of CncaGameSpace::SendComponentEventGlobal(CSymbol64, SActor, CSymbol64, unsigned)
  std::uintptr_t event_fn;    // that function, in libr15 itself
};
inline constexpr SensorWdAddrs kSensorWdPinned = {0x36495c8, 0x15b127c, 0x36495d0, 0x15b12a8, 0x36fcae8, 0x180735c};

// The code words the wrappers' reads were derived from, read before any word is swapped: OnComponentEnabled (11), OnComponentDisabled
// (11) and the first 12 of SendComponentEventGlobal. The source pins none; these are an addition. No MD5 or other file-hash gate.
inline constexpr WordPin kSensorWdWordPins[] = {
    // CModelCS::OnComponentEnabled
    {0x15b127c, 0xf9406808},
    {0x15b1280, 0x92403c29},
    {0x15b1284, 0xd37ef529},
    {0x15b1288, 0x78696908},
    {0x15b128c, 0xf9408009},
    {0x15b1290, 0x8b081128},
    {0x15b1294, 0xf9400500},
    {0x15b1298, 0xb9400501},
    {0x15b129c, 0xf9400009},
    {0x15b12a0, 0xf9403122},
    {0x15b12a4, 0xd61f0040},
    // CModelCS::OnComponentDisabled
    {0x15b12a8, 0xf9406808},
    {0x15b12ac, 0x92403c29},
    {0x15b12b0, 0xd37ef529},
    {0x15b12b4, 0x2a1f03e1},
    {0x15b12b8, 0x78696908},
    {0x15b12bc, 0xf9408009},
    {0x15b12c0, 0x8b081128},
    {0x15b12c4, 0xf9400500},
    {0x15b12c8, 0xf9400008},
    {0x15b12cc, 0xf9403102},
    {0x15b12d0, 0xd61f0040},
    // CncaGameSpace::SendComponentEventGlobal
    {0x180735c, 0xd10103ff},
    {0x1807360, 0x92800008},
    {0x1807364, 0xa902fbe8},
    {0x1807368, 0xf9402c08},
    {0x180736c, 0xa9018fe2},
    {0x1807370, 0x2a0403e2},
    {0x1807374, 0xa900a3e1},
    {0x1807378, 0x910023e1},
    {0x180737c, 0x97dbd111},
    {0x1807380, 0xf9401bfe},
    {0x1807384, 0x910103ff},
    {0x1807388, 0xd65f03c0},
};
inline constexpr std::size_t kSensorWdWordPinCount = sizeof kSensorWdWordPins / sizeof kSensorWdWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not).
bool SensorWdWordPinsMatch(std::uintptr_t base);

// Gate order: `<q15t data dir>/sensorwd.off` present (reason "off_file", one INFO line) -> the code-word pins (reason
// "word_pin_mismatch", one ERROR line: pin / found / expected) -> the two vtable words and the event slot hold the stock functions
// (reason "vtable_word_not_stock" / "event_slot_not_stock", one ERROR line: pin / found / expected) -> the three swaps. A refusal
// swaps nothing. If a swap fails after another succeeded, the ones done are put back (ERROR) and the module stays off. Register
// SensorWdTick with AddGameTick separately (plugin.cpp). The returned status is kOk or the first failure.
InstallResult InstallSensorWd(std::uintptr_t base);
// The same without the kill file and the pins, on other addresses (host tests, where the "functions" are host code).
InstallResult InstallSensorWdAt(std::uintptr_t base, const SensorWdAddrs& addrs);

// A TickFn (game_tick.h), every frame on the game thread: turns stuck spheres off, forced ones back on. One bool test per frame
// when the install was refused.
void SensorWdTick(std::uintptr_t game);

struct SensorWdStats {
  bool on = false;                  // the three words are swapped
  std::uint32_t enables = 0;        // sphere enables seen (non-sensor models are not counted)
  std::uint32_t disables = 0;       // sphere disables seen
  std::uint32_t pings = 0;          // TriggeredPing events seen
  std::uint32_t finished = 0;       // FinishedPing events seen
  std::uint32_t wd_off = 0;         // stuck spheres the tick turned off
  std::uint32_t wd_back = 0;        // forced spheres the tick turned back on
  std::uint32_t table_full = 0;     // sphere enables that found no free slot
  std::uint32_t stale = 0;          // tracked spheres dropped because their component no longer reads as a sphere
  std::uint32_t pings_lost = 0;     // TriggeredPings dropped because 8 were already waiting for the tick
  std::uint32_t tracked = 0;        // slots in use now
  std::uint32_t forced = 0;         // of those, turned off by the tick
  std::uint32_t enabled = 0;        // of those, enabled and timed
};
SensorWdStats GetSensorWdStats();

// Test seams: a millisecond clock; a hook called at stage 1 (after the event slot swap), 2 (after the enable word swap), 3 (after
// the disable word swap), and, with the state lock held, 11 (sphere enable / disable), 12 (tick), 13 (TriggeredPing), 14 (a dead
// sphere dropped), 15 (the sweep's read of the slots), 16 (GetSensorWdStats); the wrappers' on switch; whether the state lock is
// held right now; a reset of every piece of state. The kill-file directory is module_switch.h's SetModuleSwitchDirForTest.
void SetSensorWdClockForTest(std::int64_t (*clock_ms)());
void SetSensorWdHookForTest(void (*hook)(int stage));
void SetSensorWdOnForTest(bool on);
bool SensorWdLockHeldForTest();
void ResetSensorWdForTest();

}  // namespace q15t
