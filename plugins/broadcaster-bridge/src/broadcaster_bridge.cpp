/* SYNTHESIS -- custom tool code, not from binary */

#include "broadcaster_bridge.h"
#include "nevr_common.h"
#include "hook_manager.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <MinHook.h>
typedef SOCKET socket_t;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
/* Non-Windows stubs for compilation */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
typedef int socket_t;
static constexpr socket_t INVALID_SOCK = -1;
/* MinHook stubs for non-Windows builds */
enum MH_STATUS { MH_OK = 0, MH_ERROR_NOT_INITIALIZED };
inline MH_STATUS MH_Initialize()                                           { return MH_OK; }
inline MH_STATUS MH_CreateHook(void*, void*, void**)                       { return MH_OK; }
inline MH_STATUS MH_EnableHook(void*)                                      { return MH_OK; }
inline MH_STATUS MH_DisableHook(void*)                                     { return MH_OK; }
inline MH_STATUS MH_RemoveHook(void*)                                      { return MH_OK; }
inline MH_STATUS MH_Uninitialize()                                         { return MH_OK; }
inline void closesocket(socket_t s)                                        { close(s); }
#endif

namespace nevr::broadcaster_bridge {

/* ── Module state ──────────────────────────────────────────────────── */

static BridgeConfig           g_config;
static nevr::HookManager     g_hooks;
static std::atomic<void*>     g_broadcaster_ptr{nullptr};
static CBroadcasterSend_fn    g_original_send      = nullptr;
static CBroadcasterReceiveLocal_fn g_original_receive_local = nullptr;

static socket_t               g_mirror_sock  = INVALID_SOCK;
static socket_t               g_listen_sock  = INVALID_SOCK;
static sockaddr_in            g_target_addr{};

static std::thread            g_listener_thread;
static std::atomic<bool>      g_shutdown_flag{false};

/* Rate limiter state */
static std::atomic<uint32_t>  g_send_count{0};
static std::atomic<int64_t>   g_rate_window_start{0};

BridgeConfig ParseConfig(const std::string& json_text) {
    BridgeConfig cfg;
    if (json_text.empty()) return cfg;

    try {
        auto j = nlohmann::json::parse(json_text);

        std::string target = j.value("udp_debug_target", "");
        if (!target.empty()) {
            auto colon = target.rfind(':');
            if (colon != std::string::npos) {
                cfg.target_ip = target.substr(0, colon);
                cfg.target_port = static_cast<uint16_t>(
                    std::strtol(target.substr(colon + 1).c_str(), nullptr, 10));
            }
        }

        cfg.listen_port    = static_cast<uint16_t>(j.value("listen_port", static_cast<int>(cfg.listen_port)));
        cfg.mirror_send    = j.value("mirror_send", cfg.mirror_send);
        cfg.mirror_receive = j.value("mirror_receive", cfg.mirror_receive);
        cfg.log_messages   = j.value("log_messages", cfg.log_messages);
    } catch (...) {
        /* Return defaults on parse failure */
    }

    return cfg;
}

/* ── Wire format ──────────────────────────────────────────────────── */

size_t SerializeMirrorPacket(uint8_t* buf, size_t buf_size,
    uint64_t msg_symbol, uint32_t flags,
    const void* payload, uint32_t payload_size)
{
    size_t total = sizeof(MirrorPacketHeader) + payload_size;
    if (total > MAX_PACKET_SIZE || total > buf_size) return 0;

    MirrorPacketHeader hdr;
    hdr.msg_symbol   = msg_symbol;
    hdr.flags        = flags;
    hdr.payload_size = payload_size;

    std::memcpy(buf, &hdr, sizeof(hdr));
    if (payload && payload_size > 0) {
        std::memcpy(buf + sizeof(hdr), payload, payload_size);
    }
    return total;
}

bool DeserializeMirrorPacket(const uint8_t* buf, size_t buf_size,
    MirrorPacketHeader& hdr, const uint8_t*& payload_out)
{
    if (!buf || buf_size < sizeof(MirrorPacketHeader)) return false;

    std::memcpy(&hdr, buf, sizeof(hdr));
    size_t total = sizeof(MirrorPacketHeader) + hdr.payload_size;
    if (total > buf_size || total > MAX_PACKET_SIZE) return false;

    payload_out = (hdr.payload_size > 0) ? buf + sizeof(hdr) : nullptr;
    return true;
}

/* ── Rate limiter ─────────────────────────────────────────────────── */

static bool RateLimitCheck() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    int64_t window = g_rate_window_start.load(std::memory_order_relaxed);

    if (now_ms - window >= 1000) {
        g_rate_window_start.store(now_ms, std::memory_order_relaxed);
        g_send_count.store(1, std::memory_order_relaxed);
        return true;
    }

    uint32_t count = g_send_count.fetch_add(1, std::memory_order_relaxed);
    return count < MAX_MIRROR_PACKETS_PER_SEC;
}

/* ── Hook: CBroadcaster_Send ──────────────────────────────────────── */

