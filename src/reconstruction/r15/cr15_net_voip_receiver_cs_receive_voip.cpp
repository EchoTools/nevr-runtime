// NRadEngine::NRadGame::CR15NetVoipReceiverCS::ReceiveVoip
// Quest libr15.so VA 0x22c3238; ReVault binary 576.
// ReVault raw parent 100305057, reconstruction node 100600513; grade M: every branch traced against the disassembly; three data globals and the hash-table out-pointers are named by role, not symbol.
// @0x22c3238 — NRadEngine::NRadGame::CR15NetVoipReceiverCS::ReceiveVoip
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// Passed in x1 as one 64-bit value: its low 16 bits index the per-handle table, and bits 16..31 enter the packet key.
struct SCompactPoolHandle {
    std::uint64_t raw;
};

class CMemory {
  public:
    // NRadEngine::CMemory::Copy(void*, void const*, unsigned long long); ReVault 0x12cb908 (thunk 0x100cc90).
    static void Copy(void* destination, const void* source, std::uint64_t count);
};

class NWriteLog {
  public:
    // NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...); ReVault 0x1088c20
    // (thunk 0x104b960). The callers pass level 2 or 4 and flags 0.
    static void WriteLog(std::int32_t level, std::uint64_t flags, const char* format, ...);
};

class CncaGameSpace {
  public:
    // NRadEngine::CncaGameSpace::GlobalClock(); ReVault 0x1907720 (thunk 0xfd4860). The caller reads a
    // microsecond count at +0x28 of the object returned.
    static std::byte* GlobalClock();
};

class CAudioInputCallbackRegistry {
  public:
    // NRadEngine::CAudioInputCallbackRegistry::Ready(unsigned int); ReVault 0x1443ab8 (thunk 0xfedce0). Ghidra shows
    // one argument; the caller also sets w1 from the record, so the second register is declared.
    static std::int32_t Ready(std::uint32_t registry, std::int32_t slot);
};

class CHashTableU32ByU64 {
  public:
    // NRadEngine::CHashTable<unsigned int, unsigned long long, &CastHashKey<unsigned long long>>::FindOrCreate<>;
    // ReVault 0x241634c (thunk 0x103a190). Returns the uint32 value slot for the key. The two out-pointers are
    // stack slots that ReceiveVoip never reads.
    std::uint32_t* FindOrCreate(const std::uint64_t* key, void* scratch_a, void* scratch_b);
};

}  // namespace NRadEngine

namespace NRadEngine::NRadGame {

struct SR15NetUserVoipEvent;

class CR15NetGame {
  public:
    // NRadEngine::NRadGame::CR15NetGame::UserName(unsigned int); ReVault 0x1363a50 (thunk 0x10094c0). The argument
    // is the record's +0x20 word; the result is the display name for the log lines.
    const char* UserName(std::uint32_t user) const;
};

class CR15Game {
  public:
    // NRadEngine::NRadGame::CR15Game::NetGameMT() const; ReVault 0x13027d4 (thunk 0x103a070). Its result is the
    // this of CR15NetGame::UserName.
    CR15NetGame* NetGameMT() const;
};

class CR15NetVoipReceiverCS {
  public:
    // NRadEngine::NRadGame::CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, SR15NetUserVoipEvent const&,
    // void const*, unsigned int); ReVault 0x22c3238. Reached through its PLT stub (ReVault 0xfdcd30) from
    // UserVoipCB (ReVault 0x22c3030).
    void ReceiveVoip(SCompactPoolHandle handle, const SR15NetUserVoipEvent& event, const void* data,
                     std::uint32_t bytes);

  private:
    // Layout not recovered. ReceiveVoip touches bytes 0x80 through 0x207 of the object, so the storage is that long.
    alignas(8) std::byte raw_[0x208];
};

}  // namespace NRadEngine::NRadGame

// Data at ReVault 0x386f318 (in .bss; no symbol recovered): the registry value that Ready is given.
extern std::uint64_t g_audio_input_registry;
// Data reached through the slot at ReVault 0x3823818 (zero in the file, filled at load; no symbol recovered).
// Non-zero turns the [VOIP] lines on.
extern std::uint64_t g_voip_log_flag;

