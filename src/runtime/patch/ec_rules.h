#pragma once
// The EC network client's rules, for both builds (#2358): the packet and the types the wire carries, which datagram the receive hook takes
// out, the token blocks, the dedupe ring, the server pick, the client branch, the local-use watcher, the tac-mod parse and the tac-mod list
// hide (its decision, the selection settle and its write). The hooks that feed it are runtime/combat/veilnet.cpp on PC (Hooked_recvfrom,
// Hooked_sendto, Handle's client branch, Tick's use watcher, PickServer, SawLog, TacModsGuarded) and quest/combat/ec.cpp on Quest (the
// recvfrom / sendto / WriteLogV handlers, Step); each keeps its own sockets, locks, clock, reads of the game, hooks and log lines. Not here:
// PC's server half and its match-control branches ('L', 'R', 'G'), PC's LOADOUTS CLASSIC cap and StationView (which run between the
// decision and the write), the Quest's gear gate, its words and its page layout past the EC rules' own words. The PC GTest
// (runtime/tests/test_ec_rules.cpp) and the Quest host test (quest/tests/ec_test.cpp) both compile this one definition.
//
// Origin. EchoCombat's `plugin/veilnet.cpp` (he_is_the_cat, heisthecat31/EchoCombat), as released in v2.0.0 (nevr-pack 4d1f0024, whose
// veilnet.cpp is c3ff4b54's; "EC" lines below are that file). PC ports c3ff4b54 (runtime/combat/veilnet.cpp); the Quest ported c14c3e0a
// through the reference build's `runtime/native/afkxr/afk_ec_core.h` (kotorvr's q15t-upstream). Rule for rule: the packet EC 92-93 and
// 119-125, AddToken EC 207-212, FirstTime EC 236-245, SawSend and PickServer EC 262-298, the client branch EC 913-928, the receive hook's
// test EC 959-963, OursAtEnd and OurSelected EC 980-990, the tac-mod decision EC 1829-1846, the use watcher EC 2284-2290, the parse
// EC 2389-2403 and 2418-2429.
// Both builds hold these forms (#2358):
//   - A datagram is ours only when its type is one v2.0.0's wire carries (H A V P T O K M C L R G); any other type goes to the game.
//   - One receive call takes out at most kMaxConsumePerCall of ours in a row; the next is handed to the game and counted (Capped), a guard
//     against a flood that is not silent.
//   - The client branch and the use watcher carry only the types in the State's mask (PC passes all four, kAllTypes).
//   - An 'M' is taken through FirstTime, as v2.0.0 does: a resend does nothing, and an 'M' takes one of the ring's 32 slots.
//   - The tac-mod list: a capacity over kMaxTacCapacity is not a list; the count is written by a compare-and-swap against the count
//     that was read, so a count the game moved in between is left for the next look; a cut waits until the selection has been still for
//     kHideSettleMs, putting the list back does not wait; a list kept whole because one of ours is selected is result 3.
//   - There is no answer rule: whether the current server runs EchoCombat is its lobby settings' statement (#2333), an input here.
#include <stddef.h>
#include <stdint.h>

#include <cstring>

