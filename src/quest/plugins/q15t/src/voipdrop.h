// voipdrop (source build RE-33, the source build's voipdrop; row R30; default ON, option Q15T_VOIPDROP): a remote voice packet is dropped
// when the receiver's voice hash table needs a page and the page cannot be allocated.
//
// The crash it guards: CHashTable<uint,ull>::AllocPage asks the table's allocator for one page (table+0x64 bytes) and never
// checks the result; a NULL page is then written from its end backwards (SIGSEGV in CR15NetVoipReceiverCS::ReceiveVoip ->
// FindOrCreate -> AllocPage, tester dump c63511b8). Wraps libr15's JUMP_SLOT for ReceiveVoip (ELF 0x36c5d80 -> the function at
// ELF 0x21c3238). When the table's free head ([cs+0x1b8] int32) is -1 the next packet needs a page: the wrapper asks the same
// allocator, the same way (CMemoryContext::SetCurrent(CMemBlock::Allocator(cs+0x180)), vtable +0x10 with [cs+0x1cc] bytes) and
// gives the block straight back (vtable +0x38); a failed request drops the packet (one INFO line, capped). Every other packet,
// and every doubt (an unreadable field or vtable), goes to the real function unchanged. The probe is not a reservation: it
// narrows the crash to a race, it does not close it. Data-page hook only; no code word is written.
// Details, evidence and prediction: docs/quest/q15t/modules/voipdrop.md.
#pragma once

#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

inline constexpr std::uintptr_t kVoipDropSlot = 0x36c5d80;
inline constexpr std::uintptr_t kVoipDropReal = 0x21c3238;

// The four engine exports the probe makes (all exported by the game library).
struct VoipDropFns {
  void* (*block_allocator)(const void* block);  // CMemBlock::Allocator() const
  void* (*current_ptr)();                       // CMemoryContext::CurrentPtr()
  void (*set_current)(void* allocator);         // CMemoryContext::SetCurrent(CAllocator*)
  void* (*current)();                           // CMemoryContext::Current()
};

// dlsym on the loaded game library (RTLD_NOLOAD). False when one is missing.
bool ResolveVoipDropFns(VoipDropFns* out);

// `<q15t data dir>/voipdrop.off` present: refused (reason "off_file", one INFO line). A missing export: reason
// "export_missing", one ERROR line. Nothing is swapped in either case.
inline constexpr WordPin kVoipDropWordPins[] = {
    {0x21c3238, 0xd10203ff},
    {0x21c3698, 0x9105a2a0},
    {0x21c36b4, 0x97b5dab7},
    {0xedcd30, 0xb0013f50},
    {0xedcd34, 0xf946c211},
    {0xedcd38, 0x91360210},
    {0x23164d8, 0x91006016},
    {0x2316504, 0xb9406661},
    {0x2316508, 0xf9400908},
    {0x23165a8, 0xb9005269},
};
inline constexpr std::size_t kVoipDropWordPinCount = sizeof kVoipDropWordPins / sizeof kVoipDropWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not). No MD5 or other file-hash gate.
bool VoipDropWordPinsMatch(std::uintptr_t base);

InstallResult InstallVoipDrop(std::uintptr_t base);
InstallResult InstallVoipDropAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr, const VoipDropFns& fns);

// Packets dropped so far.
std::uint32_t VoipDropCount();

// Test seams.
void SetVoipDropDirForTest(const char* dir);
void ResetVoipDropForTest();

}  // namespace q15t
