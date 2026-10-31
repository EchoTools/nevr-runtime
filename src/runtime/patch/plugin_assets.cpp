/*
 * plugin_assets.cpp — serve plugin-carried assets to the engine's resource manager.
 *
 * How the engine loads a resource (echovr.exe, ReVault decompilation):
 *   LoadResource 0x140fa2820 -> SyncResourceLoad 0x140fa36f0 / 0x140fa1890 -> factory by type 0x140fa1c50 ->
 *   0x140fa3220 (submit). 0x140fa3220 asks the resource object for the key of each buffer through its
 *   vfunc[0x48], records which buffers are outstanding at +0x68, and queues one read per buffer through
 *   0x140fa1fe0 (table lookup, package handle, chunk) / 0x1415261a0, with completion callback 0x140fa16d0.
 *   0x140fa16d0 stores the buffer at +0x40+idx*8 / +0x50+idx*8, clears the buffer's bit in +0x68 and, on the
 *   last one, schedules the resource's initialization (or runs it inline when there is no scheduler).
 *
 * A plugin asset has no package, so there is nothing to read. For a registered (type, name) the submit hook
 * does what 0x140fa3220 does before it queues, then completes each buffer itself through 0x140fa16d0 with a copy of
 * the plugin's bytes made through the engine's allocator (game_buffer.h): the engine's own completion and
 * initialization run, and the buffer it later frees is one its allocator made.
 */

#include "runtime/patch/plugin_assets.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

#include <windows.h>
#include <MinHook.h>

#include "core/globals.h"
#include "core/logging.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/patch/game_buffer.h"

namespace nevr::assets {

namespace {

// ── Engine addresses (RVA from the ImageBase 0x140000000), all read from the decompilation ───────────────

constexpr uintptr_t kRvaSubmit = 0xFA3220;       // FUN_140fa3220(manager, resource, queue)
constexpr uintptr_t kRvaRequest = 0xFA1FE0;      // FUN_140fa1fe0: table lookup + request build
constexpr uintptr_t kRvaCompletion = 0xFA16D0;   // CResourceManager::AsyncResourceIOCallback
constexpr uintptr_t kRvaIsLoaded = 0xFA3410;     // FUN_140fa3410({type, name}) -> loaded?
constexpr uintptr_t kRvaManager = 0x20A2AB8;     // DAT_1420a2ab8: the resource manager
constexpr uintptr_t kRvaSymbolTable = 0x209FE18; // DAT_14209fe18: symbol table used by 0x140fa3410
constexpr uintptr_t kRvaScheduler = 0x209FE20;   // DAT_14209fe20: task scheduler (0 = initialize inline)
constexpr uintptr_t kRvaSecondaryOn = 0x20A8A68; // DAT_1420a8a68: 0 turns the second buffer off

// Resource object layout, as read by 0x140fa3220 / 0x140fa16d0 and by the existing override hook.
constexpr size_t kObjType = 0x28;
constexpr size_t kObjName = 0x38;
constexpr size_t kObjState = 0x60;
constexpr size_t kObjOutstanding = 0x64;
constexpr size_t kObjPendingMask = 0x68;
constexpr size_t kVtableBufferKey = 0x48 / sizeof(void*);  // vfunc[0x48](obj, scratch, idx) -> *u64, -1 = none

// SIORequestCallbackData, as read by 0x140fa16d0 (and documented in resource_override.cpp).
struct CompletionData {
    uint64_t status;      // 4 = success
    void* resource;
    uint32_t bufferIndex;
    uint32_t pad;
    const void* buffer;
    uint64_t size;
};
static_assert(offsetof(CompletionData, resource) == 0x08, "SIORequestCallbackData layout");
static_assert(offsetof(CompletionData, bufferIndex) == 0x10, "SIORequestCallbackData layout");
static_assert(offsetof(CompletionData, buffer) == 0x18, "SIORequestCallbackData layout");
static_assert(offsetof(CompletionData, size) == 0x20, "SIORequestCallbackData layout");

constexpr uint64_t kStatusSuccess = 4;

using SubmitFn = uint64_t(__cdecl*)(void* manager, void* resource, void* queue);
using RequestFn = uint64_t(__cdecl*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, void*, uint64_t, void*);
using CompletionFn = void(__cdecl*)(void* completion);
using IsLoadedFn = uint64_t(__cdecl*)(uint64_t* key);
using BufferKeyFn = const uint64_t*(__cdecl*)(void* resource, void* scratch, uint32_t index);

// ── State ────────────────────────────────────────────────────────────────────────────────────────────────

bool EngineSymbolsReady();
bool EngineHasLoaded(uint64_t type, uint64_t name);

std::atomic<bool> g_hooksInstalled{false};
std::atomic<bool> g_hookAttemptFailed{false};
std::mutex g_installMutex;

SubmitFn g_origSubmit = nullptr;
RequestFn g_origRequest = nullptr;
void* g_submitTarget = nullptr;
void* g_requestTarget = nullptr;

std::mutex g_warnedMutex;
std::set<std::pair<uint64_t, uint64_t>> g_warned;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress); }