static void HookedBroadcasterSend(
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
    uint64_t    pool_name)
{
    /* Call the original function first — never interfere with game logic */
    g_original_send(self, msg_sym, flags, payload, payload_size,
                    ext_payload, ext_size, target_peer, seq, priority, pool_name);

    /* Capture broadcaster pointer on first call */
    void* expected = nullptr;
    g_broadcaster_ptr.compare_exchange_strong(expected, self,
        std::memory_order_release, std::memory_order_relaxed);

    /* Mirror the packet if enabled */
    if (!g_config.mirror_send) return;
    if (g_mirror_sock == INVALID_SOCK) return;
    if (!RateLimitCheck()) return;

    uint32_t psize = static_cast<uint32_t>(
        (payload_size <= MAX_PACKET_SIZE - sizeof(MirrorPacketHeader))
        ? payload_size : 0);
    if (psize == 0 && payload_size > 0) return; /* too large, drop */

    uint8_t buf[MAX_PACKET_SIZE];
    size_t len = SerializeMirrorPacket(buf, sizeof(buf),
        msg_sym, static_cast<uint32_t>(flags), payload, psize);
    if (len == 0) return;

    /* Non-blocking send — never block the game thread */
#ifdef _WIN32
    sendto(g_mirror_sock, reinterpret_cast<const char*>(buf),
           static_cast<int>(len), 0,
           reinterpret_cast<const sockaddr*>(&g_target_addr),
           sizeof(g_target_addr));
#else
    sendto(g_mirror_sock, buf, len, MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&g_target_addr),
           sizeof(g_target_addr));
#endif
}

/* ── Hook: CBroadcaster_ReceiveLocal ──────────────────────────────── */

static uint64_t HookedBroadcasterReceiveLocal(
    void*       self,
    uint64_t    msg_sym,
    const char* msg_name,
    const void* payload,
    uint64_t    payload_size)
{
    /* Mirror incoming packet before dispatching */
    if (g_config.mirror_receive && g_mirror_sock != INVALID_SOCK && RateLimitCheck()) {
        uint32_t psize = static_cast<uint32_t>(
            (payload_size <= MAX_PACKET_SIZE - sizeof(MirrorPacketHeader))
            ? payload_size : 0);

        if (psize > 0 || payload_size == 0) {
            uint8_t buf[MAX_PACKET_SIZE];
            /* Flag 0x80000000 marks "receive" direction */
            size_t len = SerializeMirrorPacket(buf, sizeof(buf),
                msg_sym, 0x80000000u, payload, psize);
            if (len > 0) {
#ifdef _WIN32
                sendto(g_mirror_sock, reinterpret_cast<const char*>(buf),
                       static_cast<int>(len), 0,
                       reinterpret_cast<const sockaddr*>(&g_target_addr),
                       sizeof(g_target_addr));
#else
                sendto(g_mirror_sock, buf, len, MSG_DONTWAIT,
                       reinterpret_cast<const sockaddr*>(&g_target_addr),
                       sizeof(g_target_addr));
#endif
            }
        }
    }

    /* Capture broadcaster pointer */
    void* expected = nullptr;
    g_broadcaster_ptr.compare_exchange_strong(expected, self,
        std::memory_order_release, std::memory_order_relaxed);

    /* Call original */
    return g_original_receive_local(self, msg_sym, msg_name, payload, payload_size);
}

/* ── Chassis swap (experimental) ──────────────────────────────────── */

/*
 * Memory layout for loadout access:
 *   g_GameContext: *(base + 0x20a0478) → game context ptr
 *   CR15NetGame:   *(context + 0x8518)  → net game ptr
 *   Local slot:    *(uint16_t*)(netgame + 0x178) & 0x1F → slot index
 *   Loadout array: *(ptr*)(netgame + 0x51420 + slot*0x40) → LoadoutInstance array
 *   Instance count: *(uint64_t*)(netgame + 0x51450 + slot*0x40)
 *
 * LoadoutInstance layout (stride 0x40 = 8 int64_t's):
 *   [0]: instance_name (SymbolId)
 *   [1]: items_array_ptr
 *   ...
 *   [6]: slot_count / item_count
 *
 * Each item in items_array (stride 0x10):
 *   [0]: slot_symbol (SymbolId)
 *   [1]: equipped_item (SymbolId)
 *
 * LoadoutEntry struct (0xD8):
 *   +0x00: bodytype (SymbolId)
 *   +0x30: LoadoutSlot starts
 *   +0x50: chassis (SymbolId) = LoadoutSlot offset 0x20
 */

static constexpr uint64_t GAME_CONTEXT_OFFSET = 0x20a0478;
static constexpr uint64_t NETGAME_OFFSET = 0x8518;
static constexpr uint64_t GAMESPACE_OFFSET = 0x7AF0;  /* CR15Game + 0x7AF0 = global_gamespace_ptr */
static constexpr uint64_t GAMESPACE2_OFFSET = 0x7AF8; /* CR15Game + 0x7AF8 = possible game gamespace */
static constexpr uint64_t VA_ENABLE_BODY_COMPONENTS = 0x140cd07e0;

