// VeilNet: carries UMBRAL VEIL (and POINT) to every machine.
//
// The veil (the smoke tac mod) runs its script only on the client that used it. The
// one networked thing it does is create a barrier wall, which every machine gets.
// Nothing the script can send travels with the wall in a form the wall script can
// read back, so this plugin sends the news itself:
//
//   client that used the veil --"V id"--> server --"V id"--> every other client
//
// It uses the game's own connection: no ports of its own. echovr.exe talks to its
// server with plain sendto / recvfrom on one UDP socket; VeilNet hooks both. A client
// sends its packets from the game's socket to the server address and port the game
// itself sends to; on every machine, recvfrom takes VeilNet's packets out before
// the game sees them. The server answers through the socket each client's packets
// came in on, so replies go back the way the game's own do (same port forwarding,
// same NAT mapping).
//
// Every machine that hears of a veil (the user's client too) gets a "veil token" for
// TOKEN_LIFE. The barrier wall script (tools/veil_fx.py, network_wall) waits a moment
// at its spawn nodes for a token; a wall that takes one becomes the veil (no model, no
// collision, smoke clouds and the veil sound); a wall that finds none is a barrier.
//
//   echovr.exe +0x2104cc0 (spare bytes past .data, also used by the wall script):
//     +00 u32 tokens   +08 u64 expiry (interrupt time, 100 ns)
//     +10 u32 walls that became the veil   +14 u32 walls that stayed barriers
//   echovr.exe +0x2104f00: the veil's custom-param node (smoke script, mark_param)
//     bumps the u32 there each time the veil is used on this machine.
//
// Clients say hello every 2 s; the server relays only to clients that have (so a
// player without VeilNet never gets a packet the game would not understand), and answers
// each hello with an 'A'. A client that has had no answer for SERVER_TIMEOUT is on a
// server without EchoCombat: it hides the three new tac mods (UMBRAL VEIL, OVERCHARGE,
// POINT) from the tac mod list until one comes (TacModsFor).
// A nEVR plugin (nevr_plugin.h): set up from NvrPluginInitEx, taken down in
// NvrPluginShutdown; the host says whether this is the server. Its lines go to nEVR's host
// log, tagged plugin=VeilNet; the per-packet and per-tick lines are Debug.
//
// POINT goes the same way as 'P' packets: its script (also a barrier-script copy) bumps
// the u32 at +0x2104f80, every machine gets a "point token" (+0x2104ce0, same layout as
// the veil's), and a wall that takes one goes off as POINT's stun field instead.
//
// TRANSLOCATOR goes as 'T' packets: each teleport bumps the u32 at +0x2104d40 (the
// translocator script, tools/fix_loc_fx.py); the server only relays them, and every
// other client gets a "translocator token" (+0x2104d20) that the next translocator puck
// node to run there takes, showing the teleport effect and sound on its puck. The
// teleporting machine shows its own and holds no token.
//
// OVERCHARGE goes as 'O' packets: each use bumps the u32 at +0x2104ca4 (the buff
// script, tools/fix_buff.py), every machine gets an "overcharge token" (+0x2104ca8),
// and the next heal bubble in Repair Matrix's script to take one shows OVERCHARGE's
// purple look (tools/oc_switch.py).
//
// MEDIGUN goes as 'M' packets: PowerBoost.dll on the client holding it calls
// VeilNet_Medigun(id) (owner key, on / off, a sequence); the server relays it and every
// machine, the server too, hands it to PowerBoost_MedigunRemote, which ends that owner's
// heal bubble when the beam goes off.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>
#include <iterator>
#include "detours.h"
#include "nevr_plugin.h"
#include "nevr/plugin_logger.h"
#include "station_icon_keys.h"

#pragma comment(lib, "ws2_32.lib")

// Lines go to nEVR's host log, tagged plugin=<config.yaml name> (docs/standards/logging.md, Rule 15).
NEVR_DEFINE_PLUGIN_LOG("[VeilNet]")

constexpr const char *kLogName = "VeilNet";   // names the plugin in its log-cap Warning

namespace {

constexpr uintptr_t TOKEN_RVA = 0x2104cc0;
constexpr uintptr_t USE_COUNTER_RVA = 0x2104f00;
constexpr uintptr_t POINT_TOKEN_RVA = 0x2104ce0;
constexpr uintptr_t POINT_COUNTER_RVA = 0x2104f80;
constexpr uintptr_t LOC_TOKEN_RVA = 0x2104d20;
constexpr uintptr_t LOC_COUNTER_RVA = 0x2104d40;
constexpr uintptr_t OC_TOKEN_RVA = 0x2104ca8;
constexpr uintptr_t OC_COUNTER_RVA = 0x2104ca4;
constexpr uintptr_t EQUIP_RVA = 0x2104e80;          // tools/equip_guard.py: +00 equipped, +08 writer, +10 skips
constexpr uintptr_t POINT_FIRED_RVA = 0x2104d44;    // POINTs whose stun went off here
constexpr ULONGLONG TOKEN_LIFE = 15000000;      // 1.5 s, in 100 ns
constexpr uint32_t MAGIC = 0x4c564345;          // "ECVL"
constexpr uint32_t MAGIC2 = 0x4c494556;         // "VEIL"
//: The gear registry's tac mod list (filled from gear_table.tacmods, InitAndLoadGearTables
//: +0xc4eba0): a pointer to { allocator, entries, count, capacity }, entries 0x88 bytes
//: with the name (char[64]) first. Ours are the table's last three (fix_smoke.py,
//: fix_buff.py, fix_point.py append them in this order).
constexpr uintptr_t TACMOD_LIST_RVA = 0x20d3430;
constexpr size_t TACMOD_ENTRY = 0x88;
const char *const kNewTacMods[3] = {"smoke", "buff", "point"};
constexpr ULONGLONG SERVER_TIMEOUT = 6000;      // ms without an answer to the hello
//: MEDIGUN is off unless the server turns it on: PowerBoost.ini [server] medigun=1 next to this
//: DLL on the server. Its answer to each hello carries ANSWER_MEDIGUN when it does; a client
//: shows MEDIGUN (the weapon list's last entry, fix_medigun.py appends it) only while its current
//: server's answers say so, and PowerBoost.dll (VeilNet_MedigunAllowed) keeps it a plain COMET
//: otherwise. The weapon list sits just before the tac mod list (gear_table order: weapons,
//: ordnances, tac mods; each filled by +0xc4e410 from InitAndLoadGearTables).
constexpr uintptr_t WEAPON_LIST_RVA = 0x20d3420;
constexpr uint32_t ANSWER_MEDIGUN = 1;
bool g_medigun_on_server = false;               // server: [server] medigun

struct Shared {
    volatile LONG tokens;
    LONG pad;
    volatile ULONGLONG expiry;
    volatile LONG veils, barriers;
};

struct Packet {
    uint32_t magic;
    uint8_t type;                               // 'H' hello, 'A' its answer, 'V' veil, 'P' point, 'T' teleport, 'O' overcharge, 'K' a drone killed (server -> clients), 'R' the RULES unlock / capture time (server -> clients), 'C' LOADOUTS asked for (client -> server), 'G' LOADOUTS (server -> clients)
    uint8_t pad[3];
    uint32_t id;
    uint32_t magic2;
};

uint8_t *g_base;
Shared *g_shared;
Shared *g_point;                                // POINT's tokens (+10: walls that went off)
Shared *g_loc;                                  // TRANSLOCATOR's tokens (+10: teleports shown)
Shared *g_oc;                                   // OVERCHARGE's tokens (+10: bubbles shown purple)
bool g_server;
SRWLOCK g_lock = SRWLOCK_INIT;
volatile bool g_stop = false;                   // NvrPluginShutdown: the tick thread ends
HANDLE g_tick = nullptr;

// One line through nEVR's host log. Debug and Info are counted apart, so a burst of Debug lines
// never takes Info's room: Debug stops after kDebugCap lines and Info after kInfoCap, and the
// first line past either cap writes one Warning that says so. A Warning or an Error is always
// written.
constexpr LONG kDebugCap = 5000;
constexpr LONG kInfoCap = 20000;
volatile LONG g_logged_debug = 0;
volatile LONG g_logged_info = 0;

void LogLine(uint32_t level, const char *fmt, va_list args) {
    if (level < NEVR_LOG_WARNING) {
        bool debug = level == NEVR_LOG_DEBUG;
        LONG cap = debug ? kDebugCap : kInfoCap;
        LONG n = InterlockedIncrement(debug ? &g_logged_debug : &g_logged_info);
        if (n > cap) {
            if (n == cap + 1) {
                char note[160];
                snprintf(note, sizeof(note), "%s log cap reached: further %s lines are dropped (cap %ld)",
                         kLogName, debug ? "Debug" : "Info", static_cast<long>(cap));
                PluginLogFields(NEVR_LOG_WARNING, note, {});
            }
            return;
        }
    }
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, args);
    PluginLogFields(level, line, {});
}

void Log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_INFO, fmt, args);
    va_end(args);
}

void LogDebug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_DEBUG, fmt, args);
    va_end(args);
}

void LogWarning(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_WARNING, fmt, args);
    va_end(args);
}

void LogError(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_ERROR, fmt, args);
    va_end(args);
}

ULONGLONG InterruptTime() { return *(volatile ULONGLONG *)0x7ffe0008; }

const char *Addr(const sockaddr_in &a, char *buf, size_t n) {
    char ip[32];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    sprintf_s(buf, n, "%s:%u", ip, ntohs(a.sin_port));
    return buf;
}