template <typename T>
T ReadAt(uintptr_t rva) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(Base() + rva), sizeof(T));
    return v;
}

// Warn once per key: a retried load of a declined asset must not flood the log.
bool FirstWarningFor(uint64_t type, uint64_t name) {
    std::lock_guard<std::mutex> lock(g_warnedMutex);
    return g_warned.insert({type, name}).second;
}

bool EngineSymbolsReady() {
    if (!EchoVR::g_GameBaseAddress) return false;
    return ReadAt<uintptr_t>(kRvaManager) != 0 && ReadAt<uintptr_t>(kRvaSymbolTable) != 0;
}

// The engine's own "has this resource been loaded" predicate, the one 0x140fa09f0 uses before it refuses a
// manifest. Before the manager exists nothing can have been loaded.
bool EngineHasLoaded(uint64_t type, uint64_t name) {
    if (!EngineSymbolsReady()) return false;
    uint64_t key[2] = {type, name};
    auto fn = reinterpret_cast<IsLoadedFn>(Base() + kRvaIsLoaded);
    return fn(key) != 0;
}

// Registered assets are served only through the engine's own submit path. Anything the engine asks for that
// this cannot complete exactly the way the engine would is handed back to the engine untouched.
bool ServeFromPlugin(void* resource, const Asset& asset) {
    // Which buffers does the engine expect? Exactly what 0x140fa3220 computes: bit i when vfunc[0x48](i) is
    // not -1, and the second buffer off when the global at 0x1420a8a68 is 0.
    const auto* vtable = *reinterpret_cast<void* const* const*>(resource);
    const auto bufferKey = reinterpret_cast<BufferKeyFn>(vtable[kVtableBufferKey]);
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        uint64_t scratch[2] = {0, 0};
        const uint64_t* key = bufferKey(resource, scratch, i);
        uint64_t value = key ? *key : ~0ULL;
        if (i == 1 && ReadAt<uint32_t>(kRvaSecondaryOn) == 0) value = ~0ULL;
        if (value != ~0ULL) mask |= 1u << i;
    }
    if (mask != 1u) {
        if (FirstWarningFor(asset.type, asset.name)) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.ASSET] not served label=%s type=0x%016llx name=0x%016llx reason=buffer_mask_0x%x "
                "(only single-buffer resources are carried)",
                asset.label.c_str(), static_cast<unsigned long long>(asset.type),
                static_cast<unsigned long long>(asset.name), mask);
        }
        return false;
    }
    // With no scheduler the engine initializes the resource inline on whichever thread completes the last
    // buffer (0x140fa16d0, "Performing asset initialization on the IO thread."). That is this thread, which
    // may hold the manager's lock, so decline rather than risk a self-deadlock.
    if (ReadAt<uintptr_t>(kRvaScheduler) == 0) {
        if (FirstWarningFor(asset.type, asset.name)) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.ASSET] not served label=%s type=0x%016llx name=0x%016llx reason=no_task_scheduler",
                asset.label.c_str(), static_cast<unsigned long long>(asset.type),
                static_cast<unsigned long long>(asset.name));
        }
        return false;
    }

    // The engine frees buffer 0 of a resource through its own allocator when the resource unloads (game_buffer.h), so the
    // plugin's bytes are copied into that heap first. A plugin's buffer never reaches the engine: the plugin may unload it
    // at any time, and the engine would free a pointer its allocator did not make. Copied before any engine state changes,
    // so a refusal leaves the resource as the engine made it.
    void* gameCopy = nullptr;
    const auto* contextSlot = reinterpret_cast<void* const*>(Base() + nevr_game_buffer::kMemoryContextRva);
    const nevr_game_buffer::Copy copied =
        nevr_game_buffer::CopyIntoGameHeapFor(contextSlot, asset.data, asset.size, &gameCopy);
    if (copied != nevr_game_buffer::Copy::Copied) {
        if (FirstWarningFor(asset.type, asset.name)) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.ASSET] not served label=%s type=0x%016llx name=0x%016llx bytes=%llu reason=%s",
                asset.label.c_str(), static_cast<unsigned long long>(asset.type),
                static_cast<unsigned long long>(asset.name), static_cast<unsigned long long>(asset.size),
                nevr_game_buffer::CopyName(copied));
        }
        return false;
    }

    // The part of 0x140fa3220 that precedes the queueing: the pending mask, the state, the outstanding count.
    auto* bytes = static_cast<uint8_t*>(resource);
    *reinterpret_cast<volatile uint32_t*>(bytes + kObjPendingMask) = mask;
    *reinterpret_cast<volatile uint32_t*>(bytes + kObjState) = 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(bytes + kObjOutstanding));

    CompletionData done = {};
    done.status = kStatusSuccess;
    done.resource = resource;
    done.bufferIndex = 0;
    done.buffer = gameCopy;
    done.size = asset.size;
    auto complete = reinterpret_cast<CompletionFn>(Base() + kRvaCompletion);
    complete(&done);

    Log(EchoVR::LogLevel::Info,
        "[NEVR.ASSET] served from plugin label=%s type=0x%016llx name=0x%016llx bytes=%llu buffers=1",
        asset.label.c_str(), static_cast<unsigned long long>(asset.type),
        static_cast<unsigned long long>(asset.name), static_cast<unsigned long long>(asset.size));
    return true;
}

