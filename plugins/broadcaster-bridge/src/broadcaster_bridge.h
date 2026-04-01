/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <atomic>
#include <string>

#include "address_registry.h"

namespace nevr::broadcaster_bridge {

/*
 * CBroadcaster_Send function signature (x64 calling convention):
 *   RCX = CBroadcaster* self
 *   RDX = uint64_t message_symbol (CMatSym hash)
 *   R8D = int32_t message_flags
 *   R9  = const void* payload
 *   [RSP+0x28] = uint64_t payload_size
 *   [RSP+0x30] = const void* ext_payload
 *   [RSP+0x38] = uint64_t ext_size
 *   [RSP+0x40] = uint64_t target_peer (SPeer handle)
 *   [RSP+0x48] = uint64_t sequence
 *   [RSP+0x50] = float priority
 *   [RSP+0x58] = uint64_t pool_name
 *
 * Source: CBroadcaster.cpp in echovr-reconstruction
 * VA: 0x140f89af0 (VA_BROADCASTER_SEND in address_registry.h)
 */
typedef void (*CBroadcasterSend_fn)(
    void*       self,
    uint64_t    msg_sym,
    int32_t     flags,
    const void* payload,
    uint64_t    payload_size,
    const void* ext_payload,
    uint64_t    ext_size,
    uint64_t    target_peer,
    uint64_t    seq,
    float       priority,
    uint64_t    pool_name);

/*
 * CBroadcaster_ReceiveLocalEvent function signature.
 * Dispatches a message to all registered listeners.
 *
 * Source: CBroadcaster.cpp in echovr-reconstruction
 * VA: 0x140f87aa0 (VA_BROADCASTER_RECEIVE_LOCAL in address_registry.h)
 *
 * Actual signature (from reconstruction @0x140f87aa0):
 *   uint64_t ReceiveLocalEvent(CBroadcaster* self, CMatSym message_id,
 *       const char* msg_name, const void* msg, uint64_t msg_size)
 */
typedef uint64_t (*CBroadcasterReceiveLocal_fn)(
    void*       self,
    uint64_t    msg_sym,
    const char* msg_name,
    const void* payload,
    uint64_t    payload_size);

/*
 * Mirror packet wire format (sent over UDP):
 *   uint64_t msg_symbol   — CMatSym hash
 *   uint32_t flags        — message flags
 *   uint32_t payload_size — length of payload that follows
 *   uint8_t  payload[N]   — raw payload bytes
 *
 * Total header size: 16 bytes.
 */
#pragma pack(push, 1)
struct MirrorPacketHeader {
    uint64_t msg_symbol;
    uint32_t flags;
    uint32_t payload_size;
};
#pragma pack(pop)

static_assert(sizeof(MirrorPacketHeader) == 16,
    "MirrorPacketHeader must be exactly 16 bytes");

/*
 * Injection packet wire format (received on UDP listen port):
 *   uint8_t mode          — 0 = send via CBroadcaster_Send, 1 = local dispatch
 *   MirrorPacketHeader hdr
 *   uint8_t payload[N]
 */
#pragma pack(push, 1)
struct InjectionPacketHeader {
    uint8_t            mode;
    MirrorPacketHeader mirror;
};
#pragma pack(pop)

static_assert(sizeof(InjectionPacketHeader) == 17,
    "InjectionPacketHeader must be exactly 17 bytes");

/* Injection modes */
static constexpr uint8_t INJECT_MODE_SEND           = 0;
static constexpr uint8_t INJECT_MODE_LOCAL_DISPATCH  = 1;
static constexpr uint8_t INJECT_MODE_CHASSIS_SWAP    = 2;  /* Experimental: game flags + combat toggle */
static constexpr uint8_t INJECT_MODE_READ_LOADOUT    = 3;  /* Read current loadout and mirror back */
static constexpr uint8_t INJECT_MODE_ENABLE_BODIES   = 4;  /* Call EnableBodyComponents */
static constexpr uint8_t INJECT_MODE_DUMP_ACTOR_DATA = 5;  /* Dump actorDataRes component type table */
static constexpr uint8_t INJECT_MODE_DUMP_WEAPON     = 6;  /* Dump weapon scenario data from all CS instances */

/* Guardrail limits */
static constexpr size_t   MAX_PACKET_SIZE         = 4096;
static constexpr uint32_t MAX_MIRROR_PACKETS_PER_SEC = 10000;

/* Plugin configuration */
struct BridgeConfig {
    std::string target_ip   = "127.0.0.1";
    uint16_t    target_port = 9999;
    uint16_t    listen_port = 9998;
    bool        mirror_send    = true;
    bool        mirror_receive = false;
    bool        log_messages   = false;
};

/*
 * Parse a JSON config file into BridgeConfig.
 * Returns default config on any parse error.
 */
BridgeConfig ParseConfig(const std::string& json_text);

/*
 * Serialize a mirror packet into the provided buffer.
 * Returns the number of bytes written, or 0 if the buffer is too small
 * or the payload exceeds MAX_PACKET_SIZE.
 */
size_t SerializeMirrorPacket(uint8_t* buf, size_t buf_size,
    uint64_t msg_symbol, uint32_t flags,
    const void* payload, uint32_t payload_size);

/*
 * Deserialize a mirror packet header from a buffer.
 * Returns true on success, setting hdr and payload_out.
 * payload_out points into buf (not a copy).
 */
bool DeserializeMirrorPacket(const uint8_t* buf, size_t buf_size,
    MirrorPacketHeader& hdr, const uint8_t*& payload_out);

/* Lifecycle */
int  Initialize(uintptr_t base_addr, const char* config_path);
void OnFrame();
void Shutdown();

} // namespace nevr::broadcaster_bridge