/* EnableBodyComponents(uint32_t enable_mode, void* game_space, int64_t actor_hash) */
typedef void (*EnableBodyComponents_fn)(uint32_t, void*, int64_t);

struct ChassisSwapPayload {
    uint64_t new_chassis_id;  /* SymbolId of desired chassis */
    uint16_t target_slot;     /* player slot (0xFFFF = local player) */
    uint16_t _pad[3];
};

struct ReadLoadoutResponse {
    uint64_t bodytype;
    uint64_t chassis;
    uint16_t slot;
    uint16_t instance_count;
    uint32_t _pad;
};

static void MirrorResponse(uint64_t cmd_sym, const void* data, uint32_t size) {
    if (g_mirror_sock == INVALID_SOCK) return;
    uint8_t buf[MAX_PACKET_SIZE];
    size_t len = SerializeMirrorPacket(buf, sizeof(buf), cmd_sym, 0x80000000u, data, size);
    if (len == 0) return;
#ifdef _WIN32
    sendto(g_mirror_sock, reinterpret_cast<const char*>(buf),
           static_cast<int>(len), 0,
           reinterpret_cast<const sockaddr*>(&g_target_addr), sizeof(g_target_addr));
#else
    sendto(g_mirror_sock, buf, len, MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&g_target_addr), sizeof(g_target_addr));
#endif
}

/* Sentinel symbol for responses */
static constexpr uint64_t NEVR_RESPONSE_SYM = 0x4E45565252455350; /* "NEVRRESP" */

static void HandleReadLoadout(uintptr_t base) {
    char* baseAddr = reinterpret_cast<char*>(base);
    void** ctxPtr = reinterpret_cast<void**>(baseAddr + GAME_CONTEXT_OFFSET);
    void* ctx = *ctxPtr;
    if (!ctx) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_CTX", 10); return; }

    char* ctxBase = reinterpret_cast<char*>(ctx);
    void* netGame = *reinterpret_cast<void**>(ctxBase + NETGAME_OFFSET);
    if (!netGame) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_NET", 10); return; }

    char* ng = reinterpret_cast<char*>(netGame);

    /* Get local player slot */
    uint16_t rawSlot = *reinterpret_cast<uint16_t*>(ng + 0x178);
    uint16_t slot = rawSlot & 0x1F;

    uint64_t instanceCount = *reinterpret_cast<uint64_t*>(ng + 0x51450 + slot * 0x40);
    int64_t* instances = reinterpret_cast<int64_t*>(
        *reinterpret_cast<uint64_t*>(ng + 0x51420 + slot * 0x40));

    ReadLoadoutResponse resp = {};
    resp.slot = slot;
    resp.instance_count = static_cast<uint16_t>(instanceCount);

    /* Walk instances looking for item slots */
    if (instances && instanceCount > 0 && instanceCount < 16) {
        for (uint64_t i = 0; i < instanceCount; i++) {
            int64_t* entry = instances + i * 8;
            int64_t itemCount = entry[6];
            int64_t* items = reinterpret_cast<int64_t*>(entry[1]);

            if (items && itemCount > 0 && itemCount < 64) {
                for (int64_t j = 0; j < itemCount; j++) {
                    int64_t slotSym = items[j * 2];
                    int64_t itemSym = items[j * 2 + 1];
                    /* Mirror each slot:item pair */
                    uint64_t pair[2] = {static_cast<uint64_t>(slotSym),
                                        static_cast<uint64_t>(itemSym)};
                    MirrorResponse(NEVR_RESPONSE_SYM, pair, 16);
                }
            }
        }
    }

    /* Also try reading bodytype from player state (netgame + 0x3c0 + slot*0x250) */
    int64_t playerState = *reinterpret_cast<int64_t*>(ng + 0x3c0 + slot * 0x250);
    if (playerState) {
        resp.bodytype = static_cast<uint64_t>(*reinterpret_cast<int64_t*>(playerState));
    }

    MirrorResponse(NEVR_RESPONSE_SYM, &resp, sizeof(resp));
}

