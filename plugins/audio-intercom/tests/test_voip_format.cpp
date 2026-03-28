/* SYNTHESIS -- custom tool code, not from binary */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>

/* Include the header under test */
#include "audio_intercom.h"

using namespace nevr::audio_intercom;

/* ========================================================================
 * Struct layout tests
 * ======================================================================== */

TEST(VoipFormatTest, VoipFrameSize) {
    EXPECT_EQ(sizeof(VoipFrame), 0x30);
}

TEST(VoipFormatTest, VoipFrameOffsets) {
    EXPECT_EQ(offsetof(VoipFrame, speaker_id), 0x00);
    EXPECT_EQ(offsetof(VoipFrame, frame_number), 0x08);
    EXPECT_EQ(offsetof(VoipFrame, sample_count), 0x10);
    EXPECT_EQ(offsetof(VoipFrame, codec_type), 0x14);
    EXPECT_EQ(offsetof(VoipFrame, sample_rate), 0x18);
    EXPECT_EQ(offsetof(VoipFrame, payload), 0x20);
    EXPECT_EQ(offsetof(VoipFrame, payload_size), 0x28);
}

TEST(VoipFormatTest, SR15NetUserVoipEventSize) {
    EXPECT_EQ(sizeof(SR15NetUserVoipEvent), 0x24);
}

TEST(VoipFormatTest, SR15NetUserVoipEventOffsets) {
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, speaker_id), 0x00);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, frame_number), 0x04);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, sample_count), 0x0C);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, codec_type), 0x10);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, reserved), 0x11);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, sample_rate), 0x14);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, pcm_buffer_size), 0x18);
    EXPECT_EQ(offsetof(SR15NetUserVoipEvent, pcm_buffer), 0x1C);
}

/* ========================================================================
 * Config parsing tests
 * ======================================================================== */

TEST(ConfigTest, ValidConfig) {
    std::string json = R"({
        "enabled": true,
        "listen_port": 7890,
        "speaker_id": 4294967295,
        "volume": 100
    })";
    auto cfg = ParseConfig(json);
    EXPECT_TRUE(cfg.valid);
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.listen_port, 7890);
    EXPECT_EQ(cfg.speaker_id, 0xFFFFFFFF);
    EXPECT_EQ(cfg.volume, 100);
}

TEST(ConfigTest, DefaultsOnEmpty) {
    auto cfg = ParseConfig("");
    EXPECT_FALSE(cfg.valid);
    /* Defaults should remain intact even though valid is false */
    IntercomConfig def;
    EXPECT_EQ(cfg.listen_port, def.listen_port);
    EXPECT_EQ(cfg.speaker_id, def.speaker_id);
}

TEST(ConfigTest, DisabledConfig) {
    std::string json = R"({"enabled": false, "listen_port": 9999})";
    auto cfg = ParseConfig(json);
    EXPECT_TRUE(cfg.valid);
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.listen_port, 9999);
}

TEST(ConfigTest, InvalidPortClamped) {
    /* Port 0 should not be accepted (stays at default) */
    std::string json = R"({"listen_port": 0})";
    auto cfg = ParseConfig(json);
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.listen_port, 7890); /* default */
}

TEST(ConfigTest, PartialConfig) {
    std::string json = R"({"volume": 50})";
    auto cfg = ParseConfig(json);
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.volume, 50);
    EXPECT_EQ(cfg.listen_port, 7890); /* default */
    EXPECT_TRUE(cfg.enabled); /* default */
}

/* ========================================================================
 * Address constant tests
 * ======================================================================== */

TEST(AddressTest, VoipFrameProcessorAddress) {
    EXPECT_EQ(VA_VOIP_FRAME_PROCESSOR, 0x140d7bd90ULL);
}

TEST(AddressTest, LobbyJoinHandlerAddress) {
    EXPECT_EQ(VA_LOBBY_JOIN_HANDLER, 0x14000b020ULL);
}

TEST(AddressTest, LobbyLeftHandlerAddress) {
    EXPECT_EQ(VA_LOBBY_LEFT_HANDLER, 0x14000b160ULL);
}

TEST(AddressTest, VoipRouterAddress) {
    EXPECT_EQ(VA_VOIP_ROUTER, 0x140132dc0ULL);
}

TEST(AddressTest, BroadcastPeerId) {
    EXPECT_EQ(BROADCAST_PEER_ID, 0x8000000000000004ULL);
}

