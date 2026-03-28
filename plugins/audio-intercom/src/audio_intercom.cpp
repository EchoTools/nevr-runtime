/* SYNTHESIS -- custom tool code, not from binary */

#include "audio_intercom.h"
#include "nevr_common.h"

#include <cstring>
#include <atomic>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <MinHook.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

namespace nevr::audio_intercom {

/* ========================================================================
 * Module state
 * ======================================================================== */
namespace {

/* Resolved function pointers */
VoipRouter_fn          g_voip_router = nullptr;
VoipFrameProcessor_fn  g_original_voip_processor = nullptr;
LobbyJoinHandler_fn    g_original_lobby_join = nullptr;
LobbyLeftHandler_fn    g_original_lobby_left = nullptr;

/* Streaming state */
std::atomic<bool>     g_streaming_active{false};
std::atomic<uint64_t> g_frame_number{0};

/* Configuration */
IntercomConfig g_config;

/* Ring buffer (lock-free SPSC: UDP thread writes, VoIP hook reads) */
struct RingBuffer {
    RingEntry entries[RING_BUFFER_CAPACITY];
    std::atomic<uint64_t> write_pos{0};
    std::atomic<uint64_t> read_pos{0};

    bool Push(const uint8_t* data, uint32_t size) {
        if (size == 0 || size > MAX_FRAME_SIZE) return false;
        uint64_t wp = write_pos.load(std::memory_order_relaxed);
        uint64_t rp = read_pos.load(std::memory_order_acquire);
        /* If full, advance read pointer (drop oldest) */
        if (wp - rp >= RING_BUFFER_CAPACITY) {
            read_pos.store(rp + 1, std::memory_order_release);
        }
        RingEntry& entry = entries[wp % RING_BUFFER_CAPACITY];
        std::memcpy(entry.data, data, size);
        entry.size = size;
        write_pos.store(wp + 1, std::memory_order_release);
        return true;
    }

    bool Pop(uint8_t* out_data, uint32_t& out_size) {
        uint64_t rp = read_pos.load(std::memory_order_relaxed);
        uint64_t wp = write_pos.load(std::memory_order_acquire);
        if (rp >= wp) return false;
        const RingEntry& entry = entries[rp % RING_BUFFER_CAPACITY];
        out_size = entry.size;
        std::memcpy(out_data, entry.data, entry.size);
        read_pos.store(rp + 1, std::memory_order_release);
        return true;
    }

    void Clear() {
        read_pos.store(0, std::memory_order_relaxed);
        write_pos.store(0, std::memory_order_relaxed);
    }

    size_t Size() const {
        uint64_t wp = write_pos.load(std::memory_order_acquire);
        uint64_t rp = read_pos.load(std::memory_order_acquire);
        return (wp >= rp) ? static_cast<size_t>(wp - rp) : 0;
    }
};

RingBuffer g_ring_buffer;

/* UDP listener thread state */
std::atomic<bool> g_udp_running{false};
std::thread       g_udp_thread;

#if defined(_WIN32)
SOCKET g_udp_socket = INVALID_SOCKET;
#else
int g_udp_socket = -1;
#endif

bool g_hooks_installed = false;

} // anonymous namespace

/* ========================================================================
 * Config parsing (JSON, replaces registry config)
 * ======================================================================== */
IntercomConfig ParseConfig(const std::string& json_text) {
    IntercomConfig cfg;
    if (json_text.empty()) return cfg;

    /* Minimal JSON parser for flat key-value config */
    auto find_value = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = json_text.find(needle);
        if (pos == std::string::npos) return {};
        pos = json_text.find(':', pos + needle.size());
        if (pos == std::string::npos) return {};
        pos++;
        /* Skip whitespace */
        while (pos < json_text.size() && (json_text[pos] == ' ' || json_text[pos] == '\t'))
            pos++;
        if (pos >= json_text.size()) return {};
        /* Extract value until comma, brace, or newline */
        size_t end = json_text.find_first_of(",}\n", pos);
        if (end == std::string::npos) end = json_text.size();
        std::string val = json_text.substr(pos, end - pos);
        /* Trim whitespace */
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r'))
            val.pop_back();
        return val;
    };

    std::string v;

    v = find_value("enabled");
    if (!v.empty()) cfg.enabled = (v == "true");

    v = find_value("listen_port");
    if (!v.empty()) {
        int port = std::atoi(v.c_str());
        if (port > 0 && port <= 65535) cfg.listen_port = static_cast<uint16_t>(port);
    }

    v = find_value("speaker_id");
    if (!v.empty()) {
        unsigned long long sid = std::strtoull(v.c_str(), nullptr, 10);
        cfg.speaker_id = static_cast<uint32_t>(sid);
    }

