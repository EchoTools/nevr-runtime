#pragma once

/* @module: pnsrad.dll */
/* @purpose: Global variables for plugin state */

#include "../Core/Types.h"
#include <cstdint>

// Allocator
/* @addr: 0x1803766b8 (pnsrad.dll) */
extern void* g_allocator_ptr;

// CNSRAD instances (opaque pointers — typed classes defined in Social/)
/* @addr: 0x1803765d8 (pnsrad.dll) */
extern void* g_cnsrad_users;       // CNSRADUsers* (0x428 bytes)

/* @addr: 0x1803765e0 (pnsrad.dll) */
extern void* g_cnsrad_friends;     // CNSRADFriends* (0x420 bytes)

/* @addr: 0x1803765e8 (pnsrad.dll) */
extern void* g_cnsrad_party;       // CNSRADParty* (0x430 bytes)

/* @addr: 0x1803765f0 (pnsrad.dll) */
extern void* g_cnsrad_activities;  // CNSRADActivities* (0x2c8 bytes)

/* @addr: 0x180376600 (pnsrad.dll) */
extern void* g_plugin_context;     // PluginContext* (0x40 bytes)

// Environment
/* @addr: 0x180376640 (pnsrad.dll) */
extern void* g_environment_ptr;

/* @addr: 0x180380880 (pnsrad.dll) */
extern void* g_file_types_ptr;

/* @addr: 0x180380878 (pnsrad.dll) */
extern void* g_presence_factory_ptr;

// Callbacks
/* @addr: 0x180376680 (pnsrad.dll) */
extern void* g_env_method_1;

/* @addr: 0x180376688 (pnsrad.dll) */
extern void* g_env_method_2;

/* @addr: 0x180376690 (pnsrad.dll) */
extern void* g_debug_method_1;

/* @addr: 0x180376698 (pnsrad.dll) */
extern void* g_debug_method_2;

/* @addr: 0x1803766a8 (pnsrad.dll) */
extern void* g_debug_method_3;

/* @addr: 0x1803766b0 (pnsrad.dll) */
extern void* g_debug_method_4;

// System info
/* @addr: 0x1803809f0 (pnsrad.dll) */
extern char g_system_info_1[0x80];

/* @addr: 0x180380970 (pnsrad.dll) */
extern char g_system_info_2[0x80];

/* @addr: 0x180380a70 (pnsrad.dll) */
extern void* g_system_info_extra;

// VoIP
/* @addr: 0x1803765f8 (pnsrad.dll) */
extern void* g_voip_encoder;

// SNS routing context — used by sns_message_send @0x18008b310 for SNS message dispatch
/* @addr: 0x1803765c8 (pnsrad.dll) */
extern void* g_sns_routing_ctx;        // SNS routing context for party/social messages

// CNSISocial* — social interface for user ID and session GUID lookups
/* @addr: 0x1803765d0 (pnsrad.dll) */
extern void* g_cnsrad_social;         // CNSISocial* (vtable+0x68 = GetLocalUserGUID)

// Provider
/* @addr: 0x180376590 (pnsrad.dll) */
extern void* g_provider_id;

// System info
/* @addr: 0x1803809f0 (pnsrad.dll) */
extern char g_system_info_1[0x80];

/* @addr: 0x180380970 (pnsrad.dll) */
extern char g_system_info_2[0x80];

/* @addr: 0x180380a70 (pnsrad.dll) */
extern void* g_system_info_extra;

// Init flags
/* @addr: 0x180380888 (pnsrad.dll) */
extern uint32_t g_init_counter;

/* @addr: 0x180376608 (pnsrad.dll) */
extern bool g_has_environment_feature;

/* @addr: 0x180376638 (pnsrad.dll) */
extern uint32_t g_environment_feature_value;
