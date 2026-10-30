// T-26 (q15t the source build's gunconnect "T-26"): a new profile starts with the gun on the RIGHT hand, once per user id.
// Runs on the game-thread tick (game_tick.h), 1 Hz until done. Profile id = CR15NetGame::ClientProfile() "xplatformid";
// a marker file `t26arm.<id>` in the plugin's data directory means this profile was handled before -> nothing.
// Otherwise the local equipment component system is found in the gamespace tables (gamespace.h), held for 3 passes, and
//   - GetWeaponArm == 1 or profile "weaponarm" == 1: marker only (the player's own right-hand choice stands);
//   - else SetWeaponArm(cs, handle, 1) once (direct when [gs+0x368] is set, else through the gamespace's deferred method
//     queue), and the marker once GetWeaponArm reads 1 (10 s, else no marker: the next launch retries).
// Engine calls only (exported names of the game library); no code is written, no word is changed.
#pragma once

#include <cstdint>

namespace q15t {

struct WeaponArmFns {
  void* (*net_game)(void* game);                                              // CR15Game::NetGameMT
  void* (*client_profile)(void* net);                                         // CR15NetGame::ClientProfile
  const char* (*t_string)(void* json, const char* key, const char* dflt, unsigned flags);  // CJson::TString
  long long (*json_int)(void* json, const char* key, long long dflt, unsigned flags);      // CJson::Int
  bool (*is_valid)(void* cs, std::uint64_t handle);                           // CR15NetPlayerEquipmentCS::IsComponentValid
  std::uint32_t (*get_arm)(void* cs, std::uint64_t handle);                   // ::GetWeaponArm
  void (*set_arm)(void* cs, std::uint64_t handle, std::uint64_t arm);         // ::SetWeaponArm
  void (*invoke)(void* functor, char* buf);                                   // CDeferredMethodQueue::SFunctor2<...>::InvokeMethod
  void* (*queue)(void* queue_obj, void (*invoke)(void*, char*));              // CDeferredMethodQueue::QueueFunctor<...>
};

// Looks the nine exports up in the loaded game library (dlopen RTLD_NOLOAD). False when one is missing.
bool ResolveWeaponArmFns(WeaponArmFns* out);

// `base`: the game library's load base (the equipment CS vptr is base + its ELF vaddr).
void InitWeaponArm(std::uintptr_t base, const WeaponArmFns& fns);

// A TickFn (game_tick.h). One bool test per frame once done.
void WeaponArmTick(std::uintptr_t game);

// 0 looking, 1 set sent (verifying), 2 done
int WeaponArmPhase();

// Test seams: a clock, and a data directory override.
void SetWeaponArmClockForTest(double (*clock)());
void SetWeaponArmDirForTest(const char* dir);
void ResetWeaponArmForTest();

}  // namespace q15t