static void HandleChassisSwap(uintptr_t base, const ChassisSwapPayload* payload) {
    char* baseAddr = reinterpret_cast<char*>(base);
    void** ctxPtr = reinterpret_cast<void**>(baseAddr + GAME_CONTEXT_OFFSET);
    void* ctx = *ctxPtr;
    if (!ctx) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_CTX", 10); return; }

    char* ctxBase = reinterpret_cast<char*>(ctx);
    void* netGame = *reinterpret_cast<void**>(ctxBase + NETGAME_OFFSET);
    if (!netGame) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_NET", 10); return; }

    char* ng = reinterpret_cast<char*>(netGame);

    /* Read current game mode hash at netgame + 0x06A0 */
    uint64_t oldMode = *reinterpret_cast<uint64_t*>(ng + 0x06A0);

    /* payload->new_chassis_id encodes the command:
     *   0 = read only
     *   1 = flip combat bit (bit 1) in game flags byte at *(netgame+0x2DA0)
     *   2 = set combat bit on
     *   3 = set combat bit off
     *   other = write as game mode hash to netgame+0x06A0
     */
    uint64_t cmd = payload->new_chassis_id;
    uint64_t* flagsPtrPtrEarly = reinterpret_cast<uint64_t*>(ng + 0x2DA0);
    uint64_t flagsPtrEarly = *flagsPtrPtrEarly;

    /* Combat bit is byte[2] bit 1 (0x02) at the flags pointer.
     * Arena: byte[2]=0x80, Combat: byte[2]=0x82 */
    if (cmd == 1 && flagsPtrEarly) {
        uint8_t* fb = reinterpret_cast<uint8_t*>(flagsPtrEarly) + 2;
        *fb ^= 0x02;  /* toggle combat bit */
    } else if (cmd == 2 && flagsPtrEarly) {
        uint8_t* fb = reinterpret_cast<uint8_t*>(flagsPtrEarly) + 2;
        *fb |= 0x02;  /* enable combat */
    } else if (cmd == 3 && flagsPtrEarly) {
        uint8_t* fb = reinterpret_cast<uint8_t*>(flagsPtrEarly) + 2;
        *fb &= ~0x02; /* disable combat */
    } else if (cmd > 3) {
        *reinterpret_cast<uint64_t*>(ng + 0x06A0) = cmd;
    }

    uint64_t newMode = *reinterpret_cast<uint64_t*>(ng + 0x06A0);

    /* Read game flags: *(*(netgame+0x2DA0)) — double deref, first byte is the bitfield */
    uint64_t* flagsPtrPtr = reinterpret_cast<uint64_t*>(ng + 0x2DA0);
    uint64_t flagsPtr = *flagsPtrPtr;
    uint8_t flagsByte = flagsPtr ? *reinterpret_cast<uint8_t*>(flagsPtr) : 0;

    /* Also read game_flags at context+0x7AE0 (CR15Game flags, different bitfield) */
    uint64_t gameFlags = *reinterpret_cast<uint64_t*>(
        *reinterpret_cast<char**>(reinterpret_cast<char*>(base) + GAME_CONTEXT_OFFSET) + 0x7AE0);

    /* Dump 64 bytes from the flags pointer for analysis */
    uint8_t dump[64] = {};
    if (flagsPtr) {
        std::memcpy(dump, reinterpret_cast<void*>(flagsPtr), 64);
    }

    /* Response: old_mode(8) + new_mode(8) + flagsPtr(8) + 64 bytes of dump */
    uint8_t result[88];
    std::memcpy(result, &oldMode, 8);
    std::memcpy(result + 8, &newMode, 8);
    std::memcpy(result + 16, &flagsPtr, 8);
    std::memcpy(result + 24, dump, 64);
    MirrorResponse(NEVR_RESPONSE_SYM, result, sizeof(result));
}

/* ── Deferred game-thread commands ────────────────────────────────── */

static std::atomic<int> g_deferred_cmd{0};  /* 0=none, 4=enable bodies */

/* ── Enable body components (MUST run on game thread) ────────────── */

static void HandleEnableBodies(uintptr_t base) {
    char* baseAddr = reinterpret_cast<char*>(base);

    /* Get game context */
    void* ctx = *reinterpret_cast<void**>(baseAddr + GAME_CONTEXT_OFFSET);
    if (!ctx) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_CTX", 10); return; }

    /* Get game space */
    void* gameSpace = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + GAMESPACE_OFFSET);
    if (!gameSpace) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_GS", 9); return; }

    /* Resolve EnableBodyComponents function */
    auto enableFn = reinterpret_cast<EnableBodyComponents_fn>(
        nevr::ResolveVA(base, VA_ENABLE_BODY_COMPONENTS));
    if (!enableFn) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_FN", 9); return; }

    /* Call EnableBodyComponents with combat body hash only */
    enableFn(0, gameSpace, static_cast<int64_t>(0xe32dc7ddd18057a4ULL));

    uint64_t result[2] = { reinterpret_cast<uint64_t>(gameSpace), 0xe32dc7ddd18057a4ULL };
    MirrorResponse(NEVR_RESPONSE_SYM, result, sizeof(result));
}

/* ── Dump weapon scenario data ────────────────────────────────────── */
/* Source: CR15NetWeaponTypeChange.cpp:63-78
 * OnWeaponTypeChanged reads *(arg1+0xB8)+0x2E00..0x2E4B as weapon scenario.
 * arg1 is a component system (CR15NetGunCS).
 * We enumerate all CS instances and check each for weapon scenario data.
 *
 * The gamespace CS table at +0x420 has entries: stride 0x10 = [hash, cs_ptr]
 * Source: CLevelResource.cpp:298-308
 */

