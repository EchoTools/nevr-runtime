/*
 * resource_override.cpp — Hook AsyncResourceIOCallback to replace game resources.
 *
 * Provides a registry-based system for overriding game resources by their
 * type and name symbol hashes. Overrides can be either embedded data (compiled
 * into the DLL) or loaded from disk at runtime.
 *
 * Hook target: AsyncResourceIOCallback @ 0x140fa16d0
 * The callback receives SIORequestCallbackData:
 *   +0x00: status (4 = success)
 *   +0x08: CResource* pointer
 *   +0x10: buffer index (0 = primary, 1 = secondary; for a texture, the descriptor and the pixel payload)
 *   +0x18: data buffer pointer
 *   +0x20: data size
 *
 * CResource struct layout:
 *   +0x28: type_symbol  (uint64 hash, the logical type)
 *   +0x38: name_symbol  (uint64 hash)
 *
 * An entry's type names the buffer it replaces (resource_buffer_key.h, #1421): 0, the logical type or the buffer-0
 * on-disk type name buffer 0, which gets a copy in the game's heap; the buffer-1 on-disk type names buffer 1, which is
 * written over the game's own buffer when it fits and is otherwise not served.
 */

#include "runtime/patch/resource_override.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/hook/hook_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <atomic>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <MinHook.h>

// After winsock2.h: asset_cache.h includes windows.h.
#include "runtime/patch/asset_cache.h"

#include "core/globals.h"
#include "core/logging.h"
#include "runtime/patch/game_buffer.h"
#include "runtime/patch/resource_buffer_key.h"

/* Embedded assets */
#include "runtime/lifecycle/cli.h"

namespace {

/* ── Override registry ────────────────────────────────────────────── */

struct ResourceOverride {
    uint64_t type_hash;     /* 0 = match any type */
    uint64_t name_hash;
    const void* data;       /* embedded data (not owned) or VirtualAlloc'd (owned) */
    uint64_t size;
    bool owned;             /* true if data was allocated and should be freed */
    std::string label;      /* for logging; owned: a caller's buffer may be gone by the time the game loads the resource (#703) */
    bool applied;           /* set to true after first replacement */
    uint32_t in_flight;     /* game callbacks still using this replacement */
    ResourceTransformFn transform = nullptr;  /* set: answers every load of the resource instead of one replacement */
    void* user = nullptr;
    bool matched = false;     /* a game load matched this entry's key and buffer, served or not */
    bool passed_over = false; /* a load matched it and an earlier fixed replacement of the key served that load */
};

static std::unique_ptr<std::vector<std::shared_ptr<ResourceOverride>>> g_overrides;
static uint64_t g_total_hits = 0;
// The game callback can call back into this API, so never hold this mutex
// while calling it. Deregistration waits for selected entries to leave it.
static std::mutex g_overrides_mutex;
static std::condition_variable g_overrides_idle;
static std::mutex g_hook_install_mutex;
constexpr DWORD kReentrantDeregisterExitCode = 0xE0000478;
struct ActiveOverrideFrame {
    ResourceOverride* selected;
    ActiveOverrideFrame* previous;
};
static thread_local ActiveOverrideFrame* g_active_override = nullptr;
static void (*g_fatal_line_sink)(const char*) = nullptr;
// Not a measured limit: no callback has been observed to hold a lease this long. It exists so a blocked game
// callback cannot hang plugin unload with nothing in the log; the timeout is reported as an Error below.
static unsigned g_deregister_wait_ms = 30000;
static unsigned long long g_deregister_timeouts = 0;

/* The address of the 8-byte slot that holds the game's memory-context pointer; set when the hook installs. A buffer
 * written into a callback block is freed by the game through the allocator at context + 0x10, so only memory from that
 * allocator may be written there (#1146, game_buffer.h). */
static std::atomic<void* const*> g_memory_context_slot{nullptr};

/* ── Hook ─────────────────────────────────────────────────────────── */

typedef void (__cdecl* AsyncIOCallback_fn)(void* callback_data);
static AsyncIOCallback_fn g_orig = nullptr;

struct CallbackLease {
    std::shared_ptr<ResourceOverride> selected;
    ActiveOverrideFrame frame;

    explicit CallbackLease(std::shared_ptr<ResourceOverride> entry)
        : selected(std::move(entry)), frame{selected.get(), g_active_override} {
        g_active_override = &frame;
    }