bool Same(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void AddToken(Shared *t) {
    ULONGLONG now = InterruptTime();
    if (now >= t->expiry) InterlockedExchange(&t->tokens, 0);
    InterlockedIncrement(&t->tokens);
    t->expiry = now + TOKEN_LIFE;
}

Shared *Tokens(uint8_t type) {
    return type == 'P' ? g_point : type == 'T' ? g_loc : type == 'O' ? g_oc : g_shared;
}
const char *What(uint8_t type) {
    return type == 'P' ? "point" : type == 'T' ? "teleport" : type == 'O' ? "overcharge" : "veil";
}
bool Carried(uint8_t type) { return type == 'V' || type == 'P' || type == 'T' || type == 'O'; }

const char *TacMod(uint64_t s) {
    switch (s) {
    case 0xe32dc7c9d4804fb5ull: return "UMBRAL VEIL (smoke)";
    case 0xe32dc7cad6864aa4ull: return "POINT (point)";
    case 0xc8c33e48386618b6ull: return "OVERCHARGE (buff)";
    case 0xc8c33e4832761fbcull: return "Repair Matrix (heal)";
    case 0x41d2d43513260b1aull: return "barrier (shield)";
    case 0x41d2d4381430080cull: return "sensor";
    case 0x41d2d02f1b2a1316ull: return "wraith";
    default: return "?";
    }
}

// The last ids seen, so a veil that arrives twice (the resend) gives one token.
uint32_t g_seen[32];
int g_seen_next = 0;

bool FirstTime(uint32_t id) {
    AcquireSRWLockExclusive(&g_lock);
    bool seen = false;
    for (uint32_t s : g_seen) seen |= s == id;
    if (!seen) g_seen[g_seen_next++ % 32] = id;
    ReleaseSRWLockExclusive(&g_lock);
    return !seen;
}

using sendto_fn = int(WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int);
using recvfrom_fn = int(WSAAPI *)(SOCKET, char *, int, int, sockaddr *, int *);
sendto_fn Real_sendto = sendto;
recvfrom_fn Real_recvfrom = recvfrom;

void Send(SOCKET s, const sockaddr_in &to, uint8_t type, uint32_t id) {
    Packet p = { MAGIC, type, {}, id, MAGIC2 };
    Real_sendto(s, (const char *)&p, sizeof(p), 0, (const sockaddr *)&to, sizeof(to));
}

// --- client: the game's connection to its server ------------------------------------
// The destination (socket + address) the game sent the most packets to in the last
// second. Every game server on a PC has its own port (6792, 6793, ...): when the
// game moves to another one (lobby -> match), this follows within a second.
struct Dest { SOCKET sock; sockaddr_in addr; LONG count; };
Dest g_dests[16];
int g_best = -1;

// Once a second (Tick): the busiest destination of the last second becomes the
// server, and the counts start again.
void PickServer() {
    AcquireSRWLockExclusive(&g_lock);
    int best = -1;
    for (int i = 0; i < 16; ++i)
        if (g_dests[i].count && (best < 0 || g_dests[i].count > g_dests[best].count)) best = i;
    if (best >= 0) g_best = best;
    for (int i = 0; i < 16; ++i)
        if (i != g_best) g_dests[i].count = 0;
        else g_dests[i].count = 1;               // keeps its slot while quiet
    ReleaseSRWLockExclusive(&g_lock);
}

void SawSend(SOCKET s, const sockaddr *to) {
    if (!to || to->sa_family != AF_INET) return;
    const sockaddr_in &a = *(const sockaddr_in *)to;
    AcquireSRWLockExclusive(&g_lock);
    int i = 0, free_slot = -1, low = 0;
    for (; i < 16; ++i) {
        if (g_dests[i].count && g_dests[i].sock == s && Same(g_dests[i].addr, a)) break;
        if (!g_dests[i].count && free_slot < 0) free_slot = i;
        if (g_dests[i].count < g_dests[low].count) low = i;
    }
    if (i == 16) {                              // new destination
        i = free_slot >= 0 ? free_slot : low;
        if (i == g_best) g_best = -1;
        g_dests[i] = { s, a, 0 };
    }
    ++g_dests[i].count;
    if (g_best < 0) g_best = i;
    ReleaseSRWLockExclusive(&g_lock);
}

bool ServerDest(Dest &out) {
    AcquireSRWLockShared(&g_lock);
    bool ok = g_best >= 0;
    if (ok) out = g_dests[g_best];
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void SendToServer(uint8_t type, uint32_t id) {
    Dest d;
    if (ServerDest(d)) Send(d.sock, d.addr, type, id);
}

// --- server: the clients it relays to -----------------------------------------------
struct Client { SOCKET sock; sockaddr_in addr; ULONGLONG last; };
Client g_clients[64];

void Remember(SOCKET s, const sockaddr_in &a) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    int slot = -1, oldest = 0;
    bool known = false;
    for (int i = 0; i < 64 && !known; ++i) {
        Client &c = g_clients[i];
        if (c.last && Same(c.addr, a)) { c.last = now; c.sock = s; known = true; }
        else {
            if (!c.last && slot < 0) slot = i;
            if (c.last < g_clients[oldest].last) oldest = i;
        }
    }
    if (!known) g_clients[slot < 0 ? oldest : slot] = { s, a, now };
    ReleaseSRWLockExclusive(&g_lock);
    char buf[48];
    if (!known) Log("client %s has VeilNet", Addr(a, buf, sizeof(buf)));
}

int Relay(const sockaddr_in &from, uint8_t type, uint32_t id) {
    Client list[64];
    AcquireSRWLockShared(&g_lock);
    memcpy(list, g_clients, sizeof(list));
    ReleaseSRWLockShared(&g_lock);
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (const Client &c : list)
        if (c.last && now - c.last < 30000 && !Same(c.addr, from)) {
            Send(c.sock, c.addr, type, id);
            Send(c.sock, c.addr, type, id);                     // once more, against loss
            ++n;
        }
    return n;
}

// DroneFix.dll on the server, when a drone's health runs out: every client shuts that drone
// down too ('K', the drone's damageable index in the low 16 bits of the id).
volatile LONG g_kills = 0;
extern "C" __declspec(dllexport) void __cdecl VeilNet_DroneKilled(uint32_t index) {
    if (!g_server) return;
    uint32_t id = ((uint32_t)InterlockedIncrement(&g_kills) << 16) | (index & 0xffff);
    FirstTime(id);
    sockaddr_in nobody = {};
    int n = Relay(nobody, 'K', id);
    Log("drone %u killed: told %d client(s)", index & 0xffff, n);
}

// PowerBoost.dll on the client with a MEDIGUN: its beam on / off, to the server and on to everyone.
extern "C" __declspec(dllexport) void __cdecl VeilNet_Medigun(uint32_t id) {
    if (g_server) return;
    FirstTime(id);
    SendToServer('M', id);
    SendToServer('M', id);                                  // once more, against loss
}

void MedigunHere(uint32_t id) {
    using remote_fn = void(__cdecl *)(uint32_t);
    static remote_fn remote = nullptr;
    if (!remote)
        if (HMODULE pb = GetModuleHandleW(L"PowerBoost.dll"))
            remote = (remote_fn)GetProcAddress(pb, "PowerBoost_MedigunRemote");
    if (remote) remote(id);
}

float *g_capture_time = nullptr;                // RULES capture time in the mailbox (InstallCaptureTime)
//: RULES POINT UNLOCK / CAPTURE TIME as one float T = unlock * 60 + capture (fix_capture_points.py;
//: 0 = unset, the map's own). Only the server's handlers write it, but every player's capture point
//: script counts the unlock down itself (the "point unlocking in" voice and the 5..1 countdown) from
//: it: the server sends its T with each answer ('R') and a client takes it.
float *g_rules_total = nullptr;
//: RULES page 2's LOADOUTS (tools/fix_rules_pages.py): 0 ALL, 1 CLASSIC (the four stock guns),
//: 2 NEW (the newer guns only); tac mods and ordnances are never limited. The server keeps the rule;
//: a client's podium terminal asks for a change through the RULES mailbox (+12 requests, +13 the
//: value), sent here as 'C'; the server tells every client the rule ('G', with each hello's answer
//: too), which they put at +11 for the terminal to show. A server without VeilNet: ALL.
//: The RULES mailbox: the page end - 140h (deploy_and_launch.py RULES_MAILBOX_BELOW_VEIL; clear of
//: the veil's bytes at the page end - 20h). +00 / +04 starting scores, +08 T, +0C capture time,
//: then the RULES page 2 state (tools/fix_rules_pages.py): +10 page shown, +11 LOADOUTS shown (0 ALL, 1 CLASSIC, 2 NEW),
//: +12 requests, +13 the value asked for.
constexpr uint32_t RULES_BELOW_PAGE_END = 0x140;
constexpr uint32_t CLASSIC_SHOWN = 0x11, CLASSIC_REQUESTS = 0x12, CLASSIC_WANT = 0x13;
constexpr uint32_t STATION_CELLS = 0x14;        // +14..+16: equipment station cells shown per gear type
constexpr uint64_t CLASSIC_ITEMS = 4;           // each gear list's stock items come first
uint8_t *g_rules = nullptr;                     // the RULES mailbox
volatile LONG g_classic = 0;                    // server: LOADOUTS (LOADOUT_*)
volatile LONG g_chassis = 0;                    // server: SERIES-10 ONLY
constexpr uint32_t CHASSIS_SHOWN = 0x2C;        // mailbox +2C: SERIES-10 ONLY shown (client)
constexpr uint32_t WANT_CHASSIS = 0x80;         // in +13 (the value asked for): a SERIES-10 ONLY ask, bit 0 the value
//: RULES page 2 row 3, POINT RATE: a multiplier on how fast a held capture point's points rise
//: (tools/fix_point_rate.py scales the capture-point script's time step by mailbox +2D / 4).
//: The server keeps it in quarters; a podium asks for a step up / down (+13 = 40h | 1 / 40h),
//: which the client turns into the value it wants (40h | quarters << 8: sent twice, so a step
//: would have been taken twice);
//: every client puts it at +2D and writes the row's text, which the page shows through the
//: pointer at +24 (tools/fix_rules_pages.py).
volatile LONG g_rate = 4;                       // server: POINT RATE in quarters (4 = x1)
constexpr uint32_t RATE_SHOWN = 0x2D, RATE_TEXT = 0x24;
constexpr uint32_t WANT_RATE = 0x40;            // in +13: a POINT RATE ask, bit 0 = up
const uint8_t kRateSteps[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};   // 0.25x .. 2.5x in quarters

// The step after (up) or before `q` (quarters; 0 = 1.0x), held at the ends.
uint8_t RateStep(uint8_t q, bool up) {
    int k = 0, n = (int)sizeof(kRateSteps);
    if (!q) q = 4;
    while (k < n - 1 && kRateSteps[k] < q) ++k;
    k = up ? (k + 1 < n ? k + 1 : k) : (k > 0 ? k - 1 : 0);
    return kRateSteps[k];
}
// RULES page 2's value texts, written here and shown by the podium (tools/fix_rules_pages.py:
// row k's text = *(mailbox +24) + k * 48): LOADOUTS, SERIES-10 ONLY, POINT RATE. Each is padded
// so the right-aligned value sits near the row's middle.
constexpr size_t PAGE_TEXT = 48;
char g_page_text[4][PAGE_TEXT];

//: PAYLOAD maps (Fission, Gauss): the server knows it is on one because the payload's speed
//: update (InstallPayloadSpeed) runs; it tells the clients in 'G' and every machine puts it at +30,
//: where the podium (tools/fix_rules_pages.py) reads it: RULES shows only page 2 there, row 3 is
//: PAYLOAD SPEED (quarters at +31; the POINT RATE asks, mapped here); row 4 is empty (CHECKPOINT USED
//: was dropped 2026-11-05: its bytes +32 / +33 and 'G' bits 6 / 24-25 stay unused)
//: (0..3 at +32; asked as 10h | 1 up / 10h down, sent as 10h | target << 8). +33 = the server is
//: driving the payload to the chosen checkpoint right now.
constexpr uint32_t PAYLOAD_MAP = 0x30, PSPEED_SHOWN = 0x31, CHECKPOINT_SHOWN = 0x32, SKIP_SHOWN = 0x33;
constexpr uint32_t WANT_CHECKPOINT = 0x10;
volatile LONG g_pspeed = 4;                     // server: PAYLOAD SPEED in quarters (4 = x1)
volatile LONG g_payload_map = 0;                // server: the payload update ran in the last second
volatile LONG g_skipping = 0;                   // server: driving the payload to the checkpoint
volatile LONG g_pause_queued = 0;               // server: PAUSE asked, waiting for a checkpoint
constexpr uint32_t PAUSE_QUEUED_SHOWN = 0x34;   // mailbox +34: that, on clients (TEAM READY label)
uint32_t g_payload_flags = 0;                   // server: the payload's flags last seen (2 autodrive)
volatile ULONGLONG g_payload_seen = 0;         // when the payload update last ran
volatile LONG g_payload_moved = 0;              // server: it moved since this round's start
volatile LONG g_checkpoint_hits = 0;            // server: checkpoints reached (script events) so far
LONG g_pause_queue_base = 0;                    // server: the hits when a PAUSE was queued

// "1.0" / "2.5" / "0.75" from quarters (0 = 4).
void QuarterText(char *num, size_t n, uint8_t q) {
    if (!q) q = 4;
    if (q % 4 == 0 && q / 4 >= 3) sprintf_s(num, n, "%d", q / 4);
    else if (q % 4 == 0) sprintf_s(num, n, "%d.0", q / 4);
    else if (q % 2 == 0) sprintf_s(num, n, "%d.5", q / 4);
    else sprintf_s(num, n, "%d.%02d", q / 4, q % 4 * 25);
}

void PageText(int row, const char *text) {
    char t[PAGE_TEXT];
    int n = sprintf_s(t, "%s", text);
    int pad = (20 - n) / 2;
    while (pad-- > 0 && n < (int)PAGE_TEXT - 2) t[n++] = ' ';
    t[n] = 0;
    memcpy(g_page_text[row], t, n + 1);
}

// Every row's text from the mailbox (the rules as this machine knows them).
void PageTexts() {
    uint8_t m = g_rules ? g_rules[CLASSIC_SHOWN] : 0, c = g_rules ? g_rules[CHASSIS_SHOWN] : 0, q = g_rules ? g_rules[RATE_SHOWN] : 0;
    char t[PAGE_TEXT];
    sprintf_s(t, "LOADOUTS: %s", m == 1 ? "CLASSIC" : m == 2 ? "NEW" : "ALL");
    PageText(0, t);
    sprintf_s(t, "SERIES-10 ONLY: %s", c ? "ON" : "OFF");
    PageText(1, t);
    char num[16];
    if (g_rules && g_rules[PAYLOAD_MAP]) {
        QuarterText(num, sizeof(num), g_rules[PSPEED_SHOWN]);
        sprintf_s(t, "PAYLOAD SPEED: %sx", num);
        PageText(2, t);
        g_page_text[3][0] = 0;
    } else {
        QuarterText(num, sizeof(num), q);
        sprintf_s(t, "POINT RATE: %sx", num);
        PageText(2, t);
        g_page_text[3][0] = 0;
    }
    if (g_rules) *(char *volatile *)(g_rules + RATE_TEXT) = g_page_text[0];
}
void RateText(uint8_t) { PageTexts(); }

//: PAUSE (the podium's TEAM READY button once the match is live): the server keeps it; a
//: podium asks with +13 = 20h | the value (absolute: sent twice); every machine puts it at +2E.
//: Paused: capture progress (InstallCaptureTime) and the points (tools/fix_point_rate.py) stand
//: still, the server raises the game's own cease fire, each client sends its player back to the
//: war room (nevt_force_respawn) and the orbs read pre_match (tools/fix_orb_pause.py, which also
//: keeps +2F = the match is live: spawn_access past pre_match).
//: +2E is a state: 0 running, 1 paused, 2 resuming. UNPAUSE does not resume at once: the server
//: goes to 2 for RESUME_MS (the announcer counts 5..1 on every client, the orbs are the round's
//: again so players spawn back in, but cease fire and the point / capture freezes hold), then 0.
//: On capture point maps the point is made neutral (the game's own capture volume reset) when
//: the pause begins and again when the countdown starts.
volatile LONG g_paused = 0;                     // server: PAUSE state (0 / 1 / 2)
volatile ULONGLONG g_resume_at = 0;             // server: when state 2 ends
constexpr ULONGLONG RESUME_MS = 5000;
enum : LONG { PAUSE_OFF = 0, PAUSE_ON = 1, PAUSE_RESUMING = 2 };
constexpr uint32_t PAUSED_SHOWN = 0x2E, MATCH_LIVE = 0x2F;
// +35h "PAUSE allowed" (client): set PAUSE_OK_DELAY_MS after the match went live (+2Fh). The
// podium (fix_rules_pages.py) greys TEAM READY in stock fashion until then (DisableButton), and
// enables it as PAUSE from then on; the label switches to PAUSE at the same moment. Setting it
// raises delegate_onlobbymemberalignmentchanged: the podium (d4653) runs only its UpdateAll on it,
// and UpdateAll's DisableButton site then picks EnableButton. (nevt_all_update_state, raised
// before, has no handler in the podium: the label changed but the button stayed grey.)
constexpr uint32_t PAUSE_OK = 0x35;
constexpr ULONGLONG PAUSE_OK_DELAY_MS = 1000;
constexpr uint64_t EVT_ALL_UPDATE_STATE = 0x2bcc9f98038227ffull;   // nevt_all_update_state
constexpr uint64_t EVT_ALIGNMENT_CHANGED = 0x48f6befc619771a8ull;  // delegate_onlobbymemberalignmentchanged
ULONGLONG g_live_since = 0;
constexpr uint32_t WANT_PAUSE = 0x20;           // in +13: a PAUSE ask, bit 0 the value

// --- END MATCH (private matches) ------------------------------------------------------------
// A client asks with F8 (its Echo window in front): 'C' WANT_END. The server, in a private match
// only (its "[NETLOBBY] Starting session (gametype ...private...)" log line), until the game's
// nevt_game_over goes out (or END_GIVE_UP_MS):
//   * blue captures the point as if a blue player stood on it (game thread, EndMatchCapture): each
//     capture volume not owned by blue gets what the engine's capture tick writes when a capture
//     completes (+44 owner = 0 blue, +46 capturing = none, progress 0, +40 |= 4 replicate) and
//     delegate_OnPossessionChanged (what FUN_140ccfb20 raises), so the point script charges blue;
//   * blue's capture-point points = 9999 (tick thread, the point script's data found in this
//     process by its "capturepoint" symbol at +100h; +F0 orange, +F8 blue, +FC points to win):
//     the script's win check, which only runs for the charging team, gives blue the round;
//   * RULES POINT UNLOCK 1 s / CAPTURE TIME 0 (T = 60: InstantCapture makes the capture 1 ms), so a
//     real player on the point captures it at once too.
// Two rounds: the match is over. RULES are put back after.
// (Tried first, and failed: raising nevt_cbt_round_win_blue only moved the game mode on a round
// with nobody winning; 999 points for both teams alone needed somebody charging.)
constexpr uint32_t WANT_END = 0x10000000;
constexpr uint64_t EVT_GAME_OVER = 0xe2d5eb48e0d8a7c4ull;        // nevt_game_over
constexpr uint64_t SYM_CAPTUREPOINT = 0xeedeff1586bd8846ull;     // "capturepoint"
constexpr ULONGLONG END_EVERY_MS = 300, END_GIVE_UP_MS = 180000, END_RESCAN_MS = 2000;
constexpr float END_POINTS = 9999.0f;
constexpr float END_TOTAL = 60.0f;               // unlock 1 s, capture 0 (instant)
volatile LONG g_private_match = -1;             // server: -1 unknown, 0 / 1 from the session log
volatile ULONGLONG g_end_until = 0, g_end_next = 0, g_end_scanned = 0;
volatile LONG g_end_written = 0;                 // times the points were written
uint8_t *g_end_point = nullptr;                  // the point script's data (cached)
bool g_end_saved = false;                        // the RULES value to put back
float g_end_old_total = 0.0f;
float *g_rules_total_ptr();
void EndMatchRestore(const char *why) {
    if (g_end_saved)
        if (float *t = g_rules_total_ptr()) *(volatile float *)t = g_end_old_total;
    g_end_saved = false;
    g_end_until = 0;
    Log("END MATCH: %s (points written %ld time(s)); RULES put back", why, (long)g_end_written);
}
void GameOverSeen(uint64_t event) {
    if (event == EVT_GAME_OVER && g_end_until) EndMatchRestore("game over");
}

// --- MAP SWITCH (post-match terminal, private matches) --------------------------------------
// The post-match podium's four map buttons (tools/fix_map_terminal.py) send mapswitch_<map> when
// pressed (the podium ignores them). A client hears its own press (script events reach the inner
// dispatchers) and asks the server: 'C' WANT_MAP | map. The server, in a private match only, on
// its game thread: owner JSON "level" = the map (CNSLobby owner JSON setter +0x615E60, the call
// LobbySessionStartingCB makes) and the engine's own "Session Started" (+0x17D8A0: players reset,
// then LoadMap reads that "level"; leaving "in game" clears the owner JSON "ingame"). 1.5 s later
// it tells every client 'L' map (once; clients only log it). Clients follow on their own: the
// engine moves them to "server loading" (the transition screen), unloads their levels and loads
// the owner JSON's level when the server is in game again.
constexpr uint32_t WANT_MAP = 0x20000000;        // | map index 0..3
constexpr int MAP_COUNT = 4;
constexpr uint64_t EVT_MAPSWITCH[MAP_COUNT] = {
    0xc60e17c86d1f3df0ull, 0x63093755a2c24ebeull, 0x3d77042c79604528ull, 0xc60e17cb751921edull};   // mapswitch_<map>
constexpr uint64_t LEVEL_SYM[MAP_COUNT] = {
    0x43e2da7914642604ull, 0x42670f2bed45703cull, 0xdf5ca7b7dfa383d4ull, 0x43e2da7a0c623a19ull};   // mpl_combat_<map>
const char *const kMapNames[MAP_COUNT] = {"DYSON", "COMBUSTION", "FISSION", "SURGE"};   // SURGE = mpl_combat_gauss
// The map buttons' button actors (fix_map_terminal.py: symbol64("mapswitch_<map>_0")). Once a
// switch is under way every client greys them (the podium's own way: evt_btn_disable to the
// button actor, which its prp_itt_screen_button_logic script handles), so nobody presses again.
constexpr uint64_t MAP_BUTTON_ACTOR[MAP_COUNT] = {
    0x3d75142c65677576ull, 0xba2b43af12ba468aull, 0xc8cc27933f907292ull, 0x3d760c2a797a7576ull};
constexpr uint64_t EVT_BTN_DISABLE = 0x8f0791ddb90b611dull;     // evt_btn_disable
// CncaGameSpace actor lookup by nodeid (+0x4FF1A0, "FindDelegate" in revault): the space's sorted
// {nodeid, index} table at +370h -> link 0x0000ffffffff<index> (the link script events carry).
constexpr uintptr_t FIND_ACTOR_RVA = 0x4FF1A0;
const uint8_t kFindActorHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x4c, 0x8b, 0x99, 0x70, 0x03};
using find_actor_fn = uint64_t(__fastcall *)(uint64_t, uint64_t, uint64_t *);
volatile LONG g_map_grey = 0;                    // client: grey the map buttons (game thread)
constexpr uintptr_t OWNER_SET_SYMBOL_RVA = 0x615E60, OWNER_GET_SYMBOL_RVA = 0x60D3D0, OWNER_GET_BOOL_RVA = 0x60D050;
constexpr uintptr_t SESSION_STARTED_RVA = 0x17D8A0, SET_GAME_STATE_RVA = 0x1B8650;
// PopLevelSet(game space) (+0x114C20): what CR15NetGame::LeaveGame does before a new session, when
// bit 37 of *(netgame+2DA0h) (set by LoadMap: a level is loaded) is set; it clears the bit after.
// Test 1 without it: LoadMap put Gauss on top of the loaded Dyson ("There can be only one instance
// of R15NetBalanceSettings"), the load failed, and EchoRelay's server patch reset the session.
constexpr uintptr_t POP_LEVEL_SET_RVA = 0x114C20;
constexpr uint64_t LEVEL_LOADED_BIT = 1ull << 37;
const uint8_t kPopLevelSetHead[] = {0x40, 0x55, 0xb8, 0x30, 0x15, 0x00, 0x00, 0xe8};
using pop_level_set_fn = void(__fastcall *)(uint64_t);
// CNSLobby::OwnerJson_SetBool (+0x615B50: lobby, key, value). The server clears the owner "ingame"
// first and waits MAP_LEAVE_MS before it unloads and restarts: a client in game that sees
// "ingame" false goes the engine's own way (menu transition screen, its levels disabled and
// unloaded, "server loading") and loads the owner level once the server is in game again.
// Test 2: that happened when the client saw the flag; tests 3/4: the server reloaded in ~1 s, the
// flag flipped back before the client saw it, and the client stayed (or VeilNet's own state 6
// loaded Gauss on top of Dyson and crashed).
constexpr uintptr_t OWNER_SET_BOOL_RVA = 0x615B50;
const uint8_t kOwnerSetBoolHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
                                     0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xe9, 0x41, 0x8b, 0xf0};
using owner_set_bool_fn = void(__fastcall *)(uint64_t, const char *, int);
constexpr ULONGLONG MAP_LEAVE_MS = 3000;
// Test 5: owner "ingame" off alone did not move the client (the engine does not react to it).
// So the clients are told at once ('L' at step 1) and leave the game themselves while the server
// still waits: game state 6 ("server loading", the transition screen), and, if a level is still
// loaded MAP_CLIENT_UNLOAD_MS later, its unload. Then the server reloads, and when it is in game
// again the clients' Update loads the owner level.
constexpr ULONGLONG MAP_CLIENT_UNLOAD_MS = 1000;
// The menu popup (ui_mnu_popup 674f3b71da94770c, in mnu_master) shows "TRANSITIONING" on
// delegate_onnetgameloadingmultiplayer, which SetGameState raises entering state 4 (a normal join
// goes through it). The switch goes in game -> 6 and mnu_master is only enabled after the state
// change, so the popup never saw an event: the gap was a blank grey void. The client raises it
// itself (DispatchEventToSession) once mnu_master is up, and again a little later.
constexpr uint64_t EVT_NETGAME_LOADING_MP = 0x60433466edf3937aull;   // delegate_onnetgameloadingmultiplayer
// A normal join (event log 2026-11-05): the popup's page is enabled (delegate_OnPage2Enabled on an
// R15UIPage2), then MultiplayerStart, then delegate_onnetgameloadingmultiplayer -> "LOADING", ...,
// delegate_onnetgamereadyforgame -> "TRANSITIONING". The switch raised only the loading event and
// nothing showed: MultiplayerStart is raised first now (the page controller's cue, presumably).
constexpr uint64_t EVT_MULTIPLAYER_START = 0x816707342bddf95cull;   // MultiplayerStart
constexpr ULONGLONG MAP_POPUP_AGAIN_MS = 700;
volatile ULONGLONG g_map_popup_at = 0;           // client: raise the popup event again then (0 none)
volatile ULONGLONG g_map_client_unload_at = 0;   // client: unload check time (0 none)
volatile LONG g_map_leaving = 0;                 // server: map index + 1 waiting out MAP_LEAVE_MS
volatile ULONGLONG g_map_leave_at = 0;
const uint8_t kOwnerSetSymbolHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
                                       0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xe9, 0x49, 0x8b, 0xd8, 0x48, 0x81, 0xc1, 0x10, 0x03};
const uint8_t kOwnerGetSymbolHead[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x81, 0xc1, 0x10, 0x03, 0x00, 0x00};
const uint8_t kOwnerGetBoolHead[] = {0x48, 0x81, 0xc1, 0x10, 0x03, 0x00, 0x00, 0x45, 0x33, 0xc9, 0xe9};
const uint8_t kSessionStartedHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
const uint8_t kSetGameStateHead[] = {0x40, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x80, 0xfc, 0xff, 0xff, 0x48, 0x81, 0xec, 0x80, 0x04};
using owner_set_symbol_fn = void(__fastcall *)(uint64_t, const char *, uint64_t);
using owner_get_symbol_fn = uint64_t *(__fastcall *)(uint64_t, uint64_t *, const char *, uint64_t);
using owner_get_bool_fn = int(__fastcall *)(uint64_t, const char *, int);
using session_started_fn = void(__fastcall *)(uint64_t);
using set_game_state_fn = void(__fastcall *)(uint64_t, uint32_t);
volatile LONG g_map_want = 0;                    // server: map index + 1 asked for (0 none)
volatile LONG g_map_announce = 0;                // server: map index + 1 to tell the clients
volatile ULONGLONG g_map_announce_at = 0;
volatile LONG g_map_announced = 0;
volatile LONG g_map_target = 0;                  // client: map index + 1 the server switched to
volatile ULONGLONG g_map_target_since = 0;
volatile ULONGLONG g_map_asked_at = 0;           // client: last ask (one per press)

// The stock entry bytes, or a hook's jmp there (another plugin hooks SetGameState: calling the
// entry then goes through that hook, as the game's own calls do). Its tail must still be stock.
bool EntryOk(uintptr_t rva, const uint8_t *head, size_t n) {
    const uint8_t *e = g_base + rva;
    if (!memcmp(e, head, n)) return true;
    return e[0] == 0xe9 && n > 8 && !memcmp(e + 5, head + 5, n - 5);
}
bool MapSwitchCallable() {
    static bool said = false;
    const char *bad = !EntryOk(OWNER_SET_SYMBOL_RVA, kOwnerSetSymbolHead, sizeof(kOwnerSetSymbolHead)) ? "owner setter"
                    : !EntryOk(OWNER_GET_SYMBOL_RVA, kOwnerGetSymbolHead, sizeof(kOwnerGetSymbolHead)) ? "owner symbol getter"
                    : !EntryOk(OWNER_GET_BOOL_RVA, kOwnerGetBoolHead, sizeof(kOwnerGetBoolHead)) ? "owner bool getter"
                    : !EntryOk(SESSION_STARTED_RVA, kSessionStartedHead, sizeof(kSessionStartedHead)) ? "session started"
                    : !EntryOk(SET_GAME_STATE_RVA, kSetGameStateHead, sizeof(kSetGameStateHead)) ? "SetGameState"
                    : !EntryOk(POP_LEVEL_SET_RVA, kPopLevelSetHead, sizeof(kPopLevelSetHead)) ? "PopLevelSet"
                    : !EntryOk(OWNER_SET_BOOL_RVA, kOwnerSetBoolHead, sizeof(kOwnerSetBoolHead)) ? "owner bool setter"
                    : nullptr;
    if (bad && !said) { said = true; Log("MAP SWITCH: %s not callable (unexpected build)", bad); }
    return !bad;
}

// The lobby (CNSLobby) of the net game: CR15NetClientLobby *(netgame+40h), +18h.
uint64_t NetLobby(uint64_t netgame) {
    uint64_t client_lobby = netgame ? *(uint64_t *)(netgame + 0x40) : 0;
    return client_lobby ? client_lobby + 0x18 : 0;
}

void SendToServer(uint8_t type, uint32_t id);
void GreyMapButtons(uint64_t netgame);
// Client: a map button's event (from the inner dispatchers / DispatchComponentEvent).
void MapSwitchEvent(uint64_t event) {
    if (g_server) return;
    for (int k = 0; k < MAP_COUNT; ++k) {
        if (event != EVT_MAPSWITCH[k]) continue;
        ULONGLONG now = GetTickCount64();
        if (now - g_map_asked_at < 1500) return;              // one ask per press
        g_map_asked_at = now;
        SendToServer('C', WANT_MAP | (uint32_t)k);
        SendToServer('C', WANT_MAP | (uint32_t)k);             // once more, against loss
        g_map_grey = 1;                                        // greyed on the game thread
        Log("MAP SWITCH: %s pressed, asked of the server", kMapNames[k]);
        return;
    }
}