static void HandleDumpWeapon(uintptr_t base) {
    char* baseAddr = reinterpret_cast<char*>(base);
    void* ctx = *reinterpret_cast<void**>(baseAddr + GAME_CONTEXT_OFFSET);
    if (!ctx) return;

    /* Try both gamespaces */
    for (int gsIdx = 0; gsIdx < 2; gsIdx++) {
        uint64_t gsOff = (gsIdx == 0) ? GAMESPACE_OFFSET : GAMESPACE2_OFFSET;
        void* gs = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + gsOff);
        if (!gs) continue;

        char* g = reinterpret_cast<char*>(gs);
        int64_t typeArrayPtr = *reinterpret_cast<int64_t*>(g + 0x420);
        int64_t typeCount = *reinterpret_cast<int64_t*>(g + 0x448);

        if (typeArrayPtr <= 0 || typeCount <= 0 || typeCount > 500) continue;

        for (int64_t i = 0; i < typeCount; i++) {
            int64_t csHash = *reinterpret_cast<int64_t*>(typeArrayPtr + i * 0x10);
            int64_t csPtr = *reinterpret_cast<int64_t*>(typeArrayPtr + i * 0x10 + 8);
            if (csPtr == 0) continue;

            /* Check if this CS has a context at +0xB8 */
            int64_t csContext = *reinterpret_cast<int64_t*>(csPtr + 0xB8);
            if (csContext == 0) continue;

            /* Check weapon scenario at context + 0x2E00 */
            int64_t weaponScenario = *reinterpret_cast<int64_t*>(csContext + 0x2E00);

            if (weaponScenario != 0) {
                /* Found a CS with weapon scenario data! Dump the block. */
                uint8_t msg[96];  /* tag(8) + hash(8) + ptr(8) + context(8) + scenario_block(64) */
                uint64_t tag = 0x5750000000000000ULL | static_cast<uint64_t>(gsIdx);  /* "WP" */
                std::memcpy(msg, &tag, 8);
                std::memcpy(msg + 8, &csHash, 8);
                std::memcpy(msg + 16, &csPtr, 8);
                std::memcpy(msg + 24, &csContext, 8);
                /* Dump 0x2E00..0x2E40 (64 bytes of weapon scenario) */
                std::memcpy(msg + 32, reinterpret_cast<void*>(csContext + 0x2E00), 64);
                MirrorResponse(NEVR_RESPONSE_SYM, msg, 96);
            }
        }

        /* Also report total CS count for this gamespace */
        uint64_t gsInfo[3] = {
            0x5750494E464F0000ULL | static_cast<uint64_t>(gsIdx), /* "WPINFO" */
            static_cast<uint64_t>(typeCount),
            static_cast<uint64_t>(gsOff)
        };
        MirrorResponse(NEVR_RESPONSE_SYM, gsInfo, sizeof(gsInfo));
    }
}

/* ── Dump actorDataRes component type table ───────────────────────── */
/* Source: echovr-reconstruction CR15NetGameBatch176.cpp:250-310
 * gamespace + 0x370 = actorDataRes (set by CreateComponentSystems)
 * actorDataRes + 0xB0 = actor count
 * *(actorDataRes + 0x160) = component type index table (uint16 per actor)
 */