    ~CallbackLease() {
        g_active_override = frame.previous;
        if (selected) {
            std::lock_guard<std::mutex> lock(g_overrides_mutex);
            --selected->in_flight;
            g_overrides_idle.notify_all();
        }
    }
};

/* Hands the game `bytes` as the buffer of the callback block: a copy allocated through the game's own allocator, because
 * the game frees that buffer through it when the resource unloads. The caller keeps its bytes. Returns false, and
 * leaves the block alone, when no copy could be made (the game keeps its own buffer). */
static bool GiveGameACopy(uintptr_t cbd, const void* bytes, uint64_t size, const char* label, uint64_t type_hash,
                          uint64_t name_hash) {
    void* copy = nullptr;
    const nevr_game_buffer::Copy result = nevr_game_buffer::CopyIntoGameHeap(
        g_memory_context_slot.load(std::memory_order_acquire), reinterpret_cast<const unsigned char*>(cbd), bytes, size,
        &copy);
    if (result != nevr_game_buffer::Copy::Copied) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] game-heap copy refused label=%s type=0x%016llx name=0x%016llx bytes=%llu reason=%s - the "
            "game's own buffer is kept",
            label, static_cast<unsigned long long>(type_hash), static_cast<unsigned long long>(name_hash),
            static_cast<unsigned long long>(size), nevr_game_buffer::CopyName(result));
        return false;
    }
    *reinterpret_cast<void**>(cbd + 0x18) = copy;
    *reinterpret_cast<uint64_t*>(cbd + 0x20) = size;
    return true;
}

/* Whether the registry holds an entry that replaces buffer 0 of (type_hash, name_hash): for a texture, its descriptor.
 * It says nothing of whether that entry was or will be served on this load: the two buffers finish in either order. */
static bool HasDescriptorEntryFor(uint64_t type_hash, uint64_t name_hash) {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    if (!g_overrides) return false;
    for (const auto& ovr : *g_overrides) {
        if (ovr->transform || ovr->name_hash != name_hash) continue;
        if (nevr_resource_key::NamesBuffer(ovr->type_hash, type_hash, 0)) return true;
    }
    return false;
}

/* Writes a payload (buffer 1) over the game's own buffer, when it fits: no payload-sized copy is made in the game's
 * heap, and the buffer the game frees is still the one its reader allocated (#1421). Returns false, and leaves the
 * block alone, when it does not fit. A texture's descriptor is a separate entry (resource_buffer_key.h), so either
 * outcome that leaves the two unpaired is a Warning that says which. */
static bool WritePayloadInPlace(uintptr_t cbd, const ResourceOverride& ovr, uint64_t type_hash, uint64_t name_hash,
                                uint64_t game_bytes) {
    const nevr_game_buffer::InPlace result =
        nevr_game_buffer::WriteInPlace(reinterpret_cast<unsigned char*>(cbd), ovr.data, ovr.size);
    const bool descriptorEntry = HasDescriptorEntryFor(type_hash, name_hash);
    if (result != nevr_game_buffer::InPlace::Written) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] payload not replaced label=%s buffer=1 type=0x%016llx name=0x%016llx game_bytes=%llu "
            "new_bytes=%llu reason=%s descriptor_entry=%s - the game's own payload is kept, and the replacement is "
            "offered again on the next load%s",
            ovr.label.c_str(), static_cast<unsigned long long>(type_hash), static_cast<unsigned long long>(name_hash),
            static_cast<unsigned long long>(game_bytes), static_cast<unsigned long long>(ovr.size),
            nevr_game_buffer::InPlaceName(result), descriptorEntry ? "yes" : "no",
            descriptorEntry ? "; this texture has a descriptor entry, which is served on its own, so the game may "
                              "read its own payload with that descriptor"
                            : "");
        return false;
    }
    if (ovr.size < game_bytes && !descriptorEntry) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] payload smaller than the game's label=%s buffer=1 type=0x%016llx name=0x%016llx "
            "game_bytes=%llu new_bytes=%llu descriptor_entry=no - the payload is written, but the game reads it with "
            "its own descriptor, made for its own payload; a payload of another size needs its descriptor entry too",
            ovr.label.c_str(), static_cast<unsigned long long>(type_hash), static_cast<unsigned long long>(name_hash),
            static_cast<unsigned long long>(game_bytes), static_cast<unsigned long long>(ovr.size));
    }
    return true;
}

/* Offers the game's buffer to a transform and, when it answers, points the callback at a copy of the answer. Returns
 * true when the game was given the answer; false leaves the game's buffer in place. */
static bool ApplyTransform(ResourceOverride& ovr, uintptr_t cbd, uint64_t type_hash, uint64_t name_hash) {
    const void* in = *reinterpret_cast<const void**>(cbd + 0x18);
    const uint64_t inSize = *reinterpret_cast<uint64_t*>(cbd + 0x20);
    const void* out = nullptr;
    uint64_t outSize = 0;
    bool served = false;
    try {
        served = ovr.transform(in, inSize, &out, &outSize, ovr.user);
    } catch (const std::exception& e) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] transform threw label=%s type=0x%016llx name=0x%016llx what=%s - the game's own buffer is kept",
            ovr.label.c_str(), static_cast<unsigned long long>(type_hash),
            static_cast<unsigned long long>(name_hash), e.what());
        return false;
    }
    if (!served || out == nullptr || outSize == 0) {
        Log(EchoVR::LogLevel::Debug,
            "[NEVR.RESOURCE] transform kept the game's buffer label=%s type=0x%016llx name=0x%016llx bytes=%llu",
            ovr.label.c_str(), static_cast<unsigned long long>(type_hash),
            static_cast<unsigned long long>(name_hash), static_cast<unsigned long long>(inSize));
        return false;
    }
    if (!GiveGameACopy(cbd, out, outSize, ovr.label.c_str(), type_hash, name_hash)) return false;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] transform applied label=%s type=0x%016llx name=0x%016llx orig_bytes=%llu new_bytes=%llu",
        ovr.label.c_str(), static_cast<unsigned long long>(type_hash), static_cast<unsigned long long>(name_hash),
        static_cast<unsigned long long>(inSize), static_cast<unsigned long long>(outSize));
    return true;
}