    v = find_value("volume");
    if (!v.empty()) {
        int vol = std::atoi(v.c_str());
        if (vol >= 0 && vol <= 100) cfg.volume = static_cast<uint32_t>(vol);
    }

    cfg.valid = true;
    return cfg;
}

/* ========================================================================
 * UDP listener thread
 * ======================================================================== */
#if defined(_WIN32)

static void UdpListenerThread(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    g_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_socket == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    /* Set receive timeout so we can check g_udp_running periodically */
    DWORD timeout_ms = 100;
    setsockopt(g_udp_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_udp_socket, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        closesocket(g_udp_socket);
        g_udp_socket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    uint8_t recv_buf[MAX_UDP_PACKET_SIZE];
    while (g_udp_running.load(std::memory_order_acquire)) {
        int n = recvfrom(g_udp_socket, reinterpret_cast<char*>(recv_buf),
                         static_cast<int>(sizeof(recv_buf)), 0, nullptr, nullptr);
        if (n <= 0) continue;

        /*
         * Wire format: {uint32_t frame_size | uint8_t opus_data[frame_size]}
         */
        if (n < static_cast<int>(sizeof(uint32_t))) continue;
        uint32_t frame_size = 0;
        std::memcpy(&frame_size, recv_buf, sizeof(uint32_t));
        if (frame_size == 0 || frame_size > MAX_FRAME_SIZE) continue;
        if (n < static_cast<int>(sizeof(uint32_t) + frame_size)) continue;

        g_ring_buffer.Push(recv_buf + sizeof(uint32_t), frame_size);
    }

    closesocket(g_udp_socket);
    g_udp_socket = INVALID_SOCKET;
    WSACleanup();
}

#else

static void UdpListenerThread(uint16_t port) {
    g_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_socket < 0) return;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; /* 100ms */
    setsockopt(g_udp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_udp_socket, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
        return;
    }

    uint8_t recv_buf[MAX_UDP_PACKET_SIZE];
    while (g_udp_running.load(std::memory_order_acquire)) {
        ssize_t n = recvfrom(g_udp_socket, recv_buf, sizeof(recv_buf),
                             0, nullptr, nullptr);
        if (n <= 0) continue;

        if (n < static_cast<ssize_t>(sizeof(uint32_t))) continue;
        uint32_t frame_size = 0;
        std::memcpy(&frame_size, recv_buf, sizeof(uint32_t));
        if (frame_size == 0 || frame_size > MAX_FRAME_SIZE) continue;
        if (n < static_cast<ssize_t>(sizeof(uint32_t) + frame_size)) continue;

        g_ring_buffer.Push(recv_buf + sizeof(uint32_t), frame_size);
    }

    close(g_udp_socket);
    g_udp_socket = -1;
}

#endif

/* ========================================================================
 * Frame injection (called from VoIP frame processor hook)
 * ======================================================================== */
static void InjectNextFrame(void* broadcaster_ctx) {
    if (!g_streaming_active.load(std::memory_order_acquire)) return;
    if (!g_voip_router || !broadcaster_ctx) return;

    uint8_t opus_data[MAX_FRAME_SIZE];
    uint32_t opus_size = 0;
    if (!g_ring_buffer.Pop(opus_data, opus_size)) return;

    /* Build SR15NetUserVoipEvent + trailing payload buffer */
    constexpr uint32_t kMaxPayloadSize = MAX_FRAME_SIZE;
    alignas(uint64_t) uint8_t buffer[sizeof(SR15NetUserVoipEvent) + kMaxPayloadSize] = {};
    auto* packet = reinterpret_cast<SR15NetUserVoipEvent*>(buffer);
    packet->speaker_id      = g_config.speaker_id;
    packet->frame_number    = g_frame_number.fetch_add(1);
    packet->sample_count    = SAMPLES_PER_FRAME;
    packet->codec_type      = 0;
    packet->reserved[0]     = 0;
    packet->reserved[1]     = 0;
    packet->reserved[2]     = 0;
    packet->sample_rate     = SAMPLE_RATE;
    packet->pcm_buffer_size = opus_size;
    packet->pcm_buffer      = buffer + sizeof(SR15NetUserVoipEvent);
    std::memcpy(buffer + sizeof(SR15NetUserVoipEvent), opus_data, opus_size);

    g_voip_router(broadcaster_ctx,
                  BROADCAST_PEER_ID,
                  buffer,
                  static_cast<uint32_t>(sizeof(SR15NetUserVoipEvent) + opus_size),
                  VOIP_PRIORITY,
                  VOIP_TIMEOUT_MS);
}

/* ========================================================================
 * Hook functions (same pattern as streamed_audio_injector)
 * ======================================================================== */
