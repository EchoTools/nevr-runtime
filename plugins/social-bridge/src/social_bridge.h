/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include "nevr_plugin_interface.h"
#include <cstdint>

// Initialize the social bridge: resolve game function pointers.
// Returns 0 on success, negative on failure.
int SocialBridgeInit(uintptr_t base_addr);

// Handle game state transitions. Triggers social feature setup on lobby entry.
void SocialBridgeOnStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state);

// Clean up Nakama client and release resources.
void SocialBridgeShutdown();