/* Offers the game's buffer to each transform of the key in registration order, and stops at the first that serves it.
 * A transform that declines or throws hands the load to the next one (#1479); the game keeps its own buffer when none does. */
static void OfferTransforms(const std::vector<std::shared_ptr<ResourceOverride>>& transforms, uintptr_t cbd,
                            uint64_t type_hash, uint64_t name_hash) {
    for (const auto& t : transforms) {
        {
            std::lock_guard<std::mutex> lock(g_overrides_mutex);
            ++t->in_flight;
            g_total_hits++;
        }
        CallbackLease lease(t);
        if (!ApplyTransform(*t, cbd, type_hash, name_hash)) continue;
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        t->applied = true;
        return;
    }
}

static void __cdecl Hook_AsyncIOCallback(void* callback_data) {
    std::shared_ptr<ResourceOverride> selected;
    std::vector<std::shared_ptr<ResourceOverride>> transforms;  /* matching transforms, in registration order */
    AsyncIOCallback_fn original;
    uint64_t type_hash = 0, name_hash = 0, orig_size = 0;
    uint32_t buf_index = 0;
    std::string displacedTransform, displacingOverride;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        original = g_orig;
        if (callback_data && g_overrides && !g_overrides->empty()) {
            auto cbd = reinterpret_cast<uintptr_t>(callback_data);
            void* resource = *reinterpret_cast<void**>(cbd + 0x08);
            buf_index = *reinterpret_cast<uint32_t*>(cbd + 0x10);

            if (resource) {
                auto res = reinterpret_cast<uintptr_t>(resource);
                name_hash = *reinterpret_cast<uint64_t*>(res + 0x38);
                type_hash = *reinterpret_cast<uint64_t*>(res + 0x28);

                /* The first fixed replacement that matches serves the load. A fixed replacement outranks every transform
                 * of the same key whatever the registration order (#1479), so transforms are offered only when none
                 * matches. Every match is recorded, so a shutdown can tell an entry passed over from one never requested. */
                std::shared_ptr<ResourceOverride> fixedHit;
                for (auto& ovr : *g_overrides) {
                    if (ovr->applied && !ovr->transform) continue;
                    if (ovr->name_hash != name_hash) continue;
                    if (!nevr_resource_key::NamesBuffer(ovr->type_hash, type_hash, buf_index)) continue;
                    if (ovr->transform && buf_index != 0) continue;  /* a transform is offered buffer 0 only */

                    ovr->matched = true;
                    if (ovr->transform) {
                        transforms.push_back(ovr);
                    } else if (!fixedHit) {
                        fixedHit = ovr;
                    } else {
                        ovr->passed_over = true;
                    }
                }
                if (fixedHit) {
                    /* Replaced below, outside the lock: the copy calls the game's allocator */
                    selected = fixedHit;
                    orig_size = *reinterpret_cast<uint64_t*>(cbd + 0x20);
                    if (!transforms.empty()) {
                        displacedTransform = transforms.front()->label;
                        displacingOverride = fixedHit->label;
                    }
                    selected->applied = true;
                    ++selected->in_flight;
                    g_total_hits++;
                }
            }
        }
    }
    if (!displacedTransform.empty()) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] override displaces transform type=0x%016llx name=0x%016llx override=%s transform=%s",
            static_cast<unsigned long long>(type_hash), static_cast<unsigned long long>(name_hash),
            displacingOverride.c_str(), displacedTransform.c_str());
    }
    CallbackLease lease(selected);
    const bool inPlace = buf_index == 1;
    if (!selected) {
        if (!transforms.empty()) {
            OfferTransforms(transforms, reinterpret_cast<uintptr_t>(callback_data), type_hash, name_hash);
        }
    } else if (!(inPlace ? WritePayloadInPlace(reinterpret_cast<uintptr_t>(callback_data), *selected, type_hash,
                                               name_hash, orig_size)
                         : GiveGameACopy(reinterpret_cast<uintptr_t>(callback_data), selected->data, selected->size,
                                         selected->label.c_str(), type_hash, name_hash))) {
        /* Not applied: the game keeps its own buffer, and the replacement is offered again on the next load. */
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        selected->applied = false;
    } else if (selected) {
        // Resolve both hashes to human names the same way registration does.
        char typeBuf[128], nameBuf[128];
        const char* typeName = EchoVR::LookupSymbolName(type_hash);
        const char* resName = EchoVR::LookupSymbolName(name_hash);
        snprintf(typeBuf, sizeof(typeBuf), "0x%016llx%s%s",
                 static_cast<unsigned long long>(type_hash),
                 typeName ? " (" : "", typeName ? typeName : "");
        if (typeName) strncat(typeBuf, ")", sizeof(typeBuf) - strlen(typeBuf) - 1);
        snprintf(nameBuf, sizeof(nameBuf), "0x%016llx%s%s",
                 static_cast<unsigned long long>(name_hash),
                 resName ? " (" : "", resName ? resName : "");
        if (resName) strncat(nameBuf, ")", sizeof(nameBuf) - strlen(nameBuf) - 1);
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] override applied label=%s buffer=%u type=%s name=%s orig_bytes=%llu "
            "new_bytes=%llu copy=%s",
            selected->label.c_str(), buf_index, typeBuf, nameBuf,
            static_cast<unsigned long long>(orig_size),
            static_cast<unsigned long long>(selected->size), inPlace ? "in_place" : "game_heap");
    }
    original(callback_data);
}

