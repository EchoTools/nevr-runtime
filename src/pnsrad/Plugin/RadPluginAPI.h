#pragma once

/* @module: pnsrad.dll */
/* @purpose: RAD Plugin API exports — all addresses from decompiled binary */

#include <cstdint>

#ifdef _WIN32
    #define PLUGIN_EXPORT __declspec(dllexport)
#else
    #define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// === Plugin Lifecycle ===

/* @addr: 0x180091b70 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginInit(void* hModule);

/* @addr: 0x180091b80 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginInitMemoryStatics();

/* @addr: 0x180091b90 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginInitNonMemoryStatics(void* init_context);

/* @addr: 0x180088d80 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void RadPluginMain();

/* @addr: 0x180088df0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t RadPluginShutdown();

// === Configuration Setters ===

/* @addr: 0x180091ba0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetAllocator(void* allocator);

/* @addr: 0x180091bb0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetEnvironment(void* env);

/* @addr: 0x180091bd0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetEnvironmentMethods(void* m1, void* m2);

/* @addr: 0x180091bf0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetFileTypes(void* ft);

/* @addr: 0x180091c10 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetPresenceFactory(void* pf);

/* @addr: 0x180091c30 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetSymbolDebugMethodsMethod(void* m1, void* m2, void* m3, void* m4);

/* @addr: 0x180091c80 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetSystemInfo(const char* info_1, const char* info_2, void* extra);

// === Initialization ===

/* @addr: 0x180088ae0 (pnsrad.dll) */ /* @confidence: H */
// Allocates and initializes CNSRAD* singletons:
// CNSRADUsers (0x428 bytes), CNSRADFriends (0x420), CNSRADParty (0x430), CNSRADActivities (0x2c8)
extern "C" PLUGIN_EXPORT void InitGlobals(void* param_1, void* param_2);

// === Object Getters ===

/* @addr: 0x180088ac0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Activities();

/* @addr: 0x180088ad0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Friends();

/* @addr: 0x180088d60 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Party();

/* @addr: 0x180088d70 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* ProviderID();

/* @addr: 0x180089040 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Users();

// === Microphone ===

/* @addr: 0x180085fc0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t AllowGuests(); // Always returns 1

/* @addr: 0x180085fb0 (pnsrad.dll) */ /* @confidence: H */
// No-op function — shared by MicDestroy, MicStart, MicStop, Update
extern "C" PLUGIN_EXPORT void MicDestroy();

/* @addr: 0x180088d10 (pnsrad.dll) */ /* @confidence: H */
// Always returns 0 — shared by MicAvailable, MicCreate, MicDetected, MicRead
extern "C" PLUGIN_EXPORT uint64_t MicAvailable();

/* @addr: 0x180088d20 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t MicBufferSize(); // Returns 24000

/* @addr: 0x180088d30 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t MicCaptureSize(); // Returns 48000 * 0xcccccccccccccccd >> 68 = 2999

/* @addr: 0x180088d50 (pnsrad.dll) */ /* @confidence: H */
// Returns 48000 — shared by MicSampleRate and VoipSampleRate
extern "C" PLUGIN_EXPORT uint64_t MicSampleRate();

// === VoIP ===

/* @addr: 0x180089050 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t VoipBufferSize(); // Returns 0x1C20 (7200)

/* @addr: 0x180089060 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipCreateDecoder(void* param_1, void* param_2);

/* @addr: 0x180089240 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipCreateEncoder();

/* @addr: 0x180089320 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipDecode(void* decoder, void* data, uint32_t size, void* out);

/* @addr: 0x180089400 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipDestroyDecoder(void* param_1);

/* @addr: 0x1800894e0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void VoipDestroyEncoder();

/* @addr: 0x180089500 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipEncode(void* data, uint32_t size, void* out, uint32_t* out_size);

/* @addr: 0x180089550 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t VoipPacketSize(); // Returns 0x3C0 (960)

/* @addr: 0x180089560 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipSetBitRate(uint32_t bitrate);
