/* The hook registry's budget: every GotHook handle the Quest source declares, per file.
 *
 * A handle reserves one entry of the registry (got_hook.h, kRegistryCapacity) when it installs, so the handles declared in
 * the source are an upper bound on what the build can hold at once. The registry was 64 entries and nothing summed the
 * hooks against it: the crash handler's provider-library hooks (#1618) were the ones that no longer fitted, and the entry
 * they took was the matchmaking redirect's. Now the sum is a compile-time fact:
 *
 *   - the whole sum plus kGrowthHeadroom fits the registry,
 *   - the normal hooks plus the headroom fit below the critical reserve (a normal hook is refused there),
 *   - the critical hooks (the login, its prerequisites and the matchmaking redirect: the game cannot be played on the
 *     community service without them) fit the reserve, which only they may use.
 *
 * The table is checked against the source by tools/tests/test_quest_hook_budget.py: a file that declares a handle and has
 * no row, or a row whose count differs, fails there. Adding a hook is adding its handle here, and when the sum no longer
 * fits, raising kRegistryCapacity (the array costs 16 bytes an entry).
 *
 * Header only, included by got_hook.cpp so the registry itself is what the assertions guard.
 */
#pragma once

#include <cstddef>

#include "got_hook.h"

namespace sentinel {

struct HookBudgetRow {
  const char* file;       // relative to src/quest
  std::size_t handles;    // GotHook objects the file declares (an array counts its size, a macro its uses)
  std::size_t critical;   // of those, how many install with HookPriority::kCritical
};

// Room kept for what has no row yet: a hook added in the next release, a plugin's claim.
inline constexpr std::size_t kGrowthHeadroom = 32;

inline constexpr HookBudgetRow kHookBudget[] = {
    {"diag/hwdump_install.cpp", 18, 0},
    {"friends/friends_hooks_quest.cpp", 3, 0},
    {"integration/dlopen_hook.cpp", 1, 0},
    {"invite/wizard_metric_hook.cpp", 1, 0},
    {"login/login_hook.cpp", 1, 1},
    {"login/login_prerequisites_install.cpp", 22, 22},
    {"plugin/input_seam_hook.cpp", 1, 0},
    {"redirect/hook_adapter.cpp", 3, 3},
    {"sentinel/crash_first_hook.cpp", 8, 0},
    {"sentinel/entry.cpp", 1, 0},
    {"sentinel/login_prompt_hook.cpp", 3, 0},
    {"sentinel/login_title_hook.cpp", 1, 0},
    {"sentinel/menu_cards_hook.cpp", 1, 0},
    {"sentinel/obb_skip_hook.cpp", 3, 0},
    {"sentinel/os_build_gate_hook.cpp", 1, 0},
    {"sentinel/path_rewrite_hook.cpp", 8, 0},
    {"sentinel/permission_wait_hook.cpp", 2, 0},
    {"sentinel/pns_provider_hook.cpp", 4, 0},
    {"sentinel/process_end_hook.cpp", 1, 0},
    {"sentinel/quit_hook.cpp", 1, 0},
    {"sentinel/render_config_hook.cpp", 5, 0},
    {"sentinel/resource_request_hook.cpp", 1, 0},
    {"sentinel/texture_menu_hook.cpp", 5, 0},
    {"sentinel/texture_quality_hook.cpp", 2, 0},
    {"sentinel/vk_ext_window.cpp", 2, 0},
    {"sentinel/vk_free_guard.cpp", 1, 0},
    {"social/menu_version_label.cpp", 1, 0},
    {"social/social_install.cpp", 2, 0},
    {"social/social_invite_gate.cpp", 1, 0},
};

constexpr std::size_t BudgetHandles() {
  std::size_t sum = 0;
  for (const HookBudgetRow& row : kHookBudget) sum += row.handles;
  return sum;
}

constexpr std::size_t BudgetCritical() {
  std::size_t sum = 0;
  for (const HookBudgetRow& row : kHookBudget) sum += row.critical;
  return sum;
}

constexpr bool BudgetRowsAreSane() {
  for (const HookBudgetRow& row : kHookBudget) {
    if (row.critical > row.handles) return false;
  }
  return true;
}

inline constexpr std::size_t kBudgetHandles = BudgetHandles();
inline constexpr std::size_t kBudgetCritical = BudgetCritical();

static_assert(BudgetRowsAreSane(), "a budget row cannot have more critical hooks than handles");
static_assert(kBudgetHandles + kGrowthHeadroom <= kRegistryCapacity,
              "the hooks the build declares plus the growth headroom no longer fit the hook registry: raise kRegistryCapacity");
static_assert(kBudgetHandles - kBudgetCritical + kGrowthHeadroom <= kRegistryCapacity - kCriticalReserve,
              "the normal hooks plus the growth headroom no longer fit below the critical reserve");
static_assert(kBudgetCritical <= kCriticalReserve,
              "the critical hooks (login, matchmaking redirect) no longer fit the reserve only they may use");

}  // namespace sentinel