namespace {

// Format strings in libr15 rodata: ReVault 0x32ce2bc, 0x32ce2f0, 0x32ce322 and 0x32ce367.
constexpr char kFmtEstablished[] = "[VOIP] Frame %llu: Connection with '%s' established";
constexpr char kFmtRefreshed[] = "[VOIP] Frame %llu: Connection with '%s' refreshed";
constexpr char kFmtJumped[] = "[VOIP] Frame %llu: Connection with '%s' jumped forward by %hu frames";
constexpr char kFmtFull[] = "[VOIP] Frame %llu: Ran out of room for incoming VoIP data, increase buffer size";

// Packet sequence numbers are 10 bits and are compared modulo 0x3ff.
constexpr std::uint32_t kSeqModulus = 0x3ff;
constexpr std::uint32_t kSeqMask = 0x3ff;
constexpr std::uint16_t kSeqFlagsMask = 0xfc00;
// Stride of the per-handle records at this+0x100.
constexpr std::size_t kRecordStride = 0x78;
// A connection whose last packet is older than this many seconds is announced again.
constexpr float kRefreshAgeSeconds = 0.5f;
constexpr float kMicrosecondsToSeconds = 1e-6f;
// Added to the window-size word for the record's +0x30 float.
constexpr float kLatencyOffset = 100.0f;

// Raw load and store at a byte offset; the object layouts are not recovered.
template <typename T>
T LoadAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

template <typename T>
void StoreAt(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

// Display name for a log line: NetGameMT() of the game space, then UserName(record+0x20).
const char* SenderName(const std::byte* game_space, const std::byte* record) {
    const auto* game = reinterpret_cast<const NRadEngine::NRadGame::CR15Game*>(game_space);
    return game->NetGameMT()->UserName(LoadAt<std::uint32_t>(record, 0x20));
}

}  // namespace

namespace NRadEngine::NRadGame {

void CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle handle, const SR15NetUserVoipEvent& event, const void* data,
                                        std::uint32_t bytes) {
    const void* const event_bytes = &event;

    // 0x22c3258..0x22c3260: the registry value must be set; Ready takes its low 32 bits.
    const std::uint64_t registry = g_audio_input_registry;
    if (registry == 0) {
        return;
    }
    // 0x22c3264..0x22c3270: this+0xb8 -> object; its +0x20 must be set.
    if (LoadAt<std::uint64_t>(LoadAt<const void*>(raw_, 0xb8), 0x20) == 0) {
        return;
    }
    // 0x22c3274..0x22c3294: the table at this+0xd0 (4-byte entries) gives the record index; records start at this+0x100.
    const std::uint64_t handle_index = handle.raw & 0xffff;
    const auto idx = LoadAt<std::uint16_t>(LoadAt<const void*>(raw_, 0xd0), handle_index * 4);
    std::byte* const rec = LoadAt<std::byte*>(raw_, 0x100) + static_cast<std::size_t>(idx) * kRecordStride;

    // 0x22c3298..0x22c32d8 (record+0x2c set) and 0x22c3374..0x22c3388 (clear): Ready(registry, record+0x28).
    if (LoadAt<std::int32_t>(rec, 0x2c) == 0) {
        if (NRadEngine::CAudioInputCallbackRegistry::Ready(static_cast<std::uint32_t>(registry),
                                                           LoadAt<std::int32_t>(rec, 0x28)) == 0) {
            return;
        }
    } else {
        if (LoadAt<std::int32_t>(rec, 0x24) == -1) {
            return;
        }
        const std::int32_t slot = LoadAt<std::int32_t>(rec, 0x28);
        if (slot == -1) {
            return;
        }
        if (NRadEngine::CAudioInputCallbackRegistry::Ready(static_cast<std::uint32_t>(registry), slot) == 0) {
            return;
        }
    }

    // 0x22c32dc..0x22c32e8: this+0x80 -> context; context+0x8 -> game space, whose +0x1f08 must be clear.
    std::byte* const context = LoadAt<std::byte*>(raw_, 0x80);
    std::byte* const game_space = LoadAt<std::byte*>(context, 0x8);
    if (LoadAt<std::uint64_t>(game_space, 0x1f08) != 0) {
        return;
    }
    const std::uint64_t frame = LoadAt<std::uint64_t>(game_space, 0x828);

    // 0x22c32ec..0x22c3328: the clock is context+0x10, or CncaGameSpace::GlobalClock() when that is null; now_s is
    // its microsecond count times 1e-6 as a float.
    void* clock = LoadAt<void*>(context, 0x10);
    if (clock == nullptr) {
        clock = NRadEngine::CncaGameSpace::GlobalClock();
    }
    const float now_s = static_cast<float>(LoadAt<std::uint64_t>(clock, 0x28)) * kMicrosecondsToSeconds;

    // 0x22c3300..0x22c3334: the record's sequence word (u16 at +2) and the packet's sequence (u16 at +4 of the event).
    std::uint16_t seq_word = LoadAt<std::uint16_t>(rec, 0x2);
    const std::uint16_t event_word = LoadAt<std::uint16_t>(event_bytes, 0x4);
    if ((static_cast<std::uint32_t>(~seq_word) & kSeqMask) == 0) {
        // 0x22c3334..0x22c3344: no sequence yet (low ten bits all ones); the record takes this packet's.
        seq_word = static_cast<std::uint16_t>((seq_word & kSeqFlagsMask) | (event_word & kSeqMask));
        StoreAt<std::uint16_t>(rec, 0x2, seq_word);
        if (g_voip_log_flag != 0) {
            NRadEngine::NWriteLog::WriteLog(2, 0, kFmtEstablished, frame, SenderName(game_space, rec));
        }
    } else if (now_s - LoadAt<float>(rec, 0x34) > kRefreshAgeSeconds) {
        // 0x22c3390..0x22c33ac: the last packet is over half a second old; the sequence restarts at this packet.
        seq_word = static_cast<std::uint16_t>((seq_word & kSeqFlagsMask) | (event_word & kSeqMask));
        StoreAt<std::uint16_t>(rec, 0x2, seq_word);
        if (g_voip_log_flag != 0) {
            NRadEngine::NWriteLog::WriteLog(2, 0, kFmtRefreshed, frame, SenderName(game_space, rec));
        }
    }

    // 0x22c33f8..0x22c3408: reload the stored sequence; a is the packet's and b the stored one, each modulo 0x3ff.
    seq_word = LoadAt<std::uint16_t>(rec, 0x2);
    const std::uint32_t event_seq = event_word & kSeqMask;
    const std::uint32_t stored_seq = seq_word & kSeqMask;
    const std::uint32_t a = event_seq % kSeqModulus;
    const std::uint32_t b = stored_seq % kSeqModulus;
    // 0x22c3448..0x22c347c: the packet is refused on the two arms of the compiled test (a above b: the gap a - b
    // against (a ^ 0x3ff) + b; b above a: the gap b - a against (a - b) + 0x3ff).
    if (a > b) {
        const auto gap = static_cast<std::int32_t>(a - b);
        const auto wrap = static_cast<std::int32_t>((a ^ kSeqModulus) + b);
        if (gap > wrap) {
            return;
        }
    } else if ((b & 0xffff) > (a & 0xffff)) {
        const auto gap = static_cast<std::int32_t>(b - a);
        const auto wrap = static_cast<std::int32_t>(a - b) + 0x3ff;
        if (gap <= wrap) {
            return;
        }
    }

    // 0x22c3480..0x22c3488: the packet is accepted; the record's time becomes now.
    StoreAt<float>(rec, 0x34, now_s);
    bool set_word_30 = true;
    // 0x22c3494..0x22c3498: a packet with the stored sequence leaves the sequence alone.
    if (event_seq != stored_seq) {
        // 0x22c349c..0x22c34cc: forward distance of the packet from the stored sequence, modulo 0x3ff.
        const std::uint32_t forward = ((a + kSeqModulus - b) & 0xffff) % kSeqModulus;
        // 0x22c34d4..0x22c34e8: half the window at this+0x140.
        const auto window_half = static_cast<std::uint32_t>(LoadAt<std::uint16_t>(raw_, 0x140) >> 1);
        if (forward < window_half) {
            // 0x22c34f4: b.cc to 0x22c360c: neither the sequence nor the word at +0x30 changes.
            set_word_30 = false;
        } else {
            // 0x22c34f8..0x22c3544: the stored sequence moves to this packet minus (half window - 1).
            const std::uint32_t back = ((window_half - 1) & 0xffff) % kSeqModulus;
            const std::uint32_t new_seq = ((a + kSeqModulus - back) & 0xffff) % kSeqModulus;
            // 0x22c3548..0x22c35d8: with the log flag set, report how far the sequence moved.
            if (g_voip_log_flag != 0) {
                const std::uint32_t jumped = ((new_seq + kSeqModulus - b) & 0xffff) % kSeqModulus;
                NRadEngine::NWriteLog::WriteLog(2, 0, kFmtJumped, frame, SenderName(game_space, rec), jumped);
            }
            // 0x22c35dc..0x22c35e8: the word keeps its flag bits and takes the new sequence.
            seq_word = static_cast<std::uint16_t>((seq_word & kSeqFlagsMask) | new_seq);
            StoreAt<std::uint16_t>(rec, 0x2, seq_word);
        }
    }
    if (set_word_30) {
        // 0x22c35ec..0x22c3608: record+0x30 = (float)(word at this+0x142) + 100.
        StoreAt<float>(rec, 0x30, static_cast<float>(LoadAt<std::uint16_t>(raw_, 0x142)) + kLatencyOffset);
    }

    // 0x22c360c..0x22c3620: header and payload must fit: offset + bytes + 12 against the capacity at +0x1f8.
    const std::uint64_t capacity = LoadAt<std::uint64_t>(raw_, 0x1f8);
    const std::uint64_t offset = LoadAt<std::uint64_t>(raw_, 0x200);
    const std::uint64_t end = offset + static_cast<std::uint64_t>(bytes) + 0xc;
    if (end > capacity) {
        // 0x22c3624..0x22c3658: the buffer is full; report it and return.
        NRadEngine::NWriteLog::WriteLog(4, 0, kFmtFull, frame);
        return;
    }
    // 0x22c365c..0x22c3688: the write offset moves to end; the header is written, then the payload is copied.
    std::byte* const buffer = LoadAt<std::byte*>(raw_, 0x1d0);
    StoreAt<std::uint64_t>(raw_, 0x200, end);
    std::byte* const packet = buffer + static_cast<std::uint32_t>(offset);
    StoreAt<std::uint32_t>(packet, 0x0, static_cast<std::uint32_t>(handle.raw));
    StoreAt<std::uint16_t>(packet, 0x4,
                           static_cast<std::uint16_t>((LoadAt<std::uint16_t>(packet, 0x4) & kSeqFlagsMask) |
                                                      (event_word & kSeqMask)));
    StoreAt<std::uint32_t>(packet, 0x8, bytes);
    NRadEngine::CMemory::Copy(packet + 0xc, data, bytes);

    // 0x22c3690..0x22c36b4: the table key is the packet sequence (bits 0..9) and handle bits 16..31 (bits 32..47);
    // the value filed under it is the old offset.
    std::uint64_t key = (static_cast<std::uint64_t>(event_word) | (handle.raw << 16)) & 0x0000FFFFFFFF03FFull;
    std::uint32_t scratch_a = 0;
    std::uint64_t scratch_b = 0;
    auto* const table = reinterpret_cast<NRadEngine::CHashTableU32ByU64*>(raw_ + 0x168);
    std::uint32_t* const value = table->FindOrCreate(&key, &scratch_a, &scratch_b);
    // 0x22c36b8: the stored value is the old offset (low 32 bits).
    *value = static_cast<std::uint32_t>(offset);
}

}  // namespace NRadEngine::NRadGame
