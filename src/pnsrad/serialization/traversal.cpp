// @module: pnsrad.dll
// @purpose: CNSUser::CTraversal — Concrete traversal visitor for user profile fields.
//
// CNSUser::CTraversal is a CJsonTraversal subclass that reads typed values from
// the profile JSON and forwards them to the bound CNSUser object via its vtable.
// Each Process* method calls the base CJsonTraversal handler, then checks if the
// user is online, reads the typed value, and dispatches to the user's vtable.
//
// RTTI: "?AVCTraversal@CNSUser@NRadEngine@@" at 0x142064d18 (echovr.exe)
// Source: d:\projects\rad\dev\src\engine\libs\netservice\cnsuser.cpp

#include "pnsrad/serialization/traversal.h"
#include "pnsrad/serialization/json_traversal.h"

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

// @0x180097fb0
extern uint32_t CJson_ReadBool(void* json, void* path, uint32_t defaultVal, uint32_t flags);
// @0x18009afa0
extern int64_t CJson_ReadInt(void* json, void* path, int64_t defaultVal, uint32_t flags);
// @0x18009dda0
extern float CJson_ReadFloat(void* json, void* path, float defaultVal, uint32_t flags);
// @0x18009ecd0
extern const char* CJson_ReadString(void* json, void* path, const char* defaultVal, uint32_t flags);
// @0x1800a51b0 — Returns pointer to offline user ID for a given provider.
extern int64_t* GetOfflineUserId(void* outBuf, uint32_t providerId);

extern const char g_emptyString; // @0x180222db4

// CNSUser::CTraversal extends CJsonTraversal with a user pointer at +0x20.
// CNSUser layout offsets referenced:
//   +0x88: int64_t  accountId
//   +0x90: uint32_t loginState (low nibble: state enum, bit 4: isDemo)
//   +0x98: uint32_t providerId
// CNSUser vtable offsets dispatched to:
//   +0x20: user ProcessString  (from CTraversal ProcessBoolean — parses int)
//   +0x28: user ProcessInt     (from CTraversal ProcessInt — parses bool)
//   +0x30: user ProcessFloat   (from CTraversal ProcessReal)
//   +0x38: user ProcessString2 (from CTraversal ProcessString)

static void* GetUser(CTraversal* self) {
    return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(self) + 0x20);
}

// Demo users and disconnected users with no offline ID should not receive
// profile field updates during traversal.
static bool IsUserOnline(void* user) {
    auto p = reinterpret_cast<uintptr_t>(user);
    uint32_t loginState = *reinterpret_cast<uint32_t*>(p + 0x90);

    if ((loginState >> 4) & 1) {
        return false;
    }

    if (*reinterpret_cast<int64_t*>(p + 0x88) != 0) {
        return true;
    }

    uint32_t providerId = *reinterpret_cast<uint32_t*>(p + 0x98);
    uint8_t buf[8];
    int64_t* offlineId = GetOfflineUserId(buf, providerId);
    return (*offlineId != 0);
}

// Helper: call a user vtable method at a given byte offset.
template<typename Fn>
static Fn GetUserVTableFn(void* user, size_t offset) {
    auto vtable = *reinterpret_cast<uintptr_t*>(user);
    return reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(vtable + offset));
}

// @addr: 0x18008f8e0 (pnsrad.dll)
// @original: NRadEngine::CNSUser::CTraversal::ProcessInt (vfunction1)
// Reads a boolean from source JSON, forwards to user vtable +0x28.
// @confidence: H
void CNSUser_CTraversal_ProcessInt(CTraversal* self, const char* key, void* jsonPath, ...) {
    // Base CJsonTraversal::vfunction1 call elided — inlined by compiler in pnsrad.dll
    void* user = GetUser(self);
    if (!IsUserOnline(user)) return;

    uint32_t value = CJson_ReadBool(self->source, jsonPath, 0, 0);
    GetUserVTableFn<void(*)(void*, const char*, uint32_t)>(user, 0x28)(user, key, value);
}

// @addr: 0x18008f970 (pnsrad.dll)
// @original: NRadEngine::CNSUser::CTraversal::ProcessBoolean (vfunction2)
// Reads an int64 from source JSON, forwards to user vtable +0x20.
// @confidence: H
void CNSUser_CTraversal_ProcessBoolean(CTraversal* self, const char* key, const char* value, ...) {
    void* user = GetUser(self);
    if (!IsUserOnline(user)) return;

    int64_t parsedValue = CJson_ReadInt(self->source, const_cast<char*>(value), 0, 0);
    GetUserVTableFn<void(*)(void*, const char*, int64_t)>(user, 0x20)(user, key, parsedValue);
}

// @addr: 0x18008fa10 (pnsrad.dll)
// @original: NRadEngine::CNSUser::CTraversal::ProcessReal (vfunction3)
// Reads a float from source JSON, forwards to user vtable +0x30.
// @confidence: H
void CNSUser_CTraversal_ProcessReal(CTraversal* self, const char* key, const char* value, ...) {
    void* user = GetUser(self);
    if (!IsUserOnline(user)) return;

    float parsedValue = CJson_ReadFloat(self->source, const_cast<char*>(value), 0.0f, 0);
    GetUserVTableFn<void(*)(void*, const char*, float)>(user, 0x30)(user, key, parsedValue);
}

// @addr: 0x18008faa0 (pnsrad.dll)
// @original: NRadEngine::CNSUser::CTraversal::ProcessString (vfunction4)
// Reads a string from source JSON, forwards to user vtable +0x38.
// @confidence: H
void CNSUser_CTraversal_ProcessString(CTraversal* self, const char* key, const char* value, ...) {
    void* user = GetUser(self);
    if (!IsUserOnline(user)) return;

    const char* parsedValue = CJson_ReadString(self->source, const_cast<char*>(value), &g_emptyString, 0);
    GetUserVTableFn<void(*)(void*, const char*, const char*)>(user, 0x38)(user, key, parsedValue);
}

// @addr: 0x180091690 (pnsrad.dll)
// @original: NRadEngine::CNSUser::CTraversal::CTraversal
// Constructs a traversal bound to a user, executes on the server profile,
// then saves to disk.
// @confidence: H
void CNSUser_CTraversal_Ctor(void* self, void* user) {
    auto p = reinterpret_cast<uintptr_t>(self);

    // vtable is set by caller; zero source, target, flags
    *reinterpret_cast<uint64_t*>(p + 0x08) = 0;
    *reinterpret_cast<uint64_t*>(p + 0x10) = 0;
    *reinterpret_cast<uint64_t*>(p + 0x18) = 0;
    *reinterpret_cast<void**>(p + 0x20) = self;

    auto traversal = reinterpret_cast<CJsonTraversal*>(self);
    auto serverProfile = reinterpret_cast<void*>(p + 0x38);

    CJsonTraversal_ExecuteNamed(traversal, reinterpret_cast<CJson*>(serverProfile), user, "");

    extern void CNSUser_SaveLocalData(void* user, void* json, const char* filename);
    CNSUser_SaveLocalData(self, serverProfile, "serverprofile.json");
}

} // namespace NRadEngine
