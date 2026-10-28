/* nevr_autopilot's entry points (#1202, docs/adr/0025-quest-autopilot-route.md), with the two operating-system calls
 * whose failure init reports passed in, so the host test can make each one fail (src/quest/tests/autopilot_run_test.cpp).
 * The exported NvrPluginInitEx calls InitWith with the real ones. Nothing here is exported from the plugin library
 * (-fvisibility=hidden): its only exports are the four Nvr* entry points.
 */
#pragma once

#include <pthread.h>

#include "extension/plugin_interface.h"

namespace nevr_autopilot::plugin {

struct Os {
  // Opens the reporter's wake descriptor (an eventfd): the descriptor, or -1 with errno set.
  int (*openWake)() noexcept;
  // Starts the reporter thread: 0, or an error number (pthread_create's contract).
  int (*startThread)(pthread_t* thread, void* (*body)(void*), void* arg) noexcept;
};

const Os& RealOs() noexcept;

// NvrPluginInitEx's work. 0 when the filter is registered; 1 for every refusal, each logged.
int InitWith(const NvrGameContext* ctx, const char* argsJson, const Os& os);

// NvrPluginShutdown's work: stops the run and joins the reporter; logs route_stopped when the route had not ended.
void Shutdown();

}  // namespace nevr_autopilot::plugin