// The capture point script's data: "capturepoint" at +100h, plausible points at +F0 / +F8 / +FC.
bool LooksLikePoint(const uint8_t *d) {
    __try {
        if (*(const uint64_t *)(d + 0x100) != SYM_CAPTUREPOINT) return false;
        float o = *(const float *)(d + 0xF0), b = *(const float *)(d + 0xF8), r = *(const float *)(d + 0xFC);
        return o >= 0 && o <= 100000 && b >= 0 && b <= 100000 && r >= 10 && r <= 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint8_t *FindPointData() {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *addr = nullptr;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t *base = (uint8_t *)mbi.BaseAddress;
        size_t size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect == PAGE_READWRITE)) {
            __try {
                for (size_t o = 0x100; o + 8 <= size; o += 8)
                    if (*(const uint64_t *)(base + o) == SYM_CAPTUREPOINT && LooksLikePoint(base + o - 0x100))
                        return base + o - 0x100;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        addr = base + size;
        if (addr < base) break;
    }
    return nullptr;
}

// What the server's 'G' carries: LOADOUTS in bits 0-1, SERIES-10 ONLY in bit 2, PAUSE state in
// bits 3-4, payload map bit 5, driving to the checkpoint bit 6, POINT RATE (quarters) in bits
// 8-15, PAYLOAD SPEED (quarters) in bits 16-23.
uint32_t RulesWord() {
    return (uint32_t)g_classic | ((uint32_t)g_chassis & 1) << 2 | ((uint32_t)g_paused & 3) << 3 |
           ((uint32_t)g_payload_map & 1) << 5 | ((uint32_t)g_skipping & 1) << 6 | ((uint32_t)g_pause_queued & 1) << 7 |
           ((uint32_t)g_rate & 0xff) << 8 | ((uint32_t)g_pspeed & 0xff) << 16;
}
enum : uint32_t { LOADOUT_ALL = 0, LOADOUT_CLASSIC = 1, LOADOUT_NEW = 2 };
const char *LoadoutName(uint32_t m) { return m == LOADOUT_CLASSIC ? "CLASSIC" : m == LOADOUT_NEW ? "NEW" : "ALL"; }

// LOADOUTS in force here: the server's own rule, or on a client what its server said.
uint32_t LoadoutMode() {
    uint32_t m = g_server ? (uint32_t)g_classic : g_rules ? g_rules[CLASSIC_SHOWN] : 0;
    return m <= LOADOUT_NEW ? m : LOADOUT_ALL;
}
volatile ULONGLONG g_last_answer = 0;           // client: when the server last answered a hello
volatile uint32_t g_last_answer_flags = 0;      // ... and what it said (ANSWER_MEDIGUN)

// RULES CAPTURE TIME 0 = an instant capture. The script stores the capture time as float(T mod 60),
// so a chosen 0 looks like "unset" (the map's own); while T is set it is made 1 ms here instead.
void InstantCapture() {
    if (!g_server || !g_rules_total || !g_capture_time) return;
    float t = *(volatile float *)g_rules_total;
    if (t >= 1.0f && (int)t % 60 == 0 && *(volatile float *)g_capture_time == 0.0f)
        *(volatile float *)g_capture_time = 0.001f;
}

void Handle(SOCKET s, const Packet &p, const sockaddr_in &from) {
    char buf[48];
    if (g_server) {
        Remember(s, from);
        if (p.type == 'H') {
            Send(s, from, 'A', g_medigun_on_server ? ANSWER_MEDIGUN : 0);   // "this server has EchoCombat"
            uint32_t total = g_rules_total ? *(volatile uint32_t *)g_rules_total : 0;
            if (total) Send(s, from, 'R', total);                            // its RULES unlock / capture time
            Send(s, from, 'G', RulesWord());                                 // its LOADOUTS / SERIES-10 ONLY
        }
        uint8_t asked_rate = (uint8_t)(p.id >> 8);   // a podium asks for a POINT RATE (the value itself)
        // on a payload map the row is PAYLOAD SPEED: the same asks (and steps) set that instead
        volatile LONG &rate_rule = g_payload_map ? g_pspeed : g_rate;
        if (p.type == 'C' && (p.id & (WANT_CHASSIS | WANT_RATE)) == WANT_RATE &&
            memchr(kRateSteps, asked_rate, sizeof(kRateSteps)) && asked_rate != (uint8_t)rate_rule) {
            rate_rule = asked_rate;
            if (g_rules) g_rules[g_payload_map ? PSPEED_SHOWN : RATE_SHOWN] = asked_rate;
            sockaddr_in nobody = {};
            int r = Relay(nobody, 'G', RulesWord());
            Log("%s %d/4 (asked by %s): told %d client(s)", g_payload_map ? "PAYLOAD SPEED" : "POINT RATE",
                (int)asked_rate, Addr(from, buf, sizeof(buf)), r);
        }
        if (p.type == 'C' && (p.id & (WANT_CHASSIS | WANT_RATE | WANT_PAUSE)) == WANT_PAUSE) {   // PAUSE / UNPAUSE
            // PAUSE from running or resuming; UNPAUSE only from paused, and it starts the countdown
            LONG now = g_paused, want = (p.id & 1) ? PAUSE_ON : now == PAUSE_ON ? PAUSE_RESUMING : now;
            // payload maps: a pause only happens while the payload is held (before the attackers
            // start, or in a checkpoint's buffer); otherwise it waits for the next checkpoint
            if (want == PAUSE_ON && now == PAUSE_OFF && g_payload_map && g_payload_moved && !(g_payload_flags & 1)) {
                g_pause_queue_base = g_checkpoint_hits;
                if (!InterlockedExchange(&g_pause_queued, 1)) {
                    sockaddr_in nobody = {};
                    int r = Relay(nobody, 'G', RulesWord());
                    Log("PAUSE queued until the payload reaches a checkpoint (asked by %s): told %d client(s)",
                        Addr(from, buf, sizeof(buf)), r);
                }
                want = now;
            }
            if (want != now) {
                if (want == PAUSE_RESUMING) g_resume_at = GetTickCount64() + RESUME_MS;
                g_paused = want;
                if (g_rules) g_rules[PAUSED_SHOWN] = (uint8_t)want;
                sockaddr_in nobody = {};
                int r = Relay(nobody, 'G', RulesWord());
                Log("PAUSE %s (asked by %s): told %d client(s)", want == PAUSE_ON ? "on" : "resuming in 5 s",
                    Addr(from, buf, sizeof(buf)), r);
            }
        }
        if (p.type == 'C' && (p.id & 0xF0000000) == WANT_MAP && (p.id & 0xF) < MAP_COUNT) {   // a map button
            int k = (int)(p.id & 0xF);
            if (g_private_match != 1) {
                Log("MAP SWITCH to %s asked by %s: refused (%s)", kMapNames[k], Addr(from, buf, sizeof(buf)),
                    g_private_match == 0 ? "not a private match" : "match type unknown");
            } else if (!g_map_want && !g_map_announce && !g_map_leaving) {
                g_map_want = k + 1;
                Log("MAP SWITCH to %s asked by %s", kMapNames[k], Addr(from, buf, sizeof(buf)));
            }
        }
        if (p.type == 'C' && p.id == WANT_END) {                             // F8: END MATCH
            if (g_private_match != 1) {
                Log("END MATCH asked by %s: refused (%s)", Addr(from, buf, sizeof(buf)),
                    g_private_match == 0 ? "not a private match" : "match type unknown");
            } else if (!g_end_until) {
                ULONGLONG now = GetTickCount64();
                g_end_written = 0;
                g_end_next = now;
                g_end_until = now + END_GIVE_UP_MS;
                Log("END MATCH asked by %s", Addr(from, buf, sizeof(buf)));
            }
        }
        if (p.type == 'C' && (p.id & WANT_CHASSIS)) {                        // a podium asks for SERIES-10 ONLY
            LONG want = (LONG)(p.id & 1);
            if (InterlockedExchange(&g_chassis, want) != want) {
                if (g_rules) g_rules[CHASSIS_SHOWN] = (uint8_t)want;
                sockaddr_in nobody = {};
                int n = Relay(nobody, 'G', RulesWord());
                Log("SERIES-10 ONLY %s (asked by %s): told %d client(s)", want ? "on" : "off", Addr(from, buf, sizeof(buf)), n);
            }
        }
        if (p.type == 'C' && p.id <= LOADOUT_NEW) {                          // a podium asks for LOADOUTS
            LONG want = (LONG)p.id;
            if (InterlockedExchange(&g_classic, want) != want) {
                if (g_rules) g_rules[CLASSIC_SHOWN] = (uint8_t)want;
                sockaddr_in nobody = {};
                int n = Relay(nobody, 'G', RulesWord());
                Log("LOADOUTS %s (asked by %s): told %d client(s)", LoadoutName(want), Addr(from, buf, sizeof(buf)), n);
            }
        }
        if (p.type == 'M' && g_medigun_on_server && FirstTime(p.id)) {
            MedigunHere(p.id);
            Relay(from, 'M', p.id);
        }
        if (Carried(p.type) && FirstTime(p.id)) {
            if (p.type != 'T') AddToken(Tokens(p.type));   // a teleport has nothing to show here
            int n = Relay(from, p.type, p.id);
            LogDebug("%s %08x from %s: %srelayed to %d client(s)", What(p.type), p.id,
                Addr(from, buf, sizeof(buf)), p.type != 'T' ? "token here, " : "", n);
        }
    } else if (p.type == 'A') {
        Dest d;                                          // only the current server's answer counts
        if (ServerDest(d) && Same(d.addr, from)) {
            g_last_answer = GetTickCount64();
            g_last_answer_flags = p.id;
        }
    } else if (p.type == 'L') {
        Dest d;
        if (ServerDest(d) && Same(d.addr, from) && p.id < (uint32_t)MAP_COUNT && g_map_target != (LONG)p.id + 1) {
            g_map_target = (LONG)p.id + 1;
            g_map_target_since = GetTickCount64();
            g_map_grey = 1;
            Log("MAP SWITCH from the server: %s", kMapNames[p.id]);
        }
    } else if (p.type == 'R') {
        Dest d;
        if (g_rules_total && ServerDest(d) && Same(d.addr, from) && *(volatile uint32_t *)g_rules_total != p.id) {
            *(volatile uint32_t *)g_rules_total = p.id;
            float t;
            memcpy(&t, &p.id, 4);
            Log("RULES from the server: point unlock %d s, capture time %d s", (int)t / 60, (int)t % 60);
        }
    } else if (p.type == 'G') {
        Dest d;
        uint8_t m = (p.id & 3) <= LOADOUT_NEW ? (uint8_t)(p.id & 3) : (uint8_t)LOADOUT_ALL;
        uint8_t c = (uint8_t)(p.id >> 2 & 1);
        uint8_t q = (uint8_t)(p.id >> 8);
        if (g_rules && ServerDest(d) && Same(d.addr, from)) {
            if (g_rules[RATE_SHOWN] != q) {
                g_rules[RATE_SHOWN] = q;
                Log("POINT RATE from the server: %d/4", (int)q);
            }
            uint8_t pm = (uint8_t)(p.id >> 5 & 1), sk = (uint8_t)(p.id >> 6 & 1);
            uint8_t ps = (uint8_t)(p.id >> 16), cp = (uint8_t)(p.id >> 24 & 3);
            if (g_rules[PAYLOAD_MAP] != pm || g_rules[PSPEED_SHOWN] != ps || g_rules[CHECKPOINT_SHOWN] != cp) {
                g_rules[PAYLOAD_MAP] = pm;
                g_rules[PSPEED_SHOWN] = ps;
                g_rules[CHECKPOINT_SHOWN] = cp;
                Log("PAYLOAD from the server: %s map, speed %d/4, checkpoint used %d", pm ? "payload" : "other",
                    (int)ps, (int)cp);
            }
            g_rules[SKIP_SHOWN] = sk;
            g_rules[PAUSE_QUEUED_SHOWN] = (uint8_t)(p.id >> 7 & 1);
            uint8_t z = (uint8_t)(p.id >> 3 & 3);
            if (z > PAUSE_RESUMING) z = PAUSE_OFF;
            if (g_rules[PAUSED_SHOWN] != z) {
                g_rules[PAUSED_SHOWN] = z;
                Log("PAUSE from the server: %s", z == PAUSE_ON ? "on" : z == PAUSE_RESUMING ? "resuming" : "off");
            }
            if (g_rules[CLASSIC_SHOWN] != m) {
                g_rules[CLASSIC_SHOWN] = m;
                Log("LOADOUTS from the server: %s", LoadoutName(m));
            }
            if (g_rules[CHASSIS_SHOWN] != c) {
                g_rules[CHASSIS_SHOWN] = c;
                Log("SERIES-10 ONLY from the server: %s", c ? "on" : "off");
            }
            PageTexts();
        }
    } else if (p.type == 'M' && FirstTime(p.id)) {
        if (g_last_answer_flags & ANSWER_MEDIGUN) MedigunHere(p.id);
    } else if (p.type == 'K' && FirstTime(p.id)) {          // a drone the server killed: DroneFix shuts it down
        using killed_fn = void(__cdecl *)(uint32_t);
        static killed_fn killed = nullptr;
        if (!killed)
            if (HMODULE dronefix = GetModuleHandleW(L"DroneFix.dll"))
                killed = (killed_fn)GetProcAddress(dronefix, "DroneFix_Killed");
        if (killed) killed(p.id & 0xffff);
        if (killed)
            LogDebug("drone %u killed on the server: shut down here", p.id & 0xffff);
        else
            LogWarning("drone %u killed on the server: NOT shut down here (no DroneFix.dll)", p.id & 0xffff);
    } else if (Carried(p.type) && FirstTime(p.id)) {
        AddToken(Tokens(p.type));
        LogDebug("%s %08x from the server: token here", What(p.type), p.id);
    }
}

// --- the hooks ----------------------------------------------------------------------
void ApplyClassic();
void ApplyChassisRule();
void ApplyPause();
void EndMatchStep();
void ApplyPayload();
void RefreshStations();
int WSAAPI Hooked_sendto(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen) {
    if (!g_server) SawSend(s, to);
    return Real_sendto(s, buf, len, flags, to, tolen);
}

// VeilNet's packets are taken out here; the game gets the next datagram, or "nothing
// waiting" (WSAEWOULDBLOCK) when there is none, instead of a packet it cannot read.
int WSAAPI Hooked_recvfrom(SOCKET s, char *buf, int len, int flags, sockaddr *from, int *fromlen) {
    ApplyClassic();                                  // the game's thread: LOADOUTS changes land here
    ApplyChassisRule();                              // ... and SERIES-10 ONLY ones
    ApplyPause();                                    // ... and PAUSE
    ApplyPayload();                                  // ... and the payload rules
    RefreshStations();
    for (;;) {
        sockaddr_storage own;
        int own_len = sizeof(own);
        sockaddr *f = from ? from : (sockaddr *)&own;
        int *fl = from ? fromlen : &own_len;
        int saved = fl ? *fl : 0;
        int n = Real_recvfrom(s, buf, len, flags, f, fl);
        if (n != (int)sizeof(Packet) || (flags & MSG_PEEK) || !fl) return n;
        Packet p;
        memcpy(&p, buf, sizeof(p));
        if (p.magic != MAGIC || p.magic2 != MAGIC2) return n;
        if (f->sa_family == AF_INET) Handle(s, p, *(const sockaddr_in *)f);
        *fl = saved;
        u_long waiting = 0;
        if (ioctlsocket(s, FIONREAD, &waiting) != 0 || waiting == 0) {
            WSASetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
    }
}

// The tac mod list as the server allows: all of it on a server with EchoCombat, without the
// last three (ours) on one without. The entries stay where they are; only the count
// changes. A rebuilt list (SettingsUpdateCB reloads the gear tables) has a new entries
// pointer and its full count again. Nothing is touched unless its last three are ours.
uint8_t *g_list_entries;
uint64_t g_list_full;

bool OursAtEnd(uint8_t *entries, uint64_t full) {
    for (int k = 0; k < 3; ++k)
        if (strncmp((const char *)entries + (full - 3 + k) * TACMOD_ENTRY, kNewTacMods[k], 64) != 0) return false;
    return true;
}

// The tac mod the player has selected (EQUIP_RVA, from their profile update) is one of ours.
bool OurSelected() {
    uint64_t sym = *(volatile uint64_t *)(g_base + EQUIP_RVA);
    return sym == 0xe32dc7c9d4804fb5ull || sym == 0xe32dc7cad6864aa4ull || sym == 0xc8c33e48386618b6ull;   // smoke, point, buff
}

// The equipment station does not read the gear lists' own counts: it reads the gear type table
// (*(echovr+0x20d3440): entries of 98h bytes at +08h, count +10h; each {+00 the gear list,
// +08 its item count, +10 name, +50 symbol, +58 display name}), which the gear table loader
// (+0xC4EBA0) fills once, with the counts the lists had then. So a trimmed list still showed
// every item there. StationView sets a list's row to `count` items, from item `skip` on: with a
// skip, the row points at a header of our own (the list's, entries moved on by `skip`); the
// list itself is untouched. Called under the callers' __try.
constexpr uintptr_t GEAR_TYPE_TABLE_RVA = 0x20d3440;
constexpr size_t GEAR_TYPE_ENTRY = 0x98;
struct GearListHeader { uint64_t allocator; uint8_t *entries; uint64_t count, capacity; };
GearListHeader g_station_view[3];
uint8_t *g_station_view_of[3];                  // the list each own header stands for

// The station only lays its icons out again on some events (its script's setupicons): one is
// delegate_onlobbysettingsreceived, which CR15NetGame::OnLobbySettings raises through
// DispatchEventToSession(net game, event) (+0x1A9FE0). When StationView changes a row, the
// game's thread (the recvfrom hook) raises that event again so the open station redraws.
using dispatch_fn = void(__fastcall *)(uint64_t, uint64_t);
constexpr uintptr_t DISPATCH_EVENT_RVA = 0x1A9FE0;
const uint8_t kDispatchHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30};
constexpr uint64_t EVT_LOBBY_SETTINGS_RECEIVED = 0xaf4b0aa6bf6e258aull;   // delegate_onlobbysettingsreceived
dispatch_fn g_real_dispatch;
volatile uint64_t g_netgame;
volatile ULONGLONG g_station_changed = 0;      // when a row last changed (0: none waiting)
constexpr ULONGLONG STATION_SETTLE_MS = 400;    // one redraw, once the rows have stopped changing

void GameOverSeen(uint64_t event);
void __fastcall HookedDispatch(uint64_t netgame, uint64_t event) {
    g_netgame = netgame;
    GameOverSeen(event);
    g_real_dispatch(netgame, event);
}

// Each station grid cell holds one icon element per item (and a lock); the script only shows
// its current item's, never hides the one shown before. So when a cell's item changes the old
// icon stays under the new one. CCanvasUICS::SetAlpha (+0x37B2D0: system, component index,
// element key, alpha) is watched for the cells' item / lock elements (station_icon_keys.h):
// every one shown is kept, and all of them are hidden again just before a redraw.
using setalpha_fn = void(__fastcall *)(uint64_t, uint32_t, uint64_t, float);
constexpr uintptr_t SET_ALPHA_RVA = 0x37B2D0;
const uint8_t kSetAlphaHead[] = {0x48, 0x83, 0xec, 0x58, 0x48, 0x8d, 0x05};
setalpha_fn g_real_setalpha;
struct ShownIcon { uint64_t cs; uint64_t key; uint32_t index; };
ShownIcon g_shown[1024];
int g_shown_count = 0;

bool StationIcon(uint64_t key) {
    return std::binary_search(std::begin(kStationIconKeys), std::end(kStationIconKeys), key);
}

// Canvases without an element index name their gear icons plainly (WEAPON_ASSAULT, ...): the
// station's loadout bar shows one of each kind that way. When LOADOUTS swaps a player's gear,
// its script showed the new icon over the old one. So in one canvas component only one icon
// of a kind stays shown: showing one hides the one shown before.
struct ShownGear { uint64_t cs; uint64_t key; uint32_t index; int kind; };
ShownGear g_shown_gear[256];
int g_shown_gear_count = 0;

int GearKind(uint64_t key) {
    auto it = std::lower_bound(std::begin(kGearElements), std::end(kGearElements), key,
                               [](const GearElement &e, uint64_t k) { return e.key < k; });
    return it != std::end(kGearElements) && it->key == key ? it->kind : -1;
}

void PlainGearShown(uint64_t cs, uint32_t index, uint64_t key, float alpha, int kind) {
    for (int k = 0; k < g_shown_gear_count;) {
        ShownGear &g = g_shown_gear[k];
        if (g.cs == cs && g.index == index && g.kind == kind && (g.key == key || alpha > 0.0f)) {
            if (g.key != key) {
                Log("gear icon %016llx hidden in %llx/%u: %016llx shown there", g.key, cs, index & 0xffff, key);
                __try {
                    g_real_setalpha(g.cs, g.index, g.key, 0.0f);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            g = g_shown_gear[--g_shown_gear_count];
            continue;
        }
        ++k;
    }
    if (alpha > 0.0f && g_shown_gear_count < (int)(sizeof(g_shown_gear) / sizeof(g_shown_gear[0])))
        g_shown_gear[g_shown_gear_count++] = {cs, key, index, kind};
}

void __fastcall HookedSetAlpha(uint64_t cs, uint32_t index, uint64_t key, float alpha) {
    g_real_setalpha(cs, index, key, alpha);
    int kind = GearKind(key);
    if (kind >= 0) {
        Log("gear icon %016llx alpha %.2f in %llx/%u (kind %d)", key, alpha, cs, index & 0xffff, kind);
        PlainGearShown(cs, index, key, alpha, kind);
        return;
    }
    if (!StationIcon(key)) return;
    for (int k = 0; k < g_shown_count; ++k)
        if (g_shown[k].key == key && g_shown[k].cs == cs && g_shown[k].index == index) {
            if (alpha <= 0.0f) g_shown[k] = g_shown[--g_shown_count];
            return;
        }
    if (alpha > 0.0f && g_shown_count < (int)(sizeof(g_shown) / sizeof(g_shown[0])))
        g_shown[g_shown_count++] = {cs, key, index};
}

// On the game's thread, before a redraw: every station icon shown so far hidden.
void HideStationIcons() {
    int n = g_shown_count;
    g_shown_count = 0;
    for (int k = 0; k < n; ++k) {
        __try {
            g_real_setalpha(g_shown[k].cs, g_shown[k].index, g_shown[k].key, 0.0f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

// --- PAUSE: what each machine does on the game's thread -------------------------------------
// Server: the game's own cease fire (mp_combat_weapon_access_controller listens for these and
// sets the combat_cease_fire flag every gear script reads). Client: its player back to the war
// room (mp_player_teleport_manager: nevt_force_respawn), and the podium's TEAM READY label:
// PAUSE / UNPAUSE while the match is live. The label is the button's own canvas text, set once
// through CCanvasUICS::SetText (+0x37D470: system, handle, element, text); every place written
// "TEAM READY" is kept and written again here.
constexpr uint64_t EVT_CEASE_FIRE_ON = 0x16ec52a8762f90b1ull;    // nevt_to_server_enable_combat_cease_fire
constexpr uint64_t EVT_CEASE_FIRE_OFF = 0xfd8c151be6f7f309ull;   // nevt_to_server_disable_combat_cease_fire
constexpr uint64_t EVT_FORCE_RESPAWN = 0x9b38d87edfd3c576ull;    // nevt_force_respawn
using settext_fn = uint64_t(__fastcall *)(uint64_t, uint32_t, uint64_t, const char *);
constexpr uintptr_t SET_TEXT_RVA = 0x37D470;
const uint8_t kSetTextHead[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x49, 0x8b, 0xd9, 0x4d, 0x85, 0xc9};
settext_fn g_real_settext;
struct ReadyLabel { uint64_t cs, element; uint32_t handle; };
ReadyLabel g_ready_labels[8];
int g_ready_label_count = 0;
const char *g_ready_shown = "TEAM READY";
volatile LONG g_settext_own = 0;

uint64_t __fastcall HookedSetText(uint64_t cs, uint32_t handle, uint64_t element, const char *text) {
#ifdef ECHOCOMBAT_LOG_EVENTS
    if (text && !g_settext_own) {                    // MAP TERMINAL research: who writes which label
        static uint64_t seen[1024];
        static int n = 0;
        uint64_t h = 1469598103934665603ull;
        for (const char *c = text; *c && c - text < 64; ++c) h = (h ^ (uint8_t)*c) * 1099511628211ull;
        h ^= cs * 31 ^ (uint64_t)handle * 131 ^ element;
        int k = 0;
        while (k < n && seen[k] != h) ++k;
        if (k == n && n < 1024) { seen[n++] = h; Log("SetText %llx/%08x element %016llx: \"%.64s\"", cs, handle, element, text); }
    }
#endif
    if (!g_settext_own && text && !strcmp(text, "TEAM READY")) {
        int k = 0;
        while (k < g_ready_label_count && !(g_ready_labels[k].cs == cs && g_ready_labels[k].handle == handle &&
                                           g_ready_labels[k].element == element)) ++k;
        if (k == g_ready_label_count && k < (int)(sizeof(g_ready_labels) / sizeof(g_ready_labels[0]))) {
            g_ready_labels[g_ready_label_count++] = {cs, element, handle};
            Log("PAUSE: TEAM READY label found (%llx/%u)", cs, handle & 0xffff);
        }
        if (strcmp(g_ready_shown, "TEAM READY")) text = g_ready_shown;   // the podium put it back
    }
    return g_real_settext(cs, handle, element, text);
}

// Capture point neutral again: what the script's R15NetCaptureVolumeResetNode defers,
// FUN_140d3f520(capture volume CS, component handle): possession none, claims cleared, OnReset
// raised and the reset replicated. The CS is the capture tick's r12 (InstallCaptureTime keeps it
// with the time of the tick); its volumes: count u16 +FCh, dense -> sparse u16 [+E0h], sparse ->
// {dense u16, generation u16} [+D0h]; handle = sparse | generation << 16. Server only, on the game
// thread; only while the tick is current (no stale CS from another level).
using capture_reset_fn = void(__fastcall *)(uint64_t, uint32_t);
constexpr uintptr_t CAPTURE_RESET_RVA = 0xD3F520;
const uint8_t kCaptureResetHead[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40};
struct CaptureTick { volatile uint64_t cs, count; };
CaptureTick g_capture_tick = {};                // written by the capture tick stub every tick
uint64_t g_capture_count_seen = 0;              // the count when last looked at ...
ULONGLONG g_capture_count_when = 0;             // ... and when it last moved

// The capture tick ran in the last second (a capture point map is loaded and live).
bool CaptureTicking() {
    uint64_t c = g_capture_tick.count;
    ULONGLONG now = GetTickCount64();
    if (c != g_capture_count_seen) { g_capture_count_seen = c; g_capture_count_when = now; }
    return c && now - g_capture_count_when < 1000;
}

// The engine reset only raises delegate_OnReset; the point script (b7d10) takes its owner and
// which team is charging from delegate_OnPossessionChanged (reading the volume's possession). So
// after a reset that event is raised too, built as FUN_140d3e9a0 builds OnReset's: the CS's
// vtable +1D8h (cs, out, handle) twice for the actor link / param, then
// DispatchComponentEvent(*(cs+80h), event, link, param, 1) (+0x510060).
using component_ref_fn = uint64_t *(__fastcall *)(uint64_t, uint64_t *, uint32_t);
using dispatch_component_fn = void(__fastcall *)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t);
constexpr uintptr_t DISPATCH_COMPONENT_RVA = 0x510060;
const uint8_t kDispatchComponentHead[] = {0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x41, 0x58};
constexpr uint64_t EVT_POSSESSION_CHANGED = 0xea580f491680c6e0ull;   // delegate_OnPossessionChanged

// DispatchComponentEvent to call: VeilNet hooks it itself (HookedDCE), so its entry no longer
// holds the stock bytes; then the trampoline to the real one. (Comparing the entry bytes alone
// made END MATCH's capture and the PAUSE possession poke skip silently on a hooked game.)
extern dispatch_component_fn g_real_dce;
// On a client another plugin hooks it first (test: "no DispatchComponentEvent", grey buttons): then
// VeilNet has no hook of its own; the entry is that plugin's jmp, and calling it goes through it.
dispatch_component_fn DceFn() {
    if (g_real_dce) return g_real_dce;
    const uint8_t *e = g_base + DISPATCH_COMPONENT_RVA;
    if (!memcmp(e, kDispatchComponentHead, sizeof(kDispatchComponentHead)) ||
        (e[0] == 0xe9 && !memcmp(e + 8, "\x48\x89\x54\x24\x20", 5)))   // Detours: jmp + int3 pad; 3rd insn stock
        return (dispatch_component_fn)(g_base + DISPATCH_COMPONENT_RVA);
    return nullptr;
}

// Game thread: evt_btn_disable to each map button actor found in the game space (*(netgame+2B08h)).
void GreyMapButtons(uint64_t netgame) {
    if (!netgame || memcmp(g_base + FIND_ACTOR_RVA, kFindActorHead, sizeof(kFindActorHead))) {
        Log("MAP SWITCH: actor lookup not callable, buttons not greyed");
        return;
    }
    auto dispatch = DceFn();
    if (!dispatch) { Log("MAP SWITCH: no DispatchComponentEvent, buttons not greyed"); return; }
    uint64_t space = *(uint64_t *)(netgame + 0x2B08);
    if (!space) { Log("MAP SWITCH: no game space, buttons not greyed"); return; }
    int n = 0;
    for (int k = 0; k < MAP_COUNT; ++k) {
        uint64_t link = ~0ull;
        uint64_t found = ((find_actor_fn)(g_base + FIND_ACTOR_RVA))(space, MAP_BUTTON_ACTOR[k], &link);
        Log("MAP SWITCH: button %s actor lookup %llx link %016llx", kMapNames[k], found, link);
        if (link != ~0ull) {
            dispatch(space, EVT_BTN_DISABLE, link, ~0ull, 1);
            ++n;
        }
    }
    Log("MAP SWITCH: %d map button(s) greyed", n);
}

// Every capture volume of the ticking CS: reset (server only) and/or OnPossessionChanged raised.
void ForEachCapturePoint(bool reset_it, bool announce) {
    uint64_t cs = g_capture_tick.cs;
    if (!cs || !CaptureTicking()) { Log("PAUSE: no capture point ticking, nothing reset"); return; }
    if (memcmp(g_base + CAPTURE_RESET_RVA, kCaptureResetHead, sizeof(kCaptureResetHead))) return;
    auto dispatch = DceFn();
    bool can_announce = dispatch != nullptr;
    auto reset = (capture_reset_fn)(g_base + CAPTURE_RESET_RVA);
    __try {
        auto ref = *(component_ref_fn *)(*(uint64_t *)cs + 0x1D8);
        uint16_t n = *(uint16_t *)(cs + 0xFC);
        uint16_t *dense_to_sparse = *(uint16_t **)(cs + 0xE0);
        uint8_t *sparse = *(uint8_t **)(cs + 0xD0);
        for (uint16_t i = 0; i < n && i < 16; ++i) {
            uint16_t si = dense_to_sparse[i];
            uint32_t handle = si | (uint32_t)*(uint16_t *)(sparse + si * 4 + 2) << 16;
            if (reset_it) reset(cs, handle);
            if (announce && can_announce) {
                uint64_t a[2] = {}, b[2] = {};
                uint64_t param = ref(cs, a, handle)[1];
                uint64_t link = ref(cs, b, handle)[0];
                dispatch(*(uint64_t *)(cs + 0x80), EVT_POSSESSION_CHANGED, link, param, 1);
            }
            Log("PAUSE: capture point %u (cs %llx, handle %08x):%s%s", i, cs, handle,
                reset_it ? " reset" : "", announce && can_announce ? " possession change raised" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("PAUSE: capture point reset failed (cs %llx)", cs);
    }
}
void ResetCapturePoints() { ForEachCapturePoint(true, true); }

float *g_rules_total_ptr() { return g_rules_total; }

#ifdef ECHOCOMBAT_LOG
// Logging build, server: each capture volume's state, once a second when it changed (the slow
// CAPTURE TIME 0 report). Volume (50h bytes, dense, *(cs+100h)): +08 per-team {short claims,
// float progress} x2, +40 flags (1 = not ticking), +44 owner (2 none), +46 capturing (ffff
// none), +48 capture time. Mailbox +8 T, +0Ch the RULES capture time.
void TraceCapture() {
    static ULONGLONG next = 0;
    static char last[512];
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    uint64_t cs = g_capture_tick.cs;
    char line[512];
    int n = 0;
    __try {
        float t = g_rules_total ? *(volatile float *)g_rules_total : -1.0f;
        float c = g_capture_time ? *(volatile float *)g_capture_time : -1.0f;
        n += sprintf_s(line + n, sizeof(line) - n, "T %.0f cap %.3f ticking %d", t, c, CaptureTicking() ? 1 : 0);
        if (cs && CaptureTicking()) {
            uint16_t count = *(uint16_t *)(cs + 0xFC);
            uint8_t *volumes = *(uint8_t **)(cs + 0x100);
            for (uint16_t i = 0; i < count && i < 4 && volumes; ++i) {
                uint8_t *v = volumes + i * 0x50;
                uint8_t *teams = *(uint8_t **)(v + 8);
                n += sprintf_s(line + n, sizeof(line) - n, " | vol %u flags %x owner %d capturing %d time %.2f",
                               i, *(uint16_t *)(v + 0x40), (int)*(int16_t *)(v + 0x44), (int)*(int16_t *)(v + 0x46),
                               *(float *)(v + 0x48));
                if (teams)
                    n += sprintf_s(line + n, sizeof(line) - n, " claims %d/%d progress %.2f/%.2f",
                                   (int)*(int16_t *)(teams + 2), (int)*(int16_t *)(teams + 10),
                                   *(float *)(teams + 4), *(float *)(teams + 12));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n += sprintf_s(line + n, sizeof(line) - n, " (read failed)");
    }
    if (strcmp(line, last)) {
        strcpy_s(last, line);
        Log("CAPTURE %s", line);
    }
}
#endif

// END MATCH, on the server's game thread: blue captures every capture volume it does not own.
void EndMatchCapture() {
    static ULONGLONG next = 0, said = 0;
    ULONGLONG now = GetTickCount64();
    if (!g_server || !g_end_until || now < next) return;
    next = now + 500;
    uint64_t cs = g_capture_tick.cs;
    bool ticking = CaptureTicking();
    if (!cs || !ticking) {
        if (now - said > 3000) { said = now; Log("END MATCH: no capture point ticking yet (cs %llx)", cs); }
        return;
    }
    auto dispatch = DceFn();
    if (!dispatch) {
        if (now - said > 3000) { said = now; Log("END MATCH: DispatchComponentEvent not callable (unexpected build)"); }
        return;
    }
    __try {
        auto ref = *(component_ref_fn *)(*(uint64_t *)cs + 0x1D8);
        uint16_t n = *(uint16_t *)(cs + 0xFC);
        uint8_t *volumes = *(uint8_t **)(cs + 0x100);
        uint16_t *dense_to_sparse = *(uint16_t **)(cs + 0xE0);
        uint8_t *sparse = *(uint8_t **)(cs + 0xD0);
        // Test: owner forced while the point was still locked (+40 bit 0, the unlock countdown)
        // was never heard by the point script, and afterwards blue already owned it, so nothing
        // could capture it. So: only an unlocked point, and blue's ownership is announced again
        // every second while END MATCH runs.
        static ULONGLONG announced = 0;
        bool again = now - announced >= 1000;
        for (uint16_t i = 0; i < n && i < 16 && volumes; ++i) {
            uint8_t *vol = volumes + i * 0x50;
            if (*(uint16_t *)(vol + 0x40) & 1) continue;             // locked: not yet
            bool blue = *(uint16_t *)(vol + 0x44) == 0;
            if (blue && !again) continue;
            uint16_t si = dense_to_sparse[i];
            uint32_t handle = si | (uint32_t)*(uint16_t *)(sparse + si * 4 + 2) << 16;
            if (blue) {
                uint64_t a[2] = {}, b[2] = {};
                uint64_t param = ref(cs, a, handle)[1];
                uint64_t link = ref(cs, b, handle)[0];
                dispatch(*(uint64_t *)(cs + 0x80), EVT_POSSESSION_CHANGED, link, param, 1);
                announced = now;
                continue;
            }
            if (uint8_t *teams = *(uint8_t **)(vol + 8)) {
                *(float *)(teams + 4) = 0.0f;                        // both teams' capture progress
                *(float *)(teams + 12) = 0.0f;
            }
            *(uint16_t *)(vol + 0x44) = 0;                          // owner: blue
            *(uint16_t *)(vol + 0x46) = 0xffff;                     // nobody capturing
            *(uint16_t *)(vol + 0x40) |= 4;                         // replicate it
            uint64_t a[2] = {}, b[2] = {};
            uint64_t param = ref(cs, a, handle)[1];
            uint64_t link = ref(cs, b, handle)[0];
            dispatch(*(uint64_t *)(cs + 0x80), EVT_POSSESSION_CHANGED, link, param, 1);
            announced = now;
            Log("END MATCH: blue captured capture point %u", i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("END MATCH: capture failed (cs %llx)", cs);
    }
}

// END MATCH, on the server's tick thread (see WANT_END).
void EndMatchStep() {
    ULONGLONG now = GetTickCount64();
    if (now >= g_end_until) { EndMatchRestore("no game over in time, gave up"); return; }
    if (now < g_end_next) return;
    g_end_next = now + END_EVERY_MS;
    if (!g_end_saved) {
        g_end_old_total = g_rules_total ? *(volatile float *)g_rules_total : 0.0f;
        g_end_saved = true;
    }
    if (g_rules_total) *(volatile float *)g_rules_total = END_TOTAL;
    if (!g_end_point || !LooksLikePoint(g_end_point)) {
        g_end_point = nullptr;
        if (now - g_end_scanned < END_RESCAN_MS) return;
        g_end_scanned = now;
        g_end_point = FindPointData();
        Log(g_end_point ? "END MATCH: capture point data at %p" : "END MATCH: no capture point data found%s",
            g_end_point ? (void *)g_end_point : (void *)"");
        if (!g_end_point) return;
    }
    __try {
        *(volatile float *)(g_end_point + 0xF8) = END_POINTS;
        if (InterlockedIncrement(&g_end_written) % 20 == 1) Log("END MATCH: blue at 9999 points (%ld)", (long)g_end_written);
        // The point's unlock countdown (+DCh = lock time - (now - start +F4h), worked out every
        // frame by the script; T only sets the lock time a round starts with, so a round already
        // running kept its 45 s): its start moves back by what is left, so it unlocks now.
        float left = *(volatile float *)(g_end_point + 0xDC);
        if (left > 0.2f && left < 1000.0f) {
            *(volatile float *)(g_end_point + 0xF4) -= left;
            Log("END MATCH: point unlock countdown %.1f s cut to 0", left);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_end_point = nullptr;
    }
}

// While paused (or counting down to resume) nothing but VeilNet may lift the cease fire: in test a
// gun came back during a pause once. Candidates: the game-mode script's own
// nevt_to_server_disable_combat_cease_fire (state changes) and the capture point's
// evt_disable_combat_local_cease_fire (it raises it on point events; our OnPossessionChanged poke).
// Every component event goes through DispatchComponentEvent (+0x510060), SENDEVENTGLOBAL included,
// so it is hooked there on both sides and those events are dropped while +2E != 0.
constexpr uint64_t EVT_LOCAL_CEASE_FIRE_OFF = 0x0ea31347147ad3c3ull;      // evt_disable_combat_local_cease_fire
constexpr uint64_t EVT_LOCAL_CEASE_FIRE_TOGGLE = 0x899131833bb7c5b9ull;   // evt_toggle_combat_local_cease_fire
dispatch_component_fn g_real_dce;
volatile LONG g_own_cease = 0;                  // set while VeilNet itself lifts the cease fire

// Payload checkpoints, as the payload game-mode script (45e8df51b7524e20) announces them.
constexpr uint64_t kCheckpointEvents[] = {
    0x3a490573955b624dull,   // evt_cbt_obj_complete (the orbs move on for both teams)
    0xc768c939908144b4ull,   // nevt_all_major_checkpoint_1_reached
    0xba83a3cc7939f10aull,   // nevt_all_major_checkpoint_2_reached
};
ULONGLONG g_last_checkpoint_event = 0;
uint64_t g_events_logged[512];
int g_events_logged_count = 0;

// A checkpoint reached (once per 5 s, whatever said so): test 2 showed the payload's PAUSED flag
// (the script's checkpoint hold) is the reliable sign; the script's events did not show up here.
void CheckpointHit(const char *why, uint64_t event) {
    ULONGLONG now = GetTickCount64();
    if (now - g_last_checkpoint_event > 5000) {
        InterlockedIncrement(&g_checkpoint_hits);
        Log("PAYLOAD: checkpoint reached (%s %016llx), %d so far", why, event, (int)g_checkpoint_hits);
    }
    g_last_checkpoint_event = now;
}

// (CHECKPOINT USED was here: dropped 2026-11-05 at the user's request; see agent.md for what was learned)

void MapSwitchEvent(uint64_t event);
void WatchEvent(uint64_t space, uint64_t event, uint64_t link, uint64_t param) {
    GameOverSeen(event);
    MapSwitchEvent(event);
#ifdef ECHOCOMBAT_LOG
    // Two-player test: both teams readied and the match never left pre_match_wait_for_ready.
    // The server logs the ready events by name, and every other event the first time it is seen.
    if (g_server) {
        static const struct { uint64_t e; const char *n; } kReady[] = {
            {0x70f8cfcd41c4de99ull, "nevt_blue_team_ready"}, {0x1d108d26ada8c1ddull, "nevt_red_team_ready"},
            {0x303a4ded4074960full, "nevt_orange_team_ready"}, {0x6ad7a5870f1f5115ull, "nevt_team_ready"},
            {0xa25c369c8347706full, "nevt_match_ready"}};
        for (const auto &r : kReady)
            if (event == r.e) Log("READY: %s link %016llx param %016llx", r.n, link, param);
        static uint64_t seen[1024];
        static int n = 0;
        int k = 0;
        while (k < n && seen[k] != event) ++k;
        if (k == n && n < 1024) {
            seen[n++] = event;
            Log("SERVER EVT %016llx link %016llx param %016llx (first seen)", event, link, param);
        }
    }
#endif
#ifdef ECHOCOMBAT_LOG_EVENTS
    if (!g_server) {                                 // MAP TERMINAL research: which events a press raises
        static uint64_t seen[2048][2];
        static int n = 0;
        int k = 0;
        while (k < n && !(seen[k][0] == event && seen[k][1] == link)) ++k;
        if (k == n && n < 2048) {
            seen[n][0] = event; seen[n][1] = link; ++n;
            Log("EVT %016llx link %016llx param %016llx (first seen)", event, link, param);
        }
    }
#endif
    for (uint64_t e : kCheckpointEvents)
        if (event == e) CheckpointHit("event", event);
#ifdef ECHOCOMBAT_LOG
    // every global event once (link / param -1): which ones a payload round raises
    if (g_payload_seen && GetTickCount64() - g_payload_seen < 1000) {
        for (int k = 0; k < g_events_logged_count; ++k) if (g_events_logged[k] == event) return;
        if (g_events_logged_count < 512) g_events_logged[g_events_logged_count++] = event;
        Log("event %016llx (first seen)", event);
    }
#endif
}

// The payload script's evt_cbt_obj_complete (the event that moves both teams' orbs on: a
// checkpoint) did not pass DispatchComponentEvent: script sends go straight to the two inner
// dispatchers, DispatchComponentEventInternal (+0x50FED0) and its twin's body (+0x50FB60), both
// (space, SComponentEvent *: +0 the event, flags). Watched there on the server.
using dce_inner_fn = uint64_t(__fastcall *)(uint64_t, uint64_t *, uint32_t);
constexpr uintptr_t DCE_INNER_RVA = 0x50FED0, DCE_INNER2_RVA = 0x50FB60;
const uint8_t kDceInnerHead[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
const uint8_t kDceInner2Head[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
dce_inner_fn g_real_dce_inner, g_real_dce_inner2;
uint64_t __fastcall HookedDceInner(uint64_t space, uint64_t *ev, uint32_t flags) {
    if (ev) WatchEvent(space, ev[0], ev[2], ev[3]);
    return g_real_dce_inner(space, ev, flags);
}
uint64_t __fastcall HookedDceInner2(uint64_t space, uint64_t *ev, uint32_t flags) {
    if (ev) WatchEvent(space, ev[0], ev[2], ev[3]);
    return g_real_dce_inner2(space, ev, flags);
}

void __fastcall HookedDCE(uint64_t space, uint64_t event, uint64_t link, uint64_t param, uint32_t flags) {
#ifdef ECHOCOMBAT_LOG
    WatchEvent(space, event, link, param);
#else
    GameOverSeen(event);
    MapSwitchEvent(event);
#endif
    if ((event == EVT_CEASE_FIRE_OFF || event == EVT_LOCAL_CEASE_FIRE_OFF || event == EVT_LOCAL_CEASE_FIRE_TOGGLE) &&
        !g_own_cease && g_rules && g_rules[PAUSED_SHOWN] != PAUSE_OFF) {
        Log("PAUSE: dropped %s while paused", event == EVT_CEASE_FIRE_OFF ? "nevt_to_server_disable_combat_cease_fire"
                                              : event == EVT_LOCAL_CEASE_FIRE_OFF ? "evt_disable_combat_local_cease_fire"
                                                                                  : "evt_toggle_combat_local_cease_fire");
        return;
    }
    g_real_dce(space, event, link, param, flags);
}
ULONGLONG g_possession_poke_at = 0;             // client: raise OnPossessionChanged then (0 = none)

// Combat announcer's countdown lines (the capture point's unlock countdown raises the same).
constexpr uint64_t EVT_VO_COUNTDOWN[6] = {0, 0x4ca52f26bec0d5a3ull, 0x4ca52f26bec0d5a0ull, 0x4ca52f26bec0d5a1ull,
                                          0x4ca52f26bec0d5a6ull, 0x4ca52f26bec0d5a7ull};   // evt_vo_anc_countdown_01..05
// PAUSE sends the local player to the war room with nevt_force_respawn; one raise missed about
// half the time in test, so it is raised again at these times after the pause (while paused).
constexpr int RESPAWN_TRIES = 3;
constexpr ULONGLONG RESPAWN_AGAIN_MS[RESPAWN_TRIES] = {0, 800, 2000};
ULONGLONG g_respawn_started = 0;
int g_respawn_sent = RESPAWN_TRIES;
ULONGLONG g_countdown_start = 0;                // client: when state 2 was seen
int g_countdown_said = 6;                       // client: the last number said (6 = none yet)

volatile LONG g_pause_applied = 0;
// The loaded level off this machine, as LeaveGame does it (game thread).
void UnloadLevel(uint64_t netgame) {
    uint64_t *flags = *(uint64_t **)(netgame + 0x2DA0);
    if (!flags || !(*flags & LEVEL_LOADED_BIT)) { Log("MAP SWITCH: no level loaded here"); return; }
    ((pop_level_set_fn)(g_base + POP_LEVEL_SET_RVA))(*(uint64_t *)(netgame + 8));
    *flags &= ~LEVEL_LOADED_BIT;
    Log("MAP SWITCH: level unloaded");
}

void ApplyMapSwitch() {
    if ((g_map_want || g_map_target) && !g_netgame) {
        static ULONGLONG said = 0;
        if (GetTickCount64() - said > 3000) { said = GetTickCount64(); Log("MAP SWITCH: no net game seen yet"); }
    }
    if (!g_netgame || !MapSwitchCallable()) return;
    ULONGLONG now = GetTickCount64();
    __try {
        uint64_t lobby = NetLobby(g_netgame);
        if (!lobby) return;
        if (g_server && g_map_want) {                     // step 1: the clients leave the game
            int k = (int)g_map_want - 1;
            g_map_want = 0;
            ((owner_set_symbol_fn)(g_base + OWNER_SET_SYMBOL_RVA))(lobby, "level", LEVEL_SYM[k]);
            ((owner_set_bool_fn)(g_base + OWNER_SET_BOOL_RVA))(lobby, "ingame", 0);
            g_map_leaving = k + 1;
            g_map_leave_at = now + MAP_LEAVE_MS;
            g_map_announce = k + 1;                       // the clients leave now
            g_map_announce_at = now;
            Log("MAP SWITCH: owner level = %s, ingame off; restarting in %llu ms", kMapNames[k], MAP_LEAVE_MS);
        }
        if (g_server && g_map_leaving && now >= g_map_leave_at) {   // step 2: the server reloads
            int k = (int)g_map_leaving - 1;
            g_map_leaving = 0;
            UnloadLevel(g_netgame);
            ((owner_set_symbol_fn)(g_base + OWNER_SET_SYMBOL_RVA))(lobby, "level", LEVEL_SYM[k]);
            *(uint64_t *)(lobby - 0x18 + 0x468) = LEVEL_SYM[k];    // the client lobby's own copy (+8Dh qwords)
            Log("MAP SWITCH: owner level = %s; session restarting", kMapNames[k]);
            ((session_started_fn)(g_base + SESSION_STARTED_RVA))(g_netgame);
        }
        if (g_server && g_map_announce && now >= g_map_announce_at) {
            sockaddr_in nobody = {};
            int r = Relay(nobody, 'L', (uint32_t)g_map_announce - 1);
            Log("MAP SWITCH: told %d client(s)", r);
            g_map_announce = 0;                              // once: it only makes clients log
        }
        // The client: once its owner JSON shows the new level, and only while it is still in game
        // (state 9, the net game's first u32), game state 6 "server loading": leaving "in game"
        // disables and unloads its levels and shows the grey transition screen, and the engine's
        // Update loads the owner level when the server is in game again. Once per switch: in test 2
        // a repeated 'L' did it again (with an extra PopLevelSet) while Gauss was loading -> crash;
        // in test 3 without it the client stayed put.
        if (!g_server && g_map_grey) {
            g_map_grey = 0;
            GreyMapButtons(g_netgame);
        }
        if (!g_server && g_map_target) {
            int k = (int)g_map_target - 1;
            uint64_t level = 0;
            ((owner_get_symbol_fn)(g_base + OWNER_GET_SYMBOL_RVA))(lobby, &level, "level", ~0ull);
            uint32_t state = *(volatile uint32_t *)g_netgame;
            if (level == LEVEL_SYM[k]) {
                g_map_target = 0;
                if (state == 9) {
                    Log("MAP SWITCH: owner level is %s; this client to server loading", kMapNames[k]);
                    ((set_game_state_fn)(g_base + SET_GAME_STATE_RVA))(g_netgame, 6);
                    g_map_client_unload_at = now + MAP_CLIENT_UNLOAD_MS;
                } else {
                    Log("MAP SWITCH: owner level is %s; this client in state %u, left alone", kMapNames[k], state);
                }
            } else if (now - g_map_target_since > 15000) {
                g_map_target = 0;
                Log("MAP SWITCH: owner level never became %s; gave up", kMapNames[k]);
            }
        }
        if (!g_server && g_map_client_unload_at && now >= g_map_client_unload_at) {
            g_map_client_unload_at = 0;
            uint32_t state = *(volatile uint32_t *)g_netgame;
            uint64_t *flags = *(uint64_t **)(g_netgame + 0x2DA0);
            int ingame = ((owner_get_bool_fn)(g_base + OWNER_GET_BOOL_RVA))(lobby, "ingame", 0);
            if (state == 6 && !ingame && flags && (*flags & LEVEL_LOADED_BIT)) UnloadLevel(g_netgame);
            else Log("MAP SWITCH: client unload skipped (state %u, server ingame %d, level %s)", state, ingame,
                     flags && (*flags & LEVEL_LOADED_BIT) ? "loaded" : "not loaded");
            if (state == 6 && g_real_dispatch) {
                g_real_dispatch(g_netgame, EVT_MULTIPLAYER_START);
                g_real_dispatch(g_netgame, EVT_NETGAME_LOADING_MP);   // the menu popup
                g_map_popup_at = now + MAP_POPUP_AGAIN_MS;
                Log("MAP SWITCH: menu popup asked for (TRANSITIONING)");
            }
        }
        if (!g_server && g_map_popup_at && now >= g_map_popup_at) {
            g_map_popup_at = 0;
            if (*(volatile uint32_t *)g_netgame == 6 && g_real_dispatch) {
                g_real_dispatch(g_netgame, EVT_MULTIPLAYER_START);
                g_real_dispatch(g_netgame, EVT_NETGAME_LOADING_MP);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_map_want = g_map_announce = g_map_target = 0;
        g_map_client_unload_at = 0;
        g_map_popup_at = 0;
        Log("MAP SWITCH: failed");
    }
}

void EndMatchCapture();
void ApplyPause() {
    ApplyMapSwitch();
    EndMatchCapture();
    if (!g_rules) return;
    LONG paused = g_server ? g_paused : g_rules[PAUSED_SHOWN];
    if (g_server) CaptureTicking();                       // keeps "when the count last moved" current
    if (g_server && paused == PAUSE_RESUMING && GetTickCount64() >= g_resume_at) {
        g_paused = paused = PAUSE_OFF;                    // the countdown is over: the round goes on
        g_rules[PAUSED_SHOWN] = PAUSE_OFF;
        sockaddr_in nobody = {};
        int r = Relay(nobody, 'G', RulesWord());
        Log("PAUSE off (countdown over): told %d client(s)", r);
    }
    if (paused != g_pause_applied && g_real_dispatch && g_netgame) {
        LONG was = g_pause_applied;
        g_pause_applied = paused;
        __try {
            if (g_server) {
                if ((was == PAUSE_OFF) != (paused == PAUSE_OFF)) {
                    g_own_cease = 1;
                    g_real_dispatch(g_netgame, paused ? EVT_CEASE_FIRE_ON : EVT_CEASE_FIRE_OFF);
                    g_own_cease = 0;
                }
                if (paused != PAUSE_OFF) ResetCapturePoints();
            } else if (paused == PAUSE_ON) {
                g_real_dispatch(g_netgame, EVT_FORCE_RESPAWN);
                g_respawn_started = GetTickCount64();      // ... and again (RESPAWN_AGAIN_MS): it missed 1 in 2
                g_respawn_sent = 1;
            } else if (paused == PAUSE_RESUMING) {
                g_countdown_start = GetTickCount64();
                g_countdown_said = 6;
            }
            // the client's point script hears the neutral point once the server's reset is here
            if (!g_server && paused != PAUSE_OFF) g_possession_poke_at = GetTickCount64() + 600;
            Log("PAUSE %d -> %d (%s)", (int)was, (int)paused, g_server ? "server" : "client");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_netgame = 0;
        }
    }
    if (!g_server) CaptureTicking();
    if (!g_server && paused == PAUSE_ON && g_respawn_sent < RESPAWN_TRIES && g_real_dispatch && g_netgame &&
        GetTickCount64() - g_respawn_started >= RESPAWN_AGAIN_MS[g_respawn_sent]) {
        ++g_respawn_sent;
        __try {
            g_real_dispatch(g_netgame, EVT_FORCE_RESPAWN);
            Log("PAUSE: back to the war room (again, %d)", g_respawn_sent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_netgame = 0;
        }
    }
    if (!g_server && g_possession_poke_at && GetTickCount64() >= g_possession_poke_at) {
        g_possession_poke_at = 0;
        ForEachCapturePoint(false, true);
    }
    if (!g_server && paused == PAUSE_RESUMING && g_real_dispatch && g_netgame) {
        // 5 at once, then one a second: the server ends the state 5 s after it began
        int left = 5 - (int)((GetTickCount64() - g_countdown_start) / 1000);
        if (left >= 1 && left < g_countdown_said) {
            g_countdown_said = left;
            __try {
                g_real_dispatch(g_netgame, EVT_VO_COUNTDOWN[left]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_netgame = 0;
            }
        }
    }
    if (!g_server && g_real_dispatch && g_netgame) {
        ULONGLONG now = GetTickCount64();
        uint8_t ok = g_rules[PAUSE_OK];
        if (!g_rules[MATCH_LIVE]) g_live_since = 0;
        else if (!g_live_since) g_live_since = now;
        uint8_t want_ok = g_live_since && now - g_live_since >= PAUSE_OK_DELAY_MS;
        if (want_ok != ok) {
            g_rules[PAUSE_OK] = want_ok;
            __try {
                g_real_dispatch(g_netgame, EVT_ALL_UPDATE_STATE);
                g_real_dispatch(g_netgame, EVT_ALIGNMENT_CHANGED);  // the podium's UpdateAll: TEAM READY / PAUSE grey
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_netgame = 0;
            }
            Log("PAUSE: TEAM READY %s", want_ok ? "becomes PAUSE (1 s after the match went live)" : "is TEAM READY again");
        }
    }
    if (g_server || !g_real_settext) return;
    const char *want = !g_rules[PAUSE_OK] ? "TEAM READY" : g_rules[PAUSED_SHOWN] == PAUSE_ON ? "UNPAUSE"
                     : g_rules[PAUSE_QUEUED_SHOWN] ? "QUEUED" : "PAUSE";
    if (!strcmp(want, g_ready_shown)) return;
    g_ready_shown = want;
    g_settext_own = 1;
    for (int k = 0; k < g_ready_label_count; ++k) {
        __try {
            g_real_settext(g_ready_labels[k].cs, g_ready_labels[k].handle, g_ready_labels[k].element, want);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_settext_own = 0;
}

bool HookDispatch() {
    if (memcmp(g_base + SET_ALPHA_RVA, kSetAlphaHead, sizeof(kSetAlphaHead)) ||
        memcmp(g_base + SET_TEXT_RVA, kSetTextHead, sizeof(kSetTextHead))) return false;
    if (memcmp(g_base + DISPATCH_EVENT_RVA, kDispatchHead, sizeof(kDispatchHead))) return false;
    g_real_dispatch = (dispatch_fn)(g_base + DISPATCH_EVENT_RVA);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    g_real_setalpha = (setalpha_fn)(g_base + SET_ALPHA_RVA);
    g_real_settext = (settext_fn)(g_base + SET_TEXT_RVA);
    DetourAttach(&(PVOID &)g_real_dispatch, (PVOID)HookedDispatch);
    if (!memcmp(g_base + DISPATCH_COMPONENT_RVA, kDispatchComponentHead, sizeof(kDispatchComponentHead))) {
        g_real_dce = (dispatch_component_fn)(g_base + DISPATCH_COMPONENT_RVA);
        DetourAttach(&(PVOID &)g_real_dce, (PVOID)HookedDCE);
    }
    const bool inner_hooks = true;                   // clients too: the map buttons' events
    if (inner_hooks && !memcmp(g_base + DCE_INNER_RVA, kDceInnerHead, sizeof(kDceInnerHead)) &&
        !memcmp(g_base + DCE_INNER2_RVA, kDceInner2Head, sizeof(kDceInner2Head))) {
        g_real_dce_inner = (dce_inner_fn)(g_base + DCE_INNER_RVA);
        g_real_dce_inner2 = (dce_inner_fn)(g_base + DCE_INNER2_RVA);
        DetourAttach(&(PVOID &)g_real_dce_inner, (PVOID)HookedDceInner);
        DetourAttach(&(PVOID &)g_real_dce_inner2, (PVOID)HookedDceInner2);
    }
    if (!g_server) DetourAttach(&(PVOID &)g_real_setalpha, (PVOID)HookedSetAlpha);
    if (!g_server) DetourAttach(&(PVOID &)g_real_settext, (PVOID)HookedSetText);
    if (DetourTransactionCommit() != NO_ERROR) { g_real_dispatch = nullptr; g_real_setalpha = nullptr; g_real_settext = nullptr; return false; }
    return true;
}

// On the game's thread: a station row changed, so the stations lay their icons out again.
void RefreshStations() {
    // Every change of a row asked for a redraw of its own before: two layout passes then ran at
    // once in the station's script and left icons overlapping. Now one, after the last change.
    ULONGLONG changed = g_station_changed;
    if (!changed || GetTickCount64() - changed < STATION_SETTLE_MS || !g_real_dispatch || !g_netgame) return;
    g_station_changed = 0;
    HideStationIcons();                              // the redraw shows the right ones again
    __try {
        g_real_dispatch(g_netgame, EVT_LOBBY_SETTINGS_RECEIVED);
        Log("LOADOUTS: equipment stations told to redraw");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_netgame = 0;
    }
}

void StationView(uint8_t *list, uint8_t *entries, uint64_t count, uint64_t skip) {
    uint8_t *table = *(uint8_t *volatile *)(g_base + GEAR_TYPE_TABLE_RVA);
    if (!table) return;
    uint8_t *rows = *(uint8_t *volatile *)(table + 0x08);
    uint64_t n = *(volatile uint64_t *)(table + 0x10);
    if (!rows || n > 3) return;
    static uint8_t *seen_rows = nullptr;             // a new table (a level loaded): one redraw, which also
    if (rows != seen_rows) {                         // hides what the first, empty layout left (its locks)
        seen_rows = rows;
        g_station_changed = GetTickCount64();
    }
    for (uint64_t k = 0; k < n; ++k) {
        uint8_t *row = rows + k * GEAR_TYPE_ENTRY;
        volatile uint64_t *row_list = (volatile uint64_t *)row, *row_count = (volatile uint64_t *)(row + 8);
        GearListHeader &own = g_station_view[k];
        // a row pointed at our own header is that header's list's row, no other list's
        bool ours = *row_list == (uint64_t)&own;
        if (ours ? g_station_view_of[k] != list : *row_list != (uint64_t)list) continue;
        if (skip && skip < count + skip) {
            const GearListHeader &real = *(const GearListHeader *)list;
            own.allocator = real.allocator;
            own.entries = entries + skip * TACMOD_ENTRY;
            own.capacity = real.capacity > skip ? real.capacity - skip : 0;
            own.count = count;
            g_station_view_of[k] = list;
            if (*row_list != (uint64_t)&own) { *row_list = (uint64_t)&own; g_station_changed = GetTickCount64(); }
        } else if (*row_list != (uint64_t)list) {
            *row_list = (uint64_t)list;
            g_station_changed = GetTickCount64();
        }
        if (*row_count != count) { *row_count = count; g_station_changed = GetTickCount64(); }
        // the station's cells in use for this row (the items and the lock after them), for
        // tools/fix_station_cells.py: cells past it are moved off the canvas on the next layout
        if (g_rules && k < 3 && g_rules[STATION_CELLS + k] != (uint8_t)(count + 1)) {
            g_rules[STATION_CELLS + k] = (uint8_t)(count + 1);
            g_station_changed = GetTickCount64();
        }
        return;
    }
}

// The list is rebuilt (and its old one freed) on the game's thread: everything here is
// guarded. 0: unchanged, 1: shown, 2: hidden.
int TacModsGuarded(bool server_has_it, uint64_t *now_count) {
    __try {
        uint8_t *list = *(uint8_t *volatile *)(g_base + TACMOD_LIST_RVA);
        if (!list) return 0;
        uint8_t *entries = *(uint8_t *volatile *)(list + 0x08);
        volatile uint64_t *count = (volatile uint64_t *)(list + 0x10);
        uint64_t capacity = *(volatile uint64_t *)(list + 0x18);
        if (!entries) return 0;
        // a new list is filled an entry at a time: its full count is the most it has had
        if (entries != g_list_entries) {
            g_list_entries = entries;
            g_list_full = 0;
        }
        if (*count > g_list_full) g_list_full = *count;
        uint64_t full = g_list_full;
        if (full < 3 || full > capacity || !OursAtEnd(entries, full)) return 0;
        uint64_t want = server_has_it ? full : full - 3;
        // One of ours selected: the list stays whole. Cut short, the selection pointed past
        // its end (a loading match hides them until its server answers): kept, it is only
        // an EchoCombat tac mod on a server without it, as before the gate.
        if (!server_has_it && OurSelected()) want = full;
        if (LoadoutMode() == LOADOUT_CLASSIC && want > CLASSIC_ITEMS) want = CLASSIC_ITEMS;   // LOADOUTS CLASSIC: the stock four
        StationView(list, entries, want, 0);              // the equipment station's copy of the count
        if (*count == want) return 0;
        *count = want;
        *now_count = want;
        return server_has_it ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// MEDIGUN in the weapon list as the server allows (see ANSWER_MEDIGUN): the list's last entry,
// cut off by the count as the tac mods are. While it is this player's weapon it stays (the
// selection would point past the list's end); PowerBoost.dll keeps it a COMET meanwhile.
uint8_t *g_weapon_entries;
uint64_t g_weapon_full;

bool MedigunSelected() {
    using weapon_fn = uint64_t(__cdecl *)();
    static weapon_fn weapon = nullptr;
    if (!weapon)
        if (HMODULE pb = GetModuleHandleW(L"PowerBoost.dll")) weapon = (weapon_fn)GetProcAddress(pb, "PowerBoost_LocalWeapon");
    return weapon && weapon() == 0x2fda8a9b57734284ull;     // CSymbol64("medigun")
}

int MedigunListGuarded(bool allowed, uint64_t *now_count) {
    __try {
        uint8_t *list = *(uint8_t *volatile *)(g_base + WEAPON_LIST_RVA);
        if (!list) return 0;
        uint8_t *entries = *(uint8_t *volatile *)(list + 0x08);
        volatile uint64_t *count = (volatile uint64_t *)(list + 0x10);
        uint64_t capacity = *(volatile uint64_t *)(list + 0x18);
        if (!entries) return 0;
        if (entries != g_weapon_entries) {
            g_weapon_entries = entries;
            g_weapon_full = 0;
        }
        if (*count > g_weapon_full) g_weapon_full = *count;
        uint64_t full = g_weapon_full;
        if (full < 1 || full > capacity) return 0;
        bool medigun_last = strncmp((const char *)entries + (full - 1) * TACMOD_ENTRY, "medigun", 64) == 0;
        uint64_t want = !medigun_last || allowed || MedigunSelected() ? full : full - 1;
        // LOADOUTS: CLASSIC = the four stock guns (the list's first four, the list cut);
        // NEW = the rest, shown by the station from the fifth on (the list stays whole)
        uint32_t mode = LoadoutMode();
        if (mode == LOADOUT_CLASSIC && want > CLASSIC_ITEMS) want = CLASSIC_ITEMS;
        if (mode == LOADOUT_NEW && want > CLASSIC_ITEMS) StationView(list, entries, want - CLASSIC_ITEMS, CLASSIC_ITEMS);
        else StationView(list, entries, want, 0);
        if (*count == want) return 0;
        *count = want;
        *now_count = want;
        return want == full ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool MedigunAllowedNow() {
    ULONGLONG now = GetTickCount64();
    return g_last_answer && now - g_last_answer < SERVER_TIMEOUT && (g_last_answer_flags & ANSWER_MEDIGUN);
}

void MedigunListFor(bool allowed) {
    uint64_t n = 0;
    int r = MedigunListGuarded(allowed, &n);
    if (r) Log("weapon list: %llu weapon(s) (MEDIGUN %s, LOADOUTS %s)", n,
               allowed ? "allowed" : "not allowed", LoadoutName(LoadoutMode()));
}

// --- LOADOUTS -----------------------------------------------------------------------
// CLASSIC: only the four stock guns, Pulsar, Nova, Comet, Meteor (assault, blaster, scout,
// rocket); NEW: only the newer ones (Protostar, Quark, Blazar, Starburst, and MEDIGUN where
// the server allows it). Tac mods and ordnances are never limited.
//  * a client shows only the allowed guns at the loadout terminal (MedigunListGuarded);
//  * every machine changes a gun a player's equipment is set to that the rule does not allow
//    (CLASSIC: to PULSAR, NEW: to PROTOSTAR), at the equipment component's own slot setters
//    (all three take (component system, component index, item symbol); the loadout terminal's
//    CR15NetPlayerSetEquipmentNode and the game's loadout code both end there);
//  * a server whose build has MEDIGUN off changes it to a COMET the same way;
//  * when the rule changes, the local player's gun is set again (swapped, or the one asked
//    for given back).
constexpr uintptr_t ORDNANCE_LIST_RVA = 0x20d3428;
constexpr uint64_t SYM_PULSAR = 0x2fd69c8c5f615b9eull, SYM_COMET = 0xe32dc7c9da8051a4ull,
                   SYM_PROTOSTAR = 0x41d2ca3c1d2d1213ull, SYM_MEDIGUN = 0x2fda8a9b57734284ull;
const uint64_t kStockGuns[4] = {SYM_PULSAR, 0x2fd5839e4d605298ull, SYM_COMET, 0x41d2d5321928020aull};  // assault blaster scout rocket
const uint64_t kNewGuns[5] = {SYM_PROTOSTAR, 0xc8e8d0b1a883efedull, 0xe32dc7d9d18e4dbeull,
                              0xe32dc7c8d08948b5ull, SYM_MEDIGUN};                                     // magnum smg chain rifle medigun

//: CONFETTI GUN, BUBBLE GUN, MONEY GUN (fix_party_guns.py; PowerBoost.dll holds them as the Pulsar)
const uint64_t kPartyGuns[3] = {0x58bf8464917ddffeull, 0xcf9247c008baa680ull, 0x1b0ee501760c0660ull};

bool InList(const uint64_t *list, int n, uint64_t item) {
    for (int k = 0; k < n; ++k)
        if (list[k] == item) return true;
    return false;
}

// The gun to use instead of `item` in slot `slot` (0 weapon, 1 tac mod, 2 ordnance), or 0 when
// it may stay.
// newer tac mods (smoke buff point) / ordnances (loc dot drone), swapped under CLASSIC
const uint64_t kNewTacModSyms[3] = {0xe32dc7c9d4804fb5ull, 0xc8c33e48386618b6ull, 0xe32dc7cad6864aa4ull};
const uint64_t kNewOrdnances[3] = {0xc8e8d0b1a89cede9ull, 0xc8e8d0b1a894edfeull, 0xe32dc7decb804ab5ull};
constexpr uint64_t SYM_REPAIR_MATRIX = 0xc8c33e4832761fbcull, SYM_DETONATOR = 0xc8e8d0b1a894e7feull;

uint64_t ClassicSwap(int slot, uint64_t item, const char **what) {
    if (!item) return 0;
    if (slot != 0) {
        if (LoadoutMode() != LOADOUT_CLASSIC) return 0;
        if (slot == 1 && InList(kNewTacModSyms, 3, item)) { if (what) *what = "LOADOUTS CLASSIC -> REPAIR MATRIX"; return SYM_REPAIR_MATRIX; }
        if (slot == 2 && InList(kNewOrdnances, 3, item)) { if (what) *what = "LOADOUTS CLASSIC -> DETONATOR"; return SYM_DETONATOR; }
        return 0;
    }
    if (InList(kPartyGuns, 3, item)) return 0;                          // party guns: always allowed
    if (!InList(kStockGuns, 4, item) && !InList(kNewGuns, 5, item)) {   // not a gun (an ordnance picked as one)
        if (what) *what = "not a gun -> PULSAR";
        return SYM_PULSAR;
    }
    uint64_t want = item;
    if (want == SYM_MEDIGUN && g_server && !g_medigun_on_server) {
        want = SYM_COMET;
        if (what) *what = "MEDIGUN off on this server -> COMET";
    }
    uint32_t mode = LoadoutMode();
    if (mode == LOADOUT_CLASSIC && InList(kNewGuns, 5, want)) {
        want = SYM_PULSAR;
        if (what) *what = "LOADOUTS CLASSIC -> PULSAR";
    } else if (mode == LOADOUT_NEW && InList(kStockGuns, 4, want)) {
        want = SYM_PROTOSTAR;
        if (what) *what = "LOADOUTS NEW -> PROTOSTAR";
    }
    return want != item ? want : 0;
}

// The player equipment component system (CR15NetPlayerEquipmentCS): entries of 58h bytes at
// [cs+F8h], found through the u16 map [cs+C8h][index * 4]; slots +18h weapon, +20h tac mod,
// +28h ordnance; [cs+130h] = this machine's own player's index (FFFFh: none). The local
// player's entry is filled from the client profile by its init (+0xCF5E60, which writes the
// slots itself and publishes them as the profile's weapon / grenade / ability); every later
// change goes through a slot's setter (component index, item), which also replicates it. A
// player owns their equipment, so each client keeps its own player classic.
using setslot_fn = void(__fastcall *)(uint64_t, uint32_t, uint64_t);
constexpr uintptr_t kSetSlotRva[3] = {0xD498E0, 0xD43F00, 0xD45990};   // weapon +18h, tac mod +20h, ordnance +28h
constexpr uint32_t kSlotOffset[3] = {0x18, 0x20, 0x28};
const uint8_t kSetSlotHead[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
                                0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00};
using equipinit_fn = uint64_t(__fastcall *)(uint64_t, uint64_t, uint64_t, uint64_t *, uint32_t);
constexpr uintptr_t EQUIP_INIT_RVA = 0xCF5E60;
const uint8_t kEquipInitHead[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                  0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x58};
setslot_fn g_real_slot[3];
equipinit_fn g_real_equip_init;
volatile uint64_t g_equip_cs;                   // the system the local player's entry is in
uint64_t g_asked[3];                            // a newer item swapped out per slot (given back when off)
volatile LONG g_classic_applied = -1;           // the rule the local player's gun was last set for (-1: none yet)

// The local player's index and entry, or false.
bool LocalEntry(uint64_t cs, uint32_t *index, uint8_t **entry) {
    if (!cs) return false;
    uint32_t idx = *(volatile uint32_t *)(cs + 0x130) & 0xffff;
    if (idx == 0xffff) return false;
    uint8_t *map = *(uint8_t **)(cs + 0xC8), *entries = *(uint8_t **)(cs + 0xF8);
    if (!map || !entries) return false;
    *index = idx;
    *entry = entries + (uint64_t)*(uint16_t *)(map + idx * 4) * 0x58;
    return true;
}

// The local player's gun set for the rule: one it does not allow swapped (the one asked for
// kept, to give back), or the kept one given back once allowed. On the game's thread.
void EnforceLocal(const char *why) {
    uint64_t cs = g_equip_cs;
    __try {
        uint32_t idx;
        uint8_t *entry;
        if (!LocalEntry(cs, &idx, &entry)) return;
        for (int k = 0; k < 3; ++k) {
            uint64_t cur = *(uint64_t *)(entry + kSlotOffset[k]);
            uint64_t asked = g_asked[k] ? g_asked[k] : cur;
            const char *what = "given back";
            uint64_t swap = ClassicSwap(k, asked, &what);
            uint64_t target = swap ? swap : asked;
            g_asked[k] = swap ? asked : 0;
            if (target == cur) continue;
            g_real_slot[k](cs, idx, target);
            Log("LOADOUTS %s (%s): slot %d %016llx -> %016llx (%s)", LoadoutName(LoadoutMode()), why, k, cur, target, what);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("LOADOUTS (%s): the equipment system is gone", why);
        g_equip_cs = 0;
    }
}

template <int I> void __fastcall HookedSlot(uint64_t sys, uint32_t idx, uint64_t item) {
    const char *what = nullptr;
    uint64_t swap = ClassicSwap(I, item, &what);
    if (sys == g_equip_cs && (idx & 0xffff) == (*(volatile uint32_t *)(sys + 0x130) & 0xffff))
        g_asked[I] = swap ? item : 0;              // the local player's own pick
    if (swap) Log("slot %d of component %u set to %016llx: %s", I, idx & 0xffff, item, what);
    g_real_slot[I](sys, idx, swap ? swap : item);
}
const setslot_fn kHookedSlot[3] = {HookedSlot<0>, HookedSlot<1>, HookedSlot<2>};

uint64_t __fastcall HookedEquipInit(uint64_t cs, uint64_t entry, uint64_t data, uint64_t *ref, uint32_t index) {
    uint64_t r = g_real_equip_init(cs, entry, data, ref, index);
    uint32_t idx;
    uint8_t *e;
    if (LocalEntry(cs, &idx, &e) && idx == (index & 0xffff)) {     // this machine's own player
        g_equip_cs = cs;
        g_asked[0] = g_asked[1] = g_asked[2] = 0;
        Log("LOADOUTS: own equipment (component %u): %016llx %016llx %016llx", idx,
            *(uint64_t *)(e + 0x18), *(uint64_t *)(e + 0x20), *(uint64_t *)(e + 0x28));
        g_classic_applied = -1;                  // the next recvfrom (after the init) applies the rule
    }
    return r;
}

// On the game's thread (the recvfrom hook): the local player's slots when the rule changed.
// The gear lists (and the station's rows) as the server and LOADOUTS say. Run by the tick
// thread every 250 ms and, on a rule change, by the game's thread before any gear is swapped:
// the loadout bar's script hides every gun of the row and shows the one in hand only if it is
// in the row, so a swap seen against the old rows (CLASSIC's four, PROTOSTAR not in them)
// left the bar without a gun icon.
void OrdnanceListFor();
void TacModsFor(bool server_has_it);
SRWLOCK g_lists_lock = SRWLOCK_INIT;
void UpdateGearLists() {
    if (g_server) return;
    AcquireSRWLockExclusive(&g_lists_lock);
    ULONGLONG now = GetTickCount64();
    bool answered = g_last_answer && now - g_last_answer < SERVER_TIMEOUT;
    if (g_rules && !answered) {                                  // no EchoCombat server: ALL, off, 1.0x
        g_rules[CLASSIC_SHOWN] = g_rules[CHASSIS_SHOWN] = 0;
        g_rules[RATE_SHOWN] = g_rules[PAUSED_SHOWN] = 0;
        g_rules[PAYLOAD_MAP] = g_rules[PSPEED_SHOWN] = g_rules[CHECKPOINT_SHOWN] = g_rules[SKIP_SHOWN] = 0;
        g_rules[PAUSE_QUEUED_SHOWN] = 0;
        PageTexts();
    }
    TacModsFor(answered);
    MedigunListFor(MedigunAllowedNow());
    OrdnanceListFor();
    ReleaseSRWLockExclusive(&g_lists_lock);
}

void ApplyClassic() {
    LONG mode = (LONG)LoadoutMode();
    if (mode == g_classic_applied || !g_real_slot[0]) return;
    g_classic_applied = mode;
    UpdateGearLists();                               // the rows first, then the swap
    EnforceLocal("rule changed");
}

// --- SERIES-10 ONLY ------------------------------------------------------------------------
// While it is on, every player's chassis shows as rwd_chassis_body_s10_a. A cosmetic item goes
// on through CR15NetCustomizationCS::LoadAndApplyItem (+0xD0D170: system, player index, item,
// slot, flag), which loads it ("[NETGAME] Load and apply %s for user %s") and then applies it
// through ApplyItem (+0xCABF10, same arguments; an item not loaded is refused: "Item %s not
// loaded while trying to apply"). The game has the same swap built in for its own
// net_standard_chassis flag, at the top of LoadAndApplyItem. Here:
//  * LoadAndApplyItem: a chassis item (any rwd_chassis_*) becomes S10;
//  * ApplyItem called directly with another chassis goes to LoadAndApplyItem with S10 instead;
//  * each player's last chassis call is kept, so a rule change puts every chassis on again at
//    once (the game's thread, ApplyChassisRule).
using applyitem_fn = void(__fastcall *)(uint64_t, uint64_t, uint64_t, uint64_t, int);
constexpr uintptr_t APPLY_ITEM_RVA = 0xCABF10, LOAD_APPLY_RVA = 0xD0D170;
const uint8_t kApplyItemHead[] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53};
const uint8_t kLoadApplyHead[] = {0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x48, 0x20, 0x4c, 0x89, 0x40, 0x18, 0x89, 0x50, 0x10};
constexpr uint64_t SYM_CHASSIS_S10 = 0xca4cc178f3717385ull;   // rwd_chassis_body_s10_a
const uint64_t kChassisItems[] = {   // every rwd_chassis_* (sorted)
    0x0797ccc9ae3f8d8dull, 0x23af24019c1c3737ull, 0x23af2f158c163737ull, 0x23b2281692183737ull, 0x23b22e1a941c3737ull,
    0x23ba351b8d093737ull, 0x298b795a9630bfadull, 0x2a4e340310f4235full, 0x34d4e6a17c90a6d7ull, 0x34d4e6b73c9da6d7ull,
    0x390ce82cbed9a60dull, 0x3918e139a1d3a60dull, 0x5754eaea0f8b0086ull, 0x5cf204e0ebf99cf9ull, 0x6a3f9a9e4bc3c5b7ull,
    0x7abed4372e84c2f8ull, 0x9f0eb2652b802fa7ull, 0xa113896a79978117ull, 0xa73482226547ff29ull, 0xa7349338674aff29ull,
    0xb15db7348d52a0c7ull, 0xb8e9ba07e350d19full, 0xb9f8bb12f25bd19full, 0xbf10b8b392ff17c7ull, 0xc3f5d29017d91c93ull,
    0xca4cc178f3707385ull, 0xca4cc178f3717385ull, 0xd10d35fe875b4801ull, 0xdf25e978295af905ull, 0xea0c87cf0f8e47bbull,
    0xfb0080c90f8947bbull, 0xfe871fa683628137ull};
applyitem_fn g_real_apply_item, g_real_load_apply;
struct ChassisCall { uint64_t cs, player, item, slot; int flag; };
ChassisCall g_chassis_calls[64];
int g_chassis_call_count = 0;
volatile LONG g_chassis_applied = 0;

bool ChassisOn() { return g_server ? g_chassis != 0 : g_rules && g_rules[CHASSIS_SHOWN] != 0; }

bool ChassisItem(uint64_t item) {
    return std::binary_search(std::begin(kChassisItems), std::end(kChassisItems), item);
}

// A player's chassis as asked for (kept to put back when the rule goes off).
void KeepChassis(uint64_t cs, uint64_t player, uint64_t item, uint64_t slot, int flag) {
    int k = 0;
    while (k < g_chassis_call_count &&
           !(g_chassis_calls[k].cs == cs && (g_chassis_calls[k].player & 0xffff) == (player & 0xffff))) ++k;
    if (k >= (int)(sizeof(g_chassis_calls) / sizeof(g_chassis_calls[0]))) return;
    g_chassis_calls[k] = {cs, player, item, slot, flag};
    if (k == g_chassis_call_count) ++g_chassis_call_count;
}

volatile LONG g_chassis_replay = 0;              // ApplyChassisRule's own calls: not kept

void __fastcall HookedLoadApply(uint64_t cs, uint64_t player, uint64_t item, uint64_t slot, int flag) {
    if (ChassisItem(item)) {
        if (!g_chassis_replay) KeepChassis(cs, player, item, slot, flag);
        if (ChassisOn()) item = SYM_CHASSIS_S10;
    }
    g_real_load_apply(cs, player, item, slot, flag);
}

void __fastcall HookedApplyItem(uint64_t cs, uint64_t player, uint64_t item, uint64_t slot, int flag) {
    if (ChassisItem(item) && item != SYM_CHASSIS_S10 && ChassisOn() && g_real_load_apply) {
        if (!g_chassis_replay) KeepChassis(cs, player, item, slot, flag);
        g_real_load_apply(cs, player, SYM_CHASSIS_S10, slot, flag);   // S10 may not be loaded yet
        return;
    }
    g_real_apply_item(cs, player, item, slot, flag);
}

// On the game's thread: the rule changed, so every chassis seen goes on again.
void ApplyChassisRule() {
    LONG on = ChassisOn() ? 1 : 0;
    if (on == g_chassis_applied || !g_real_load_apply) return;
    g_chassis_applied = on;
    int n = g_chassis_call_count, done = 0;
    g_chassis_replay = 1;
    for (int k = 0; k < n; ++k) {
        ChassisCall c = g_chassis_calls[k];
        __try {
            g_real_load_apply(c.cs, c.player, on ? SYM_CHASSIS_S10 : c.item, c.slot, c.flag);
            ++done;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_chassis_replay = 0;
    Log("SERIES-10 ONLY %s: %d of %d chassis put on again", on ? "on" : "off", done, n);
}

bool HookApplyItem() {
    if (memcmp(g_base + APPLY_ITEM_RVA, kApplyItemHead, sizeof(kApplyItemHead)) ||
        memcmp(g_base + LOAD_APPLY_RVA, kLoadApplyHead, sizeof(kLoadApplyHead))) return false;
    g_real_apply_item = (applyitem_fn)(g_base + APPLY_ITEM_RVA);
    g_real_load_apply = (applyitem_fn)(g_base + LOAD_APPLY_RVA);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_real_apply_item, (PVOID)HookedApplyItem);
    DetourAttach(&(PVOID &)g_real_load_apply, (PVOID)HookedLoadApply);
    if (DetourTransactionCommit() != NO_ERROR) { g_real_apply_item = g_real_load_apply = nullptr; return false; }
    return true;
}

bool HookClassicSlots() {
    for (uintptr_t rva : kSetSlotRva)
        if (memcmp(g_base + rva, kSetSlotHead, sizeof(kSetSlotHead))) return false;
    if (memcmp(g_base + EQUIP_INIT_RVA, kEquipInitHead, sizeof(kEquipInitHead))) return false;
    for (int i = 0; i < 3; ++i) g_real_slot[i] = (setslot_fn)(g_base + kSetSlotRva[i]);
    g_real_equip_init = (equipinit_fn)(g_base + EQUIP_INIT_RVA);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (int i = 0; i < 3; ++i) DetourAttach(&(PVOID &)g_real_slot[i], (PVOID)kHookedSlot[i]);
    DetourAttach(&(PVOID &)g_real_equip_init, (PVOID)HookedEquipInit);
    if (DetourTransactionCommit() != NO_ERROR) {
        for (auto &f : g_real_slot) f = nullptr;
        g_real_equip_init = nullptr;
        return false;
    }
    return true;
}

// The ordnance list as the station shows it: the stock four under LOADOUTS CLASSIC, whole otherwise.
uint8_t *g_ord_entries;
uint64_t g_ord_full;
void OrdnanceListFor() {
    __try {
        uint8_t *list = *(uint8_t *volatile *)(g_base + ORDNANCE_LIST_RVA);
        if (!list) return;
        uint8_t *entries = *(uint8_t *volatile *)(list + 0x08);
        volatile uint64_t *count = (volatile uint64_t *)(list + 0x10);
        uint64_t capacity = *(volatile uint64_t *)(list + 0x18);
        if (!entries) return;
        if (entries != g_ord_entries) {
            g_ord_entries = entries;
            g_ord_full = 0;
        }
        if (*count > g_ord_full) g_ord_full = *count;
        if (g_ord_full > capacity) return;
        uint64_t want = LoadoutMode() == LOADOUT_CLASSIC && g_ord_full > CLASSIC_ITEMS ? CLASSIC_ITEMS : g_ord_full;
        if (*count != want) *count = want;
        StationView(list, entries, want, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TacModsFor(bool server_has_it) {
    uint64_t n = 0;
    int r = TacModsGuarded(server_has_it, &n);
    if (r == 1) Log("the server has EchoCombat: UMBRAL VEIL, OVERCHARGE and POINT shown (%llu tac mods)", n);
    if (r == 2) Log("the server has no EchoCombat: UMBRAL VEIL, OVERCHARGE and POINT hidden (%llu tac mods)", n);
}

// Watches this machine's veil uses (client), says hello (client), shows / hides the new
// tac mods (client) and logs what the barrier walls decided (both).
void HookSelectionLogs();

// SawLog (in the platform module's logger hook) hands the selected tac mod to Tick here,
// so the hook itself never logs; Tick writes the "selected:" line. One producer, one consumer.
volatile LONG64 g_selected_pending = 0;

DWORD WINAPI Tick(LPVOID) {
    volatile LONG *uses = (volatile LONG *)(g_base + USE_COUNTER_RVA);
    volatile LONG *point_uses = (volatile LONG *)(g_base + POINT_COUNTER_RVA);
    volatile LONG *loc_uses = (volatile LONG *)(g_base + LOC_COUNTER_RVA);
    volatile LONG *oc_uses = (volatile LONG *)(g_base + OC_COUNTER_RVA);
    LONG seen_uses = *uses, seen_veils = g_shared->veils, seen_barriers = g_shared->barriers;
    LONG seen_point_uses = *point_uses, seen_points = g_point->veils;
    LONG seen_loc_uses = *loc_uses, seen_locs = g_loc->veils;
    LONG seen_oc_uses = *oc_uses, seen_ocs = g_oc->veils;
    volatile LONG *point_fired = (volatile LONG *)(g_base + POINT_FIRED_RVA);
    LONG seen_fired = *point_fired;
    volatile uint64_t *equip = (volatile uint64_t *)(g_base + EQUIP_RVA);
    volatile LONG *skips = (volatile LONG *)(g_base + EQUIP_RVA + 0x10);
    volatile LONG *reruns = (volatile LONG *)(g_base + EQUIP_RVA + 0x20);
    uint64_t seen_queried = equip[3], seen_writer = equip[1];
    LONG seen_skips = *skips, seen_reruns = *reruns;
    sockaddr_in logged_server = {};
    ULONGLONG next_hello = 0, next_pick = 0, next_hook = 0, next_tacmods = 0;
    uint8_t seen_classic_requests = g_rules ? g_rules[CLASSIC_REQUESTS] : 0;
    bool f8_was = false;
    while (!g_stop) {
        Sleep(10);                                       // (5 ms: 200 wakeups a second, for checks that need far fewer)
        if (g_server && g_end_until) EndMatchStep();     // server: F8 END MATCH
#ifdef ECHOCOMBAT_LOG
        if (g_server) TraceCapture();
#endif
        InstantCapture();                                // server: RULES CAPTURE TIME 0
        if (GetTickCount64() >= next_hook) {             // client: tac mod selections; server: session type
            next_hook = GetTickCount64() + 1000;
            HookSelectionLogs();
        }
        if (!g_server) {
            for (int k = 0; k < 4; ++k) {
                uint8_t type = "VPTO"[k];
                LONG u = k == 3 ? *oc_uses : k == 2 ? *loc_uses : k ? *point_uses : *uses;
                LONG &seen = k == 3 ? seen_oc_uses : k == 2 ? seen_loc_uses : k ? seen_point_uses : seen_uses;
                if (u == seen) continue;
                seen = u;
                uint32_t id = GetTickCount() * 2654435761u ^ GetCurrentProcessId() << 16 ^ (uint32_t)u ^ k;
                FirstTime(id);
                if (type != 'T') AddToken(Tokens(type));     // a teleport is shown here already
                Dest d;
                bool ok = ServerDest(d);
                SendToServer(type, id);
                SendToServer(type, id);                          // once more, against loss
                if (ok)
                    Log("%s used here: token here, %08x sent to the server", What(type), id);
                else
                    LogWarning("%s used here: token here (%08x not sent: no game server connection seen yet)",
                        What(type), id);
            }
            ULONGLONG now = GetTickCount64();
            if (now >= next_pick) {
                next_pick = now + 1000;
                PickServer();
            }
            if (now >= next_tacmods) {
                next_tacmods = now + 250;
                UpdateGearLists();
            }
            if (g_rules && g_rules[CLASSIC_REQUESTS] != seen_classic_requests) {   // the podium asks
                seen_classic_requests = g_rules[CLASSIC_REQUESTS];
                uint8_t asked = g_rules[CLASSIC_WANT];
                bool payload = g_rules[PAYLOAD_MAP] != 0;
                uint32_t want = asked & WANT_CHASSIS ? WANT_CHASSIS | (asked & 1)
                              : asked & WANT_RATE    ? WANT_RATE | RateStep(g_rules[payload ? PSPEED_SHOWN : RATE_SHOWN], asked & 1) << 8
                              : asked & WANT_PAUSE   ? WANT_PAUSE | (asked & 1)
                              : asked <= LOADOUT_NEW ? asked : LOADOUT_ALL;
                if ((asked & 0xF0) == WANT_CHECKPOINT) continue;               // row 4 (CHECKPOINT USED) was dropped
                SendToServer('C', want);
                SendToServer('C', want);                                     // once more, against loss
                if (want & WANT_PAUSE) Log("PAUSE %s asked of the server", want & 1 ? "on" : "off");
                else if (want & WANT_RATE) Log("RULES page 2: POINT RATE %d/4 asked of the server", (int)(want >> 8));
                else if (want & WANT_CHASSIS) Log("RULES page 2: SERIES-10 ONLY %s asked of the server", want & 1 ? "on" : "off");
                else Log("RULES page 2: LOADOUTS %s asked of the server", LoadoutName(want));
            }
            bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (f8 && !f8_was) {
                DWORD pid = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &pid);
                if (pid == GetCurrentProcessId()) {
                    SendToServer('C', WANT_END);
                    SendToServer('C', WANT_END);                             // once more, against loss
                    Log("END MATCH asked of the server (F8)");
                }
            }
            f8_was = f8;
            if (now >= next_hello) {
                next_hello = now + 2000;
                SendToServer('H', 0);
                Dest d;
                if (ServerDest(d) && !Same(d.addr, logged_server)) {
                    logged_server = d.addr;
                    char buf[48];
                    Log("server %s (the game's own connection)", Addr(d.addr, buf, sizeof(buf)));
                }
            }
        }
        LONG64 selected = InterlockedExchange64(&g_selected_pending, 0);
        if (selected) Log("selected: %s", TacMod((uint64_t)selected));
        LONG v = g_shared->veils, b = g_shared->barriers;
        if (v != seen_veils) { seen_veils = v; Log("a barrier wall took a token: UMBRAL VEIL (%ld so far)", v); }
        if (b != seen_barriers) { seen_barriers = b; Log("a barrier wall found no token: energy barrier (%ld so far)", b); }
        LONG pt = g_point->veils;
        if (pt != seen_points) { seen_points = pt; Log("a barrier wall took a point token: POINT (%ld so far)", pt); }
        LONG pf = *point_fired;
        if (pf != seen_fired) { seen_fired = pf; Log("POINT went off here: stun explosion activated (%ld so far)", pf); }
        LONG lc = g_loc->veils;
        if (lc != seen_locs) { seen_locs = lc; Log("a translocator puck took a teleport token: effect and sound shown (%ld so far)", lc); }
        uint64_t eq = equip[0], qd = equip[3], wr = equip[1];
        if (qd != seen_queried || wr != seen_writer) {
            Log("controller enabled for: %s (controller %016llx%s; selected: %s)", TacMod(qd), wr,
                seen_writer && wr != seen_writer ? ", A DIFFERENT CONTROLLER" : "", TacMod(eq));
            seen_queried = qd;
            seen_writer = wr;
        }
        LONG rr = *reruns;
        if (rr != seen_reruns) {
            if (rr) Log("controller chain ended on %s, selected %s: re-run %ld", TacMod(qd), TacMod(eq), rr);
            seen_reruns = rr;
        }
        LONG sk = *skips;
        if (sk != seen_skips) {
            seen_skips = sk;
            Log("a tac mod that is not the selected one (%s) was used: skipped (%ld so far)", TacMod(eq), sk);
        }
        LONG oc = g_oc->veils;
        if (oc != seen_ocs) { seen_ocs = oc; Log("a heal bubble took an overcharge token: shown purple (%ld so far)", oc); }
    }
    return 0;
}

// The tac mod the player selected, as the client sends it ("[NSUSER] sent profile
// update: {"ability":"point"}"): stored at EQUIP_RVA for tools/equip_guard.py's checks
// and the controller's re-run. Read from the game's log the moment it is picked. The
// profile code is in the platform module (pnsovr.dll for Oculus, pnsrad.dll for the
// others), each with its own copy of the logger: in each, the logger is found from the
// code that logs that line (lea r8, <format>; ...; call WriteLog; WriteLog calls
// PrintfImpl(level, flags, fmt, va_list)) and hooked. echovr.exe has a copy too, but its
// logger is nEVR's: nEVR hooks it itself, and a second hook there breaks nEVR's log and
// stops a server (its hook guard), so that one is left alone. The hook does no logging (it
// runs inside the platform module's own logger); Tick writes the "selected:" line.
using printf_fn = void(*)(int, uint64_t, const char *, va_list);
constexpr char kProfileFmt[] = "[NSUSER] sent profile update: %s";
const wchar_t *const kLogModules[] = {L"pnsovr.dll", L"pnsrad.dll"};
constexpr int kLogHooks = 2;
printf_fn g_real_log[kLogHooks];
bool g_log_tried[kLogHooks];

uint64_t AbilitySymbol(const char *name, size_t n) {
    static const struct { const char *name; uint64_t sym; } k[] = {
        {"smoke", 0xe32dc7c9d4804fb5ull}, {"point", 0xe32dc7cad6864aa4ull}, {"buff", 0xc8c33e48386618b6ull},
        {"heal", 0xc8c33e4832761fbcull}, {"shield", 0x41d2d43513260b1aull}, {"sensor", 0x41d2d4381430080cull},
        {"wraith", 0x41d2d02f1b2a1316ull}};
    for (const auto &e : k)
        if (strlen(e.name) == n && memcmp(e.name, name, n) == 0) return e.sym;
    return 0;
}

constexpr char kSessionFmt[] = "[NETLOBBY] Starting session (gametype ";
void SawLog(const char *fmt, va_list args) {
    if (fmt && g_server && !strncmp(fmt, kSessionFmt, sizeof(kSessionFmt) - 1)) {
        va_list copy = args;
        const char *gametype = va_arg(copy, const char *);
        __try {
            g_private_match = gametype && strstr(gametype, "private") ? 1 : 0;
            Log("session gametype %s: %s match", gametype ? gametype : "?", g_private_match ? "private" : "public");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_private_match = -1;
        }
        return;
    }
    if (!fmt || strcmp(fmt, kProfileFmt) != 0) return;
    va_list copy = args;
    const char *json = va_arg(copy, const char *);
    const char *a = json ? strstr(json, "\"ability\":\"") : nullptr;
    if (!a) return;
    a += 11;
    const char *e = strchr(a, '"');
    uint64_t sym = e ? AbilitySymbol(a, e - a) : 0;
    if (!sym) return;
    *(volatile LONG *)(g_base + EQUIP_RVA + 0x20) = 0;                // a new selection: re-runs allowed
    *(volatile uint64_t *)(g_base + EQUIP_RVA) = sym;
    InterlockedExchange64(&g_selected_pending, (LONG64)sym);         // Tick logs it
}

template <int I> void HookedLog(int level, uint64_t flags, const char *fmt, va_list args) {
    SawLog(fmt, args);
    g_real_log[I](level, flags, fmt, args);
}
const printf_fn kHookedLog[kLogHooks] = {HookedLog<0>, HookedLog<1>};

// The PrintfImpl the module's profile update goes through, or nullptr.
printf_fn FindProfileLogger(uint8_t *m) {
    auto *dos = (IMAGE_DOS_HEADER *)m;
    auto *nt = (IMAGE_NT_HEADERS64 *)(m + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;
    const uint8_t *str = nullptr;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !str; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uint8_t *p = m + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t o = 0; o + sizeof(kProfileFmt) <= n; ++o)
            if (p[o] == '[' && memcmp(p + o, kProfileFmt, sizeof(kProfileFmt)) == 0) { str = p + o; break; }
    }
    if (!str) return nullptr;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t *p = m + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t o = 0; o + 7 <= n; ++o) {
            if (p[o] != 0x4c || p[o + 1] != 0x8d || p[o + 2] != 0x05) continue;      // lea r8, [rip+x]
            if (p + o + 7 + *(int32_t *)(p + o + 3) != str) continue;
            for (size_t c = o + 7; c < o + 7 + 24 && c + 5 <= n; ++c) {               // the call after it
                if (p[c] != 0xe8) continue;
                uint8_t *wl = p + c + 5 + *(int32_t *)(p + c + 1);
                // WriteLog: mov [rsp+18h], r8; mov [rsp+20h], r9; sub rsp, 28h; lea r9, [rsp+48h]; call PrintfImpl
                static const uint8_t head[] = {0x4c, 0x89, 0x44, 0x24, 0x18, 0x4c, 0x89, 0x4c, 0x24, 0x20,
                                               0x48, 0x83, 0xec, 0x28, 0x4c, 0x8d, 0x4c, 0x24, 0x48, 0xe8};
                if (wl < m || wl + sizeof(head) + 4 > m + size || memcmp(wl, head, sizeof(head)) != 0) return nullptr;
                return (printf_fn)(wl + sizeof(head) + 4 + *(int32_t *)(wl + sizeof(head)));
            }
            return nullptr;
        }
    }
    return nullptr;
}

// Hooks each module's logger once it is loaded (the platform module can load late).
void HookSelectionLogs() {
    for (int i = 0; i < kLogHooks; ++i) {
        if (g_log_tried[i]) continue;
        HMODULE m = GetModuleHandleW(kLogModules[i]);
        if (!m) continue;
        g_log_tried[i] = true;
        printf_fn f = FindProfileLogger((uint8_t *)m);
        if (!f) continue;                                                  // no profile code in it
        g_real_log[i] = f;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_real_log[i], (PVOID)kHookedLog[i]);
        LONG rc = DetourTransactionCommit();
        if (rc != NO_ERROR) g_real_log[i] = nullptr;
        if (rc == NO_ERROR)
            Log("watching tac mod selections in %ls", kLogModules[i]);
        else
            LogWarning("hooking the logger in %ls failed", kLogModules[i]);
    }
}

// --- RULES: capture time -------------------------------------------------------------------
// A private Combat match's RULES page has CAPTURE TIME (its old seconds row). Combat's
// game-mode script keeps it in spare bytes past echovr.exe's .data, beside the RULES
// starting scores (tools/fix_capture_points.py: the RULES mailbox, the page end - 140h, +0Ch;
// a float, 0 = unset).
// A capture volume's progress grows by the frame time until it reaches the volume's time
// to capture (+48h, the map's capture_point.time_to_capture); clients show the server's
// replicated progress against their own +48h. So the server makes progress grow by
//   frame time * (the volume's time to capture / the RULES capture time)
// and every client's bar fills in the RULES time with nothing changed on the client.
// CR15NetCaptureVolumeCS's tick loads the frame time once, before its volume loop:
//   +0xD649C6  movss xmm6, [rax+3Ch]        the frame time (xmm6: every progress change)
//   +0xD649CB  xor r15d, r15d
//   +0xD649CE  movzx eax, r15w
//   +0xD649D2  mov [rbp+130h], ax
// (r12 = the component: its volumes at +100h, 50h bytes each.)
constexpr uint32_t CAPTURE_DT_RVA = 0xD649C6, CAPTURE_SLOT = 0xC;
const uint8_t kCaptureDt[19] = {0xf3, 0x0f, 0x10, 0x70, 0x3c, 0x45, 0x33, 0xff, 0x41, 0x0f, 0xb7, 0xc7,
                                0x66, 0x89, 0x85, 0x30, 0x01, 0x00, 0x00};

uint32_t RulesMailboxRva(const uint8_t *base) {
    auto nt = base + *(const uint32_t *)(base + 0x3c);
    uint16_t count = *(const uint16_t *)(nt + 6), opt = *(const uint16_t *)(nt + 20);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *s = nt + 24 + opt + i * 40;
        if (memcmp(s, ".data\0\0\0", 8)) continue;
        uint32_t vsize = *(const uint32_t *)(s + 8), va = *(const uint32_t *)(s + 12);
        uint32_t page_end = (va + vsize + 0xfff) & ~0xfffu;
        return page_end - (va + vsize) < RULES_BELOW_PAGE_END ? 0 : page_end - RULES_BELOW_PAGE_END;
    }
    return 0;
}

uint8_t *g_capture_patched = nullptr;             // nEVR unload: the site to restore (kCaptureDt)
uint8_t *g_payload_patched = nullptr;             // ... (kPayloadSpeedSite)

void InstallCaptureTime() {
    uint8_t *site = g_base + CAPTURE_DT_RVA;
    if (memcmp(site, kCaptureDt, sizeof(kCaptureDt))) { Log("capture time: unexpected echovr.exe build, not patched"); return; }
    uint32_t mb = RulesMailboxRva(g_base);
    if (!mb) { Log("capture time: no spare bytes after .data, not patched"); return; }
    g_rules_total = (float *)(g_base + mb + 8);          // T, synced to the clients
    auto stub = (uint8_t *)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return;
    uint8_t code[] = {
        0x50,                                           // push rax
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rax, &g_capture_cs
        0x4c, 0x89, 0x20,                               // mov [rax], r12          the CS (PAUSE resets)
        0x48, 0xff, 0x40, 0x08,                         // inc qword [rax+8]       its tick count
        0x58,                                           // pop rax
        0xf3, 0x0f, 0x10, 0x70, 0x3c,                   // movss xmm6, [rax+3Ch]   the frame time
        0x50,                                           // push rax
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rax, &capture time
        0x80, 0x78, PAUSED_SHOWN - CAPTURE_SLOT, 0x00,  // cmp byte [rax + (paused - capture)], 0
        0x74, 0x05,                                     // je running
        0x0f, 0x57, 0xf6,                               // xorps xmm6, xmm6        PAUSE: no progress
        0xeb, 0x1d,                                     // jmp unset
        0x83, 0x38, 0x00,                               // running: cmp dword [rax], 0
        0x7e, 0x18,                                     // jle unset
        0x51,                                           // push rcx
        0x49, 0x8b, 0x8c, 0x24, 0x00, 0x01, 0x00, 0x00, // mov rcx, [r12+100h]     the volumes
        0x48, 0x85, 0xc9,                               // test rcx, rcx
        0x74, 0x09,                                     // jz none
        0xf3, 0x0f, 0x59, 0x71, 0x48,                   // mulss xmm6, [rcx+48h]   * the map's time
        0xf3, 0x0f, 0x5e, 0x30,                         // divss xmm6, [rax]       / the RULES time
        0x59,                                           // none: pop rcx
        0x58,                                           // unset: pop rax
        0x45, 0x33, 0xff,                               // xor r15d, r15d
        0x41, 0x0f, 0xb7, 0xc7,                         // movzx eax, r15w
        0x66, 0x89, 0x85, 0x30, 0x01, 0x00, 0x00,       // mov [rbp+130h], ax
        0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // jmp [rip+0] -> after the site
    };
    g_capture_time = (float *)(g_base + mb + CAPTURE_SLOT);
    uint64_t at = (uint64_t)g_capture_time, back = (uint64_t)(site + sizeof(kCaptureDt));
    uint64_t cs_at = (uint64_t)&g_capture_tick;
    memcpy(code + 3, &cs_at, 8);
    memcpy(code + 19 + 8, &at, 8);                      // after the 19-byte CS prefix
    memcpy(code + sizeof(code) - 8, &back, 8);
    memcpy(stub, code, sizeof(code));
    uint8_t jump[19] = {0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    uint64_t to = (uint64_t)stub;                       // jmp [rip+0] -> stub
    memcpy(jump + 6, &to, 8);
    DWORD old;
    if (!VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site, jump, sizeof(jump));
    VirtualProtect(site, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    g_capture_patched = site;
    Log("capture time: progress follows the RULES page's CAPTURE TIME (mailbox %#x)", mb + CAPTURE_SLOT);
}

// --- PAYLOAD SPEED ---------------------------------------------------------------------------
// CR15NetPayloadCS::UpdatePreTrackMover (+0xD84FF0) works out each payload's speed every frame
// (attackers / defenders on it, the per-count multipliers, accel) into xmm3 and hands it to the
// payload's track mover, which moves it along the track:
//   +0xD858ED  movss [rax+rdx*8], xmm3     the track mover's speed
//   +0xD858F2  mov r9d, [r14+30h]          r14 = the payload's data (58h bytes): +18 speed, +30 flags
// Flags (+30): 1 paused (the script's checkpoint buffer, R15NetPayloadSetPaused), 2 autodrive,
// 8 locked (before the attackers can start, R15NetPayloadSetLocked), 20h changed.
// A stub there calls PayloadSpeed(payload data, mover entry, speed) for the speed to use:
//   * PAYLOAD SPEED: the speed x quarters / 4 (only what the mover is given; the payload's own
//     speed, accel and claims stay the game's);
//   * PAUSE: 0 while paused / resuming (autodrive too);
//   * the server's bookkeeping for the payload PAUSE rule: moved this round (forward speed),
//     checkpoint holds (PAUSED rising edge), round start (AUTODRIVE) / end (LOCKED);
//   * that it ran at all says "payload map" (the server tells the clients).
constexpr uint32_t PAYLOAD_SPEED_RVA = 0xD858ED;
const uint8_t kPayloadSpeedSite[9] = {0xf3, 0x0f, 0x11, 0x1c, 0xd0, 0x45, 0x8b, 0x4e, 0x30};
extern "C" float __fastcall PayloadSpeed(uint8_t *payload, uint8_t *mover, float speed) {
    g_payload_seen = GetTickCount64();
    if (!g_rules) return speed;
    uint32_t flags = *(volatile uint32_t *)(payload + 0x30);
    bool paused_rule = g_rules[PAUSED_SHOWN] != PAUSE_OFF;
    if (g_server) {
        uint32_t was = g_payload_flags;
        g_payload_flags = flags;
        if ((flags & 1) && !(was & 1)) CheckpointHit("held", flags);             // the script's checkpoint hold
        if (flags & 8) {                                                         // locked: the round is over
            g_payload_moved = 0;
        }
        // AUTODRIVE (2): the game drives the payload itself: placing it for a round, at every
        // checkpoint, and after quickfinish_checkpoint_on_start (test 6: slowly to the next
        // checkpoint, claims ignored). Only the first one after the round ended (LOCKED) or the
        // map loaded is a round's start.
        if (flags & 2) {
            g_payload_moved = 0;                                                 // at the start / a checkpoint
            if (paused_rule) return 0.0f;                                        // PAUSE holds it even then
            return speed;
        }
        if (paused_rule) return 0.0f;                                            // PAUSE: the payload stands still
        uint8_t q = (uint8_t)g_pspeed;
        float out = q && q != 4 ? speed * q / 4.0f : speed;
        if (out > 0.05f && !g_payload_moved) g_payload_moved = 1;               // forward only
        return out;
    }
    if (paused_rule) return 0.0f;                                                // PAUSE: it stands still here too
    if (flags & 2) return speed;
    uint8_t q = g_rules[PSPEED_SHOWN];
    return q && q != 4 ? speed * q / 4.0f : speed;
}

// The stub is near echovr.exe so a 5-byte jmp reaches it.
uint8_t *AllocNear(uint8_t *base, size_t size) {
    for (uintptr_t at = ((uintptr_t)base - 0x10000) & ~(uintptr_t)0xFFFF; at > (uintptr_t)base - 0x70000000; at -= 0x10000)
        if (auto p = (uint8_t *)VirtualAlloc((void *)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return p;
    return nullptr;
}

void InstallPayloadSpeed() {
    uint8_t *site = g_base + PAYLOAD_SPEED_RVA;
    if (memcmp(site, kPayloadSpeedSite, sizeof(kPayloadSpeedSite))) { Log("payload speed: unexpected echovr.exe build, not hooked"); return; }
    uint8_t *stub = AllocNear(g_base, 256);
    if (!stub) { Log("payload speed: no memory near echovr.exe, not hooked"); return; }
    uint8_t code[] = {
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,   // push rax rcx rdx r8 r9 r10 r11
        0x48, 0x83, 0xec, 0x78,                                             // sub rsp, 78h (aligned for the call)
        0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x20,                                 // movdqu [rsp+20h], xmm0
        0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x30,                                 // movdqu [rsp+30h], xmm1
        0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x40,                                 // movdqu [rsp+40h], xmm2
        0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x50,                                 // movdqu [rsp+50h], xmm4
        0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x60,                                 // movdqu [rsp+60h], xmm5
        0x48, 0x8d, 0x14, 0xd0,                                             // lea rdx, [rax+rdx*8]   the mover entry
        0x4c, 0x89, 0xf1,                                                   // mov rcx, r14           the payload
        0x0f, 0x28, 0xd3,                                                   // movaps xmm2, xmm3      the speed
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,                                 // mov rax, PayloadSpeed
        0xff, 0xd0,                                                         // call rax
        0x0f, 0x28, 0xd8,                                                   // movaps xmm3, xmm0
        0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20,                                 // restore xmm0
        0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x30,                                 // xmm1
        0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x40,                                 // xmm2
        0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x50,                                 // xmm4
        0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x60,                                 // xmm5
        0x48, 0x83, 0xc4, 0x78,                                             // add rsp, 78h
        0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58,   // pop r11 r10 r9 r8 rdx rcx rax
        0xf3, 0x0f, 0x11, 0x1c, 0xd0,                                       // movss [rax+rdx*8], xmm3 (the site's own)
        0x45, 0x8b, 0x4e, 0x30,                                             // mov r9d, [r14+30h]
        0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                     // jmp [rip+0] -> after the site
    };
    uint64_t fn = (uint64_t)&PayloadSpeed, back = (uint64_t)(site + sizeof(kPayloadSpeedSite));
    size_t at_fn = 0;
    while (!(code[at_fn] == 0x48 && code[at_fn + 1] == 0xb8)) ++at_fn;
    memcpy(code + at_fn + 2, &fn, 8);
    memcpy(code + sizeof(code) - 8, &back, 8);
    memcpy(stub, code, sizeof(code));
    int64_t rel = (int64_t)stub - (int64_t)(site + 5);
    if (rel != (int32_t)rel) { Log("payload speed: stub out of reach, not hooked"); return; }
    uint8_t jump[9] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t r32 = (int32_t)rel;
    memcpy(jump + 1, &r32, 4);
    DWORD old;
    if (!VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site, jump, sizeof(jump));
    VirtualProtect(site, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    g_payload_patched = site;
    Log("payload speed: hooked (PAYLOAD SPEED, payload PAUSE)");
}

// On the game's thread: the server tells the clients when the payload map / driving state changes.
void ApplyPayload() {
    if (!g_server || !g_rules) return;
    LONG pm = g_payload_seen && GetTickCount64() - g_payload_seen < 1000 ? 1 : 0;
    static LONG sent_pm = 0, sent_skip = 0;
    // a queued PAUSE goes on once the payload is held (a checkpoint's buffer, or locked)
    if (g_pause_queued && (!pm || g_paused != PAUSE_OFF)) g_pause_queued = 0;
    if (g_pause_queued && g_checkpoint_hits != g_pause_queue_base) {
        g_pause_queued = 0;
        g_paused = PAUSE_ON;
        g_rules[PAUSED_SHOWN] = PAUSE_ON;
        sockaddr_in nobody = {};
        int r = Relay(nobody, 'G', RulesWord());
        Log("PAUSE on (queued; the payload is held at a checkpoint): told %d client(s)", r);
    }
    LONG sk = g_skipping;
    if (pm != sent_pm || sk != sent_skip) {
        sent_pm = pm;
        sent_skip = sk;
        g_payload_map = pm;
        g_rules[PAYLOAD_MAP] = (uint8_t)pm;
        sockaddr_in nobody = {};
        int r = Relay(nobody, 'G', RulesWord());
        Log("PAYLOAD: %s map, %s: told %d client(s)", pm ? "payload" : "no payload",
            sk ? "driving to the checkpoint" : "not driving", r);
    }
    static uint32_t logged_flags = 0xFFFFFFFF;
    uint32_t f = g_payload_flags & 0x1F;
    if (pm && f != logged_flags) {
        logged_flags = f;
        Log("PAYLOAD flags %02x:%s%s%s", f, f & 8 ? " locked" : "", f & 1 ? " paused" : "", f & 2 ? " autodrive" : "");
    }
}

bool g_hooked = false;

bool Install(bool server) {
    if (g_hooked) return true;
    g_base = (uint8_t *)GetModuleHandleW(nullptr);
    if (uint32_t mb = RulesMailboxRva(g_base)) g_rules = g_base + mb;
    RateText(g_rules ? g_rules[RATE_SHOWN] : 0);         // the POINT RATE row's text (1.0x until told)
    InstallCaptureTime();
    InstallPayloadSpeed();
    g_shared = (Shared *)(g_base + TOKEN_RVA);
    g_point = (Shared *)(g_base + POINT_TOKEN_RVA);
    g_loc = (Shared *)(g_base + LOC_TOKEN_RVA);
    g_oc = (Shared *)(g_base + OC_TOKEN_RVA);
    g_server = server;
    if (g_server) *(volatile LONG *)(g_base + EQUIP_RVA + 0x14) = 1;   // equip_guard: a client's check only
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_sendto, Hooked_sendto);
    DetourAttach(&(PVOID &)Real_recvfrom, Hooked_recvfrom);
    LONG rc = DetourTransactionCommit();
    if (rc != NO_ERROR) { LogWarning("hooking sendto / recvfrom failed (%ld): UMBRAL VEIL stays local", rc); return false; }
    g_hooked = true;
    Log(g_server ? "server: relaying UMBRAL VEIL over the game's own connection"
                 : "client: UMBRAL VEIL goes over the game's own connection");
    if (!g_server) Log(HookApplyItem() ? "SERIES-10 ONLY: LoadAndApplyItem / ApplyItem hooked"
                                       : "SERIES-10 ONLY: unexpected echovr.exe build, chassis not swapped");
    Log(HookDispatch() ? "LOADOUTS / PAUSE: event dispatch hooked"
                                      : "LOADOUTS: unexpected echovr.exe build, stations not redrawn");
    Log(HookClassicSlots() ? "LOADOUTS: equipment slot setters hooked"
                           : "LOADOUTS: unexpected echovr.exe build, equipment not enforced");
    g_stop = false;
    g_tick = CreateThread(nullptr, 0, Tick, nullptr, 0, nullptr);
    return true;
}

void Uninstall() {
    g_stop = true;
    if (g_tick) {
        WaitForSingleObject(g_tick, 2000);
        CloseHandle(g_tick);
        g_tick = nullptr;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_hooked) {
        DetourDetach(&(PVOID &)Real_sendto, Hooked_sendto);
        DetourDetach(&(PVOID &)Real_recvfrom, Hooked_recvfrom);
    }
    for (int i = 0; i < kLogHooks; ++i)
        if (g_real_log[i]) DetourDetach(&(PVOID &)g_real_log[i], (PVOID)kHookedLog[i]);
    if (g_real_dispatch) DetourDetach(&(PVOID &)g_real_dispatch, (PVOID)HookedDispatch);
    if (g_real_dce) DetourDetach(&(PVOID &)g_real_dce, (PVOID)HookedDCE);
    if (g_real_dce_inner) DetourDetach(&(PVOID &)g_real_dce_inner, (PVOID)HookedDceInner);
    if (g_real_dce_inner2) DetourDetach(&(PVOID &)g_real_dce_inner2, (PVOID)HookedDceInner2);
    if (!g_server && g_real_dispatch && g_real_setalpha) DetourDetach(&(PVOID &)g_real_setalpha, (PVOID)HookedSetAlpha);
    if (!g_server && g_real_dispatch && g_real_settext) DetourDetach(&(PVOID &)g_real_settext, (PVOID)HookedSetText);
    if (g_real_apply_item) DetourDetach(&(PVOID &)g_real_apply_item, (PVOID)HookedApplyItem);
    if (g_real_load_apply) DetourDetach(&(PVOID &)g_real_load_apply, (PVOID)HookedLoadApply);
    if (g_real_slot[0]) {
        for (int i = 0; i < 3; ++i) DetourDetach(&(PVOID &)g_real_slot[i], (PVOID)kHookedSlot[i]);
        if (g_real_equip_init) DetourDetach(&(PVOID &)g_real_equip_init, (PVOID)HookedEquipInit);
    }
    DetourTransactionCommit();
    g_real_dispatch = nullptr; g_real_dce = nullptr; g_real_dce_inner = g_real_dce_inner2 = nullptr;
    g_real_setalpha = nullptr; g_real_settext = nullptr;
    g_real_apply_item = g_real_load_apply = nullptr;
    for (auto &f : g_real_slot) f = nullptr;
    g_real_equip_init = nullptr;
    struct { uint8_t **site; const uint8_t *orig; size_t n; } patches[] = {
        { &g_capture_patched, kCaptureDt, sizeof(kCaptureDt) },
        { &g_payload_patched, kPayloadSpeedSite, sizeof(kPayloadSpeedSite) },
    };
    for (auto &pt : patches) {                         // the game's own bytes back
        if (!*pt.site) continue;
        DWORD old;
        if (VirtualProtect(*pt.site, pt.n, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(*pt.site, pt.orig, pt.n);
            VirtualProtect(*pt.site, pt.n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), *pt.site, pt.n);
        }
        *pt.site = nullptr;
    }
    g_hooked = false;
    for (int i = 0; i < kLogHooks; ++i) { g_real_log[i] = nullptr; g_log_tried[i] = false; }
    Log("unhooked");
}

// A clean exit. CR15Game's shutdown (+0x1223a0, its vslot 7, run once by Main_ as the game
// quits) logs the player out, then hands over to the engine's own teardown (+0x512590, the
// call at +0x122755). That teardown frees every resource, and one GPU particle effect's
// instance list is broken by then (+0x633643 reads through -1), so nearly every close with
// EchoCombat's data ended in an access violation and a BugSplat report. The call ends the
// process there instead (the logout and everything before it has run). The stub calls
// TerminateProcess itself and is never freed, so it outlives this DLL: nothing to undo.
constexpr uintptr_t TEARDOWN_CALL_RVA = 0x122752;
constexpr uint8_t TEARDOWN_CALL_BYTES[] = { 0x48, 0x8b, 0xcf, 0xe8, 0x36, 0xfe, 0x3e, 0x00 };   // mov rcx, rdi; call +0x512590

void InstallQuickExit() {
    static bool done;
    if (done) return;
    uint8_t *base = (uint8_t *)GetModuleHandleW(nullptr);
    uint8_t *call = base + TEARDOWN_CALL_RVA + 3;
    if (memcmp(base + TEARDOWN_CALL_RVA, TEARDOWN_CALL_BYTES, sizeof TEARDOWN_CALL_BYTES) != 0) {
        LogWarning("shutdown call not found (another build?): the game exits as stock");
        return;
    }
    uint8_t *stub = nullptr;                              // a call rel32 reaches +-2 GB
    for (uintptr_t at = ((uintptr_t)base & ~0xffffull) - 0x10000; at > (uintptr_t)base - 0x70000000; at -= 0x10000)
        if ((stub = (uint8_t *)VirtualAlloc((void *)at, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))) break;
    if (!stub) {
        LogWarning("no room for the shutdown stub: the game exits as stock");
        return;
    }
    uint8_t code[] = {
        0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff,         // mov rcx, -1 (this process)
        0x31, 0xd2,                                       // xor edx, edx (exit code 0)
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,               // mov rax, TerminateProcess
        0xff, 0xe0,                                       // jmp rax
    };
    uint64_t fn = (uint64_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess");
    if (!fn) return;
    memcpy(code + 11, &fn, 8);
    memcpy(stub, code, sizeof code);
    int32_t rel = (int32_t)(stub - (call + 5));
    DWORD old;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(call + 1, &rel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    done = true;
    Log("clean exit: the engine's teardown is skipped once the game has shut down");
}

} // namespace

// PowerBoost.dll (client): may this player's MEDIGUN work here? Only on a server that allows it.
extern "C" __declspec(dllexport) int __cdecl VeilNet_MedigunAllowed() {
    return g_server ? (g_medigun_on_server ? 1 : 0) : (MedigunAllowedNow() ? 1 : 0);
}

ECHOCOMBAT_EXPORT NvrPluginInfo NvrPluginGetInfo(void) {
    return EchoCombatPluginInfo("veil_net", "EchoCombat: UMBRAL VEIL, POINT, TRANSLOCATOR and OVERCHARGE between players");
}

ECHOCOMBAT_EXPORT uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

ECHOCOMBAT_EXPORT uint32_t NvrPluginGetCapabilities(void) {
    return NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY | NEVR_PLUGIN_CAP_NETWORK | NEVR_PLUGIN_CAP_HOOKS_ENGINE;
}

ECHOCOMBAT_EXPORT int NvrPluginInitEx(const NvrGameContext *ctx, const char *) {
    PluginLogBind(ctx);
    HMODULE self = nullptr;                              // MEDIGUN: PowerBoost.ini [server] medigun, beside the DLLs
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&NvrPluginInitEx, &self);
    char ini[MAX_PATH];
    EchoCombatBesideDll(self, ini, sizeof(ini), "PowerBoost.ini");
    g_medigun_on_server = GetPrivateProfileIntA("server", "medigun", 0, ini) != 0;
    InstallQuickExit();
    return Install(EchoCombatIsServer(ctx)) ? 0 : 1;
}

ECHOCOMBAT_EXPORT int NvrPluginInit(const NvrGameContext *ctx) { return NvrPluginInitEx(ctx, "{}"); }

ECHOCOMBAT_EXPORT void NvrPluginShutdown(void) { Uninstall(); }

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(self);
    return TRUE;
}