/* ── File loading helper ──────────────────────────────────────────── */

static void* LoadFileFromDisk(const char* path, uint64_t* out_size, DWORD* outError = nullptr) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (outError) *outError = GetLastError();
        return nullptr;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) {
        if (outError) *outError = GetLastError();
        CloseHandle(hFile);
        return nullptr;
    }

    uint64_t sz = static_cast<uint64_t>(li.QuadPart);
    void* buf = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        if (outError) *outError = GetLastError();
        CloseHandle(hFile);
        return nullptr;
    }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf, static_cast<DWORD>(sz), &bytesRead, NULL);
    if (outError) *outError = GetLastError();
    CloseHandle(hFile);

    if (!ok || bytesRead != sz) { VirtualFree(buf, 0, MEM_RELEASE); return nullptr; }

    *out_size = sz;
    return buf;
}

/* ── Cache helpers (#703) ─────────────────────────────────────────── */

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

/* Registers bytes the registry will own (VirtualAlloc'd copy). `source` is only for the log line. */
static bool RegisterOwnedCopy(uint64_t type_hash, uint64_t name_hash, const uint8_t* bytes, size_t size,
                              const std::string& label, const std::string& source) {
    void* data = VirtualAlloc(NULL, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!data) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] cache override not registered label=%s reason=VirtualAlloc failed error=%lu",
            label.c_str(), GetLastError());
        return false;
    }
    if (size) memcpy(data, bytes, size);
    auto ovr = std::make_shared<ResourceOverride>();
    ovr->type_hash = type_hash;
    ovr->name_hash = name_hash;
    ovr->data = data;
    ovr->size = size;
    ovr->owned = true;
    ovr->label = label;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) {
            VirtualFree(data, 0, MEM_RELEASE);
            return false;
        }
        g_overrides->push_back(ovr);
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered file override: %s (%s, %llu bytes)",
        label.c_str(), source.c_str(), static_cast<unsigned long long>(size));
    return true;
}

/* ── Lazy hook installation ───────────────────────────────────────── */

static void EnsureHookInstalled() {
    std::lock_guard<std::mutex> installLock(g_hook_install_mutex);
    // Memoizes SUCCESS (g_orig) only — without this latch, a persistent
    // failure (bad hook target) re-attempts and re-logs an identical Warning
    // on every RegisterResourceOverride call for the rest of the process
    // (once per _overrides/ file, again at the end of
    // InstallResourceOverride, and again for every
    // plugin that later calls the exported NEVR_RegisterResourceOverride).
    static bool s_hookAttemptFailed = false;
    {
        std::lock_guard<std::mutex> registryLock(g_overrides_mutex);
        if (g_orig) return;  /* Already installed */
    }
    if (s_hookAttemptFailed) return;  /* Already tried and failed this session */

    // This DLL is loaded for the process lifetime by the game. MinHook cannot
    // drain a callback that has entered our detour, and DllMain cannot wait for
    // worker threads under the loader lock. Pin before exposing the detour so
    // FreeLibrary cannot unmap it while a callback is active.
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCSTR>(&Hook_AsyncIOCallback), &self)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] runtime pin failed error=%lu — resource hook disabled",
            GetLastError());
        s_hookAttemptFailed = true;
        return;
    }
    Log(EchoVR::LogLevel::Info, "[NEVR.RESOURCE] runtime pinned for resource hook");

    void* hookTarget = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + 0xFA16D0);
    g_memory_context_slot.store(
        reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) +
                                       nevr_game_buffer::kMemoryContextRva),
        std::memory_order_release);
    AsyncIOCallback_fn original = nullptr;

    MH_STATUS cst = MH_CreateHook(hookTarget, reinterpret_cast<void*>(Hook_AsyncIOCallback),
                                  reinterpret_cast<void**>(&original));
    if (cst != MH_OK) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] hook create failed target=%p status=%s — resource overrides will not "
            "be applied",
            hookTarget, MH_StatusToString(cst));
        s_hookAttemptFailed = true;
        return;
    }

    // Publish the trampoline before enabling the target; a callback can begin
    // as soon as MinHook restores the other threads.
    {
        std::lock_guard<std::mutex> registryLock(g_overrides_mutex);
        g_orig = original;
    }
    MH_STATUS est = MH_EnableHook(hookTarget);
    if (est != MH_OK) {
        MH_STATUS rst = MH_RemoveHook(hookTarget);
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] hook enable failed target=%p status=%s cleanup=%s — resource overrides "
            "will not be applied",
            hookTarget, MH_StatusToString(est), MH_StatusToString(rst));
        {
            std::lock_guard<std::mutex> registryLock(g_overrides_mutex);
            g_orig = nullptr;
        }
        s_hookAttemptFailed = true;
        return;
    }

    nevr_hook_guard::Record(hookTarget, "AsyncResourceIOCallback");
    Log(EchoVR::LogLevel::Debug, "[NEVR.RESOURCE] hook installed target=%p", hookTarget);
}

} // anonymous namespace

