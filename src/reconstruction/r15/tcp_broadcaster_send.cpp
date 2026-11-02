// NRadEngine::CTcpBroadcaster::Send
// Quest libr15.so VA 0x2607090; ReVault binary 576.
// ReVault raw parent 100318413, reconstruction node 100600776; grade M: all 28 instructions (0x2607090-0x26070fc, 112 bytes) checked; the call through vtable slot 0xb0 (index 22) of the object at *this is declared as STcpBroadcasterData::Send, the name ReVault gives that slot in SClientData (0x25ff920) and SServerData (0x2605434), PURE_VIRTUAL in the base vtable 0x37a2ed0; slots 2..21 of STcpBroadcasterData are 20 placeholder pure virtuals with unknown signatures and slots 0 and 1 are the destructor pair, nothing past slot 22 is declared; which derived class lives at *this is not shown by this function; STcpPeer is modeled as one 64-bit value because it travels in a single register, its internal layout is not modeled; RequireConnection and ConnectionPeer are CTcpBroadcaster members (ConnectionPeer const) taking CSymbol64 and returning STcpPeer, matching their ReVault symbols; no other placeholder remains.
// @0x2607090 — NRadEngine::CTcpBroadcaster::Send
/* @confidence: M */

#include <cstdint>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t value;
};

// Peer handle: passed in x1 to the Send slot and returned in x0 by the two lookup functions.
struct STcpPeer {
    std::uint64_t value;
};

// Vtable at libr15 0x37a2ed0 (base of SClientData 0x37a3128 and SServerData 0x37a3220).
// Slots 0 and 1 are the destructor pair, slots 2..21 are pure virtual placeholders whose signatures
// this function does not show, and slot 22 (byte offset 0xb0) is Send. Slots after 22 are not declared.
class STcpBroadcasterData {
public:
    virtual ~STcpBroadcasterData();
    virtual void Slot02() = 0;
    virtual void Slot03() = 0;
    virtual void Slot04() = 0;
    virtual void Slot05() = 0;
    virtual void Slot06() = 0;
    virtual void Slot07() = 0;
    virtual void Slot08() = 0;
    virtual void Slot09() = 0;
    virtual void Slot10() = 0;
    virtual void Slot11() = 0;
    virtual void Slot12() = 0;
    virtual void Slot13() = 0;
    virtual void Slot14() = 0;
    virtual void Slot15() = 0;
    virtual void Slot16() = 0;
    virtual void Slot17() = 0;
    virtual void Slot18() = 0;
    virtual void Slot19() = 0;
    virtual void Slot20() = 0;
    virtual void Slot21() = 0;
    // vtable slot 22, byte offset 0xb0
    virtual void Send(STcpPeer peer, CSymbol64 type, const void* payload, unsigned long long payload_count,
                      const void* extra, unsigned long long extra_count) = 0;
};

class CTcpBroadcaster {
public:
    // libr15 0x2607090 (thunk 0xfd8360)
    void Send(CSymbol64 first, CSymbol64 second, const void* payload, std::uint64_t payload_count,
              const void* extra, std::uint64_t extra_count, std::uint32_t flags);

    // libr15 0x2607100, ReVault name CTcpBroadcaster::RequireConnection(CSymbol64). Called through the 16-byte thunk at 0xffd970
    // (ReVault: thunk_FUN_02607100). Reads the object at *this and indexes a 4-byte table in it with key & 0xfff; returns 64 bits in x0.
    STcpPeer RequireConnection(CSymbol64 key);

    // libr15 0x26072b0, ReVault name CTcpBroadcaster::ConnectionPeer(CSymbol64) const. Called through the 16-byte thunk at 0x100d870
    // (ReVault: thunk_FUN_026072b0). Reads the object at *this and indexes a 4-byte table in it with key & 0xfff; returns 64 bits in x0.
    STcpPeer ConnectionPeer(CSymbol64 key) const;
};

void CTcpBroadcaster::Send(CSymbol64 first, CSymbol64 second, const void* payload,
                           std::uint64_t payload_count, const void* extra,
                           std::uint64_t extra_count, std::uint32_t flags) {
    // ldr x24, [x0]: the object is the first qword of *this, read before the lookup call.
    STcpBroadcasterData* const object = *reinterpret_cast<STcpBroadcasterData* const*>(this);
    // cbz w7: flags == 0 selects ConnectionPeer, otherwise RequireConnection. Both receive this and first.
    const STcpPeer peer = flags == 0 ? ConnectionPeer(first) : RequireConnection(first);
    // ldr x8, [x24]; ldr x7, [x8, #0xb0]; br x7: a tail call to vtable slot 0xb0 of the object.
    // flags (w7) is not forwarded. The slot's result is discarded, since Send is void.
    object->Send(peer, second, payload, payload_count, extra, extra_count);
}

}  // namespace NRadEngine
