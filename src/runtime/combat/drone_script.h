#pragma once

// The drone's death explosion, from the stock drone script (#2242, EchoCombat's fix_drone_fx.py as a hook, read as text
// at 82ad7ba; the stock bytes are in revault, project echovr).
//
// DroneFix (dronefix.cpp) puts an explosion on a dead drone with two things it reads from the exe at DEATH_TOKEN_RVA
// (0x2104D60): the node context of the drone's last node (+0x20), and the addresses of two stubs that spawn the effect
// (+0x28, the particle effect; +0x30, POINT's sound). Its author's drone script wrote them from a patched dispatcher. Here
// the stock script's dispatcher (execute_single_node, RVA 0x5D70; hooked at 0x5D74 where the 9 bytes below start) is
// wrapped by an assembly stub that records each node context and then runs the displaced instructions, and the two stubs
// are C++ functions that call the detonator script's own node constructor and Execute (its SPAWNPARTICLEEFFECTNODE and
// PLAYSOUNDNODE, RVAs 0x2710/0x2830 and 0x3900/0x39B0), which are called only while their bytes are the pinned ones
// (drone_script_pins.h). Nothing else is written, and the cells outside the token are left alone.
//
// Hot path (ADR 0038): the dispatcher wrapper runs once per node the drone script executes, on the game thread. Its
// work is seven register saves and restores, one call, an interlocked add to the node count and one store of the
// context. No allocation, no lock, no Log(). Its rate is not measured here (no live run; see the commit message).

#include <cstddef>
#include <cstdint>

