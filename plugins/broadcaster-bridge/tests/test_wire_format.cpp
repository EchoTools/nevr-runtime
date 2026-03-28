/* SYNTHESIS -- custom tool code, not from binary */

#include <gtest/gtest.h>
#include "broadcaster_bridge.h"
#include "address_registry.h"

#include <cstring>
#include <cstdint>

using namespace nevr::broadcaster_bridge;

/* ── Static assertions on packet struct sizes ─────────────────────── */

static_assert(sizeof(MirrorPacketHeader) == 16);
static_assert(sizeof(InjectionPacketHeader) == 17);

/* ── Mirror packet serialize/deserialize roundtrip ────────────────── */

TEST(WireFormat, SerializeDeserializeRoundtrip) {
    const uint64_t sym   = 0xDEADBEEF12345678ULL;
    const uint32_t flags = 0x03;
    const uint8_t  payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    const uint32_t psize = sizeof(payload);

    uint8_t buf[MAX_PACKET_SIZE];
    size_t len = SerializeMirrorPacket(buf, sizeof(buf), sym, flags, payload, psize);

    ASSERT_GT(len, 0u);
    ASSERT_EQ(len, sizeof(MirrorPacketHeader) + psize);

    MirrorPacketHeader hdr;
    const uint8_t* out_payload = nullptr;
    ASSERT_TRUE(DeserializeMirrorPacket(buf, len, hdr, out_payload));

    EXPECT_EQ(hdr.msg_symbol, sym);
    EXPECT_EQ(hdr.flags, flags);
    EXPECT_EQ(hdr.payload_size, psize);
    ASSERT_NE(out_payload, nullptr);
    EXPECT_EQ(std::memcmp(out_payload, payload, psize), 0);
}

TEST(WireFormat, EmptyPayloadRoundtrip) {
    const uint64_t sym   = 0xABCD;
    const uint32_t flags = 0;

    uint8_t buf[MAX_PACKET_SIZE];
    size_t len = SerializeMirrorPacket(buf, sizeof(buf), sym, flags, nullptr, 0);

    ASSERT_EQ(len, sizeof(MirrorPacketHeader));

    MirrorPacketHeader hdr;
    const uint8_t* out_payload = nullptr;
    ASSERT_TRUE(DeserializeMirrorPacket(buf, len, hdr, out_payload));

    EXPECT_EQ(hdr.msg_symbol, sym);
    EXPECT_EQ(hdr.flags, flags);
    EXPECT_EQ(hdr.payload_size, 0u);
    EXPECT_EQ(out_payload, nullptr);
}

TEST(WireFormat, OversizedPayloadRejected) {
    uint8_t buf[MAX_PACKET_SIZE];
    /* Payload that would exceed MAX_PACKET_SIZE with header */
    size_t len = SerializeMirrorPacket(buf, sizeof(buf), 0, 0, buf,
        MAX_PACKET_SIZE - sizeof(MirrorPacketHeader) + 1);
    EXPECT_EQ(len, 0u);
}

TEST(WireFormat, BufferTooSmall) {
    const uint8_t payload[] = {0x01, 0x02};
    uint8_t buf[4]; /* way too small */
    size_t len = SerializeMirrorPacket(buf, sizeof(buf), 0, 0, payload, sizeof(payload));
    EXPECT_EQ(len, 0u);
}

TEST(WireFormat, DeserializeTruncatedBuffer) {
    uint8_t buf[8]; /* smaller than MirrorPacketHeader */
    MirrorPacketHeader hdr;
    const uint8_t* payload = nullptr;
    EXPECT_FALSE(DeserializeMirrorPacket(buf, sizeof(buf), hdr, payload));
}

TEST(WireFormat, DeserializeNullBuffer) {
    MirrorPacketHeader hdr;
    const uint8_t* payload = nullptr;
    EXPECT_FALSE(DeserializeMirrorPacket(nullptr, 100, hdr, payload));
}

TEST(WireFormat, DeserializePayloadSizeMismatch) {
    /* Header claims large payload but buffer is too small */
    uint8_t buf[sizeof(MirrorPacketHeader)];
    MirrorPacketHeader fake;
    fake.msg_symbol   = 0;
    fake.flags        = 0;
    fake.payload_size = 1000; /* claims 1000 bytes but buffer has 0 payload bytes */
    std::memcpy(buf, &fake, sizeof(fake));

    MirrorPacketHeader hdr;
    const uint8_t* payload = nullptr;
    EXPECT_FALSE(DeserializeMirrorPacket(buf, sizeof(buf), hdr, payload));
}

/* ── Config parsing ───────────────────────────────────────────────── */

TEST(ConfigParsing, ValidConfig) {
    const char* json = R"({
        "udp_debug_target": "192.168.1.100:8888",
        "listen_port": 7777,
        "mirror_send": false,
        "mirror_receive": true,
        "log_messages": true
    })";

    auto cfg = ParseConfig(json);
    EXPECT_EQ(cfg.target_ip, "192.168.1.100");
    EXPECT_EQ(cfg.target_port, 8888);
    EXPECT_EQ(cfg.listen_port, 7777);
    EXPECT_FALSE(cfg.mirror_send);
    EXPECT_TRUE(cfg.mirror_receive);
    EXPECT_TRUE(cfg.log_messages);
}

TEST(ConfigParsing, EmptyStringReturnsDefaults) {
    auto cfg = ParseConfig("");
    EXPECT_EQ(cfg.target_ip, "127.0.0.1");
    EXPECT_EQ(cfg.target_port, 9999);
    EXPECT_EQ(cfg.listen_port, 9998);
    EXPECT_TRUE(cfg.mirror_send);
    EXPECT_FALSE(cfg.mirror_receive);
    EXPECT_FALSE(cfg.log_messages);
}

TEST(ConfigParsing, MissingFieldsUseDefaults) {
    const char* json = R"({ "listen_port": 5555 })";
    auto cfg = ParseConfig(json);

    EXPECT_EQ(cfg.target_ip, "127.0.0.1");
    EXPECT_EQ(cfg.target_port, 9999);
    EXPECT_EQ(cfg.listen_port, 5555);
    EXPECT_TRUE(cfg.mirror_send);
    EXPECT_FALSE(cfg.mirror_receive);
}

TEST(ConfigParsing, InvalidJsonReturnsDefaults) {
    auto cfg = ParseConfig("this is not json at all");
    EXPECT_EQ(cfg.target_ip, "127.0.0.1");
    EXPECT_EQ(cfg.target_port, 9999);
    EXPECT_EQ(cfg.listen_port, 9998);
}

/* ── Address constant validation ──────────────────────────────────── */

TEST(AddressConstants, BroadcasterSendIsValid) {
    EXPECT_GT(nevr::addresses::VA_BROADCASTER_SEND, 0x140000000ULL);
    EXPECT_LT(nevr::addresses::VA_BROADCASTER_SEND, 0x150000000ULL);
    EXPECT_EQ(nevr::addresses::VA_BROADCASTER_SEND, 0x140f89af0ULL);
}

TEST(AddressConstants, BroadcasterReceiveLocalIsValid) {
    EXPECT_GT(nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL, 0x140000000ULL);
    EXPECT_LT(nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL, 0x150000000ULL);
    EXPECT_EQ(nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL, 0x140f87aa0ULL);
}

TEST(AddressConstants, SendBeforeReceive) {
    /* Send VA should be after ReceiveLocal VA in the binary */
    EXPECT_GT(nevr::addresses::VA_BROADCASTER_SEND,
              nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL);
}