namespace nevr_ec {

// ---- the EC page: echovr.exe's spare bytes past .data, RVAs [kPageRva, kPageRva + kPageSize). The Quest publishes a page of its own with
// the same layout (re33_ec_page). Offsets below are from the page's start.
constexpr std::uint32_t kPageRva = 0x2104000, kPageSize = 0x1000;
// token blocks (+00 u32 count, +08 u64 expiry, +10 u32 shown) and the local use counters (u32)
constexpr std::uint32_t kOcUse = 0xca4, kOcTok = 0xca8, kVeilTok = 0xcc0, kPointTok = 0xce0, kLocTok = 0xd20, kLocUse = 0xd40;
constexpr std::uint32_t kVeilUse = 0xf00, kPointUse = 0xf80;
// the EQUIP block: +00 u64 the selected tac mod's symbol (0: none known), +20 u32 the controller's re-run counter
constexpr std::uint32_t kEquipSel = 0xe80, kEquipReruns = 0xea0;
constexpr std::uint64_t kTokenLife = 15000000;                       // 1.5 s, in 100 ns
constexpr std::uint32_t kMagic = 0x4c564345, kMagic2 = 0x4c494556;   // "ECVL" / "VEIL"

struct Packet {  // 16 bytes, little-endian on both sides
    std::uint32_t magic;
    std::uint8_t type;  // 'H' hello, 'A' its answer, 'V' veil, 'P' point, 'T' teleport, 'O' overcharge, 'K' drone killed, 'M' medigun,
                        // 'C' match control asked (client -> server), 'L' map, 'R' rules, 'G' loadouts (server -> clients)
    std::uint8_t pad[3];
    std::uint32_t id;
    std::uint32_t magic2;
};
static_assert(sizeof(Packet) == 16, "EC network packet");

inline bool Carried(std::uint8_t t) { return t == 'V' || t == 'P' || t == 'T' || t == 'O'; }
// every type v2.0.0's wire carries
inline bool Known(std::uint8_t t) {
    return Carried(t) || t == 'H' || t == 'A' || t == 'K' || t == 'M' || t == 'C' || t == 'L' || t == 'R' || t == 'G';
}
inline std::uint32_t TokenOf(std::uint8_t t) { return t == 'P' ? kPointTok : t == 'T' ? kLocTok : t == 'O' ? kOcTok : kVeilTok; }
inline std::uint32_t UseOf(int k) { return k == 3 ? kOcUse : k == 2 ? kLocUse : k ? kPointUse : kVeilUse; }  // k over "VPTO"
constexpr std::uint32_t kAllTypes = 0xf;  // bit k of "VPTO": every carried type

inline void Store32(std::uint8_t* p, std::uint32_t v) { __atomic_store_n(reinterpret_cast<std::uint32_t*>(p), v, __ATOMIC_SEQ_CST); }
inline std::uint32_t Load32(const std::uint8_t* p) { return __atomic_load_n(reinterpret_cast<const std::uint32_t*>(p), __ATOMIC_SEQ_CST); }
inline std::uint64_t Load64(const std::uint8_t* p) { return __atomic_load_n(reinterpret_cast<const std::uint64_t*>(p), __ATOMIC_SEQ_CST); }

// Is this datagram ours? Exactly 16 bytes, both magics, a known type.
inline bool Ours(const void* buf, long n, Packet* out) {
    if (n != static_cast<long>(sizeof(Packet))) return false;
    std::memcpy(out, buf, sizeof(Packet));
    return out->magic == kMagic && out->magic2 == kMagic2 && Known(out->type);
}

// The receive hook's test, for the datagram the real receive call just returned (n bytes into a buffer of bufLen): it is taken out (not
// handed to the game) only when the call did not peek, the caller can be given its sender's length back, the buffer holds a whole packet,
// fewer than kMaxConsumePerCall were taken out before it in this call, and it is ours.
constexpr int kMaxConsumePerCall = 256;
inline bool TakeOut(const void* buf, long n, unsigned long long bufLen, bool peek, bool haveFromLen, int consumed, Packet* out) {
    if (peek || !haveFromLen || bufLen < sizeof(Packet) || consumed >= kMaxConsumePerCall) return false;
    return Ours(buf, n, out);
}

// The datagram TakeOut refuses only because the call has already taken out kMaxConsumePerCall of ours: it is ours, and it goes to the game.
// The caller counts it, so the flood the cap held back is not silent. The same preconditions as TakeOut; the two are complementary.
inline bool Capped(const void* buf, long n, unsigned long long bufLen, bool peek, bool haveFromLen, int consumed) {
    if (peek || !haveFromLen || bufLen < sizeof(Packet) || consumed < kMaxConsumePerCall) return false;
    Packet p;
    return Ours(buf, n, &p);
}

// A token block whose life is over starts again at 0; then one more token, good for kTokenLife. now100: the clock, in 100 ns.
inline void AddToken(std::uint8_t* tok, std::uint64_t now100) {
    auto* count = reinterpret_cast<std::uint32_t*>(tok);
    auto* expiry = reinterpret_cast<std::uint64_t*>(tok + 8);
    if (now100 >= __atomic_load_n(expiry, __ATOMIC_SEQ_CST)) __atomic_store_n(count, 0u, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(count, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(expiry, now100 + kTokenLife, __ATOMIC_SEQ_CST);
}

// A destination the game sends to: its socket, and its address and port as on the wire (network order).
template <typename Sock>
struct BasicDest {
    Sock sock;
    std::uint32_t addr;
    std::uint16_t port;
    int count;
};

// What the client branch did with a datagram that is ours.
enum class ClientAct { kNone, kAnswer, kKill, kToken, kDropped, kMedigun };
struct ClientResult {
    ClientAct act;
    std::uint32_t drone;  // kKill: the drone's index (the id's low 16 bits)
};

// The dedupe ring, the destinations and the carried-type mask. Plain fields: the caller holds its own lock around every call. The page
// writes are atomic by themselves.
template <typename Sock>
struct BasicState {
    std::uint32_t seen[32] = {};  // the last ids; zero-filled, so id 0 is never "first"
    unsigned seenNext = 0;
    BasicDest<Sock> dests[16] = {};
    int best = -1;
    std::uint32_t types = 0;  // bit k of "VPTO": this build carries that type
    std::uint64_t dropped = 0, tokens = 0, kills = 0, answers = 0, medi = 0;

    BasicState() = default;
    constexpr explicit BasicState(std::uint32_t carried) : types(carried) {}

    bool Carries(std::uint8_t t) const {
        const int k = t == 'V' ? 0 : t == 'P' ? 1 : t == 'T' ? 2 : t == 'O' ? 3 : -1;
        return k >= 0 && ((types >> k) & 1u) != 0;
    }
    bool FirstTime(std::uint32_t id) {
        bool was = false;
        for (std::uint32_t s : seen) was |= s == id;
        if (!was) seen[seenNext++ % 32] = id;
        return !was;
    }
    // a send the game made: a known destination counts one more, a new one takes a free slot, else the least used
    void SawSend(Sock sock, std::uint32_t addr, std::uint16_t port) {
        int i = 0, freeSlot = -1, low = 0;
        for (; i < 16; ++i) {
            if (dests[i].count && dests[i].sock == sock && dests[i].addr == addr && dests[i].port == port) break;
            if (!dests[i].count && freeSlot < 0) freeSlot = i;
            if (dests[i].count < dests[low].count) low = i;
        }
        if (i == 16) {
            i = freeSlot >= 0 ? freeSlot : low;
            if (i == best) best = -1;
            dests[i] = BasicDest<Sock>{sock, addr, port, 0};
        }
        ++dests[i].count;
        if (best < 0) best = i;
    }
    // once a second: the busiest destination of the last second is the server; the counts start again, the server's at 1 (it keeps its slot)
    void PickServer() {
        int b = -1;
        for (int i = 0; i < 16; ++i)
            if (dests[i].count && (b < 0 || dests[i].count > dests[b].count)) b = i;
        if (b >= 0) best = b;
        for (int i = 0; i < 16; ++i) dests[i].count = i == best ? 1 : 0;
    }
    bool Server(BasicDest<Sock>* out) const {
        if (best < 0) return false;
        *out = dests[best];
        return true;
    }
    // The client branch, for a datagram that is ours. 'K', 'M' and the carried types are taken from any sender, each once per id. An 'A'
    // (a server's answer to the hello) is counted and opens nothing (#2333). 'H', 'C', 'L', 'R' and 'G' are not this branch's: kNone.
    ClientResult Client(std::uint8_t* page, const Packet& p, std::uint64_t now100) {
        if (p.type == 'A') {
            ++answers;
            return ClientResult{ClientAct::kAnswer, 0};
        }
        if (p.type == 'M') {
            if (!FirstTime(p.id)) return ClientResult{ClientAct::kNone, 0};
            ++medi;
            return ClientResult{ClientAct::kMedigun, 0};
        }
        if (p.type == 'K') {
            if (!FirstTime(p.id)) return ClientResult{ClientAct::kNone, 0};
            ++kills;
            return ClientResult{ClientAct::kKill, p.id & 0xffffu};
        }
        if (Carried(p.type) && FirstTime(p.id)) {
            if (!Carries(p.type)) {
                ++dropped;
                return ClientResult{ClientAct::kDropped, 0};
            }
            AddToken(page + TokenOf(p.type), now100);
            ++tokens;
            return ClientResult{ClientAct::kToken, 0};
        }
        return ClientResult{ClientAct::kNone, 0};
    }
    // The client branch as the Quest's receive handler reads it: the drone index + 1 for a first-time 'K', else 0.
    std::uint32_t Handle(std::uint8_t* page, const Packet& p, std::uint64_t now100) {
        const ClientResult r = Client(page, p, now100);
        return r.act == ClientAct::kKill ? r.drone + 1 : 0;
    }
};

// One pass over the four local use counters. seenUse[4]: the values of the last pass. For each counter that moved and whose type the
// State carries: an id (tickMs * 2654435761 ^ pid << 16 ^ count ^ k), remembered; a local token unless it is 'T' (the teleporting
// machine shows its own); the packet for the caller to send twice. Returns how many (type, id) pairs were written to out (at most 4).
struct Use {
    std::uint8_t type;
    std::uint32_t id;
};
template <typename Sock>
inline int WatchUses(BasicState<Sock>& st, std::uint8_t* page, std::uint32_t seenUse[4], std::uint32_t tickMs, std::uint32_t pid,
                     std::uint64_t now100, Use out[4]) {
    int n = 0;
    for (int k = 0; k < 4; ++k) {
        const std::uint8_t type = static_cast<std::uint8_t>("VPTO"[k]);
        const std::uint32_t u = Load32(page + UseOf(k));
        if (u == seenUse[k]) continue;
        seenUse[k] = u;
        if (!st.Carries(type)) continue;
        const std::uint32_t id = tickMs * 2654435761u ^ pid << 16 ^ u ^ static_cast<std::uint32_t>(k);
        st.FirstTime(id);
        if (type != 'T') {
            AddToken(page + TokenOf(type), now100);
            ++st.tokens;
        }
        out[n++] = Use{type, id};
    }
    return n;
}

// ---- the selected tac mod. The client logs "[NSUSER] sent profile update: <compact json>" each time it sends profile changes; a changed
// tac mod is in it as "ability":"<name>". The seven names and their symbols are EchoCombat's.
constexpr char kProfileFmt[] = "[NSUSER] sent profile update: %s";
constexpr std::uint64_t kSymSmoke = 0xe32dc7c9d4804fb5ull, kSymPoint = 0xe32dc7cad6864aa4ull, kSymBuff = 0xc8c33e48386618b6ull;
inline std::uint64_t AbilitySymbol(const char* name, std::size_t n) {
    static const struct {
        const char* name;
        std::uint64_t sym;
    } k[] = {{"smoke", kSymSmoke}, {"point", kSymPoint}, {"buff", kSymBuff},
             {"heal", 0xc8c33e4832761fbcull}, {"shield", 0x41d2d43513260b1aull}, {"sensor", 0x41d2d4381430080cull},
             {"wraith", 0x41d2d02f1b2a1316ull}};
    for (const auto& e : k)
        if (std::strlen(e.name) == n && std::memcmp(e.name, name, n) == 0) return e.sym;
    return 0;
}
inline bool IsProfileFmt(const char* fmt) { return fmt && fmt[0] == '[' && fmt[1] == 'N' && std::strcmp(fmt, kProfileFmt) == 0; }
// The symbol of the json's "ability", or 0. nameOut (may be null): the name as sent, cut to nameLen - 1, also when it is not one of the seven.
inline std::uint64_t SelectedFromJson(const char* json, char* nameOut = nullptr, std::size_t nameLen = 0) {
    if (nameOut && nameLen) nameOut[0] = 0;
    const char* a = json ? std::strstr(json, "\"ability\":\"") : nullptr;
    if (!a) return 0;
    a += 11;
    const char* e = std::strchr(a, '"');
    if (!e) return 0;
    const std::size_t n = static_cast<std::size_t>(e - a);
    if (nameOut && nameLen) {
        const std::size_t c = n < nameLen - 1 ? n : nameLen - 1;
        std::memcpy(nameOut, a, c);
        nameOut[c] = 0;
    }
    return AbilitySymbol(a, n);
}
// re-runs allowed again, then the symbol; each store atomic (the script guard loads +0xe80 once per node)
inline void StoreSelected(std::uint8_t* page, std::uint64_t sym) {
    Store32(page + kEquipReruns, 0);
    __atomic_store_n(reinterpret_cast<std::uint64_t*>(page + kEquipSel), sym, __ATOMIC_SEQ_CST);
}
// the selected tac mod is smoke, point or buff (EchoCombat's three)
inline bool OurSelected(std::uint64_t sym) { return sym == kSymSmoke || sym == kSymPoint || sym == kSymBuff; }

// ---- the tac-mod list hide. The gear registry's tac-mod list is { allocator, entries, u64 count, u64 capacity }, entries of 0x88 bytes
// with the name (char[64]) first; EchoCombat's three are the table's last three, in this order. On a server without EchoCombat the count
// is lowered by 3 (the entries stay); with it, or with one of the three selected, the full count is put back. A rebuilt list has a new
// entries pointer and its full count again. Nothing is touched unless the last three are ours.
//
// Env reads the game's memory: `bool Read(std::uintptr_t address, void* out, std::size_t length)`, false when it cannot (the Quest reads
// through SafeRead; PC reads plainly inside its EC_TRY frame and returns true).
constexpr std::uint32_t kTacEntry = 0x88;
constexpr std::uint64_t kMaxTacCapacity = 4096;  // a larger capacity is a list read as garbage (an unreadable or freed one)
constexpr char kNewTacMods[3][8] = {"smoke", "buff", "point"};
struct TacList {
    std::uintptr_t entries = 0;
    std::uint64_t full = 0;  // the most the list has had: it is filled an entry at a time
    std::uint32_t gen = 0;
};
template <typename Env>
inline bool OursAtEnd(Env& env, std::uintptr_t entries, std::uint64_t full) {
    for (int k = 0; k < 3; ++k) {
        char name[64];
        if (!env.Read(entries + (full - 3 + static_cast<std::uint64_t>(k)) * kTacEntry, name, sizeof name)) return false;
        if (std::strncmp(name, kNewTacMods[k], 64) != 0) return false;
    }
    return true;
}

// The list is only cut short once the selection has been still for kHideSettleMs; putting it back does not wait. A burst of selection
// changes flips the list's count once at most, not on every change.
constexpr std::uint64_t kHideSettleMs = 2000;
inline bool HideSettled(std::uint64_t nowMs, std::uint64_t selChangedMs) { return nowMs - selChangedMs >= kHideSettleMs; }
struct SelectionSettle {
    std::uint64_t sel = 0, changedMs = 0;
    // one look at the selection: true when it has been still for kHideSettleMs
    bool Look(std::uint64_t nowSel, std::uint64_t nowMs) {
        if (nowSel != sel) {
            sel = nowSel;
            changedMs = nowMs;
        }
        return HideSettled(nowMs, changedMs);
    }
};

// The decision. ok: the list is ours to change (read, at least 3 entries, ours at the end, a plausible capacity). count: the count read.
// want: the count the rules want (a cut that is not settled holds the count read). result: 1 shown (the server has EchoCombat), 2 hidden,
// 3 kept whole (one of ours selected). gen: the caller's count of the game's rebuilds and frees; a new one forgets the list.
struct TacPlan {
    bool ok;
    std::uintptr_t entries;
    std::uint64_t count, want;
    int result;
};
template <typename Env>
inline TacPlan PlanTacMods(TacList& st, Env& env, std::uintptr_t list, bool serverHasIt, std::uint32_t gen, bool keepWhole, bool settled) {
    TacPlan plan{false, 0, 0, 0, 0};
    if (gen != st.gen) {
        st.entries = 0;
        st.full = 0;
        st.gen = gen;
    }
    if (!list) return plan;
    std::uintptr_t entries = 0;
    std::uint64_t count = 0, capacity = 0;
    if (!env.Read(list + 0x08, &entries, sizeof entries) || !env.Read(list + 0x10, &count, sizeof count) ||
        !env.Read(list + 0x18, &capacity, sizeof capacity) || !entries)
        return plan;
    if (entries != st.entries) {  // a new list: its full count is the most it has had
        st.entries = entries;
        st.full = 0;
    }
    if (count > st.full) st.full = count;
    const std::uint64_t full = st.full;
    if (full < 3 || full > capacity || capacity > kMaxTacCapacity || !OursAtEnd(env, entries, full)) return plan;
    std::uint64_t want = serverHasIt || keepWhole ? full : full - 3;
    if (want < count && !settled) want = count;  // a cut waits for the selection to settle
    plan.ok = true;
    plan.entries = entries;
    plan.count = count;
    plan.want = want;
    plan.result = serverHasIt ? 1 : keepWhole ? 3 : 2;
    return plan;
}
// The write of `want` (the plan's, or the caller's after its own caps). Nothing when the plan is not ok or the count is already right;
// write = false only tells. The count is swapped against the one the plan read: a count that moved since is left alone (the next look
// decides again). Returns the plan's result when it wrote (or would), else 0; nowCount gets the count written.
inline int WriteTacMods(std::uintptr_t list, const TacPlan& plan, std::uint64_t want, bool write, std::uint64_t* nowCount) {
    if (!plan.ok || plan.count == want) return 0;
    if (write) {
        std::uint64_t expect = plan.count;
        if (!__atomic_compare_exchange_n(reinterpret_cast<std::uint64_t*>(list + 0x10), &expect, want, false, __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST))
            return 0;
    }
    *nowCount = want;
    return plan.result;
}
// The decision and the write together, for a caller with nothing to do between them. Returns 0: nothing to do, 1: shown, 2: hidden,
// 3: kept whole.
template <typename Env>
inline int TacModsFor(TacList& st, Env& env, std::uintptr_t list, bool serverHasIt, bool write, std::uint64_t* nowCount,
                      std::uint32_t gen, bool keepWhole, bool settled) {
    const TacPlan plan = PlanTacMods(st, env, list, serverHasIt, gen, keepWhole, settled);
    return WriteTacMods(list, plan, plan.want, write, nowCount);
}

}  // namespace nevr_ec
