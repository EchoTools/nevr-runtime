/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <string>

#include "address_registry.h"

namespace nevr::audio_intercom {

/* ========================================================================
 * Address constants (verified in streamed_audio_injector sources)
 * ======================================================================== */
static constexpr uint64_t VA_VOIP_FRAME_PROCESSOR = nevr::addresses::VA_VOIP_FRAME_PROCESSOR;
static constexpr uint64_t VA_LOBBY_JOIN_HANDLER   = nevr::addresses::VA_LOBBY_JOIN_HANDLER;
static constexpr uint64_t VA_LOBBY_LEFT_HANDLER   = nevr::addresses::VA_LOBBY_LEFT_HANDLER;
static constexpr uint64_t VA_VOIP_ROUTER          = nevr::addresses::VA_VOIP_ROUTER;

/* ========================================================================
 * Opus codec constants
 * ======================================================================== */
static constexpr uint32_t SAMPLE_RATE        = 48000;
static constexpr uint32_t FRAME_DURATION_MS  = 20;
static constexpr uint32_t SAMPLES_PER_FRAME  = 960;
static constexpr uint32_t CHANNELS           = 1;
static constexpr uint32_t MAX_FRAME_SIZE     = 4096;

/* ========================================================================
 * Network constants
 * ======================================================================== */
static constexpr uint64_t BROADCAST_PEER_ID  = 0x8000000000000004ULL;
static constexpr uint32_t VOIP_PRIORITY      = 5;
static constexpr int64_t  VOIP_TIMEOUT_MS    = 5000;

/* ========================================================================
 * Ring buffer constants
 * ======================================================================== */
static constexpr size_t RING_BUFFER_CAPACITY = 64;
static constexpr size_t MAX_UDP_PACKET_SIZE  = 4096;

/* ========================================================================
 * VoipFrame - internal frame representation
 * Copied from streamed_audio_injector/voip_broadcaster.hpp
 * ======================================================================== */
struct VoipFrame {
    uint32_t speaker_id;
    uint64_t frame_number;
    uint32_t sample_count;
    uint8_t  codec_type;
    uint32_t sample_rate;
    const uint8_t* payload;
    uint32_t payload_size;
};
static_assert(sizeof(VoipFrame) == 0x30);
static_assert(offsetof(VoipFrame, speaker_id) == 0x00);
static_assert(offsetof(VoipFrame, frame_number) == 0x08);
static_assert(offsetof(VoipFrame, sample_count) == 0x10);
static_assert(offsetof(VoipFrame, codec_type) == 0x14);
static_assert(offsetof(VoipFrame, sample_rate) == 0x18);
static_assert(offsetof(VoipFrame, payload) == 0x20);
static_assert(offsetof(VoipFrame, payload_size) == 0x28);

/* ========================================================================
 * SR15NetUserVoipEvent - wire format for VoIP injection
 * Copied from streamed_audio_injector/voip_broadcaster.cpp
 * ======================================================================== */
#pragma pack(push, 1)
struct SR15NetUserVoipEvent {
    uint32_t       speaker_id;       // +0x00
    uint64_t       frame_number;     // +0x04
    uint32_t       sample_count;     // +0x0C
    uint8_t        codec_type;       // +0x10
    uint8_t        reserved[3];      // +0x11
    uint32_t       sample_rate;      // +0x14
    uint32_t       pcm_buffer_size;  // +0x18
    const uint8_t* pcm_buffer;       // +0x1C
};
#pragma pack(pop)
static_assert(sizeof(SR15NetUserVoipEvent) == 0x24);
static_assert(offsetof(SR15NetUserVoipEvent, speaker_id) == 0x00);
static_assert(offsetof(SR15NetUserVoipEvent, frame_number) == 0x04);
static_assert(offsetof(SR15NetUserVoipEvent, sample_count) == 0x0C);
static_assert(offsetof(SR15NetUserVoipEvent, codec_type) == 0x10);
static_assert(offsetof(SR15NetUserVoipEvent, reserved) == 0x11);
static_assert(offsetof(SR15NetUserVoipEvent, sample_rate) == 0x14);
static_assert(offsetof(SR15NetUserVoipEvent, pcm_buffer_size) == 0x18);
static_assert(offsetof(SR15NetUserVoipEvent, pcm_buffer) == 0x1C);

/* ========================================================================
 * VoIP router function signature
 * Source: streamed_audio_injector/voip_broadcaster.cpp
 * ======================================================================== */
typedef void (*VoipRouter_fn)(void* broadcaster_ctx, uint64_t peer_id,
    const void* frame_data, uint32_t frame_size, uint32_t priority,
    int64_t timeout_ms);

/* Hook target function signatures */
typedef void (*VoipFrameProcessor_fn)(void* broadcaster_ctx);
typedef void (*LobbyJoinHandler_fn)(void* event_data);
typedef void (*LobbyLeftHandler_fn)(void* event_data);

/* ========================================================================
 * Ring buffer entry for received UDP Opus frames
 * ======================================================================== */
struct RingEntry {
    uint8_t  data[MAX_FRAME_SIZE];
    uint32_t size;
};

/* ========================================================================
 * Plugin configuration (loaded from JSON instead of registry)
 * ======================================================================== */
struct IntercomConfig {
    bool     enabled    = true;
    uint16_t listen_port = 7890;
    uint32_t speaker_id = 0xFFFFFFFF;
    uint32_t volume     = 100;
    bool     valid      = false;
};

/*
 * Parse a JSON config string into IntercomConfig.
 * Returns a config with valid=false on parse error.
 */
IntercomConfig ParseConfig(const std::string& json_text);

/* ========================================================================
 * Lifecycle
 * ======================================================================== */
int  Initialize(uintptr_t base_addr, const char* config_path);
void Shutdown();

} // namespace nevr::audio_intercom