uint64_t __cdecl Hook_Submit(void* manager, void* resource, void* queue) {
    if (resource != nullptr && !Global().Empty()) {
        const auto* bytes = static_cast<const uint8_t*>(resource);
        uint64_t type = 0;
        uint64_t name = 0;
        std::memcpy(&type, bytes + kObjType, sizeof(type));
        std::memcpy(&name, bytes + kObjName, sizeof(name));
        Asset asset;
        if (Global().Find(type, name, &asset) && ServeFromPlugin(resource, asset)) return 0;
    }
    return g_origSubmit(manager, resource, queue);
}

// 0x140fa1fe0 is also reached directly (texture mip streaming, archive loads) without going through the submit
// hook. A registered key arriving there has no package to read; say so by name instead of letting the engine
// fail with its generic "Cannot find resource" assert.
uint64_t __cdecl Hook_Request(void* manager, uint64_t typeKey, uint64_t nameKey, uint64_t a4, uint64_t a5,
                              void* resource, uint64_t index, void* request) {
    if (!Global().Empty()) {
        Asset asset;
        if (Global().Find(typeKey, nameKey, &asset) && FirstWarningFor(typeKey, nameKey ^ 1)) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.ASSET] registered asset requested through a loader path that reads packages "
                "label=%s type=0x%016llx name=0x%016llx index=%llu (texture mip or archive load); the "
                "engine will fail this read",
                asset.label.c_str(), static_cast<unsigned long long>(typeKey),
                static_cast<unsigned long long>(nameKey), static_cast<unsigned long long>(index));
        }
    }
    return g_origRequest(manager, typeKey, nameKey, a4, a5, resource, index, request);
}

bool InstallOne(void* target, void* detour, void** original, const char* what) {
    MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.ASSET] hook create failed what=%s target=%p status=%s", what,
            target, MH_StatusToString(created));
        return false;
    }
    MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        MH_RemoveHook(target);
        Log(EchoVR::LogLevel::Warning, "[NEVR.ASSET] hook enable failed what=%s target=%p status=%s", what,
            target, MH_StatusToString(enabled));
        return false;
    }
    return true;
}