TEST(AddressTest, AddressesAboveImageBase) {
    constexpr uint64_t IMAGE_BASE = 0x140000000ULL;
    EXPECT_GT(VA_VOIP_FRAME_PROCESSOR, IMAGE_BASE);
    EXPECT_GT(VA_LOBBY_JOIN_HANDLER, IMAGE_BASE);
    EXPECT_GT(VA_LOBBY_LEFT_HANDLER, IMAGE_BASE);
    EXPECT_GT(VA_VOIP_ROUTER, IMAGE_BASE);
}

/* ========================================================================
 * Ring buffer tests
 * ======================================================================== */

/* Expose the ring buffer internals for testing by reimplementing the same struct */
namespace {

struct TestRingBuffer {
    RingEntry entries[RING_BUFFER_CAPACITY];
    std::atomic<uint64_t> write_pos{0};
    std::atomic<uint64_t> read_pos{0};

    bool Push(const uint8_t* data, uint32_t size) {
        if (size == 0 || size > MAX_FRAME_SIZE) return false;
        uint64_t wp = write_pos.load(std::memory_order_relaxed);
        uint64_t rp = read_pos.load(std::memory_order_acquire);
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

} // anonymous namespace

TEST(RingBufferTest, PushPop) {
    TestRingBuffer rb;
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_TRUE(rb.Push(data, sizeof(data)));

    uint8_t out[MAX_FRAME_SIZE];
    uint32_t out_size = 0;
    EXPECT_TRUE(rb.Pop(out, out_size));
    EXPECT_EQ(out_size, sizeof(data));
    EXPECT_EQ(std::memcmp(out, data, sizeof(data)), 0);
}

TEST(RingBufferTest, PopFromEmpty) {
    TestRingBuffer rb;
    uint8_t out[MAX_FRAME_SIZE];
    uint32_t out_size = 0;
    EXPECT_FALSE(rb.Pop(out, out_size));
}

TEST(RingBufferTest, PushZeroSize) {
    TestRingBuffer rb;
    uint8_t data[] = {0x00};
    EXPECT_FALSE(rb.Push(data, 0));
}

TEST(RingBufferTest, OverflowDropsOldest) {
    TestRingBuffer rb;
    /* Fill buffer to capacity */
    for (size_t i = 0; i < RING_BUFFER_CAPACITY; ++i) {
        uint8_t val = static_cast<uint8_t>(i);
        rb.Push(&val, 1);
    }
    EXPECT_EQ(rb.Size(), RING_BUFFER_CAPACITY);

    /* Push one more -- should drop the oldest (value 0) */
    uint8_t overflow_val = 0xFF;
    EXPECT_TRUE(rb.Push(&overflow_val, 1));

    /* First pop should yield value 1 (oldest surviving), not 0 */
    uint8_t out[MAX_FRAME_SIZE];
    uint32_t out_size = 0;
    EXPECT_TRUE(rb.Pop(out, out_size));
    EXPECT_EQ(out_size, 1u);
    EXPECT_EQ(out[0], 1);
}

TEST(RingBufferTest, FIFO_Order) {
    TestRingBuffer rb;
    for (uint8_t i = 0; i < 5; ++i) {
        rb.Push(&i, 1);
    }
    for (uint8_t i = 0; i < 5; ++i) {
        uint8_t out[MAX_FRAME_SIZE];
        uint32_t out_size = 0;
        EXPECT_TRUE(rb.Pop(out, out_size));
        EXPECT_EQ(out[0], i);
    }
}

TEST(RingBufferTest, ClearResetsBuffer) {
    TestRingBuffer rb;
    uint8_t data = 42;
    rb.Push(&data, 1);
    EXPECT_EQ(rb.Size(), 1u);
    rb.Clear();
    EXPECT_EQ(rb.Size(), 0u);
    uint8_t out[MAX_FRAME_SIZE];
    uint32_t out_size = 0;
    EXPECT_FALSE(rb.Pop(out, out_size));
}

/* ========================================================================
 * Opus constant tests
 * ======================================================================== */

TEST(OpusConstantsTest, SampleRate) {
    EXPECT_EQ(SAMPLE_RATE, 48000u);
}

TEST(OpusConstantsTest, FrameDuration) {
    EXPECT_EQ(FRAME_DURATION_MS, 20u);
}

TEST(OpusConstantsTest, SamplesPerFrame) {
    EXPECT_EQ(SAMPLES_PER_FRAME, 960u);
    /* Verify consistency: sample_rate * frame_duration / 1000 = samples_per_frame */
    EXPECT_EQ(SAMPLE_RATE * FRAME_DURATION_MS / 1000, SAMPLES_PER_FRAME);
}

TEST(OpusConstantsTest, Channels) {
    EXPECT_EQ(CHANNELS, 1u);
}

TEST(OpusConstantsTest, MaxFrameSize) {
    EXPECT_EQ(MAX_FRAME_SIZE, 4096u);
}
