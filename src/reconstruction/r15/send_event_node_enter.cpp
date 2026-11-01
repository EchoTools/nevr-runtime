// CR15NetSendEventNode::Enter
// Quest libr15.so VA 0x243489c (RVA 0x242489c); PC echovr.exe: no matching function located.
// ReVault binary 576, raw parent 100313283, reconstruction node 100600178; tier: reconstructed; grade: M, assessed by revault-codex.
// @0x243489c — CR15NetSendEventNode::Enter
/* @confidence: M */
#include <cstdint>

struct SendEventNodeData {
    void* owner;
    std::uint64_t event_id;
    std::uint64_t peer;
    std::uint64_t sequence;
    std::uint16_t reserved_18;
    std::uint16_t user_index;
    std::uint32_t enabled;
};

struct ComponentEvent {
    std::uint64_t event_id;
    std::uint64_t owner_id;
    std::uint32_t component_type;
    std::uint32_t source_user;
    std::uint64_t payload_a;
    std::uint64_t payload_b;
};

struct NetGameOps {
    virtual void resolve_net_id_component(void* owner, void* handle_out);
    virtual void refresh_net_id_component(void* handle);
    virtual std::uint64_t owner_id(void* owner);
    virtual std::uint16_t component_type_from_handle(void* handle);
    virtual bool local_user_ready(void* net_game);
    virtual std::uint16_t local_user_slot(void* net_game);
    virtual std::uint16_t local_user_generation(void* net_game, std::uint16_t slot);
    virtual void broadcast_component_event(void* net_game, ComponentEvent* event,
                                           std::uint64_t zero_a, std::uint64_t zero_b,
                                           std::uint64_t peer, std::uint64_t sequence,
                                           std::uint64_t invalid);
};

void Enter(const SendEventNodeData& data, NetGameOps& ops, void* net_game) {
    alignas(8) std::uint8_t component_handle[0x30]{};
    ComponentEvent event{};
    event.event_id = data.event_id;
    event.owner_id = UINT64_MAX;
    event.component_type = 0x50fff;

    ops.resolve_net_id_component(data.owner, component_handle);
    ops.refresh_net_id_component(component_handle);
    ops.refresh_net_id_component(component_handle);
    event.owner_id = ops.owner_id(data.owner);

    std::uint16_t channel = 5;
    if (data.user_index != UINT16_MAX) channel = data.user_index & 7;
    const std::uint16_t component_type = ops.component_type_from_handle(component_handle);
    event.component_type = component_type |
                           static_cast<std::uint16_t>((data.enabled != 0) << 3) |
                           static_cast<std::uint32_t>(channel) << 16;

    event.source_user = UINT32_MAX;
    if (ops.local_user_ready(net_game)) {
        const std::uint16_t slot = ops.local_user_slot(net_game) & 0x1f;
        event.source_user = slot |
            (static_cast<std::uint32_t>(ops.local_user_generation(net_game, slot)) << 16);
    }

    ops.broadcast_component_event(net_game, &event, 0, 0, data.peer, data.sequence, UINT64_MAX);
}