static void HandleDumpActorData(uintptr_t base) {
    char* baseAddr = reinterpret_cast<char*>(base);
    void* ctx = *reinterpret_cast<void**>(baseAddr + GAME_CONTEXT_OFFSET);
    if (!ctx) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_CTX", 10); return; }

    void* gameSpace = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + GAMESPACE_OFFSET);
    if (!gameSpace) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_GS", 9); return; }

    char* gs = reinterpret_cast<char*>(gameSpace);

    /* actorDataRes = *(gamespace + 0x370) — CR15NetGameBatch176.cpp:251 */
    int64_t actorDataRes = *reinterpret_cast<int64_t*>(gs + 0x370);
    if (!actorDataRes) { MirrorResponse(NEVR_RESPONSE_SYM, "ERR:NO_ADR", 10); return; }

    char* adr = reinterpret_cast<char*>(actorDataRes);

    /* actorCount = *(actorDataRes + 0xB0) — CR15NetGameBatch176.cpp:252 */
    uint64_t actorCount = *reinterpret_cast<uint64_t*>(adr + 0xB0);
    if (actorCount == 0xFFFFFFFFFFFFFFFF) actorCount = 0xFFFF;
    actorCount &= 0xFFFF;

    /* compTypeTable = *(actorDataRes + 0x160) — CR15NetGameBatch176.cpp:310 */
    uint16_t* compTypeTable = reinterpret_cast<uint16_t*>(
        *reinterpret_cast<int64_t*>(adr + 0x160));

    /* Send summary: actorCount + gamespace ptr + actorDataRes ptr */
    uint64_t summary[3] = {
        actorCount,
        reinterpret_cast<uint64_t>(gameSpace),
        static_cast<uint64_t>(actorDataRes)
    };
    MirrorResponse(NEVR_RESPONSE_SYM, summary, sizeof(summary));

    /* Send component type indices (uint16 per actor, max 512) */
    if (compTypeTable && actorCount > 0 && actorCount <= 512) {
        /* Pack indices: up to 200 per packet (400 bytes) */
        uint16_t buf[200];
        uint64_t sent = 0;
        while (sent < actorCount) {
            uint64_t batch = actorCount - sent;
            if (batch > 200) batch = 200;
            std::memcpy(buf, compTypeTable + sent, batch * 2);
            MirrorResponse(NEVR_RESPONSE_SYM, buf, static_cast<uint32_t>(batch * 2));
            sent += batch;
        }
    }

    /* Also try the second gamespace at +0x7AF8 */
    void* gameSpace2 = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + GAMESPACE2_OFFSET);
    if (gameSpace2 && gameSpace2 != gameSpace) {
        char* gs2 = reinterpret_cast<char*>(gameSpace2);
        int64_t adr2 = *reinterpret_cast<int64_t*>(gs2 + 0x370);
        if (adr2 && adr2 != actorDataRes) {
            char* a2 = reinterpret_cast<char*>(adr2);
            uint64_t count2 = *reinterpret_cast<uint64_t*>(a2 + 0xB0) & 0xFFFF;
            uint64_t alt[4] = { count2, reinterpret_cast<uint64_t>(gameSpace2),
                                static_cast<uint64_t>(adr2), GAMESPACE2_OFFSET };
            MirrorResponse(NEVR_RESPONSE_SYM, alt, sizeof(alt));

            uint16_t* t2 = reinterpret_cast<uint16_t*>(*reinterpret_cast<int64_t*>(a2 + 0x160));
            if (t2 && count2 > 0 && count2 <= 512) {
                uint16_t b2[200];
                uint64_t s = 0;
                while (s < count2) {
                    uint64_t batch = (count2 - s > 200) ? 200 : count2 - s;
                    std::memcpy(b2, t2 + s, batch * 2);
                    MirrorResponse(NEVR_RESPONSE_SYM, b2, static_cast<uint32_t>(batch * 2));
                    s += batch;
                }
            }
        }
    }

    /* Dump component system table from BOTH gamespaces.
     * CncaGameSpace struct: +0x020 = component_system_table (CMemBlock)
     * CMemBlock: base_ptr at +0x00, some metadata follows
     * From FindComponentSystemMT: binary search, entries sorted by hash.
     *
     * Let's dump the first 512 bytes of each gamespace to see the CS table structure.
     */
    for (int gsIdx = 0; gsIdx < 2; gsIdx++) {
        void* gsPtr = (gsIdx == 0) ? gameSpace : gameSpace2;
        if (!gsPtr) continue;
        char* g = reinterpret_cast<char*>(gsPtr);

        /* Dump gamespace +0x000 to +0x060 (header + CS table CMemBlock) */
        uint8_t gsDump[512];
        std::memcpy(gsDump, g, 96);  /* first 96 bytes */

        uint8_t msg[104];
        uint64_t tag = (gsIdx == 0) ? 0x4753310000000000ULL : 0x4753320000000000ULL; /* "GS1" / "GS2" */
        std::memcpy(msg, &tag, 8);
        std::memcpy(msg + 8, gsDump, 96);
        MirrorResponse(NEVR_RESPONSE_SYM, msg, 104);

        /* Component type array at +0x420 (from CLevelResource.cpp:288)
         * +0x420: CMemBlock (type array base ptr)
         * +0x448: type count
         * Each entry: stride 0x10 = [int64_t typeHash, int64_t data]
         * Source: CLevelResource.cpp:298-308
         */
        int64_t typeArrayPtr = *reinterpret_cast<int64_t*>(g + 0x420);
        int64_t typeCount = *reinterpret_cast<int64_t*>(g + 0x448);

        uint64_t typeInfo[3] = {
            static_cast<uint64_t>(tag) | 0x0000000000000010ULL, /* tag with marker */
            static_cast<uint64_t>(typeArrayPtr),
            static_cast<uint64_t>(typeCount)
        };
        MirrorResponse(NEVR_RESPONSE_SYM, typeInfo, sizeof(typeInfo));

        /* Dump the type hash array if valid */
        if (typeArrayPtr > 0x100000 && typeCount > 0 && typeCount < 500) {
            /* Read typeCount * 0x10 bytes, send in chunks */
            for (int64_t t = 0; t < typeCount && t < 100; t++) {
                uint64_t entry[2];
                std::memcpy(entry, reinterpret_cast<void*>(typeArrayPtr + t * 0x10), 16);
                /* Send pairs: type hash + data */
                uint8_t entryMsg[24];
                uint64_t entryTag = static_cast<uint64_t>(tag) | 0x0000000000000020ULL;
                std::memcpy(entryMsg, &entryTag, 8);
                std::memcpy(entryMsg + 8, entry, 16);
                MirrorResponse(NEVR_RESPONSE_SYM, entryMsg, 24);
            }
        }

        /* The CS table at +0x020 is a CMemBlock. Read the data pointer from it.
         * CMemBlock typically: [data_ptr(8), size(8), capacity(8), flags(4), ...]
         * Let's read the data ptr and count, then dump entries.
         */
        int64_t csTablePtr = *reinterpret_cast<int64_t*>(g + 0x020);
        int64_t csTableSize = *reinterpret_cast<int64_t*>(g + 0x028);
        /* CMemBlock count might be at a different offset */
        int64_t csTableCap = *reinterpret_cast<int64_t*>(g + 0x030);

        uint64_t csInfo[4] = {
            static_cast<uint64_t>(tag),
            static_cast<uint64_t>(csTablePtr),
            static_cast<uint64_t>(csTableSize),
            static_cast<uint64_t>(csTableCap)
        };
        MirrorResponse(NEVR_RESPONSE_SYM, csInfo, sizeof(csInfo));

        /* If csTablePtr looks valid and csTableSize is reasonable, dump entries */
        if (csTablePtr > 0x100000 && csTableSize > 0 && csTableSize < 0x10000) {
            /* Dump first min(csTableSize, 50) entries * 0x38 bytes (guessed stride) */
            /* Actually, we don't know the stride. Dump raw 1024 bytes from the table. */
            uint8_t tableDump[1024];
            size_t dumpLen = 1024;
            std::memcpy(tableDump, reinterpret_cast<void*>(csTablePtr), dumpLen);

            /* Send in 2 packets of 512 */
            uint8_t pkt[520];
            uint64_t pktTag = tag | 0x0000000000000001ULL;
            std::memcpy(pkt, &pktTag, 8);
            std::memcpy(pkt + 8, tableDump, 512);
            MirrorResponse(NEVR_RESPONSE_SYM, pkt, 520);

            pktTag = tag | 0x0000000000000002ULL;
            std::memcpy(pkt, &pktTag, 8);
            std::memcpy(pkt + 8, tableDump + 512, 512);
            MirrorResponse(NEVR_RESPONSE_SYM, pkt, 520);
        }
    }
}