/* ── Public API ───────────────────────────────────────────────────── */

void RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                              const void* data, uint64_t size,
                              const char* label) {
    auto ovr = std::make_shared<ResourceOverride>();
    ovr->type_hash = type_hash;
    ovr->name_hash = name_hash;
    ovr->data = data;
    ovr->size = size;
    ovr->owned = false;
    ovr->label = label ? label : "(null)";
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return;
        g_overrides->push_back(ovr);
    }

    /* Lazily install hook if this is the first override after init */
    EnsureHookInstalled();

    char typeBuf[128], nameBuf[128];
    const char* typeName = EchoVR::LookupSymbolName(type_hash);
    const char* resName = EchoVR::LookupSymbolName(name_hash);
    snprintf(typeBuf, sizeof(typeBuf), "0x%016llx%s%s",
             static_cast<unsigned long long>(type_hash),
             typeName ? " (" : "", typeName ? typeName : "");
    if (typeName) strncat(typeBuf, ")", sizeof(typeBuf) - strlen(typeBuf) - 1);
    snprintf(nameBuf, sizeof(nameBuf), "0x%016llx%s%s",
             static_cast<unsigned long long>(name_hash),
             resName ? " (" : "", resName ? resName : "");
    if (resName) strncat(nameBuf, ")", sizeof(nameBuf) - strlen(nameBuf) - 1);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered embedded override: %s (type=%s name=%s, %llu bytes)",
        ovr->label.c_str(),
        typeBuf,
        nameBuf,
        static_cast<unsigned long long>(size));
}

bool RegisterResourceTransform(uint64_t type_hash, uint64_t name_hash, ResourceTransformFn transform, void* user,
                               const char* label) {
    if (transform == nullptr) return false;
    auto ovr = std::make_shared<ResourceOverride>();
    ovr->type_hash = type_hash;
    ovr->name_hash = name_hash;
    ovr->data = nullptr;
    ovr->size = 0;
    ovr->owned = false;
    ovr->label = label ? label : "(null)";
    ovr->transform = transform;
    ovr->user = user;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return false;
        g_overrides->push_back(ovr);
    }
    EnsureHookInstalled();
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.RESOURCE] Registered transform: %s (type=0x%016llx name=0x%016llx)", ovr->label.c_str(),
        static_cast<unsigned long long>(type_hash), static_cast<unsigned long long>(name_hash));
    return true;
}

static bool RegisterResourceOverrideFromFileImpl(uint64_t type_hash, uint64_t name_hash,
                                                  const char* file_path,
                                                  const char* label) {
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return false;
    }
    uint64_t size = 0;
    DWORD err = 0;
    void* data = LoadFileFromDisk(file_path, &size, &err);
    if (!data) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] override file load failed path=%s error=%lu", file_path, err);
        return false;
    }
    auto ovr = std::make_shared<ResourceOverride>();
    ovr->type_hash = type_hash;
    ovr->name_hash = name_hash;
    ovr->data = data;
    ovr->size = size;
    ovr->owned = true;
    ovr->label = label ? label : "(null)";
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) {
            VirtualFree(data, 0, MEM_RELEASE);
            return false;
        }
        g_overrides->push_back(ovr);
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered file override: %s (%s, %llu bytes)",
        ovr->label.c_str(), file_path, static_cast<unsigned long long>(size));
    return true;
}

void RegisterResourceOverrideFromFile(uint64_t type_hash, uint64_t name_hash,
                                      const char* file_path,
                                      const char* label) {
    RegisterResourceOverrideFromFileImpl(type_hash, name_hash, file_path, label);
}

