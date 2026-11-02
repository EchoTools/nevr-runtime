// Quest libr15.so VA 0x1388c64; ReVault binary libr15.so.
// ReVault raw parent 100224040, reconstruction node 100600619; grade M.
#include <cstddef>
#include <cstdint>

// @0x1388c64 — NRadEngine::NRadGame::CR15NetGame::EndMultiplayer
/* @confidence: M */

namespace NRadEngine {

struct CAllocator;

class CMemoryContext {
public:
    static CAllocator* Current();
    static CAllocator* CurrentPtr();
    static void SetCurrent(CAllocator* allocator);
};

namespace NWriteLog {
void WriteLog(std::int32_t level, std::uint64_t channel, const char* format,
              ...);
}

class CMemBlock {
public:
    void Detach();
    ~CMemBlock();
    void Resize(std::uint64_t bytes, std::uint32_t keep_contents);
    std::uint64_t Size() const;
};

namespace NRadGame {

class CR15NetGame {
public:
    void LeaveGame(std::uint32_t flags);
    void UpdateRemoteLogs(std::uint32_t flag);
    void EndMultiplayer();
};

class CR15NetSpectatorCameraCS {
public:
    static void FreeCameraData();
};

class CHttpListener {
public:
    void Unbind();
};

class CBroadcaster {
public:
    void Unlisten(std::uint16_t id);
    // 0x25edbf8 (CBroadcaster::UnlistenAll): with a null owner it unlistens
    // every entry; otherwise only the entries whose owner equals this argument.
    void UnlistenAll(const void* owner);
};

class CTcpBroadcaster {
public:
    void Unlisten(std::uint16_t id);
};

class CR15NetAIScenarios {
public:
    void Disable();
};

class CSmallBlockAllocator {
public:
    void Shutdown();
    ~CSmallBlockAllocator();
};

class CR15NetStoreTransactions {
public:
    ~CR15NetStoreTransactions();
};

class CR15NetGameplay {
public:
    void UnregisterEvents();
};

class CR15PlayerNavCS {
public:
    static void InitializeTweaks();
};

}  // namespace NRadGame

namespace CVR {
void SetHUDVisible(std::uint32_t visible);
}

namespace CGRenderConfig {
void SetEnableAutoExposure(std::uint32_t enabled);
void SetForcedUIResolution(std::uint64_t resolution);
}

namespace {

template <typename T>
T Load(const void* base, std::size_t offset) {
    return *reinterpret_cast<const T*>(
        static_cast<const std::byte*>(base) + offset);
}

template <typename T>
void Store(void* base, std::size_t offset, T value) {
    *reinterpret_cast<T*>(static_cast<std::byte*>(base) + offset) = value;
}

// The slot the binary loads: ldr x8,[obj]; ldr x8,[x8,#off]; blr x8.
void* VirtualSlot(void* object, std::size_t byte_offset) {
    auto** const table = *reinterpret_cast<void***>(object);
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(table) +
                                     byte_offset);
}

void CallVirtualNoArgs(void* object, std::size_t byte_offset) {
    using Function = void (*)(void*);
    reinterpret_cast<Function>(VirtualSlot(object, byte_offset))(object);
}

void CallVirtualFree(void* allocator, void* value) {
    using Function = void (*)(void*, void*);
    reinterpret_cast<Function>(VirtualSlot(allocator, 0x38))(allocator, value);
}

void Acquire(std::int32_t* lock) {
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::int32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1, false,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
    }
}

}  // namespace