/* ── Listener thread ──────────────────────────────────────────────── */

static uintptr_t g_game_base = 0;

static void ListenerThreadFunc() {
    uint8_t buf[MAX_PACKET_SIZE];

    while (!g_shutdown_flag.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);

        int n = recvfrom(g_listen_sock,
#ifdef _WIN32
                         reinterpret_cast<char*>(buf),
#else
                         buf,
#endif
                         sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);

        if (n <= 0) continue;
        if (g_shutdown_flag.load(std::memory_order_acquire)) break;

        /* Need at least injection header */
        if (static_cast<size_t>(n) < sizeof(InjectionPacketHeader)) continue;

        uint8_t mode = buf[0];
        const uint8_t* mirror_data = buf + 1;
        size_t mirror_len = static_cast<size_t>(n) - 1;

        MirrorPacketHeader hdr;
        const uint8_t* payload = nullptr;
        if (!DeserializeMirrorPacket(mirror_data, mirror_len, hdr, payload)) continue;

        /* Handle commands that don't need broadcaster first */
        if (mode == INJECT_MODE_READ_LOADOUT && g_game_base) {
            HandleReadLoadout(g_game_base);
            continue;
        } else if (mode == INJECT_MODE_CHASSIS_SWAP && g_game_base) {
            if (hdr.payload_size >= sizeof(ChassisSwapPayload)) {
                HandleChassisSwap(g_game_base,
                    reinterpret_cast<const ChassisSwapPayload*>(payload));
            }
            continue;
        } else if (mode == INJECT_MODE_ENABLE_BODIES && g_game_base) {
            HandleEnableBodies(g_game_base);
            continue;
        } else if (mode == INJECT_MODE_DUMP_ACTOR_DATA && g_game_base) {
            HandleDumpActorData(g_game_base);
            continue;
        } else if (mode == INJECT_MODE_DUMP_WEAPON && g_game_base) {
            HandleDumpWeapon(g_game_base);
            continue;
        }

        void* broadcaster = g_broadcaster_ptr.load(std::memory_order_acquire);
        if (!broadcaster) continue;

        if (mode == INJECT_MODE_SEND && g_original_send) {
            g_original_send(broadcaster, hdr.msg_symbol,
                static_cast<int32_t>(hdr.flags),
                payload, hdr.payload_size,
                nullptr, 0, 0, 0, 0.0f, 0);
        } else if (mode == INJECT_MODE_LOCAL_DISPATCH && g_original_receive_local) {
            g_original_receive_local(broadcaster, hdr.msg_symbol,
                "", payload, hdr.payload_size);
        }
    }
}

/* ── Address verification ─────────────────────────────────────────── */