bool ParseOverrideFileName(const char* file_name, uint64_t* type_hash, uint64_t* name_hash) {
    uint64_t type_h = 0, name_h = 0;
    if (file_name[0] == '0' && (file_name[1] == 'x' || file_name[1] == 'X')) {
        const char* dot = strchr(file_name, '.');
        if (dot && dot[1] == '0' && (dot[2] == 'x' || dot[2] == 'X')) {
            type_h = strtoull(file_name + 2, nullptr, 16);
            name_h = strtoull(dot + 3, nullptr, 16);
        } else {
            name_h = strtoull(file_name + 2, nullptr, 16);
        }
    }
    if (name_h == 0) return false;
    *type_hash = type_h;
    *name_hash = name_h;
    return true;
}

size_t RegisterCachedResourceOverrides(const wchar_t* cache_dir) {
    return RegisterCachedResourceOverrides(cache_dir, nevr_asset_cache::kDefaultCapBytes);
}

size_t RegisterCachedResourceOverrides(const wchar_t* cache_dir, uint64_t cap_bytes) {
    const std::wstring dir = cache_dir ? cache_dir : L"";
    if (dir.empty()) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] cache disabled reason=no cache directory (LOCALAPPDATA unset)");
        return 0;
    }
    const std::string dirUtf8 = WideToUtf8(dir);
    const nevr_asset_cache::IndexResult index = nevr_asset_cache::ReadIndex(dir);
    if (!index.present) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] cache init dir=%s entries=0 loaded=0 refused=0 missing=0 reason=no index.json",
            dirUtf8.c_str());
        return 0;
    }
    if (!index.ok) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] cache index unusable dir=%s reason=%s — no cached override is applied",
            dirUtf8.c_str(), index.error.c_str());
        return 0;
    }
    for (const std::string& why : index.skipped) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.RESOURCE] cache index entry ignored dir=%s reason=%s",
            dirUtf8.c_str(), why.c_str());
    }
    size_t loaded = 0, refused = 0, missing = 0;
    std::set<std::string> referenced;
    for (const nevr_asset_cache::IndexEntry& entry : index.entries) {
        referenced.insert(entry.sha256);
        const std::string label = entry.label.empty() ? "cache:" + entry.sha256.substr(0, 12) : entry.label;
        const std::string blob = WideToUtf8(nevr_asset_cache::BlobPath(dir, entry.sha256));
        nevr_asset_cache::LoadResult r = nevr_asset_cache::Load(dir, entry.sha256);
        switch (r.status) {
        case nevr_asset_cache::LoadStatus::Hit:
            Log(EchoVR::LogLevel::Info,
                "[NEVR.RESOURCE] cache hit sha256=%s bytes=%llu type=0x%016llx name=0x%016llx label=%s",
                entry.sha256.c_str(), static_cast<unsigned long long>(r.bytes.size()),
                static_cast<unsigned long long>(entry.typeHash), static_cast<unsigned long long>(entry.nameHash),
                label.c_str());
            if (RegisterOwnedCopy(entry.typeHash, entry.nameHash, r.bytes.data(), r.bytes.size(), label, blob)) ++loaded;
            break;
        case nevr_asset_cache::LoadStatus::HashMismatch:
            ++refused;
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.RESOURCE] cache refused reason=sha256 mismatch expected=%s actual=%s path=%s label=%s",
                r.expectedSha256.c_str(), r.actualSha256.c_str(), blob.c_str(), label.c_str());
            break;
        case nevr_asset_cache::LoadStatus::Missing:
            ++missing;
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.RESOURCE] cache miss reason=blob not in cache sha256=%s path=%s label=%s",
                entry.sha256.c_str(), blob.c_str(), label.c_str());
            break;
        default:
            ++refused;
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.RESOURCE] cache refused reason=%s sha256=%s path=%s error=%lu label=%s",
                r.status == nevr_asset_cache::LoadStatus::TooLarge ? "blob over the size limit" : "blob unreadable",
                entry.sha256.c_str(), blob.c_str(), r.error, label.c_str());
            break;
        }
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] cache init dir=%s entries=%zu loaded=%zu refused=%zu missing=%zu",
        dirUtf8.c_str(), index.entries.size(), loaded, refused, missing);

    const nevr_asset_cache::EvictReport ev =
        nevr_asset_cache::Evict(dir, cap_bytes, referenced);
    Log(ev.overCapAllReferenced ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] cache size blob_bytes_before=%llu blob_bytes_after=%llu cap=%llu evicted=%u "
        "stale_temps_removed=%u over_cap_all_referenced=%s",
        static_cast<unsigned long long>(ev.blobBytesBefore), static_cast<unsigned long long>(ev.blobBytesAfter),
        static_cast<unsigned long long>(cap_bytes), ev.blobsEvicted, ev.staleTempsRemoved,
        ev.overCapAllReferenced ? "true" : "false");
    return loaded;
}

static void RegisterInitialOverrides(const wchar_t* cache_dir) {
    RegisterCachedResourceOverrides(cache_dir);
}

