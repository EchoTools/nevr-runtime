/* One writer of config.yaml at a time (#2082).
 *
 * The sentinel has two poller threads that write a choice the player made in the menu: the Texture Resolution control's
 * (texture_menu_hook.cpp, graphics.texture_quality) and the Controller Hand Flip toggle's (hand_flip.h, graphics.hand_flip). Both
 * go through NevrConfig::Set, which reads the file afresh, splices the key in and replaces the file through one temporary file
 * (`<file>.tmp`) and a rename. Two of them at once would share that temporary file and the later one could publish a file that
 * lacks the earlier one's key. Each takes this lock around its Set, so the second reads the file the first published.
 *
 * Only the poller threads take it, never the game's call path (nothing on that path writes a file). A pthread mutex with a constant
 * initializer has no destructor and adds no load-time initializer (tools/check_quest_static_init.sh).
 */
#pragma once

#include <pthread.h>

namespace nevr_quest {

inline pthread_mutex_t g_configWriteMutex = PTHREAD_MUTEX_INITIALIZER;

class ConfigWriteLock {
 public:
  ConfigWriteLock() noexcept { pthread_mutex_lock(&g_configWriteMutex); }
  ~ConfigWriteLock() { pthread_mutex_unlock(&g_configWriteMutex); }
  ConfigWriteLock(const ConfigWriteLock&) = delete;
  ConfigWriteLock& operator=(const ConfigWriteLock&) = delete;
};

}  // namespace nevr_quest
