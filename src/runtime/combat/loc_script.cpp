// #2242 (H-LOC): the translocator's stun-burst effect and its counter. See loc_script.h for what the two hooks do and why.

#include "runtime/combat/loc_script.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hook_calls.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::loc_script {
namespace {

using TeleportFn = std::uint64_t (*)(void* ctx, void* a2, void* a3, void* a4);
using DispatchFn = std::int64_t (*)(void* node);

TeleportFn g_teleport = nullptr;
DispatchFn g_dispatch = nullptr;
std::uint8_t* g_token = nullptr;             // the exe's LOC token block (kTokenRva), set at Register
volatile std::int32_t* g_counter = nullptr;  // the exe's LOC use counter (kCounterRva), set at Register
std::atomic_bool g_registered{false};

// Interrupt time in 100 ns units: the clock the token's expiry is written in (veilnet.cpp writes now + kTokenLife). Read from
// the shared user page, as veilnet.cpp's InterruptTime() reads it: QueryInterruptTime has no import in this link line.
constexpr std::uintptr_t kInterruptTimeAddress = 0x7ffe0008;
std::uint64_t InterruptNow() {
    return *reinterpret_cast<const volatile ULONGLONG*>(kInterruptTimeAddress);
}

// The teleport node's state field: a qword, read and written as bytes so the pinned layout is the only assumption.
std::uint64_t NodeState(const std::uint8_t* node) {
    std::uint64_t state = 0;
    std::memcpy(&state, node + kNodeState, sizeof state);
    return state;
}

void SetNodeState(std::uint8_t* node, std::uint64_t state) { std::memcpy(node + kNodeState, &state, sizeof state); }

// The teleport node runs its effect on its first run, then the original teleport runs with the arguments it was given.
// Each effect is spawned on this machine; the counter tells the other clients (VeilNet's watcher sends the 'T' message).
std::uint64_t TeleportHook(void* ctx, void* a2, void* a3, void* a4) {
    NEVR_SCRIPT_HOOK_CALL("LOC teleport");
    if (TeleportEffectDue(static_cast<const std::uint8_t*>(ctx))) {
        BumpTeleportCount(g_counter);
        drone_script::SpawnEffect(ctx, kLocSpawn);
    }
    return g_teleport(ctx, a2, a3, a4);
}

// Every node the translocator script runs passes here. A held token makes the node's puck show the burst; the node keeps
// its own state, which the spawn clears.
std::int64_t DispatchHook(void* node) {
    NEVR_SCRIPT_HOOK_CALL("LOC dispatch");
    auto* at = static_cast<std::uint8_t*>(node);
    if (ClaimEffect(at, g_token, InterruptNow())) {
        const std::uint64_t state = NodeState(at);
        drone_script::SpawnEffect(node, kLocSpawn);
        SetNodeState(at, state);
    }
    return g_dispatch(node);
}

}  // namespace

void BumpTeleportCount(volatile std::int32_t* counter) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(counter));
}

bool TeleportEffectDue(const std::uint8_t* ctx) {
    if (!drone_script::ContextEnabled(ctx)) return false;
    return NodeState(ctx) == 0;
}

bool TokenHeld(const std::uint8_t* token, std::uint64_t now) {
    if (token == nullptr) return false;
    const LONG count = *reinterpret_cast<const volatile LONG*>(token + kTokenCount);
    const ULONGLONG expiry = *reinterpret_cast<const volatile ULONGLONG*>(token + kTokenExpiry);
    return count > 0 && now < expiry;
}

bool TakeToken(std::uint8_t* token, std::uint64_t now) {
    if (token == nullptr) return false;
    auto* count = reinterpret_cast<volatile LONG*>(token + kTokenCount);
    const auto* expiry = reinterpret_cast<const volatile ULONGLONG*>(token + kTokenExpiry);
    for (;;) {
        const LONG seen = *count;
        if (seen <= 0 || now >= *expiry) return false;
        if (InterlockedCompareExchange(count, seen - 1, seen) == seen) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(token + kTokenShown));
            return true;
        }
    }
}

bool ClaimEffect(const std::uint8_t* node, std::uint8_t* token, std::uint64_t now) {
    if (!TokenHeld(token, now)) return false;
    if (drone_script::DetonatorFor(node) == nullptr) return false;
    return TakeToken(token, now);
}

void Register() {
    auto* exe = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (exe == nullptr || g_registered.exchange(true)) return;
    g_token = exe + kTokenRva;
    g_counter = reinterpret_cast<volatile std::int32_t*>(exe + kCounterRva);
    const nevr_script_hooks::Hook rows[] = {
        {kScript, kTeleportRva, kTeleportBytes, sizeof kTeleportBytes, reinterpret_cast<void*>(&TeleportHook),
         reinterpret_cast<void**>(&g_teleport), "LOC translocator: the teleport's burst and counter"},
        {kScript, kDispatchRva, kDispatchBytes, sizeof kDispatchBytes, reinterpret_cast<void*>(&DispatchHook),
         reinterpret_cast<void**>(&g_dispatch), "LOC translocator: the held token's burst on this puck"},
    };
    nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.COMBAT] LOC translocator fix registered script=%016llx hooks=2 (a teleport shows its stun burst and counts "
        "for the other clients; a held token shows it on the next translocator puck; installed when the copy loads)",
        static_cast<unsigned long long>(kScript));
}

}  // namespace nevr_combat::loc_script