void InstallResourceOverride() {
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (g_overrides) return;
        g_overrides = std::make_unique<std::vector<std::shared_ptr<ResourceOverride>>>();
    }
    size_t overridesFailed = 0;

    /* ── Register built-in overrides ──────────────────────────────── */

    RegisterInitialOverrides(nevr_asset_cache::DefaultDir().c_str());

    /* ── Scan _overrides/ directory for additional files ──────────── */

    char moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, moduleDir, MAX_PATH);
    char* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char overrideDir[MAX_PATH];
    snprintf(overrideDir, sizeof(overrideDir), "%s_overrides", moduleDir);

    WIN32_FIND_DATAA fd;
    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", overrideDir);
    HANDLE hFind = FindFirstFileA(searchPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const char* name = fd.cFileName;

            uint64_t type_h = 0, name_h = 0;
            if (!ParseOverrideFileName(name, &type_h, &name_h)) continue;

            char filePath[MAX_PATH];
            snprintf(filePath, sizeof(filePath), "%s\\%s", overrideDir, name);
            if (!RegisterResourceOverrideFromFileImpl(type_h, name_h, filePath, name)) {
                overridesFailed++;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    /* Install hook if overrides exist; otherwise keep g_overrides alive
     * for plugins that register overrides later via the exported API. */
    size_t registered;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        registered = g_overrides->size();
    }
    if (registered != 0) {
        EnsureHookInstalled();
    }

    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] init complete overrides_registered=%zu overrides_failed=%zu",
        registered, overridesFailed);
}

void ResetResourceOverrides() {
    uint64_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return;
        for (auto& ovr : *g_overrides) {
            if (ovr->applied) {
                ovr->applied = false;
                count++;
            }
        }
    }
    if (count > 0) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] Reset %llu override(s) for re-application",
            static_cast<unsigned long long>(count));
    }
}

void SetResourceOverrideFatalLineSink(void (*sink)(const char* line)) {
    g_fatal_line_sink = sink;
}

ResourceOverrideShutdownReport ShutdownResourceOverride() {
    ResourceOverrideShutdownReport report = {};
    uint64_t owned_buffers_process_lifetime = 0;
    std::unique_ptr<std::vector<std::shared_ptr<ResourceOverride>>> released;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return report;
        report.registered = g_overrides->size();
        report.hook_installed = g_orig != nullptr;
        for (const auto& ovr : *g_overrides) {
            if (report.hook_installed && ovr->owned) ++owned_buffers_process_lifetime;
            if (ovr->applied) continue;
            // A transform answers only the loads its resource makes, and a level's transform is expected to see none
            // (a session that never loads the level), so it is counted and kept at Debug, never a Warning.
            if (ovr->transform) {
                ++report.transforms_unserved;
                Log(EchoVR::LogLevel::Debug,
                    "[NEVR.RESOURCE] transform not served label=%s name=0x%016llx matched=%s",
                    ovr->label.c_str(), static_cast<unsigned long long>(ovr->name_hash), ovr->matched ? "yes" : "no");
                continue;
            }
            ++report.never_applied;
            // A registered-but-never-matched override (wrong hash, or the expected resource genuinely never
            // loaded this session) must not read the same as one that worked, nor as one an earlier entry took.
            if (ovr->passed_over) {
                Log(EchoVR::LogLevel::Warning,
                    "[NEVR.RESOURCE] override passed over label=%s name=0x%016llx — an earlier override of the same "
                    "resource served its loads",
                    ovr->label.c_str(), static_cast<unsigned long long>(ovr->name_hash));
            } else if (ovr->matched) {
                Log(EchoVR::LogLevel::Warning,
                    "[NEVR.RESOURCE] override matched a load but was not served label=%s name=0x%016llx — the "
                    "failure is logged where it happened",
                    ovr->label.c_str(), static_cast<unsigned long long>(ovr->name_hash));
            } else {
                Log(EchoVR::LogLevel::Warning,
                    "[NEVR.RESOURCE] override never applied label=%s name=0x%016llx — resource was never requested "
                    "this session",
                    ovr->label.c_str(), static_cast<unsigned long long>(ovr->name_hash));
            }
        }
        // With the hook live the module is pinned and a callback may be running: keep the registry. With no
        // hook nothing can reach the buffers, so release them.
        if (!report.hook_installed) released = std::move(g_overrides);
    }
    if (released) {
        for (const auto& ovr : *released) {
            if (ovr->owned && ovr->data) VirtualFree(const_cast<void*>(ovr->data), 0, MEM_RELEASE);
        }
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] shutdown complete overrides_registered=%llu overrides_never_applied=%llu "
        "transforms_unserved=%llu hook_installed=%s registry_released=%s owned_buffers_process_lifetime=%llu",
        static_cast<unsigned long long>(report.registered), static_cast<unsigned long long>(report.never_applied),
        static_cast<unsigned long long>(report.transforms_unserved), report.hook_installed ? "true" : "false",
        report.hook_installed ? "false" : "true", static_cast<unsigned long long>(owned_buffers_process_lifetime));
    return report;
}

