// impactsnd (source build: the source build's impactsnd v1..v6; rows R12..R16): the Quest logs "Cannot find default impact sound for
// bullet type 0x<gun>" on every bullet hit and plays no impact sound. The engine's impact table (CR15NetBulletCS::
// kSoundSettings, rows {u64 key = sym(subkey) ^ sym(gun), u32 id}) is built wrong on the Quest in two ways, and this module
// repairs the one that data writes can reach:
//   (a) the "default" symbol word D the hit code XORs into the gun symbol (R13): written with CSymbol64("default") only
//       when it reads 0 (disassembly shows the library's own static init stores it, so on the pinned build this is a no-op);
//   (b) every row's id is the stock 1 instead of the Wwise event id (FNV-1 32 of the lowercase event name): rewritten
//       in place, data writes to the 16-byte rows only (keys and order are never touched). Runs on the game tick
//       (ImpactSndTick) for every sorted / unsorted table instance of the process, and once more straight after each load
//       through a GOT wrapper on LoadSoundSettings (R14), before any hit can post the stock id.
// (c) the loader builds only 5 of the 15 rows on the stock code (its row scratch overlaps the path string). R16: the loader is
//     rebuilt in C++ (LoadSoundSettings, the wrapper runs it in place of the stock one) and appends every row; it runs only with
//     the Shutdown wrapper in and every callee stub pinned. docs/quest/q15t/modules/impactsnd.md, "R16, rebuilt".
// The four hit-code words (R15) are checked, never changed. No code page is written.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct ImpactSndRow {
  std::uint64_t key;  // CSymbol64(subkey) ^ CSymbol64(gun)
  std::uint32_t id;   // FNV-1 32 of the lowercase Wwise event name
};

// The 15 known rows (5 guns x {player, barrier, default}), as the source build's kIds.
const ImpactSndRow* ImpactSndRows(std::size_t* count);

// CSymbol64("default"): the value D must hold.
constexpr std::uint64_t kImpactDefaultSymbol = 0x2fd38a995f615b9eull;

// Checks the four hit-code words and the kSoundSettings GOT word, reads D (writes it only when it reads 0 and the build is
// not a nowrite build), swaps the vtable word of CR15NetBulletCS::Shutdown for a wrapper that holds the table lock across the
// real Shutdown (which frees the table), and swaps the GOT slot of LoadSoundSettings for the after-load wrapper. Register
// ImpactSndTick with AddGameTick separately (plugin.cpp). Row writes (tick and loader thread) take the same lock, so they may
// run on any thread; they are enabled only when the Shutdown wrapper is in: if its slot is not the pinned one the module reads
// and logs only (ERROR). The returned status is the first failing wrapper's (Shutdown, then loader), else kOk; a refusal before
// them (a word that is not the pinned one) is kUnexpectedOriginal with a reason token, nothing is written and the tick does
// nothing. A failed loader wrapper does not stop the tick.
InstallResult InstallImpactSnd(std::uintptr_t base);

// A TickFn (game_tick.h): polls the table 4 times a second. One bool test per frame when the install was refused.
void ImpactSndTick(std::uintptr_t game);

struct ImpactSndStats {
  bool on = false;                 // the install checks passed
  bool writers_ok = false;         // the Shutdown wrapper is in: row writes are enabled (else the module reads and logs only)
  std::uint32_t polls = 0;         // tick polls that ran (not the frames that returned early)
  std::uint32_t loads = 0;         // LoadSoundSettings returns seen by the wrapper
  std::uint32_t passes = 0;        // row passes that had something to write
  std::uint32_t rows_written = 0;  // ids rewritten (read back right)
  std::uint32_t rows_left = 0;     // known rows that needed a write and did not get one (nowrite, or a lost race)
  std::uint32_t skipped = 0;       // row passes that wrote nothing because the table lock was busy or the table had changed
  std::uint32_t shutdowns = 0;     // CR15NetBulletCS::Shutdown calls seen by the wrapper
};
ImpactSndStats GetImpactSndStats();

// Test seams: a clock; the nowrite build switch (Q15T_IMPACTSND_NOWRITE=1 sets it for the whole build); a hook called at
// stage 1 (between a table read and the re-read before the rows are written), stage 2 (after a row store, before its
// read-back, table lock held), stage 3 (after the store to D, before its read-back) and stage 4 (in a row pass, after the
// table was validated and before the lock is taken); a reset of every piece of state.
void SetImpactSndClockForTest(double (*clock)());
void SetImpactSndNoWriteForTest(bool nowrite);
void SetImpactSndHookForTest(void (*hook)(int stage));
void ResetImpactSndForTest();

}  // namespace q15t