namespace NRadGame {

void CR15NetGame::EndMultiplayer() {
    auto* const self = reinterpret_cast<std::byte*>(this);
    auto* const flags = reinterpret_cast<std::byte*>(Load<std::uintptr_t>(this, 0x2da0));
    if (((Load<std::uint8_t>(flags, 1) >> 6) & 1U) == 0) {
        return;
    }

    NWriteLog::WriteLog(2, 0, "[NETGAME] EndMultiplayer");
    auto* object = reinterpret_cast<std::byte*>(Load<std::uintptr_t>(this, 0x2b08));
    if (object != nullptr) {
        Store<std::uint32_t>(object, 0x368,
                             Load<std::uint32_t>(object, 0x368) + 1U);
    }
    object = reinterpret_cast<std::byte*>(Load<std::uintptr_t>(this, 0x2b10));
    if (object != nullptr) {
        Store<std::uint32_t>(object, 0x368,
                             Load<std::uint32_t>(object, 0x368) + 1U);
    }

    if (((Load<std::uint8_t>(flags, 0) >> 2) & 1U) == 0) {
        CR15NetSpectatorCameraCS::FreeCameraData();
    }
    // 0x1388ce0: the CHttpListener object is at this + 0x64750; Unbind takes it as `this`.
    reinterpret_cast<CHttpListener*>(self + 0x64750)->Unbind();
    const std::uint64_t state = Load<std::uint64_t>(flags, 0);
    if ((state & 0x44U) != 0x40U) {
        void* const listener = reinterpret_cast<void*>(Load<std::uintptr_t>(this, 0x647c8));
        if (listener != nullptr) {
            CallVirtualNoArgs(listener, 0x90);
        }
    }
    LeaveGame(0);

    // Loop 1 (0x1388d28): the handles at this + 0x2d6c, counted by 0x2d84. Each
    // pass re-reads the count and the lock pointer *(this + 0x30); the id is read
    // before the lock is taken, and the broadcaster is read under the lock.
    for (std::uint32_t index = 0; index < Load<std::uint32_t>(this, 0x2d84); ++index) {
        const std::uint16_t id = Load<std::uint16_t>(self, 0x2d6c + index * 2U);
        auto* const lock = reinterpret_cast<std::int32_t*>(Load<std::uintptr_t>(this, 0x30));
        Acquire(lock);
        reinterpret_cast<CBroadcaster*>(Load<std::uintptr_t>(this, 0x28c8))->Unlisten(id);
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    }
    Store<std::uint32_t>(this, 0x2d84, 0);

    // Loop 2 (0x1388d84): TCP handles at this + 0x2d8c, counted by 0x2d94; the
    // count and the TCP broadcaster at *(this + 0x28c0) are re-read per pass.
    for (std::uint32_t index = 0; index < Load<std::uint32_t>(this, 0x2d94); ++index) {
        const std::uint16_t id = Load<std::uint16_t>(self, 0x2d8c + index * 2U);
        reinterpret_cast<CTcpBroadcaster*>(Load<std::uintptr_t>(this, 0x28c0))->Unlisten(id);
    }
    Store<std::uint32_t>(this, 0x2d94, 0);

    // Loop 3 (0x1388db8): the handles at this + 0x2d20, counted by 0x2d50; no lock.
    const std::uint32_t count3 = Load<std::uint32_t>(this, 0x2d50);
    for (std::uint32_t index = 0; index < count3; ++index) {
        const std::uint16_t id = Load<std::uint16_t>(self, 0x2d20 + index * 2U);
        reinterpret_cast<CBroadcaster*>(Load<std::uintptr_t>(this, 0x28c8))->Unlisten(id);
    }
    Store<std::uint32_t>(this, 0x2d50, 0);
    UpdateRemoteLogs(1U);

    // Net game block (0x40). The allocator is read once, before the dtor (0x1388e18).
    {
        CAllocator* const previous = CMemoryContext::Current();
        CMemoryContext::SetCurrent(CMemoryContext::Current());
        CAllocator* const allocator = CMemoryContext::Current();
        auto* const net_game_block = reinterpret_cast<void*>(Load<std::uintptr_t>(this, 0x40));
        if (net_game_block != nullptr) {
            CallVirtualNoArgs(net_game_block, 0);
        }
        CallVirtualFree(allocator, net_game_block);
        CMemoryContext::SetCurrent(previous);
        Store<std::uintptr_t>(this, 0x40, 0);
    }

    // AI scenarios block (0x2ab0). The scenario lock is *(value + 0x30), where
    // value = *(scenarios + 0x558); the broadcaster is *(value + 0x28c8).
    {
        CAllocator* const previous = CMemoryContext::Current();
        CMemoryContext::SetCurrent(CMemoryContext::Current());
        CAllocator* const allocator = CMemoryContext::Current();
        auto* const scenarios = reinterpret_cast<std::byte*>(Load<std::uintptr_t>(this, 0x2ab0));
        if (scenarios != nullptr) {
            reinterpret_cast<CR15NetAIScenarios*>(scenarios)->Disable();
            reinterpret_cast<CSmallBlockAllocator*>(scenarios + 0x508)->Shutdown();
            const auto* const value = reinterpret_cast<const std::byte*>(
                Load<std::uintptr_t>(scenarios, 0x558));
            auto* const scenario_lock = reinterpret_cast<std::int32_t*>(
                Load<std::uintptr_t>(value, 0x30));
            Acquire(scenario_lock);
            reinterpret_cast<CBroadcaster*>(Load<std::uintptr_t>(value, 0x28c8))
                ->UnlistenAll(scenarios);
            __atomic_store_n(scenario_lock, 0, __ATOMIC_RELEASE);
            reinterpret_cast<CSmallBlockAllocator*>(scenarios + 0x508)
                ->~CSmallBlockAllocator();
        }
        CallVirtualFree(allocator, scenarios);
        CMemoryContext::SetCurrent(previous);
        Store<std::uintptr_t>(this, 0x2ab0, 0);
    }

    if (((Load<std::uint8_t>(flags, 0) >> 2) & 1U) != 0) {
        CAllocator* const previous = CMemoryContext::Current();
        CMemoryContext::SetCurrent(CMemoryContext::Current());
        CAllocator* const allocator = CMemoryContext::Current();
        auto* const transactions = reinterpret_cast<CR15NetStoreTransactions*>(
            Load<std::uintptr_t>(this, 0x2aa0));
        if (transactions != nullptr) {
            transactions->~CR15NetStoreTransactions();
        }
        CallVirtualFree(allocator, transactions);
        CMemoryContext::SetCurrent(previous);
        Store<std::uintptr_t>(this, 0x2aa0, 0);
    }

    reinterpret_cast<CR15NetGameplay*>(Load<std::uintptr_t>(this, 0x2aa8))->UnregisterEvents();
    auto* const global_game_state = reinterpret_cast<void*>(
        Load<std::uintptr_t>(this, 0x2b08));
    if (global_game_state != nullptr) {
        NWriteLog::WriteLog(2, 0, "[NETGAME] Destroying global gs");
        auto* const owner = reinterpret_cast<void*>(Load<std::uintptr_t>(this, 8));
        using DestroyGlobalState = void (*)(void*, void*, std::uint32_t,
                                            std::uint32_t);
        // ldr x8,[x0]; ldr x8,[x8,#0x358]; blr x8 (0x1388f78-0x1388f80).
        reinterpret_cast<DestroyGlobalState>(VirtualSlot(owner, 0x358))(
            owner, global_game_state, 1, 1);
    }

    // Gameplay block (0x2aa8). The allocator is read before the gameplay pointer
    // is reloaded (0x1388f98) and before any of the block's own calls.
    {
        CAllocator* const previous = CMemoryContext::Current();
        CMemoryContext::SetCurrent(CMemoryContext::Current());
        CAllocator* const allocator = CMemoryContext::Current();
        auto* const gameplay = reinterpret_cast<std::byte*>(
            Load<std::uintptr_t>(this, 0x2aa8));
        if (gameplay != nullptr) {
            const std::uint8_t block_flags = Load<std::uint8_t>(gameplay, 0x4b4);
            if ((block_flags & 4U) == 0) {
                auto* const block_allocator = reinterpret_cast<CAllocator*>(
                    Load<std::uintptr_t>(gameplay, 0x4a8));
                CAllocator* const current = CMemoryContext::CurrentPtr();
                CMemoryContext::SetCurrent(block_allocator);
                Store<std::uintptr_t>(gameplay, 0x4c8, 0);
                CMemoryContext::SetCurrent(current);
            } else {
                reinterpret_cast<CMemBlock*>(gameplay + 0x498)->Detach();
            }
            const std::uint8_t final_block_flags =
                Load<std::uint8_t>(gameplay, 0x4b4);
            if ((final_block_flags & 6U) != 0) {
                reinterpret_cast<CMemBlock*>(gameplay + 0x498)->Detach();
            }
            reinterpret_cast<CMemBlock*>(gameplay + 0x498)->~CMemBlock();
        }
        CallVirtualFree(allocator, gameplay);
        CMemoryContext::SetCurrent(previous);
        Store<std::uintptr_t>(this, 0x2aa8, 0);
    }

    CR15PlayerNavCS::InitializeTweaks();
    Store<std::uint64_t>(this, 0x63058, UINT64_MAX);
    Store<std::uint64_t>(this, 0x63060, UINT64_MAX);
    Store<std::uint64_t>(this, 0x63050, UINT64_MAX);

    const std::uint64_t old_tail = Load<std::uint64_t>(this, 0x2af0);
    Store<std::uint64_t>(this, 0x2ae8, 0);
    // 0x1389064 (count already set): next = 1. 0x138906c (count zero): Resize,
    // then next = *(this + 0x2ae8) + 1 with the field reloaded after Resize
    // (0x1389084, 0x1389090).
    std::uint64_t next = 1;
    if (Load<std::uint64_t>(this, 0x2ae0) == 0) {
        const std::uint64_t capacity = Load<std::uint64_t>(this, 0x2ad8);
        reinterpret_cast<CMemBlock*>(self + 0x2ab8)->Resize(capacity << 4, 1);
        Store<std::uint64_t>(this, 0x2ae0,
                             Load<std::uint64_t>(this, 0x2ae0) + capacity);
        next = Load<std::uint64_t>(this, 0x2ae8) + 1;
    }
    Store<std::uint64_t>(this, 0x2ae8, next);
    const std::uintptr_t entries = Load<std::uintptr_t>(this, 0x2ab8);
    Store<std::uint64_t>(reinterpret_cast<void*>(entries + next * 0x10 - 0x10),
                         0, 0);
    Store<std::uint64_t>(reinterpret_cast<void*>(entries + next * 0x10 - 0x10),
                         8, old_tail - 1);
    Store<std::uint64_t>(this, 0x2af0, 0);
    Store<std::uint64_t>(this, 0x2af8, 0);

    const std::uint32_t block_state = Load<std::uint32_t>(this, 0x2ad4);
    if ((block_state & 6U) == 0) {
        Store<std::uint64_t>(this, 0x2ae8, 0);
        if (((block_state >> 2) & 1U) == 0) {
            const std::uint64_t block_size =
                reinterpret_cast<CMemBlock*>(self + 0x2ab8)->Size();
            const std::uint64_t used = Load<std::uint64_t>(this, 0x2ae8) << 4;
            if (used < block_size) {
                reinterpret_cast<CMemBlock*>(self + 0x2ab8)->Resize(used, 1);
                Store<std::uint64_t>(this, 0x2ae0,
                                     Load<std::uint64_t>(this, 0x2ae8));
                (void)reinterpret_cast<CMemBlock*>(self + 0x2ab8)->Size();
            }
        }
    } else {
        reinterpret_cast<CMemBlock*>(self + 0x2ab8)->Detach();
        Store<std::uint64_t>(this, 0x2ae0, 0);
        Store<std::uint64_t>(this, 0x2ae8, 0);
        Store<std::uint32_t>(this, 0x2ad4, block_state & 0xfffffff9U);
    }
    Store<std::uint64_t>(this, 0x2af0, 0);
    Store<std::uint64_t>(this, 0x2af8, 0);
    CVR::SetHUDVisible(1);
    CGRenderConfig::SetEnableAutoExposure(1);
    CGRenderConfig::SetForcedUIResolution(Load<std::uint64_t>(this, 0x6484c));
    Store<std::uint64_t>(flags, 0, Load<std::uint64_t>(flags, 0) &
                                      0xfffffffffffbbfffULL);
}

}  // namespace NRadGame
}  // namespace NRadEngine