static void HookedVoipFrameProcessor(void* broadcaster_ctx) {
    if (g_original_voip_processor) {
        g_original_voip_processor(broadcaster_ctx);
    }
    InjectNextFrame(broadcaster_ctx);
}

static void HookedLobbyJoin(void* event_data) {
    if (g_original_lobby_join) {
        g_original_lobby_join(event_data);
    }
    g_frame_number.store(0, std::memory_order_relaxed);
    g_ring_buffer.Clear();
    g_streaming_active.store(true, std::memory_order_release);
}

static void HookedLobbyLeft(void* event_data) {
    g_streaming_active.store(false, std::memory_order_release);
    if (g_original_lobby_left) {
        g_original_lobby_left(event_data);
    }
}

/* ========================================================================
 * Address verification
 * ======================================================================== */
#if defined(_WIN32)
static bool VerifyAddresses(uintptr_t base_addr) {
    /* Check that resolved addresses point to valid code */
    void* voip_proc = nevr::ResolveVA(base_addr, VA_VOIP_FRAME_PROCESSOR);
    void* lobby_join = nevr::ResolveVA(base_addr, VA_LOBBY_JOIN_HANDLER);
    void* lobby_left = nevr::ResolveVA(base_addr, VA_LOBBY_LEFT_HANDLER);
    void* voip_router = nevr::ResolveVA(base_addr, VA_VOIP_ROUTER);

    if (!voip_proc || !lobby_join || !lobby_left || !voip_router) return false;

    /* Basic sanity: addresses should be within reasonable range */
    uintptr_t min_addr = base_addr;
    uintptr_t max_addr = base_addr + 0x2000000; /* ~32MB image */
    auto in_range = [&](void* p) {
        auto a = reinterpret_cast<uintptr_t>(p);
        return a >= min_addr && a < max_addr;
    };

    return in_range(voip_proc) && in_range(lobby_join) &&
           in_range(lobby_left) && in_range(voip_router);
}
#endif

/* ========================================================================
 * Lifecycle
 * ======================================================================== */
int Initialize(uintptr_t base_addr, const char* config_path) {
#if defined(_WIN32)
    /* Load config */
    if (config_path) {
        std::string json = nevr::LoadConfigFile(config_path);
        g_config = ParseConfig(json);
    }
    if (!g_config.valid) {
        /* Use defaults */
        g_config = IntercomConfig{};
        g_config.valid = true;
    }

    if (!g_config.enabled) return 0;

    /* Verify addresses */
    if (!VerifyAddresses(base_addr)) return -1;

    /* Resolve VoIP router */
    g_voip_router = reinterpret_cast<VoipRouter_fn>(
        nevr::ResolveVA(base_addr, VA_VOIP_ROUTER));
    if (!g_voip_router) return -1;

    /* Install hooks via MinHook */
    if (MH_Initialize() != MH_OK) return -1;

    void* target;

    target = nevr::ResolveVA(base_addr, VA_VOIP_FRAME_PROCESSOR);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedVoipFrameProcessor),
                      reinterpret_cast<void**>(&g_original_voip_processor)) != MH_OK) {
        MH_Uninitialize();
        return -1;
    }

    target = nevr::ResolveVA(base_addr, VA_LOBBY_JOIN_HANDLER);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedLobbyJoin),
                      reinterpret_cast<void**>(&g_original_lobby_join)) != MH_OK) {
        MH_Uninitialize();
        return -1;
    }

    target = nevr::ResolveVA(base_addr, VA_LOBBY_LEFT_HANDLER);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedLobbyLeft),
                      reinterpret_cast<void**>(&g_original_lobby_left)) != MH_OK) {
        MH_Uninitialize();
        return -1;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        return -1;
    }

    g_hooks_installed = true;

    /* Start UDP listener thread */
    g_udp_running.store(true, std::memory_order_release);
    g_udp_thread = std::thread(UdpListenerThread, g_config.listen_port);

    return 0;
#else
    (void)base_addr;
    (void)config_path;
    return -1;
#endif
}

void Shutdown() {
    /* Stop UDP listener */
    g_udp_running.store(false, std::memory_order_release);
    if (g_udp_thread.joinable()) {
        g_udp_thread.join();
    }

    g_streaming_active.store(false, std::memory_order_relaxed);
    g_ring_buffer.Clear();

#if defined(_WIN32)
    if (g_hooks_installed) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hooks_installed = false;
    }
#endif

    g_voip_router = nullptr;
    g_original_voip_processor = nullptr;
    g_original_lobby_join = nullptr;
    g_original_lobby_left = nullptr;
}

} // namespace nevr::audio_intercom
