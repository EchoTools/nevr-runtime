// @0x24207b0 — NRadEngine::NRadGame::CR15NetDamageableHealNode::Enter
/* @confidence: M */
// Quest libr15.so VA 0x24207b0 (RVA 0x23207b0).
// ReVault binary 576, raw parent 100312863, reconstruction node 100600279; tier: reconstructed; grade: M, assessed by revault-codex.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t value;
    const char* String() const;
};

namespace NWriteLog {
void WriteLog(int severity, void* context, const char* format, ...);
}

template<class T>
struct CComponentHandle {
    std::byte reserved[0x40];
    T* component;
    std::uint64_t compact_handle;
    void RefreshCache() const;
};

namespace SComponentNodeUtils {
template<class T>
void ComponentHandle(void* node_context, CComponentHandle<T>* handle,
                     const std::uint64_t* component_key,
                     std::uint64_t component_ref);
}

using DeferredQueueDispatcher = void (*)(void*, char*);
extern DeferredQueueDispatcher g_damageable_heal_queue_dispatcher; // Quest VA 0x3825b80: runtime-loaded.
struct CDeferredMethodQueue {
    static std::byte* QueueDamageableHealFunctor(
        void* queue, DeferredQueueDispatcher dispatcher);
};

namespace NRadGame {

struct SCompactPoolHandle { std::uint32_t value; };
struct SR15NetUserId { std::uint32_t value; };
struct SR15NetDamageableHealNodeData { std::byte bytes[0x28]; };

class CR15NetDamageableCS {
public:
    void Heal(SCompactPoolHandle handle, float amount, SR15NetUserId user_id);
};

class CR15NetDamageableHealNode {
public:
    void* node_context;
    void Enter(SR15NetDamageableHealNodeData const& data);
};

static_assert(sizeof(CComponentHandle<CR15NetDamageableCS>) == 0x50);
static_assert(offsetof(CComponentHandle<CR15NetDamageableCS>, component) == 0x40);
static_assert(offsetof(CComponentHandle<CR15NetDamageableCS>, compact_handle) == 0x48);

void CR15NetDamageableHealNode::Enter(
    SR15NetDamageableHealNodeData const& data)
{
    auto const* const data_bytes = reinterpret_cast<const std::byte*>(&data);
    std::array<std::uint64_t, 3> component_key{};
    std::memcpy(component_key.data(), data_bytes, sizeof(component_key));

    CComponentHandle<CR15NetDamageableCS> damageable;
    std::memset(&damageable, 0xff, sizeof(damageable));
    *reinterpret_cast<std::uint64_t*>(&damageable) = 0;
    *reinterpret_cast<std::uint16_t*>(
        reinterpret_cast<std::byte*>(&damageable) + 0x1e) = 0;
    SComponentNodeUtils::ComponentHandle<CR15NetDamageableCS>(
        node_context, &damageable, component_key.data(),
        *reinterpret_cast<const std::uint64_t*>(data_bytes + 0x18));

    const float heal_amount =
        *reinterpret_cast<const float*>(data_bytes + 0x20);
    const std::uint32_t user_id =
        *reinterpret_cast<const std::uint32_t*>(data_bytes + 0x24);
    if (!(heal_amount >= 0.0f)) {
        NWriteLog::WriteLog(
            8, nullptr,
            "CR15NetDamageableHealNode cannot heal a negative amount! (%f.2)",
            static_cast<double>(heal_amount));
        return;
    }

    damageable.RefreshCache();
    CR15NetDamageableCS* const component = damageable.component;
    if (component != nullptr && damageable.compact_handle != 0) {
        damageable.RefreshCache();
        auto** const component_vtable =
            *reinterpret_cast<void***>(component);
        using IsValidHandle = int (*)(void*, std::uint32_t);
        const auto is_valid = reinterpret_cast<IsValidHandle>(
            component_vtable[0x1d0 / sizeof(void*)]);
        if (is_valid(component, static_cast<std::uint32_t>(
                                    damageable.compact_handle)) != 0) {
            damageable.RefreshCache();
            CR15NetDamageableCS* const refreshed_component =
                damageable.component;
            damageable.RefreshCache();
            const std::uint32_t handle =
                static_cast<std::uint32_t>(damageable.compact_handle);
            auto* const component_context =
                *reinterpret_cast<std::byte**>(
                    reinterpret_cast<std::byte*>(refreshed_component) + 0x80);
            if (*reinterpret_cast<std::uint32_t*>(
                    component_context + 0x368) != 0) {
                refreshed_component->Heal(SCompactPoolHandle{handle},
                                          heal_amount,
                                          SR15NetUserId{user_id});
                return;
            }

            auto* const call =
                CDeferredMethodQueue::QueueDamageableHealFunctor(
                    component_context + 0x170,
                    g_damageable_heal_queue_dispatcher);
            *reinterpret_cast<CR15NetDamageableCS**>(call) =
                refreshed_component;
            *reinterpret_cast<DeferredQueueDispatcher*>(
                reinterpret_cast<std::byte*>(call) + 8) =
                g_damageable_heal_queue_dispatcher;
            *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::byte*>(call) + 0x10) = 0;
            *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::byte*>(call) + 0x18) = handle;
            *reinterpret_cast<float*>(
                reinterpret_cast<std::byte*>(call) + 0x1c) = heal_amount;
            *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::byte*>(call) + 0x20) = user_id;
            return;
        }
    }

    const char* const component_name =
        reinterpret_cast<const CSymbol64*>(data_bytes + 0x18)->String();
    NWriteLog::WriteLog(
        8, nullptr,
        "CR15NetDamageableHealNode could not find damageable component %s",
        component_name);
}

} // namespace NRadGame
} // namespace NRadEngine
