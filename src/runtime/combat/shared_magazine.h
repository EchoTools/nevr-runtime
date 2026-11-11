#pragma once

// The shared magazine script's frisbee condition (#600, EchoCombat slice 7 PR 6; he_is_the_cat's fix_shared_frisbee.py,
// read as text, reconstructed here as hooks on the shared magazine script, 190ed14fe6c74dd3).
//
// On every throw the script runs node 93def80d, "magazine type != arc", and when it holds node e8657155 enables the
// magazine's CR15FrisbeeCR. The arc is left out on purpose (without the frisbee its dampener slows it to a stop). The
// DOT FIELD and the drone fly like the arc and have no frisbee component, so each throw of either logged "Scriptnode
// can't find actor/component ... Target Component: 0x84C65832F0B8B181". The condition becomes "type is not arc, dot or
// drone".
//
// The condition (expression_93def80d_condition, RVA 0x1F290, `bool (thread)`) reads the type through the engine's field
// accessor the script calls at slot RVA 0x2BD60 (`call [rip]` at 0x1F2EF), for the field id at 0x2B194, loaded as a
// dword (`mov edx, dword [rip]`, 8b 15, at 0x1F2DC), into a qword it compares in full (`cmp [rsp+0x28], rax` at 0x1F2FF).
// The condition is wrapped to mark the thread, and the slot is replaced by one that calls the engine and, while the
// condition runs and for that field only, records the qword type it read; after the condition returns, a recorded dot
// or drone makes the result false. The accessor's other callers (19 more: 20 `call [rip+0x2BD60]` in the script, the condition's among them) are untouched. The
// symbols are compared whole: his patch compared their low 16 bits, which are distinct among the seven ordnance types.

#include <cstdint>

namespace nevr_combat::shared_magazine {

inline constexpr std::uint64_t kScript = 0x190ed14fe6c74dd3ULL;
inline constexpr std::uint32_t kConditionRva = 0x1F290;
inline constexpr std::uint32_t kAccessorSlotRva = 0x2BD60;
inline constexpr std::uint32_t kTypeFieldIdRva = 0x2B194;

inline constexpr std::uint64_t kArc = 0xc8e8d0b1a891f0e9ULL;    // CSymbol64("arc")
inline constexpr std::uint64_t kDot = 0xc8e8d0b1a894edfeULL;    // CSymbol64("dot")
inline constexpr std::uint64_t kDrone = 0xe32dc7decb804ab5ULL;  // CSymbol64("drone")

// `sub rsp, 0x388; mov rax, [rcx]; movdqa xmm0, [rip+...]`: the condition's first bytes.
inline constexpr std::uint8_t kConditionPrologue[] = {0x48, 0x81, 0xec, 0x88, 0x03, 0x00, 0x00,
                                                      0x48, 0x8b, 0x01, 0x66, 0x0f, 0x6f, 0x05};

using AccessorFn = std::uint64_t (*)(void* ctx, std::uint64_t field, void* out);

/// Whether the frisbee is enabled: the game's answer (type != arc), and not for a dot or a drone.
bool Enables(bool stock, bool type_seen, std::uint64_t type);

/// What the replaced accessor does around the engine's: inside the condition, for the type field (compared on the low
/// dword: the id is a dword), it records the qword the engine wrote. Exposed for tests.
std::uint64_t ReadField(AccessorFn engine, std::uint32_t type_field, bool in_condition, void* ctx, std::uint64_t field,
                        void* out);

/// The type the last ReadField recorded on this thread inside the condition, and whether one was.
bool RecordedType(std::uint64_t* type);

/// Registers the hook and the slot with the script hook table. Once.
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::shared_magazine
