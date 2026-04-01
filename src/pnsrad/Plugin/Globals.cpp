#include "Globals.h"

/* @module: pnsrad.dll */

// Allocator
/* @addr: 0x1803766b8 (pnsrad.dll) */
void* g_allocator_ptr = nullptr;

// CNSRAD instances
/* @addr: 0x1803765d8 (pnsrad.dll) */
void* g_cnsrad_users = nullptr;

/* @addr: 0x1803765e0 (pnsrad.dll) */
void* g_cnsrad_friends = nullptr;

/* @addr: 0x1803765e8 (pnsrad.dll) */
void* g_cnsrad_party = nullptr;

/* @addr: 0x1803765f0 (pnsrad.dll) */
void* g_cnsrad_activities = nullptr;

/* @addr: 0x180376600 (pnsrad.dll) */
void* g_plugin_context = nullptr;

// Environment
/* @addr: 0x180376640 (pnsrad.dll) */
void* g_environment_ptr = nullptr;

/* @addr: 0x180380880 (pnsrad.dll) */
void* g_file_types_ptr = nullptr;

/* @addr: 0x180380878 (pnsrad.dll) */
void* g_presence_factory_ptr = nullptr;

// Callbacks
/* @addr: 0x180376680 (pnsrad.dll) */
void* g_env_method_1 = nullptr;

/* @addr: 0x180376688 (pnsrad.dll) */
void* g_env_method_2 = nullptr;

/* @addr: 0x180376690 (pnsrad.dll) */
void* g_debug_method_1 = nullptr;

/* @addr: 0x180376698 (pnsrad.dll) */
void* g_debug_method_2 = nullptr;

/* @addr: 0x1803766a8 (pnsrad.dll) */
void* g_debug_method_3 = nullptr;

/* @addr: 0x1803766b0 (pnsrad.dll) */
void* g_debug_method_4 = nullptr;

// VoIP
/* @addr: 0x1803765f8 (pnsrad.dll) */
void* g_voip_encoder = nullptr;

// SNS routing context
/* @addr: 0x1803765c8 (pnsrad.dll) */
void* g_sns_routing_ctx = nullptr;

// CNSISocial*
/* @addr: 0x1803765d0 (pnsrad.dll) */
void* g_cnsrad_social = nullptr;

// Provider
/* @addr: 0x180376590 (pnsrad.dll) */
void* g_provider_id = nullptr;

// System info
/* @addr: 0x1803809f0 (pnsrad.dll) */
char g_system_info_1[0x80] = {};

/* @addr: 0x180380970 (pnsrad.dll) */
char g_system_info_2[0x80] = {};

/* @addr: 0x180380a70 (pnsrad.dll) */
void* g_system_info_extra = nullptr;

// Init flags
/* @addr: 0x180380888 (pnsrad.dll) */
uint32_t g_init_counter = 0;

/* @addr: 0x180376608 (pnsrad.dll) */
bool g_has_environment_feature = false;

/* @addr: 0x180376638 (pnsrad.dll) */
uint32_t g_environment_feature_value = 0;

// Vtable externals (addresses from Ghidra)
void* NRadEngine_CNSRADUsers_vftable = nullptr;
void* NRadEngine_CNSRADParty_vftable = nullptr;