static bool VerifyAddresses(uintptr_t base) {
    void* send_addr = nevr::ResolveVA(base, nevr::addresses::VA_BROADCASTER_SEND);
    void* recv_addr = nevr::ResolveVA(base, nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL);

    /* Basic check: addresses should be in executable memory range */
    if (!send_addr || !recv_addr) {
        std::fprintf(stderr, "[broadcaster_bridge] Address resolution failed: send=%p recv=%p (base=0x%llX)\n",
                     send_addr, recv_addr, (unsigned long long)base);
        return false;
    }

    std::fprintf(stderr, "[broadcaster_bridge] Resolved: send=%p recv=%p\n", send_addr, recv_addr);

#ifdef _WIN32
    /* Log actual prologue bytes for debugging */
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(send_addr);
    std::fprintf(stderr, "[broadcaster_bridge] Send prologue: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);

    /* Expected prologue: 48 89 5C 24 (mov [rsp+...], rbx) */
    const uint8_t expected_send[] = {0x48, 0x89, 0x5C, 0x24};
    if (!nevr::ValidatePrologue(send_addr, expected_send, sizeof(expected_send))) {
        std::fprintf(stderr, "[broadcaster_bridge] Prologue mismatch — proceeding anyway (Wine compatibility)\n");
    }
#endif

    return true;
}

/* ── Socket helpers ───────────────────────────────────────────────── */

static socket_t CreateNonBlockingUdpSocket() {
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCK) return INVALID_SOCK;

#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif

    return s;
}

static socket_t CreateBlockingUdpSocket(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCK) return INVALID_SOCK;

    /* Allow address reuse */
    int opt = 1;
    setsockopt(s,SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
               reinterpret_cast<const char*>(&opt),
#else
               &opt,
#endif
               sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCK;
    }

    return s;
}

/* ── Public lifecycle ─────────────────────────────────────────────── */

int Initialize(uintptr_t base_addr, const char* config_path) {
    g_game_base = base_addr;

    /* Load config */
    std::string json;
    if (config_path) {
        json = nevr::LoadConfigFile(config_path);
    }
    g_config = ParseConfig(json);

    /* Verify hook target addresses (only meaningful in-process) */
#ifdef _WIN32
    if (!VerifyAddresses(base_addr)) {
        return -1;
    }
#endif

#ifdef _WIN32
    /* Initialize Winsock */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* Set up mirror socket (non-blocking, for sending) */
    g_mirror_sock = CreateNonBlockingUdpSocket();

    g_target_addr.sin_family = AF_INET;
    g_target_addr.sin_port   = htons(g_config.target_port);
    inet_pton(AF_INET, g_config.target_ip.c_str(), &g_target_addr.sin_addr);

    /* Set up listen socket (blocking, for the listener thread) */
    g_listen_sock = CreateBlockingUdpSocket(g_config.listen_port);

    /* Resolve function pointers */
    void* send_ptr = nevr::ResolveVA(base_addr, nevr::addresses::VA_BROADCASTER_SEND);
    void* recv_ptr = nevr::ResolveVA(base_addr, nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL);

    /* Install hooks via MinHook */
    MH_Initialize();

    /* Hook CBroadcaster_Send */
    if (g_hooks.CreateAndEnable(send_ptr,
                      reinterpret_cast<void*>(&HookedBroadcasterSend),
                      reinterpret_cast<void**>(&g_original_send)) != MH_OK) {
        return -3;
    }

    /* Hook CBroadcaster_ReceiveLocal (non-fatal) */
    g_hooks.CreateAndEnable(recv_ptr,
                      reinterpret_cast<void*>(&HookedBroadcasterReceiveLocal),
                      reinterpret_cast<void**>(&g_original_receive_local));

    /* Spawn listener thread */
    g_shutdown_flag.store(false, std::memory_order_release);
    g_listener_thread = std::thread(ListenerThreadFunc);

    return 0;
}

void Shutdown() {
    /* Signal threads to stop */
    g_shutdown_flag.store(true, std::memory_order_release);

    /* Send a dummy packet to unblock recvfrom */
    if (g_listen_sock != INVALID_SOCK) {
        sockaddr_in self_addr{};
        self_addr.sin_family      = AF_INET;
        self_addr.sin_port        = htons(g_config.listen_port);
        self_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        socket_t wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wake_sock != INVALID_SOCK) {
            uint8_t dummy = 0;
            sendto(wake_sock,
#ifdef _WIN32
                   reinterpret_cast<const char*>(&dummy),
#else
                   &dummy,
#endif
                   1, 0,
                   reinterpret_cast<const sockaddr*>(&self_addr),
                   sizeof(self_addr));
            closesocket(wake_sock);
        }
    }

    /* Join listener thread */
    if (g_listener_thread.joinable()) {
        g_listener_thread.join();
    }

    /* Remove hooks */
    g_hooks.RemoveAll();

    /* Close sockets */
    if (g_mirror_sock != INVALID_SOCK) {
        closesocket(g_mirror_sock);
        g_mirror_sock = INVALID_SOCK;
    }
    if (g_listen_sock != INVALID_SOCK) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCK;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    g_broadcaster_ptr.store(nullptr, std::memory_order_release);
    g_original_send  = nullptr;
    g_original_receive_local = nullptr;
}

void OnFrame() {
    int cmd = g_deferred_cmd.exchange(0, std::memory_order_acq_rel);
    if (cmd == 4 && g_game_base) {
        HandleEnableBodies(g_game_base);
    }
}

} // namespace nevr::broadcaster_bridge