void DeregisterResourceOverrides(const void* data_start, const void* data_end) {
    std::unique_lock<std::mutex> lock(g_overrides_mutex);
    if (!g_overrides) return;
    auto start = reinterpret_cast<uintptr_t>(data_start);
    auto end = reinterpret_cast<uintptr_t>(data_end);
    std::vector<std::shared_ptr<ResourceOverride>> retired;

    auto it = g_overrides->begin();
    while (it != g_overrides->end()) {
        auto ptr = reinterpret_cast<uintptr_t>((*it)->data);
        if (ptr >= start && ptr < end) {
            retired.push_back(*it);
            it = g_overrides->erase(it);
        } else {
            ++it;
        }
    }

    for (auto* frame = g_active_override; frame; frame = frame->previous) {
        for (const auto& ovr : retired) {
            if (frame->selected != ovr.get()) continue;
            // The void plugin API cannot safely return while the caller may
            // unload bytes that this callback still uses. Log() allocates and
            // takes locks, and this can run with arbitrary game/plugin locks
            // held, so the one record goes out through a raw, non-allocating
            // sink (the crash record's WriteFile path) before the process ends.
            char line[192];
            snprintf(line, sizeof(line),
                     "[NEVR.RESOURCE] FATAL reentrant deregistration: label=%s was deregistered from inside its own "
                     "game callback; terminating with exit code 0x%lX",
                     ovr->label.c_str(), static_cast<unsigned long>(kReentrantDeregisterExitCode));
            if (g_fatal_line_sink) {
                g_fatal_line_sink(line);
            } else {
                fputs(line, stderr);
                fputc('\n', stderr);
                fflush(stderr);
            }
            TerminateProcess(GetCurrentProcess(), kReentrantDeregisterExitCode);
            std::abort();
        }
    }

    // Future callbacks cannot select these entries. The game callback may
    // still be reading their bytes, so wait without holding the registry lock,
    // and say so: an unattributed wait looks like a hang.
    auto busy = [&] {
        for (const auto& ovr : retired) {
            if (ovr->in_flight != 0) return true;
        }
        return false;
    };
    bool timedOut = false;
    if (busy()) {
        uint32_t inFlight = 0;
        for (const auto& ovr : retired) inFlight += ovr->in_flight;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] deregistration waiting for %u in-flight game callback(s) first_label=%s bound_ms=%u",
            inFlight, retired.front()->label.c_str(), g_deregister_wait_ms);
        timedOut = !g_overrides_idle.wait_for(lock, std::chrono::milliseconds(g_deregister_wait_ms),
                                              [&] { return !busy(); });
        if (timedOut) ++g_deregister_timeouts;
    }
    lock.unlock();
    if (timedOut) {
        // Freeing now would hand the game's callback freed memory. The entries are already out of the registry,
        // so nothing new selects them; the owned buffers are leaked on purpose and the caller is told.
        for (const auto& ovr : retired) {
            Log(EchoVR::LogLevel::Error,
                "[NEVR.RESOURCE] deregistration gave up after %u ms with the game callback still running: label=%s "
                "buffer %s; the plugin's bytes may still be read",
                g_deregister_wait_ms, ovr->label.c_str(), ovr->owned ? "leaked" : "not owned here");
        }
        return;
    }
    for (const auto& ovr : retired) {
        if (ovr->owned && ovr->data) {
            VirtualFree(const_cast<void*>(ovr->data), 0, MEM_RELEASE);
        }
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] Deregistered override: %s", ovr->label.c_str());
    }
    if (!retired.empty()) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] Deregistered %llu override(s)",
            static_cast<unsigned long long>(retired.size()));
    }
}

/* ── Exported wrappers (called by plugins via GetProcAddress) ────── */

extern "C" {

void NEVR_RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                                   const void* data, uint64_t size,
                                   const char* label) {
    RegisterResourceOverride(type_hash, name_hash, data, size, label);
}

void NEVR_ResetResourceOverrides() {
    ResetResourceOverrides();
}

void NEVR_DeregisterResourceOverrides(const void* data_start,
                                      const void* data_end) {
    DeregisterResourceOverrides(data_start, data_end);
}

} /* extern "C" */

#ifdef NEVR_TEST_HOOKS
void TestHook_InitResourceOverrides(void (*original)(void*)) {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_overrides = std::make_unique<std::vector<std::shared_ptr<ResourceOverride>>>();
    g_orig = original;
}

void TestHook_RegisterInitialOverrides(const wchar_t* cache_dir) { RegisterInitialOverrides(cache_dir); }
void TestHook_SetDeregisterWaitMs(unsigned ms) { g_deregister_wait_ms = ms; }
unsigned long long TestHook_DeregisterTimeouts() { return g_deregister_timeouts; }

void TestHook_SetOriginal(void (*original)(void*)) {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_orig = original;
}

void TestHook_RunResourceCallback(void* callback_data) {
    Hook_AsyncIOCallback(callback_data);
}

void TestHook_SetMemoryContextSlot(void* const* slot) { g_memory_context_slot.store(slot, std::memory_order_release); }

void TestHook_CloseResourceOverrides() {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_overrides.reset();
    g_orig = nullptr;
}
#endif