namespace nevr_combat::drone_script {

inline constexpr std::uint64_t kScript = 0x221a0a3a2e56d247ULL;           // the stock drone script (mp_combat_ordnance_drone)
inline constexpr std::uint64_t kDetonatorScript = 0x190ed14fe3da5ed1ULL;  // the detonator (its explosion smoke and POINT's sound)

// The stock script's dispatcher: mov rax, [rcx+10h]; mov edx, 0ca5c4648h (checked against the stock bytes before the hook).
inline constexpr std::uint32_t kDispatchRva = 0x5D74;
inline constexpr std::uint8_t kDispatchBytes[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0x48, 0x46, 0x5c, 0xca};

// DroneFix's death token (dronefix.cpp DEATH_TOKEN_RVA, fix_drone_fx.py's): exe RVA; the fields this module writes.
inline constexpr std::uint32_t kTokenRva = 0x2104D60;
inline constexpr std::uint32_t kTokenNodesRun = 0x18;   // dword: nodes the dispatcher ran (counted, read by no one here)
inline constexpr std::uint32_t kTokenLastCtx = 0x20;    // qword: the context of the last node run
inline constexpr std::uint32_t kTokenSpawnStub = 0x28;  // qword: the spawn stub (SpawnDeathEffect)
inline constexpr std::uint32_t kTokenSoundStub = 0x30;  // qword: the sound stub (PlayDeathSound)

// The detonator's node functions (RVAs from its base), and their fields.
inline constexpr std::uint32_t kSpawnCtorRva = 0x2710;
inline constexpr std::uint32_t kSpawnExecRva = 0x2830;
inline constexpr std::uint32_t kSoundCtorRva = 0x3900;
inline constexpr std::uint32_t kSoundExecRva = 0x39B0;

// A node's context (the one the dispatcher receives): the state the spawn stub clears, and the byte that disables it.
inline constexpr std::uint32_t kCtxState = 0x18;     // qword
inline constexpr std::uint32_t kCtxDisabled = 0xDB;  // byte: nonzero, the stubs do nothing

// SPAWNPARTICLEEFFECTNODE: what the spawn stub writes after its constructor (the constructor sets the rest).
inline constexpr std::size_t kNodeBufferBytes = 0x100;   // the constructor writes through +0xB8
inline constexpr std::uint32_t kSpawnAsset = 0x08;       // qword: the particle effect's id
inline constexpr std::uint32_t kSpawnDurationOn = 0x11;  // byte: enableduration
inline constexpr std::uint32_t kSpawnDuration = 0x14;    // float: the duration, seconds
inline constexpr std::uint32_t kSpawnKillAtEnd = 0x18;   // byte: killatend
inline constexpr std::uint32_t kSpawnActor = 0x20;       // qword: the actor symbol ("self")
inline constexpr std::uint32_t kSpawnEmission = 0x44;    // float
inline constexpr std::uint32_t kSpawnLife = 0x48;        // float
inline constexpr std::uint32_t kSpawnColorR = 0x4C;      // float
inline constexpr std::uint32_t kSpawnColorG = 0x50;      // float
inline constexpr std::uint32_t kSpawnColorB = 0x54;      // float
inline constexpr std::uint32_t kSpawnAlpha = 0x58;       // float
inline constexpr std::uint32_t kSpawnRefPoint = 0x40;    // word: refpoint, leavebehind
inline constexpr std::uint32_t kSpawnParticle[3] = {0x78, 0x7C, 0x80};  // floats: particle scale
inline constexpr std::uint32_t kSpawnScale[3] = {0xA8, 0xAC, 0xB0};     // floats: scale
inline constexpr std::uint32_t kSpawnRing = 0x90;                        // three dwords: the cloud's offset

// PLAYSOUNDNODE: its actor and event.
inline constexpr std::uint32_t kSoundActor = 0x18;  // qword
inline constexpr std::uint32_t kSoundEvent = 0x28;  // dword

// The values the stock drone's config gives (config/mp_weapon_settings.json, grenade_properties.drone; the keys the file
// does not name take the defaults in fix_drone_fx.py): death_effect_size 0.5, death_effect_lifetime 1.0 and
// death_effect_time 2.0 seconds; the sound is POINT's (its event, veil_sound.py's POINT_SND_EVENT).
inline constexpr float kDeathScale = 0.5f;
inline constexpr float kDeathLife = 1.0f;
inline constexpr float kDeathDuration = 2.0f;
inline constexpr std::uint64_t kSelfSymbol = 0xC8C33E48297612B6ULL;   // CSymbol64("self")
inline constexpr std::uint64_t kDeathAsset = 0xC9EB32D35038EB4CULL;   // the detonator's explosion smoke
inline constexpr std::uint32_t kPointSoundEvent = 3404182162U;        // POINT's sound event

/// The dispatcher's record for one node run: the context into the token's last-context field, and the node count up by one.
/// `token` is the token block (the exe's kTokenRva). Pure, so a test can run it on a buffer.
void RecordDispatch(std::uint8_t* token, void* ctx);

/// Writes the fields the spawn stub sets after the constructor ran on `node` (kNodeBufferBytes).
void BuildSpawnNode(std::uint8_t* node, float life, float scale, float duration);

/// Writes the fields the sound stub sets after the constructor ran on `node`.
void BuildSoundNode(std::uint8_t* node, std::uint32_t event);

/// Whether `len` bytes at `at` are exactly `pin`.
bool PinnedBytesMatch(const void* at, const std::uint8_t* pin, std::size_t len);

/// Whether the detonator at `base` has its four functions as pinned (false when `base` is null).
bool DetonatorPinned(const std::uint8_t* base);

/// Whether the stubs run for a context: its disabled byte is 0.
bool ContextEnabled(const std::uint8_t* ctx);

#ifdef NEVR_TEST_HOOKS
/// Tests only: the token block the dispatcher wrapper writes (Register sets the exe's). Does not install anything.
void SetTokenForTest(std::uint8_t* token);
#endif

/// Installs the dispatcher hook (one row on the stock script) and writes the two stub addresses into the token. Called
/// once at boot, under the same gate as DroneFix (combat.cpp). A refused hook (the script's bytes differ) runs stock.
void Register();

/// The two stubs DroneFix calls through the token (+0x28, +0x30). Each does nothing unless its detonator node
/// functions are loaded and pinned and the context is enabled.
void __fastcall SpawnDeathEffect(void* ctx);
void __fastcall PlayDeathSound(void* ctx);

}  // namespace nevr_combat::drone_script