void EnsureHooksInstalled() {
    if (g_hooksInstalled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_hooksInstalled.load() || g_hookAttemptFailed.load()) return;
    if (!EchoVR::g_GameBaseAddress) {
        g_hookAttemptFailed = true;
        Log(EchoVR::LogLevel::Warning, "[NEVR.ASSET] no game base address; plugin assets will not be served");
        return;
    }

    g_submitTarget = reinterpret_cast<void*>(Base() + kRvaSubmit);
    if (!InstallOne(g_submitTarget, reinterpret_cast<void*>(Hook_Submit),
                    reinterpret_cast<void**>(&g_origSubmit), "submit")) {
        g_submitTarget = nullptr;
        g_hookAttemptFailed = true;
        return;
    }
    g_requestTarget = reinterpret_cast<void*>(Base() + kRvaRequest);
    if (!InstallOne(g_requestTarget, reinterpret_cast<void*>(Hook_Request),
                    reinterpret_cast<void**>(&g_origRequest), "request")) {
        // The submit hook alone serves loads; only the named diagnosis for the direct paths is missing.
        g_requestTarget = nullptr;
    }
    g_hooksInstalled = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.ASSET] hooks installed submit=%p request=%p", g_submitTarget,
        g_requestTarget);
}

}  // namespace

RegisterStatus RegisterPluginAsset(uint64_t type, uint64_t name, const void* data, uint64_t size,
                                   const char* label) {
    Global().SetLoadedProbe(&EngineHasLoaded);
    const RegisterStatus status = Global().Register(type, name, data, size, label);
    const char* shown = label ? label : "";
    if (status != RegisterStatus::Ok) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.ASSET] register refused label=%s type=0x%016llx name=0x%016llx bytes=%llu reason=%s", shown,
            static_cast<unsigned long long>(type), static_cast<unsigned long long>(name),
            static_cast<unsigned long long>(size), ToString(status));
        return status;
    }
    const char* typeName = EchoVR::LookupSymbolName(type);
    const char* resName = EchoVR::LookupSymbolName(name);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.ASSET] registered label=%s type=0x%016llx%s%s%s name=0x%016llx%s%s%s bytes=%llu", shown,
        static_cast<unsigned long long>(type), typeName ? " (" : "", typeName ? typeName : "",
        typeName ? ")" : "", static_cast<unsigned long long>(name), resName ? " (" : "",
        resName ? resName : "", resName ? ")" : "", static_cast<unsigned long long>(size));
    EnsureHooksInstalled();
    return status;
}

size_t DeregisterPluginAssets(const void* start, const void* end) {
    std::vector<Asset> removed;
    const size_t count = Global().Deregister(start, end, &removed);
    for (const Asset& a : removed) {
        Log(EchoVR::LogLevel::Info, "[NEVR.ASSET] deregistered label=%s type=0x%016llx name=0x%016llx",
            a.label.c_str(), static_cast<unsigned long long>(a.type),
            static_cast<unsigned long long>(a.name));
    }
    return count;
}

void ShutdownPluginAssets() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_requestTarget) {
        MH_DisableHook(g_requestTarget);
        MH_RemoveHook(g_requestTarget);
        g_requestTarget = nullptr;
    }
    if (g_submitTarget) {
        MH_DisableHook(g_submitTarget);
        MH_RemoveHook(g_submitTarget);
        g_submitTarget = nullptr;
    }
    g_origSubmit = nullptr;
    g_origRequest = nullptr;
    g_hooksInstalled = false;
    Log(EchoVR::LogLevel::Info, "[NEVR.ASSET] shutdown complete");
}

}  // namespace nevr::assets

extern "C" {

int NEVR_RegisterAsset(uint64_t type_hash, uint64_t name_hash, const void* data, uint64_t size,
                       const char* label) {
    return static_cast<int>(
        nevr::assets::RegisterPluginAsset(type_hash, name_hash, data, size, label));
}

void NEVR_DeregisterAssets(const void* data_start, const void* data_end) {
    nevr::assets::DeregisterPluginAssets(data_start, data_end);
}

}  // extern "C"
