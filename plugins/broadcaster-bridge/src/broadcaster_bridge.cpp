/* SYNTHESIS -- custom tool code, not from binary */

#include "broadcaster_bridge.h"
#include "nevr_common.h"

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
static constexpr void* MH_ALL_HOOKS = nullptr;
inline void closesocket(socket_t s)                                        { close(s); }
#endif

namespace nevr::broadcaster_bridge {

/* ── Module state ──────────────────────────────────────────────────── */

static BridgeConfig           g_config;
static std::atomic<void*>     g_broadcaster_ptr{nullptr};
static CBroadcasterSend_fn    g_original_send      = nullptr;
static CBroadcasterReceiveLocal_fn g_receive_local  = nullptr;

static socket_t               g_mirror_sock  = INVALID_SOCK;
static socket_t               g_listen_sock  = INVALID_SOCK;
static sockaddr_in            g_target_addr{};

static std::thread            g_listener_thread;
static std::atomic<bool>      g_shutdown_flag{false};

/* Rate limiter state */
static std::atomic<uint32_t>  g_send_count{0};
static std::atomic<int64_t>   g_rate_window_start{0};

/* ── JSON config parser (minimal, no dependency) ───────────────────── */

static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};

    auto q1 = json.find('"', pos + 1);
    if (q1 == std::string::npos) return {};

    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};

    return json.substr(q1 + 1, q2 - q1 - 1);
}

static bool ExtractJsonBool(const std::string& json, const std::string& key, bool default_val) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_val;

    auto val_start = json.find_first_not_of(" \t\r\n", pos + 1);
    if (val_start == std::string::npos) return default_val;

    if (json.compare(val_start, 4, "true") == 0) return true;
    if (json.compare(val_start, 5, "false") == 0) return false;
    return default_val;
}

static int ExtractJsonInt(const std::string& json, const std::string& key, int default_val) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_val;

    auto val_start = json.find_first_not_of(" \t\r\n", pos + 1);
    if (val_start == std::string::npos) return default_val;

    char* end = nullptr;
    long val = std::strtol(json.c_str() + val_start, &end, 10);
    if (end == json.c_str() + val_start) return default_val;
    return static_cast<int>(val);
}

BridgeConfig ParseConfig(const std::string& json_text) {
    BridgeConfig cfg;
    if (json_text.empty()) return cfg;

    /* Parse "udp_debug_target": "host:port" */
    std::string target = ExtractJsonString(json_text, "udp_debug_target");
    if (!target.empty()) {
        auto colon = target.rfind(':');
        if (colon != std::string::npos) {
            cfg.target_ip = target.substr(0, colon);
            cfg.target_port = static_cast<uint16_t>(
                std::strtol(target.substr(colon + 1).c_str(), nullptr, 10));
        }
    }

    cfg.listen_port    = static_cast<uint16_t>(
        ExtractJsonInt(json_text, "listen_port", cfg.listen_port));
    cfg.mirror_send    = ExtractJsonBool(json_text, "mirror_send", cfg.mirror_send);
    cfg.mirror_receive = ExtractJsonBool(json_text, "mirror_receive", cfg.mirror_receive);
    cfg.log_messages   = ExtractJsonBool(json_text, "log_messages", cfg.log_messages);

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

/* ── Listener thread ──────────────────────────────────────────────── */

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

        void* broadcaster = g_broadcaster_ptr.load(std::memory_order_acquire);
        if (!broadcaster) continue; /* broadcaster not captured yet, drop */

        if (mode == INJECT_MODE_SEND && g_original_send) {
            g_original_send(broadcaster, hdr.msg_symbol,
                static_cast<int32_t>(hdr.flags),
                payload, hdr.payload_size,
                nullptr, 0, 0, 0, 0.0f, 0);
        } else if (mode == INJECT_MODE_LOCAL_DISPATCH && g_receive_local) {
            g_receive_local(broadcaster, hdr.msg_symbol,
                static_cast<int32_t>(hdr.flags),
                payload, hdr.payload_size);
        }
    }
}

/* ── Address verification ─────────────────────────────────────────── */

static bool VerifyAddresses(uintptr_t base) {
    void* send_addr = nevr::ResolveVA(base, nevr::addresses::VA_BROADCASTER_SEND);
    void* recv_addr = nevr::ResolveVA(base, nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL);

    /* Basic check: addresses should be in executable memory range */
    if (!send_addr || !recv_addr) return false;

    /*
     * On a real Windows build, we would check known prologue bytes here.
     * For portability and testing, we skip byte-level validation on non-Windows.
     */
#ifdef _WIN32
    /* Expected prologue: 48 89 5C 24 (mov [rsp+...], rbx) */
    const uint8_t expected_send[] = {0x48, 0x89, 0x5C, 0x24};
    if (!nevr::ValidatePrologue(send_addr, expected_send, sizeof(expected_send))) {
        return false;
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
    g_receive_local = reinterpret_cast<CBroadcasterReceiveLocal_fn>(recv_ptr);

    /* Install hook via MinHook */
    if (MH_Initialize() != MH_OK) return -2;

    if (MH_CreateHook(send_ptr,
                      reinterpret_cast<void*>(&HookedBroadcasterSend),
                      reinterpret_cast<void**>(&g_original_send)) != MH_OK) {
        MH_Uninitialize();
        return -3;
    }

    if (MH_EnableHook(send_ptr) != MH_OK) {
        MH_RemoveHook(send_ptr);
        MH_Uninitialize();
        return -4;
    }

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
    void* send_ptr = nullptr; /* MinHook tracks hooks internally */
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

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
    g_receive_local  = nullptr;
}

} // namespace nevr::broadcaster_bridge
