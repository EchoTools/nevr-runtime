// PowerBoost: the engine side of POWER BOOST (the cut `buff` tac mod).
//
// ability_properties.buff gives a linked teammate `dr` damage reduction for
// `dr_duration` seconds. The engine has no such buff, so this plugin adds it on
// the server, where damage is decided:
//
//   +0xcecc40  HealServer(cs, index, amount, ...)          CR15NetDamageableCS: heal one damageable
//   +0xd29770  ProcessDamageServer(cs, index, damage, ...)  CR15NetDamageableCS: damage one damageable
//
//   +0xcc0670  SendDamageDealt(cs, index, amount, type, info)  client -> server
//                                         R15NetDamageableDamageDealt message
//
// The POWER BOOST script tells the plugin whom to buff through the engine's own heal
// node: a heal of a "marker" amount, 1,000,000 + dr_permille * 1000 + seconds
// (e.g. 1,020,010 = dr 0.020 for 10 s; every such value is exact in a float).
//
// That heal node runs on the client that used the tac mod, so the marker first shows
// up in the client's HealServer. The client swallows it and sends it on as a
// DamageDealt of -marker: the server's handler for that message turns any amount <= 0
// into HealServer(-amount), so the server's HealServer sees the marker too. There it
// is swallowed again and remembered: that damageable (component system + index) is
// buffed until then, and ProcessDamageServer gets damage * (1 - dr) before any shield
// or health is taken off.
//
// Phase 2, the link. POWER BOOST's bubble is a heal bubble like Repair Matrix's (its own
// component on each player, fix_buff.OC_BUBBLE), and it links to and heals every target
// within its radius:
//
//   +0xd6d1d0  CR15NetHealBubbleCS::UpdateActiveHealBubble(handle, dt)
//   +0xd6dcc6    radius check: |target - owner|^2 >= radius^2 -> unlink
//
// fix_buff.py gives POWER BOOST's bubble radius len + init_angle / 100000 (12.00037
// = 12 m, 37 deg). The radius check is patched to call ConeFilter first: for such a
// bubble, a target also needs to be within that angle of the owner's aim, and a
// target that passes gets the dr buff. The bubble's targeting runs on clients only,
// so the client that used POWER BOOST sends each link to the server as a link marker
// (2,000,000 + the same encoding), the way it sends the use marker (see LinkState; leaving
// the cone sends an unlink, 6,000,000; ConeFilter's slack and LINK_MIN_MS keep a teammate on
// the edge from flipping). The cone also keeps the bubble's radius (whole metres): the link
// reaches no further than the tether. Repair Matrix (a whole radius) is untouched. The owner is
// looked up the way the function does it (+0x44cc40, +0x638830): position at +8F8h,
// orientation quaternion (x, y, z, w) at +BD0h. Which local axis is "aim" is set by
// axis= in PowerBoost.ini ([cone], default +z); the log shows the angle to each
// target along all six axes so the right one can be picked. With [cone] aim=gun the
// cone follows the owner's gun instead (default head: the user found the gun hand wrong for it) (OwnerGunPose, MEDIGUN's gun_axis),
// so it goes where the hand points; the head is the fallback when no gun is found.
//
// The cost: the server takes [buff] self_damage off the user every self_damage_tick_ms for
// as long as OVERCHARGE runs (SelfDamageTick), never the last point of health unless
// self_damage_lethal=1.
//
// Cheat death (do_rez_buff): a linked teammate whose buff carries the rez cannot die for
// [rez] seconds (PowerBoost.ini, default 5) from their FIRST link in that OVERCHARGE use:
// every hit that would kill them then leaves them at 1 health, as often as it comes, until
// those seconds are up or the link breaks (a relink inside them carries on to the same
// end; FirstLink). The damage reduction runs its full time.
// ProcessDamageServer keeps
// each damageable in a 0xB8-byte record (cs+100h, slot from the u16 table at
// cs+D0h): flags at +02h (4 = dead, 2 = shield down), shield at +1Ch, health at +20h.
// Damage only reaches health once the shield is down or the hit is at least twice
// the shield, and then only what the shield did not take; WouldKill follows that.
// A rez marker is the plain one + REZ_FLAG.
//
// Damage boost: a boosted player's own client sends its hits as R15NetDamageableDamageDealt
// (+0xcc0670, client -> server; damage is decided by the shooter's client), so that is
// where they are scaled up: x(1 + damage_bonus) (PowerBoost.ini [buff], default 0.25)
// while this client's player is boosted -- for dr_duration after using OVERCHARGE here,
// or after another player's bubble links this client's player (known once this client
// has used OVERCHARGE itself: the use marker names its own damageable).
//
// All prologues are checked first; on a mismatch nothing is hooked (and nEVR unloads the
// plugin). A nEVR plugin (nevr_plugin.h): hooked from NvrPluginInitEx, unhooked in
// NvrPluginShutdown; the host says whether this is the server. Its lines go to nEVR's host log,
// tagged plugin=PowerBoost; the per-hit, per-tick and per-cone lines are Debug, and the extra
// diagnostic lines are built with ECHOCOMBAT_LOG. Settings: PowerBoost.ini next to the DLL.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <intrin.h>
#include <stdarg.h>
#include "detours.h"
#include "nevr_plugin.h"
#include "nevr/plugin_logger.h"

// Lines go to nEVR's host log, tagged plugin=<config.yaml name> (docs/standards/logging.md, Rule 15).
NEVR_DEFINE_PLUGIN_LOG("[PowerBoost]")

constexpr const char *kLogName = "PowerBoost";   // names the plugin in its log-cap Warning

namespace {

constexpr uintptr_t HEAL_RVA = 0xcecc40;
constexpr uint8_t HEAL_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70 };
constexpr uintptr_t DAMAGE_RVA = 0xd29770;
constexpr uint8_t DAMAGE_BYTES[] = { 0x4c, 0x8b, 0xdc, 0x4d, 0x89, 0x4b, 0x20, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 };
// CR15NetDamageableCS::ApplyDamageAndNotify (server): (cs, handle, damage, shooter, hit pos *,
// hit dir *, body part (u8), gun symbol, ...) -- it calls ProcessDamageServer with the hit.
constexpr uintptr_t APPLY_RVA = 0xcc10b0;
constexpr uint8_t APPLY_BYTES[] = { 0xf3, 0x0f, 0x11, 0x54, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56 };
// The game's shooter -> player slot (what ApplyDamageAndNotify stores as "last hit shooter"):
// (game = damageable cs + B8h, u32 *out, shooter) -> out.
constexpr uintptr_t SHOOTER_SLOT_RVA = 0x1c9520;
constexpr uint8_t SHOOTER_SLOT_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40, 0x49, 0x8b, 0xc0, 0x48, 0x8b, 0xfa };
using shooter_slot_fn = uint32_t *(__fastcall *)(void *game, uint32_t *out, uint64_t shooter);
shooter_slot_fn ShooterSlotFn = nullptr;
// Player slot -> damageable, the way the damage message handlers do it (0x140cb5d50):
// check the slot against the game's table (+138h value, +13Ah generation), turn the
// table value into the player's actor with CPlayerSlot::GetPosition (0x1401c76b0), then
// the damageable system's virtual +1B0h (dcs, &out, {actor, symbol at +16D9A58h}).
constexpr uintptr_t SLOT_ACTOR_RVA = 0x1c76b0;
constexpr uint8_t SLOT_ACTOR_BYTES[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x41, 0x0f, 0xb7, 0xc0, 0x48, 0x8b, 0xda };
constexpr uintptr_t PLAYER_SYMBOL_RVA = 0x16d9a58;
using slot_actor_fn = uint8_t *(__fastcall *)(void *game, uint8_t *out, uint32_t value);
slot_actor_fn SlotActorFn = nullptr;
uint64_t *g_player_symbol = nullptr;
constexpr uintptr_t SEND_RVA = 0xcc0670;
constexpr uint8_t SEND_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x60, 0x33, 0xc0 };

constexpr float MARKER_BASE = 1000000.0f;
constexpr float MARKER_END = 2000000.0f;
constexpr float LINK_BASE = 2000000.0f;   // link marker: the client's "I linked this one" (same encoding)
constexpr float LINK_END = 3000000.0f;
constexpr float REZ_FLAG = 2000000.0f;    // use / link marker + this: the buff carries a rez
constexpr float UNLINK_BASE = 6000000.0f; // unlink marker: "this one left my cone", its buff ends
constexpr float UNLINK_END = 7000000.0f;

using heal_fn = void(__fastcall *)(void *cs, uint32_t index, float amount, uint64_t a4, uint64_t a5);
using damage_fn = void(__fastcall *)(void *cs, uint32_t index, float damage, void *info, uint64_t a5, uint64_t a6);
heal_fn Real_Heal = nullptr;
damage_fn Real_Damage = nullptr;
using send_fn = void(__fastcall *)(void *cs, uint32_t index, float amount, uint8_t type, uint64_t info);
send_fn SendDamageDealt = nullptr;
send_fn Real_Send = nullptr;              // the hooked original (SendDamageDealt stays the plain call)
float g_damage_bonus = 0.25f;             // PowerBoost.ini [buff] damage_bonus
void *volatile g_self_cs = nullptr;       // this client's own damageable (from its use marker)
void *volatile g_damage_cs = nullptr;     // the damageable system, from the first hit (CombatBots reads it)
volatile uint32_t g_self_index = 0xffffffff;
volatile ULONGLONG g_boosted_until = 0;   // this client's player deals more damage until then
bool g_server = false;

constexpr uintptr_t RADIUS_RVA = 0xd6dcc6;
constexpr uint8_t RADIUS_BYTES[] = { 0x4c, 0x8b, 0x7d, 0xc8,                    // mov r15, [rbp-38h]
                                     0xf3, 0x41, 0x0f, 0x10, 0x47, 0x38,        // movss xmm0, [r15+38h]
                                     0xf3, 0x0f, 0x59, 0xc0,                    // mulss xmm0, xmm0
                                     0x0f, 0x2f, 0xc8 };                        // comiss xmm1, xmm0
constexpr uintptr_t OWNER_HANDLE_RVA = 0x44cc40;
constexpr uint8_t OWNER_HANDLE_BYTES[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00 };
constexpr uintptr_t OWNER_OBJECT_RVA = 0x638830;
constexpr uint8_t OWNER_OBJECT_BYTES[] = { 0x44, 0x8b, 0x49, 0x08, 0x41, 0x83, 0xf9, 0xff };
using handle_fn = void *(__fastcall *)(void *cs, void *out, uint32_t index);
using object_fn = uint8_t *(__fastcall *)(void *handle);
handle_fn OwnerHandle = nullptr;
object_fn OwnerObject = nullptr;

volatile float g_dr = 0.09f;               // from the latest marker
volatile unsigned g_dr_seconds = 10;
volatile bool g_rez = false;              // the latest marker's buff carries a rez
unsigned g_rez_grace_ms = 1500;           // PowerBoost.ini [rez] grace_ms (no longer used: no grace after a save)
unsigned g_rez_window_ms = 5000;          // PowerBoost.ini [rez] seconds: cheat death lasts this long
volatile ULONGLONG g_last_use = 0;        // server: when OVERCHARGE was last used
// Damage boost (server): only the OVERCHARGE user, for the buff's seconds -- each gun hit
// they land deals +body_bonus, or +head_bonus on the head (PowerBoost.ini [buff]). The user
// also gets the damage reduction (not cheat death); linked teammates get the reduction and
// cheat death but no damage boost. A hit's shooter is the victim's record +04h (the shooter's
// player slot, as each damageable's own record holds at +0Ch); its body part is +03h.
float g_body_bonus = 1.0f, g_head_bonus = 2.0f;
// the hit being applied (set by Hooked_Apply around ProcessDamageServer, same thread)
thread_local uint64_t t_shooter = ~0ull, t_gun = 0;
thread_local int t_part = -1;
thread_local bool t_in_apply = false;
thread_local bool t_self_damage = false;  // OVERCHARGE's own cost being applied (SelfDamageTick)
int g_head_part = 5;
// OVERCHARGE costs its user (server): self_damage every self_damage_tick_ms while it runs
// (PowerBoost.ini [buff]); unless self_damage_lethal, it never takes the last point of health.
float g_self_damage = 2.0f;
unsigned g_self_tick_ms = 1000;
bool g_self_lethal = false;
struct Boost { void *cs; uint32_t index; ULONGLONG until; uint64_t shooter; };   // shooter: ~0 unknown
constexpr int MAX_BOOSTS = 16;
Boost g_boosts[MAX_BOOSTS];

uint8_t *Record(void *cs, uint32_t index);

// A damageable's own player slot (+0Ch), or ~0 if its record can't be read (stale system).
uint32_t PlayerSlot(void *cs, uint16_t index) {
    __try {
        return *(uint32_t *)(Record(cs, index) + 0x0c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xffffffffu;
    }
}

void Log(const char *fmt, ...);

// The damageable's actor id, through its system's virtual +1E0h (cs, &out, handle) -- the
// same call component systems use to name their entity for WorldTeleport.
uint64_t ActorId(void *cs, uint32_t handle) {
    __try {
        using actor_of_fn = uint64_t *(__fastcall *)(void *cs, uint64_t *out, uint32_t handle);
        uint64_t out = ~0ull;
        uint64_t *a = ((actor_of_fn)(*(void ***)cs)[0x1e0 / 8])(cs, &out, handle);
        return a ? *a : ~0ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ~0ull;
    }
}

// The damageable handle of the player in `slot` (index | generation << 16), or ~0.
uint32_t SlotDamageable(void *dcs, uint32_t slot) {
    if (!SlotActorFn || !g_player_symbol) return 0xffffffffu;
    __try {
        uint8_t *game = *(uint8_t **)((uint8_t *)dcs + 0xb8);
        uint32_t idx = slot & 0xffff, gen = slot >> 16;
        if (!game || idx >= 16 || gen == 0xffff || *(uint16_t *)(game + 0x13a + idx * 4) != gen) return 0xffffffffu;
        uint16_t value = *(uint16_t *)(game + 0x138 + idx * 4);
        if (value == 0xffff) return 0xffffffffu;
        alignas(16) uint8_t tmp[64] = {};
        uint8_t *r = SlotActorFn(game, tmp, value);
        alignas(16) uint64_t key[2] = { *(uint64_t *)(r + 8), *g_player_symbol };
        alignas(16) uint8_t out[64] = {};
        using find_fn = uint32_t *(__fastcall *)(void *dcs, void *out, void *key);
        uint32_t *h = ((find_fn)(*(void ***)dcs)[0x1b0 / 8])(dcs, out, key);
        return h ? *h : 0xffffffffu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xffffffffu;
    }
}

void AddBoost(void *cs, uint32_t index, unsigned seconds, uint64_t shooter = ~0ull) {
    ULONGLONG now = GetTickCount64();
    int slot = -1;
    for (int i = 0; i < MAX_BOOSTS; ++i) {
        if (g_boosts[i].cs == cs && (g_boosts[i].index & 0xffff) == (index & 0xffff)) { slot = i; break; }
        if (slot < 0 && (!g_boosts[i].cs || g_boosts[i].until <= now)) slot = i;
    }
    if (slot < 0) return;
    if (shooter == ~0ull && g_boosts[slot].cs == cs) shooter = g_boosts[slot].shooter;
    g_boosts[slot] = { cs, index, now + seconds * 1000ull, shooter };
    Log("damage boost: %p/%u for %u s (shooter id %016llx, actor %016llx)", cs, index & 0xffff, seconds,
        (unsigned long long)shooter, (unsigned long long)ActorId(cs, index));
}
volatile ULONGLONG g_used_at = 0;         // when this client last used POWER BOOST
float g_axis[3] = { 0, 0, 1 };            // owner-local aim axis (PowerBoost.ini [cone] axis=)

struct Buff {
    void *cs;
    uint16_t index;
    ULONGLONG until;      // GetTickCount64
    float mult;           // 1 - dr
    bool rez;             // cheat death while linked
    ULONGLONG grace;      // cheat death holds until then (its first link + [rez] seconds)
};
constexpr int MAX_BUFFS = 64;
Buff g_buffs[MAX_BUFFS];
SRWLOCK g_lock = SRWLOCK_INIT;

char g_ini_path[MAX_PATH];                // PowerBoost.ini, next to the DLL
HMODULE g_self = nullptr;

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

// When (cs, index) was first linked in the current use (g_last_use): its cheat death runs
// [rez] seconds from then, however often the link breaks and comes back.
struct FirstLinkAt { void *cs; uint16_t index; ULONGLONG use; ULONGLONG at; };
FirstLinkAt g_first_links[MAX_BUFFS];

ULONGLONG FirstLink(void *cs, uint16_t index, ULONGLONG now) {   // (under g_lock)
    ULONGLONG use = g_last_use && now - g_last_use < 60000 ? g_last_use : 0;
    FirstLinkAt *slot = nullptr;
    for (auto &f : g_first_links) {
        if (f.cs == cs && f.index == index) {
            if (f.use == use) return f.at;
            slot = &f;
            break;
        }
        if (!slot && (!f.cs || f.use != use)) slot = &f;
    }
    if (!slot) slot = &g_first_links[0];
    *slot = { cs, index, use, now };
    return now;
}

// Buffs (cs, index) for `seconds`. Unless `refresh`, a buff still running is left as is.
void AddBuff(void *cs, uint16_t index, float dr, unsigned seconds, bool rez, bool refresh = true) {
    ULONGLONG now = GetTickCount64(), until = now + seconds * 1000ull;
    AcquireSRWLockExclusive(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_BUFFS; ++i) {
        if (g_buffs[i].cs == cs && g_buffs[i].index == index) {
            if (!refresh && g_buffs[i].until > now) { ReleaseSRWLockExclusive(&g_lock); return; }
            slot = i;
            break;
        }
        if (slot < 0 && (g_buffs[i].cs == nullptr || g_buffs[i].until <= now)) slot = i;
    }
    ULONGLONG rez_until = FirstLink(cs, index, now) + g_rez_window_ms;
    if (slot >= 0) g_buffs[slot] = { cs, index, until, 1.0f - dr, rez, rez_until };
    ReleaseSRWLockExclusive(&g_lock);
    Log("buff: damageable %p/%u takes x%.3f damage for %u s%s%s", cs, index, 1.0f - dr, seconds,
        rez && rez_until > now ? ", cannot die for the rest of its first link's seconds" : "",
        slot < 0 ? " -- NO FREE SLOT, ignored" : "");
}

// The buff of (cs, index) ends now (its link broke): damage reduction and cheat death.
void EndBuff(void *cs, uint16_t index) {
    ULONGLONG now = GetTickCount64();
    bool found = false;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < MAX_BUFFS; ++i)
        if (g_buffs[i].cs == cs && g_buffs[i].index == index && g_buffs[i].until > now) {
            g_buffs[i].until = now;
            g_buffs[i].rez = false;
            g_buffs[i].grace = now;
            found = true;
        }
    ReleaseSRWLockExclusive(&g_lock);
    Log("buff: damageable %p/%u unlinked: buff ended%s", cs, index, found ? "" : " (none running)");
}

void AddSelfDamage(void *cs, uint32_t index, unsigned seconds);

// A marker's kind: 1 = use, 2 = link, 0 = a real heal; rez and the encoded value.
int Marker(float amount, bool *rez, unsigned *v) {
    *rez = amount >= MARKER_BASE + REZ_FLAG;
    if (*rez) amount -= REZ_FLAG;
    int kind = amount >= MARKER_BASE && amount < MARKER_END ? 1 : amount >= LINK_BASE && amount < LINK_END ? 2 : 0;
    if (kind) *v = (unsigned)(amount - (kind == 1 ? MARKER_BASE : LINK_BASE) + 0.5f);
    return kind;
}

void __fastcall Hooked_Heal(void *cs, uint32_t index, float amount, uint64_t a4, uint64_t a5) {
    if (amount >= UNLINK_BASE && amount < UNLINK_END) {
        if (g_server) EndBuff(cs, (uint16_t)index);  // a client's link to this damageable broke
        return;
    }
    bool rez;
    unsigned v;
    int kind = Marker(amount, &rez, &v);
    if (kind == 1) {
        g_dr = (v / 1000) / 1000.0f;              // what the link hands out (see ConeFilter)
        g_dr_seconds = v % 1000;
        g_rez = rez;
        if (!g_server) {                          // used here: hand it to the server
            g_used_at = GetTickCount64();
            g_self_cs = cs;
            g_self_index = index & 0xffff;
            g_boosted_until = g_used_at + g_dr_seconds * 1000ull;
            Log("marker %.0f for %p/%u -> server", amount, cs, index & 0xffff);
            SendDamageDealt(cs, index, -amount, 0, 0);
            return;
        }
        g_last_use = GetTickCount64();
        // the user: the damage boost and the damage reduction, but no cheat death (that is
        // for the teammates it links)
        AddBuff(cs, (uint16_t)index, g_dr, g_dr_seconds, false, true);
        AcquireSRWLockExclusive(&g_lock);
        AddBoost(cs, index, g_dr_seconds, t_in_apply ? t_shooter : ~0ull);
        ReleaseSRWLockExclusive(&g_lock);
        AddSelfDamage(cs, index, g_dr_seconds);
        Log("POWER BOOST used by %p/%u: links give x%.3f damage for %u s%s", cs, index & 0xffff,
            1.0f - g_dr, (unsigned)g_dr_seconds, rez ? " and cheat death" : "");
        return;                                   // a marker, not a heal
    }
    if (kind == 2) {
        if (g_server) {                           // a client linked this damageable
            AddBuff(cs, (uint16_t)index, (v / 1000) / 1000.0f, v % 1000, rez, false);   // no damage boost
        }
        return;
    }
    Real_Heal(cs, index, amount, a4, a5);
}

// The damageable's record in ProcessDamageServer's layout (see the top of the file).
uint8_t *Record(void *cs, uint32_t index) {
    uint8_t *c = (uint8_t *)cs;
    uint16_t slot = (*(uint16_t **)(c + 0xd0))[index & 0xffff];
    return *(uint8_t **)(c + 0x100) + slot * 0xb8ull;
}

// Would `damage` kill? Mirrors ProcessDamageServer: the shield (+1Ch) takes it first,
// and health (+20h) only sees the rest once the shield is down or the hit is at
// least twice the shield.
bool WouldKill(const uint8_t *rec, float damage) {
    uint8_t flags = rec[2];
    float shield = *(const float *)(rec + 0x1c), health = *(const float *)(rec + 0x20);
    if ((flags & 4) || health <= 0) return false;              // already dead
    bool big = damage >= shield + shield;
    float to_health;
    bool reaches;
    if (shield > 0) {
        to_health = big ? damage - shield : 0;
        reaches = (flags & 2) || big;
    } else {
        to_health = damage;
        reaches = (flags & 2) != 0;
    }
    return reaches && health - to_health <= 0;
}

// The largest damage that leaves at least 1 health.
float Survivable(const uint8_t *rec, float damage) {
    float shield = *(const float *)(rec + 0x1c), health = *(const float *)(rec + 0x20);
    float keep = health - 1.0f > 0 ? health - 1.0f : 0;
    float most = (shield > 0 ? shield : 0) + keep;
    return damage < most ? damage : most;
}

void __fastcall Hooked_Damage(void *cs, uint32_t index, float damage, void *info, uint64_t a5, uint64_t a6) {
    g_damage_cs = cs;
    if (t_self_damage) {                                  // OVERCHARGE's cost: no reduction, boost or cheat death
        Real_Damage(cs, index, damage, info, a5, a6);
        return;
    }
    ULONGLONG now = GetTickCount64();
    float mult = 1.0f;
    bool rez = false;
    int slot = -1;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < MAX_BUFFS; ++i)
        if (g_buffs[i].cs == cs && g_buffs[i].index == (uint16_t)index) {
            if (g_buffs[i].until > now) { mult = g_buffs[i].mult; rez = g_buffs[i].rez && now < g_buffs[i].grace; slot = i; }
            break;
        }
    // the shooter boosted: +body_bonus / +head_bonus (before the target's own reduction)
    if (damage > 0) {
        uint32_t shooter = 0xffffffffu;
        int part = t_part;
        if (t_in_apply && ShooterSlotFn && t_shooter != ~0ull) {
            __try {
                ShooterSlotFn(*(void **)((uint8_t *)cs + 0xb8), &shooter, t_shooter);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                shooter = 0xffffffffu;
            }
        }
        uint32_t from = shooter != 0xffffffffu ? SlotDamageable(cs, shooter) : 0xffffffffu;   // shooter's damageable
        bool boosted = false;
        for (int i = 0; i < MAX_BOOSTS && !boosted; ++i)
            if (g_boosts[i].cs == cs && g_boosts[i].until > now && from != 0xffffffffu &&
                (g_boosts[i].index & 0xffff) == (from & 0xffff))
                boosted = true;
        bool gun = t_in_apply && t_gun != ~0ull && t_gun != 0;          // guns only, not the drone etc.
        float add = boosted && gun ? (part == g_head_part ? g_head_bonus : g_body_bonus) : 0.0f;
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 200)
            LogDebug("hit %p/%u: %.2f, shooter %016llx (slot %08x, damageable %u), body part %d, gun %016llx%s", cs,
                index & 0xffff, damage, (unsigned long long)t_shooter, shooter, from & 0xffff, part,
                (unsigned long long)t_gun,
                add > 0 ? (part == g_head_part ? " BOOSTED (head)" : " BOOSTED (body)") : boosted ? " (boosted, not a gun)" : "");
        damage += add;
    }
    if (mult != 1.0f) {
        LogDebug("damage to buffed %p/%u: %.2f -> %.2f", cs, index & 0xffff, damage, damage * mult);
        damage *= mult;
    }
    // Cheat death holds for [rez] seconds from a teammate's first link in the use, while
    // linked: every hit that would kill them leaves them at 1 health instead.
    if (rez && slot >= 0) {
        uint8_t *rec = Record(cs, index);
        if (WouldKill(rec, damage)) {
            float left = Survivable(rec, damage);
            static volatile LONG logged = 0;
            if (InterlockedIncrement(&logged) <= 200)
                Log("CHEAT DEATH: %p/%u would die to %.2f (shield %.2f, health %.2f): takes %.2f, stays alive",
                    cs, index & 0xffff, damage, *(float *)(rec + 0x1c), *(float *)(rec + 0x20), left);
            damage = left;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    Real_Damage(cs, index, damage, info, a5, a6);
}

// q (x, y, z, w) applied to v.
void Rotate(const float *q, const float *v, float *out) {
    float t[3] = { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

float AngleTo(const float *q, const float *axis, const float *d, float dist) {
    float f[3];
    Rotate(q, axis, f);
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    float c = (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]) / (len * dist);
    return acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 57.29578f;
}

// A link to (dcs, index) made (linked) or broken. The bubble's targeting only runs on
// clients, so the client that used POWER BOOST (it sent a marker within the last 30 s)
// tells the server: a DamageDealt of -(link marker), which the server's handler turns into
// a heal of the marker, where Hooked_Heal buffs that damageable for the use's remaining
// seconds; or of -(unlink marker), which ends that buff. A teammate is only buffed while
// linked. Other clients see the same link and stay quiet.
void LinkState(void *dcs, uint32_t index, bool linked) {
    ULONGLONG now = GetTickCount64();
    unsigned left = g_dr_seconds;
    if (g_used_at && now - g_used_at < g_dr_seconds * 1000ull)
        left = (unsigned)((g_dr_seconds * 1000ull - (now - g_used_at) + 999) / 1000);
    if (left < 1) left = 1;
    if (!g_server && dcs == g_self_cs && (index & 0xffff) == g_self_index)   // someone linked us
        g_boosted_until = linked ? now + left * 1000ull : now;
    if (g_server) {
        if (linked) AddBuff(dcs, (uint16_t)index, g_dr, left, g_rez, false);   // no damage boost
        else EndBuff(dcs, (uint16_t)index);
        return;
    }
    if (g_used_at == 0 || now - g_used_at > 30000) return;
    float marker = linked ? LINK_BASE + (float)((unsigned)(g_dr * 1000.0f + 0.5f) * 1000 + left) + (g_rez ? REZ_FLAG : 0.0f)
                          : UNLINK_BASE;
    Log("%s %p/%u -> server (marker %.0f)", linked ? "link to" : "unlink from", dcs, index & 0xffff, marker);
    SendDamageDealt(dcs, index, -marker, 0, 0);
}

using apply_fn = void(__fastcall *)(void *cs, uint64_t handle, float damage, uint64_t shooter, uint64_t a5,
                                    uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10);
apply_fn Real_Apply = nullptr;

bool PartyShooter(uint64_t shooter);

void __fastcall Hooked_Apply(void *cs, uint64_t handle, float damage, uint64_t shooter, uint64_t a5, uint64_t a6,
                             uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10) {
    if (shooter != ~0ull && PartyShooter(shooter)) {     // a party gun's hit: no damage
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 20) Log("PARTY GUN: hit by %016llx dropped (%.1f)", (unsigned long long)shooter, damage);
        return;
    }
    bool outer = !t_in_apply;
    uint64_t s0 = t_shooter, g0 = t_gun;
    int p0 = t_part;
    t_shooter = shooter;
    t_part = (int)(uint8_t)a7;
    t_gun = a8;
    t_in_apply = true;
    Real_Apply(cs, handle, damage, shooter, a5, a6, a7, a8, a9, a10);
    t_shooter = s0, t_part = p0, t_gun = g0;
    if (outer) t_in_apply = false;
}

// OVERCHARGE's users on the server and when each next pays its cost. The damage goes in the
// way a client's hit does (the DamageDealt handler, +0xcb5d50: no shooter, zero hit position
// and direction, body part 0, no gun), so health, events and sync are the game's own.
struct SelfDamage { void *cs; uint32_t index; ULONGLONG until, next; };
SelfDamage g_self_damages[16];

void AddSelfDamage(void *cs, uint32_t index, unsigned seconds) {
    if (!g_server || g_self_damage <= 0) return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    SelfDamage *slot = nullptr;
    for (SelfDamage &s : g_self_damages) {
        if (s.cs == cs && (s.index & 0xffff) == (index & 0xffff)) { slot = &s; break; }
        if (!slot && (!s.cs || s.until <= now)) slot = &s;
    }
    if (slot) *slot = { cs, index, now + seconds * 1000ull, now + g_self_tick_ms };
    ReleaseSRWLockExclusive(&g_lock);
    Log("OVERCHARGE cost: %p/%u takes %.1f every %u ms for %u s%s", cs, index & 0xffff, g_self_damage,
        g_self_tick_ms, seconds, slot ? "" : " -- NO FREE SLOT, ignored");
}

// Server, game thread (the heal bubble system's update): every user due a tick takes it.
void SelfDamageTick(ULONGLONG now) {
    if (!g_server || !Real_Apply) return;
    for (SelfDamage &s : g_self_damages) {
        if (!s.cs || now < s.next) continue;
        if (now >= s.until) { s.cs = nullptr; continue; }
        s.next += g_self_tick_ms;
        if (s.next <= now) s.next = now + g_self_tick_ms;      // (after a hitch: no burst)
        float damage = g_self_damage;
        __try {
            const uint8_t *rec = Record(s.cs, s.index);
            if (rec[2] & 4) { s.cs = nullptr; continue; }       // dead: the cost ends with the use
            if (!g_self_lethal && WouldKill(rec, damage)) damage = Survivable(rec, damage);
            if (damage <= 0) continue;
            alignas(16) float pos[4] = {}, dir[4] = {};
            t_self_damage = true;
            Real_Apply(s.cs, s.index, damage, ~0ull, (uint64_t)pos, (uint64_t)dir, 0, 0, 0, 0);
            t_self_damage = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            t_self_damage = false;
            s.cs = nullptr;
            LogWarning("OVERCHARGE cost: damaging the user faulted, stopped");
            continue;
        }
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 100) LogDebug("OVERCHARGE cost: %p/%u -%.2f", s.cs, s.index & 0xffff, damage);
    }
}

// This client's hits on their way to the server: boosted, they deal x(1 + damage_bonus).
// Negative amounts are markers / heals, passed through untouched.
void __fastcall Hooked_Send(void *cs, uint32_t index, float amount, uint8_t type, uint64_t info) {
    if (g_damage_bonus > 0 && amount > 0 && amount < MARKER_BASE && GetTickCount64() < g_boosted_until) {
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 50)
            LogDebug("boosted hit on %p/%u: %.2f -> %.2f", cs, index & 0xffff, amount, amount * (1.0f + g_damage_bonus));
        amount *= 1.0f + g_damage_bonus;
    }
    Real_Send(cs, index, amount, type, info);
}

// Called from the bubble's radius check (see InstallCone). Returns 0 to treat the
// target as out of range. bubble: the bubble's record; target: its position;
// dcs/index: the target's damageable.
// The bubble's radius check runs for the bubble's whole life: a teammate is linked while
// inside the cone (within the bubble's radius, whole metres, and the angle), at any point of
// the use, and unlinks (its buff ends) when it leaves; it links again (for the use's
// remaining seconds) when it comes back. (An earlier version only let the first
// link_window_ms pick targets and kept them for the whole use: aiming at a teammate a
// moment after using it linked no one.)
// A linked teammate only leaves once CONE_SLACK_DEG past the cone's angle, so one on the
// edge doesn't flip every frame (no slack on the radius: past it the game's own check drops
// the tether anyway), and the server hears of a change at most every
// LINK_MIN_MS per teammate (the latest state, once that time is up).
unsigned g_link_window_ms = 500;                  // PowerBoost.ini [cone] link_window_ms (no longer used)
constexpr float CONE_SLACK_DEG = 5.0f;
constexpr ULONGLONG LINK_MIN_MS = 250;
struct BubbleLinks {
    uint8_t *bubble;
    ULONGLONG first, last;
    int count;
    struct { void *dcs; uint32_t index; bool inside, sent; ULONGLONG sent_at; } linked[8];   // sent: what the server has
};
BubbleLinks g_bubbles[8];

BubbleLinks &BubbleFor(uint8_t *bubble, ULONGLONG now) {
    BubbleLinks *oldest = &g_bubbles[0];
    for (BubbleLinks &b : g_bubbles) {
        // a bubble record unseen for 2 s is a new bubble (records are reused)
        if (b.bubble == bubble && now - b.last < 2000) { b.last = now; return b; }
        if (b.last < oldest->last) oldest = &b;
    }
    *oldest = {};
    oldest->bubble = bubble;
    oldest->first = oldest->last = now;
    return *oldest;
}

int MediCone(uint8_t *bubble, float off, float dist, float range, int angle, void *dcs, uint32_t index);
bool GunPose(uint8_t *bubble, float pos[3], float q[4]);
bool OwnerGunPose(uint8_t *bubble, float pos[3], float q[4]);
void DiagGunSpace(uint8_t *bubble, const float *hp, const float *hq);
bool GunOf(uint64_t player, uint64_t *space_id, uint64_t *gun, uint64_t *weapon = nullptr, int *arm = nullptr);
float g_gun_axis[3] = { 0, 0, 1 };        // the gun's local aim axis (PowerBoost.ini [medigun] gun_axis=)
bool g_medi_aim_gun = false;             // MEDIGUN aims with its gun transform (PowerBoost.ini [medigun] aim=gun)
bool g_aim_gun = false;                   // OVERCHARGE aims with the gun hand (PowerBoost.ini [cone] aim=gun; default head)

int __fastcall ConeFilter(uint8_t *bubble, const float *target, void *dcs, uint32_t index) {
    float radius = *(float *)(bubble + 0x38);
    int angle = (int)((radius - floorf(radius)) * 100000.0f + 0.5f);
    bool medigun = angle > 500 && angle <= 590;           // MEDIGUN's bubble (fix_medigun.CONE_TAG)
    if (!medigun && (angle < 1 || angle > 180)) return 1; // not POWER BOOST's bubble
    uint8_t handle[64] = {};
    void *h = OwnerHandle(*(void **)bubble, handle, *(uint32_t *)(bubble + 8));
    uint8_t *owner = h ? OwnerObject(h) : nullptr;
    if (!owner) return 0;
    const float *pos = (const float *)(owner + 0x8f8), *q = (const float *)(owner + 0xbd0);
    const float *axis = g_axis;
    {   // the owner: never linked (measured from the head: the gun is up to a metre from its own body)
        const float *head = pos;
        float h[3] = { target[0] - head[0], target[1] - head[1], target[2] - head[2] };
        if (h[0] * h[0] + h[1] * h[1] + h[2] * h[2] < 0.09f) return 0;
    }
    float gun_pos[3], gun_q[4];
    if (medigun) {                                        // MEDIGUN: the head unless [medigun] aim=gun
        // (the gun transform found is on the OFF hand: it changes side with the weapon arm, the
        // opposite way to the gun -- logged 2026-10-27 -- so it can't aim the beam yet)
        if (g_medi_aim_gun && GunPose(bubble, gun_pos, gun_q)) { pos = gun_pos; q = gun_q; axis = g_gun_axis; }
    } else if (g_aim_gun) {                               // OVERCHARGE: where the gun hand points, too
        if (OwnerGunPose(bubble, gun_pos, gun_q)) { pos = gun_pos; q = gun_q; axis = g_gun_axis; }
    }
    float d[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist < 0.3f) return 0;                            // the owner: never self-linked
    float off = AngleTo(q, axis, d, dist);
    if (medigun) {
        static ULONGLONG told;
        ULONGLONG t = GetTickCount64();
        if (t - told >= 1000) {
            told = t;
            static const float axes[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
            float a[6];
            for (int i = 0; i < 6; ++i) a[i] = AngleTo(q, axes[i], d, dist);
            DiagGunSpace(bubble, (const float *)(owner + 0x8f8), (const float *)(owner + 0xbd0));
            // where that gun is, seen from the head (head axes): which hand the aim comes from
            const float *hp = (const float *)(owner + 0x8f8), *hq = (const float *)(owner + 0xbd0);
            float rel[3] = { pos[0] - hp[0], pos[1] - hp[1], pos[2] - hp[2] }, inv[4] = { -hq[0], -hq[1], -hq[2], hq[3] }, hl[3];
            Rotate(inv, rel, hl);
            LogDebug("MEDIGUN cone from %s: target %.1f m, %.0f deg | +x %.0f -x %.0f +y %.0f -y %.0f +z %.0f -z %.0f | "
                "gun from the head %.2f %.2f %.2f", pos == gun_pos ? "the gun" : "the head (no gun pose)", dist, off,
                a[0], a[1], a[2], a[3], a[4], a[5], hl[0], hl[1], hl[2]);
        }
    }
    float range = floorf(radius);
    if (medigun) return MediCone(bubble, off, dist, range, angle - 500, dcs, index);
    ULONGLONG now0 = GetTickCount64();
    BubbleLinks &bl = BubbleFor(bubble, now0);
    int picked = -1;
    for (int i = 0; i < bl.count; ++i)
        if (bl.linked[i].dcs == dcs && (bl.linked[i].index & 0xffff) == (index & 0xffff)) picked = i;
    bool was = picked >= 0 && bl.linked[picked].inside;   // linked: the cone is a little wider to leave
    bool ok = off <= (float)angle + (was ? CONE_SLACK_DEG : 0.0f) && dist <= range;
    if (picked >= 0) {                                    // one of the bubble's targets: follows the cone
        bl.linked[picked].inside = ok;
    } else if (ok && bl.count < 8) {                     // a new teammate in the cone
        picked = bl.count++;
        bl.linked[picked] = { dcs, index, true, false, 0 };
    } else {
        ok = false;
    }
    bool send = false;
    if (picked >= 0 && bl.linked[picked].sent != ok && now0 - bl.linked[picked].sent_at >= LINK_MIN_MS) {
        bl.linked[picked].sent = ok;
        bl.linked[picked].sent_at = now0;
        send = true;
    }

    static ULONGLONG last;                                // diagnostics, twice a second
    ULONGLONG now = GetTickCount64();
    if (now - last >= 500) {
        last = now;
        static const float axes[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
        float a[6];
        for (int i = 0; i < 6; ++i) a[i] = AngleTo(q, axes[i], d, dist);
        LogDebug("cone (from the %s): target %p/%u at %.1f m (max %.0f), %.0f deg off aim (max %d) -> %s | +x %.0f -x %.0f +y %.0f "
            "-y %.0f +z %.0f -z %.0f | q %.2f %.2f %.2f %.2f", pos == gun_pos ? "gun" : "head", dcs, index & 0xffff, dist, range, off, angle,
            ok ? "LINK" : "no", a[0], a[1], a[2], a[3], a[4], a[5], q[0], q[1], q[2], q[3]);
    }
    if (send) LinkState(dcs, index, ok);                  // linked / unlinked
    return ok;
}

uint8_t g_radius_saved[14];
uint8_t *g_radius_site = nullptr;          // patched by InstallCone

// The radius check, 14 bytes at +0xd6dcc6, becomes an absolute jump to this stub:
// the original three instructions, then ConeFilter (volatile registers saved); a
// rejected target gets radius^2 = 0, so the check that follows unlinks it.
bool InstallCone(uint8_t *base) {
    uint8_t *site = base + RADIUS_RVA;
    uint8_t *stub = (uint8_t *)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    uint8_t code[] = {
        0x4c, 0x8b, 0x7d, 0xc8,                         // mov r15, [rbp-38h]
        0xf3, 0x41, 0x0f, 0x10, 0x47, 0x38,             // movss xmm0, [r15+38h]
        0xf3, 0x0f, 0x59, 0xc0,                         // mulss xmm0, xmm0
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,   // push rax rcx rdx r8-r11
        0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00,       // sub rsp, 88h (aligned again)
        0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x20,             // movdqu [rsp+20h], xmm0
        0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x30,             // movdqu [rsp+30h], xmm1
        0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x40,             // movdqu [rsp+40h], xmm2
        0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x50,             // movdqu [rsp+50h], xmm3
        0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x60,             // movdqu [rsp+60h], xmm4
        0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x70,             // movdqu [rsp+70h], xmm5
        0x4c, 0x89, 0xf9,                               // mov rcx, r15      bubble
        0x48, 0x8d, 0x95, 0x10, 0x01, 0x00, 0x00,       // lea rdx, [rbp+110h] target position
        0x4d, 0x89, 0xe8,                               // mov r8, r13       target damageable cs
        0x41, 0x89, 0xd9,                               // mov r9d, ebx      ... and index
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rax, ConeFilter
        0xff, 0xd0,                                     // call rax
        0x85, 0xc0,                                     // test eax, eax
        0x75, 0x08,                                     // jnz keep
        0xc7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, // mov dword [rsp+20h], 0 (radius^2 = 0)
        0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20,             // keep: movdqu xmm0, [rsp+20h]
        0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x30,
        0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x40,
        0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x50,
        0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x60,
        0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x70,
        0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00,       // add rsp, 88h
        0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58,   // pop r11-r8 rdx rcx rax
        0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // jmp [rip+0] -> the comiss after the site
    };
    uint64_t fn = (uint64_t)&ConeFilter, back = (uint64_t)(site + 14);
    size_t at_fn = 0, at_back = sizeof(code) - 8;
    for (size_t i = 0; i + 1 < sizeof(code); ++i)
        if (code[i] == 0x48 && code[i + 1] == 0xb8) { at_fn = i + 2; break; }
    memcpy(code + at_fn, &fn, 8);
    memcpy(code + at_back, &back, 8);
    memcpy(stub, code, sizeof(code));
    uint8_t jump[14] = { 0xff, 0x25, 0, 0, 0, 0 };      // jmp [rip+0] -> stub
    uint64_t to = (uint64_t)stub;
    memcpy(jump + 6, &to, 8);
    // Checked patch: the radius check must still be the bytes we expect, and the page must
    // be writable, or nothing is written.
    if (!EchoCombatBytesMatch(site, RADIUS_BYTES, sizeof(RADIUS_BYTES))) {
        LogWarning("radius check bytes changed before patching at +0x%llx -- cone not patched", (unsigned long long)RADIUS_RVA);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
        LogWarning("VirtualProtect failed (error %lu) at +0x%llx -- cone not patched", GetLastError(),
            (unsigned long long)RADIUS_RVA);
        return false;
    }
    memcpy(g_radius_saved, site, sizeof(jump));         // put back by NvrPluginShutdown
    g_radius_site = site;
    memcpy(site, jump, sizeof(jump));
    DWORD ignored;
    if (!VirtualProtect(site, sizeof(jump), old, &ignored))
        LogWarning("VirtualProtect could not restore the page protection (error %lu)", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    return true;
}

// --- MEDIGUN ----------------------------------------------------------------------------
// A gun in gear_table.weapons (tools/fix_medigun.py) with no gun actors of its own:
//
//   +0xcea200  CR15NetPlayerEquipmentCS::GetWeaponSlot(cs, &out, handle): record (0x58, at
//              cs+F8h through the u16 table at cs+C8h) +10h this machine's player, +18h weapon,
//              +30h weapon arm. Called from the gun enable manager's weapon slot expression
//              (returns to +0xf0dc45), which enables each player gun whose type is the slot:
//              there a MEDIGUN slot reads as "scout", so the player holds COMET's gun.
//              Every call also tells this machine's own weapon, actor and arm.
//
// With MEDIGUN equipped this client hides the gun hand's index trigger from the game (LibOVR's
// ovr_GetInputState, IndexTrigger at +10h), so COMET never charges or fires, and the trigger
// drives the beam instead: MEDI_BUBBLE, a third heal bubble on each player with a copy of
// Repair Matrix's script (its effects). The heal bubble system (0x6c0 records: heal +30h,
// duration +34h, radius +38h, self heal x +3Ch, cap +40h, min x +48h, time to max +4Ch,
// elapsed +50h, params changed +660h):
//
//   +0xd66310  Update(cs): every frame, every bubble (elapsed past duration: it ends)
//   +0xca5930  ActivateHealBubbleLocally(cs, handle): every machine, local use or network
//   +0xf1bb20  ActivateHealBubble(cs, handle), through the deferred queue: broadcast to all
//
// Trigger held: tuning written (heal_per_second x beam_seconds over beam_seconds, radius =
// range + (cone_angle + 500) / 100000, no self heal), bubble activated. ConeFilter (MediCone)
// links one teammate in the cone at a time. Released, or beam_seconds up (overheat for
// overheat_seconds): the bubble ends (elapsed = duration). Every machine needs to know: the
// server heals and runs the targeting too. The state goes over VeilNet ('M': owner key in the
// low 16 bits, 1 << 16 = on, a sequence above), resent each second while on; a bubble whose
// owner has said "off", or nothing for 2.5 s, ends on every machine.
constexpr uintptr_t SLOT_RVA = 0xcea200;
constexpr uint8_t SLOT_BYTES[] = { 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xd1, 0x45, 0x0f, 0xb7 };
constexpr uintptr_t MANAGER_RET_RVA = 0xf0dc45;
constexpr uint8_t MANAGER_CALL_BYTES[] = { 0xe8, 0xbb, 0xc5, 0xdd, 0xff };   // at MANAGER_RET_RVA - 5
constexpr uintptr_t BUBBLE_UPDATE_RVA = 0xd66310;
constexpr uint8_t BUBBLE_UPDATE_BYTES[] = { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x56, 0x48, 0x81, 0xec, 0xa0, 0x00 };
constexpr uintptr_t ACTIVATE_LOCAL_RVA = 0xca5930;
constexpr uint8_t ACTIVATE_LOCAL_BYTES[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b };
constexpr uintptr_t ACTIVATE_RVA = 0xf1bb20;
constexpr uint8_t ACTIVATE_BYTES[] = { 0x40, 0x53, 0x55, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xb1, 0x80, 0x00 };
// CR15NetGunCS::FireIntegratedGunLocal (gun cs, handle, charge, ...): the gun script's fire node,
// on the shooter's client. Swallowed while this client's player has MEDIGUN: COMET's gun never fires.
constexpr uintptr_t GUN_FIRE_RVA = 0xce3670;
constexpr uint8_t GUN_FIRE_BYTES[] = { 0x40, 0x55, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xd9 };
constexpr uint64_t MEDIGUN = 0x2fda8a9b57734284ull;       // CSymbol64("medigun")
constexpr uint64_t SCOUT = 0xe32dc7c9da8051a4ull;         // CSymbol64("scout"): COMET
constexpr uint64_t MEDI_BUBBLE = 0x51ef178e73ccf82full;   // CSymbol64("echocombat_medigun_bubble")
constexpr int CONE_TAG = 500;

// --- PARTY GUNS ---------------------------------------------------------------------------
// CONFETTI GUN, BUBBLE GUN, MONEY GUN (tools/fix_party_guns.py): weapons with no gun actors of
// their own. The enable manager reads them as "assault" (Hooked_Slot), so the player holds the
// Pulsar; the shooter's client lets one shot through every PARTY_SHOT_MS (Hooked_GunFire); and on
// every machine the Pulsar's muzzle flash of a party gun's owner becomes the party effect
// (Hooked_PfxNode: CreateParticleEffect data +0 the effect, +48h the actor it is attached to).
constexpr uint64_t CONFETTI_GUN = 0x58bf8464917ddffeull;  // CSymbol64("confetti_gun")
constexpr uint64_t BUBBLE_GUN = 0xcf9247c008baa680ull;    // CSymbol64("bubble_gun")
constexpr uint64_t MONEY_GUN = 0x1b0ee501760c0660ull;     // CSymbol64("money_gun")
constexpr uint64_t ASSAULT = 0x2fd69c8c5f615b9eull;       // CSymbol64("assault"): the Pulsar
constexpr uint64_t ASSAULT_MUZZLE = 0xa8cd797f5e8693feull; // pfx_wpn_cbt_assault_muzzle_flash
constexpr uint64_t ASSAULT_BREAKER = 0x5859d9f1deba8f62ull; // pfx_wpn_cbt_assault_breaker (the shot's flame)
constexpr uint64_t CONFETTI_ORANGE = 0x3116554e1e6b17b3ull; // fx_party_confetti_orange (tools/fix_party_fx.py)
constexpr uint64_t CONFETTI_BLUE = 0x31121feed13ae191ull;   // fx_party_confetti_blue
constexpr uint64_t PARTY_EMPTY = 0x3cc02f53518abe25ull;    // fx_party_empty (a burst of no particles)
constexpr uint32_t PARTY_FN = 0x38;                       // RULES mailbox +38h: PartyEffect (the Pulsar's muzzle node)
constexpr uint32_t PARTY_GUN_FN = 0x40;                    // RULES mailbox +40h: PartyGunActor (the Pulsar's shot sound node)
constexpr uint32_t RULES_BELOW_PAGE_END = 0x140;           // the RULES mailbox (VeilNet.dll, deploy_and_launch.py)
constexpr ULONGLONG PARTY_SHOT_MS = 3000;                  // one shot, then this long before the next
inline bool PartyGun(uint64_t w) { return w == CONFETTI_GUN || w == BUBBLE_GUN || w == MONEY_GUN; }
volatile ULONGLONG g_party_shot_at = 0;                    // this machine's last party gun shot let through
// One shot per trigger pull, PARTY_SHOT_MS apart (the Pulsar is full auto): while a party gun is
// equipped the game sees the gun hand's trigger let go (LibOVR, as for MEDIGUN), except for a
// PARTY_PULSE_MS pull when the player pulls and the last pull let through is PARTY_SHOT_MS old.
// (Dropping the game's shots instead left them queued in the Pulsar script, which fired them
// later by itself, tested 2026-11-05.)
constexpr ULONGLONG PARTY_PULSE_MS = 60;
volatile ULONGLONG g_party_input_at = 0;
volatile ULONGLONG g_party_pulse_at = 0;                    // last pull let through (diagnostics)                    // last trigger reading (0: none: another runtime)
volatile ULONGLONG g_party_bullet_until = 0;               // ... no bullet of this machine's until then

// The game's item catalogue (a table of 68h-byte records: +0 unlock level, +4 Arena's, +8 Combat's)
// has no record for the party guns, so the equipment station saw them as not purchased / never
// unlocked ("Looking for purchased status of unknown item"): their cells could not be picked.
// For the three of them these answer purchased (1) and unlocked from the start (level 0).
constexpr uintptr_t PURCHASED_RVA = 0x177860;           // (this, item) -> purchased
constexpr uint8_t PURCHASED_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
constexpr uintptr_t UNLOCK_RVAS[3] = { 0x178500, 0x178560, 0x1785c0 };   // (this, item) -> Arena / Combat / unlock level
constexpr uint8_t UNLOCK_BYTES[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x20, 0xe8 };
using item_fn = int(__fastcall *)(void *self, uint64_t item);
item_fn Real_Purchased = nullptr;
item_fn Real_Unlock[3] = {};
int __fastcall Hooked_Purchased(void *self, uint64_t item) { return PartyGun(item) ? 1 : Real_Purchased(self, item); }
int __fastcall Hooked_Unlock0(void *self, uint64_t item) { return PartyGun(item) ? 0 : Real_Unlock[0](self, item); }
int __fastcall Hooked_Unlock1(void *self, uint64_t item) { return PartyGun(item) ? 0 : Real_Unlock[1](self, item); }
int __fastcall Hooked_Unlock2(void *self, uint64_t item) { return PartyGun(item) ? 0 : Real_Unlock[2](self, item); }

using slot_fn = uint64_t *(__fastcall *)(void *cs, uint64_t *out, uint32_t handle);
using update_fn = void(__fastcall *)(void *cs);
using activate_fn = void(__fastcall *)(void *cs, uint32_t handle);
slot_fn Real_Slot = nullptr;
update_fn Real_BubbleUpdate = nullptr;
activate_fn Real_ActivateLocal = nullptr;
activate_fn ActivateBubble = nullptr;
uint8_t *g_base = nullptr;

struct MediConfig {
    float heal_per_second = 8.075f, beam_seconds = 40, overheat_seconds = 3, cool_seconds = 5, range = 20, slack = 5;
    int angle = 20;
    int hand = -1;                                        // ovr hand: 0 left, 1 right, -1 the weapon arm (equipment +30h: 0 left, 1 right)
} g_medi;

volatile uint64_t g_local_weapon = 0;                     // this machine's player's weapon slot
// MEDIGUN works only where the server allows it (VeilNet.dll: PowerBoost.ini [server] medigun=1 on
// the server). Elsewhere a MEDIGUN slot is COMET for everything: no beam, the trigger and shots left
// to the game. Asked of VeilNet four times a second.
volatile bool g_medi_allowed = false;

void UpdateMedigunAllowed(ULONGLONG now) {
    static ULONGLONG next = 0;
    if (now < next) return;
    next = now + 250;
    using allowed_fn = int(__cdecl *)();
    static allowed_fn allowed = nullptr;
    if (!allowed)
        if (HMODULE vn = GetModuleHandleW(L"VeilNet.dll")) allowed = (allowed_fn)GetProcAddress(vn, "VeilNet_MedigunAllowed");
    bool a = allowed && allowed() != 0;
    if (a != g_medi_allowed) {
        g_medi_allowed = a;
        Log("MEDIGUN %s by the server", a ? "allowed" : "not allowed: it is a COMET");
    }
}
volatile uint64_t g_local_actor = 0;                      // ... and actor (its equipment's)
volatile int g_local_arm = -1;
volatile uint64_t g_weapon_actor[2] = { ~0ull, ~0ull };   // equipment record +38h / +40h
void *volatile g_equip_cs = nullptr;                      // the player equipment system
volatile bool g_gun_out = true;                           // COMET drawn (not holstered / retracted for ordnance)
volatile float g_trigger = 0;                             // the gun hand's index trigger (hidden from the game)
volatile ULONGLONG g_trigger_at = 0;
void *volatile g_bubble_cs = nullptr;

// The medigun on this client.
bool g_beam = false, g_overheated = false;
ULONGLONG g_beam_start = 0, g_overheat_until = 0, g_next_resend = 0;
uint32_t g_medi_handle = 0xffffffffu;
uint8_t *g_medi_record = nullptr;
uint32_t g_seq = 0;

// Every MEDIGUN bubble this machine has seen activated: (system, handle) -> its owner's key.
// Never kept as a record address: the heal bubble system moves its records (a handle finds its
// record through the slot table at cs+C8h), so a remembered address later held another bubble.
// Ending "the MEDIGUN bubble" by address ended a running OVERCHARGE instead (tested 2026-10-27).
struct MediBubble {
    void *cs; uint32_t handle; uint16_t key; ULONGLONG at;
    uint64_t owner;                                        // the owner's actor reference
    void *tcs; uint32_t th; ULONGLONG looked;              // its gun's root transform, when last looked up
};
MediBubble g_medi_bubbles[16];
// What each owner said last ('M' packets).
struct MediOwner { uint16_t key; bool on; ULONGLONG at; };
MediOwner g_owners[16];
SRWLOCK g_medi_lock = SRWLOCK_INIT;

uint16_t KeyOf(uint64_t actor) { return (uint16_t)(actor ^ actor >> 16 ^ actor >> 32 ^ actor >> 48); }

// MEDIGUN owners are named by player slot (0..15), as the game's own activation message does:
// actor references (+1E0h) are this machine's own (the same player is ..003f on one client and
// ..003a on another). Slot -> its player's bubble the way the game's handler (+0xd11eb0) finds
// it: the net game (space +8 -> +8518h), its slot table (+138h value, +13Ah generation), the
// slot's actor (CPlayerSlot, +0x1c76b0: r+8), and the bubble system's lookup (virtual +1A8h:
// (cs, &out, actor, component id) -> &handle; +1C8h: valid).
uint32_t SlotBubble(void *cs, int slot) {
    if (!SlotActorFn || slot < 0 || slot >= 16) return 0xffffffffu;
    __try {
        uint8_t *space = *(uint8_t **)((uint8_t *)cs + 0x80);
        uint8_t *game = *(uint8_t **)(*(uint8_t **)(space + 8) + 0x8518);
        if (!game) return 0xffffffffu;
        uint16_t value = *(uint16_t *)(game + 0x138 + slot * 4);
        if (value == 0xffff) return 0xffffffffu;
        alignas(16) uint8_t tmp[64] = {};
        uint8_t *r = SlotActorFn(game, tmp, value);
        uint64_t actor = r ? *(uint64_t *)(r + 8) : ~0ull;
        if (actor == ~0ull) return 0xffffffffu;
        alignas(16) uint8_t out[64] = {};
        using find_fn = uint32_t *(__fastcall *)(void *cs, void *out, uint64_t actor, uint64_t id);
        uint32_t *h = ((find_fn)(*(void ***)cs)[0x1a8 / 8])(cs, out, actor, MEDI_BUBBLE);
        if (!h || *h == 0xffffffffu) return 0xffffffffu;
        using valid_fn = int(__fastcall *)(void *cs, uint32_t handle);
        return ((valid_fn)(*(void ***)cs)[0x1c8 / 8])(cs, *h) ? *h : 0xffffffffu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xffffffffu;
    }
}

int SlotOfBubble(void *cs, uint32_t handle) {
    for (int slot = 0; slot < 16; ++slot)
        if ((SlotBubble(cs, slot) & 0xffff) == (handle & 0xffff)) return slot;
    return -1;
}

volatile int g_local_slot = -1;                            // this client's MEDIGUN owner key

uint8_t *BubbleRecord(void *cs, uint32_t handle) {
    uint8_t *c = (uint8_t *)cs;
    uint16_t slot = (*(uint16_t **)(c + 0xc8))[(handle & 0xffff) * 2];
    return *(uint8_t **)(c + 0xf8) + slot * 0x6c0ull;
}

// A component's actor reference (virtual +1E0h: the value the game's own network messages name
// an actor by) and component id (virtual +1F0h), both read from its descriptor (+1D0h).
uint64_t DescField(void *cs, uint32_t handle, int slot) {
    __try {
        using get_fn = uint64_t *(__fastcall *)(void *cs, uint64_t *out, uint32_t handle);
        alignas(16) uint64_t buf[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
        uint64_t *a = ((get_fn)(*(void ***)cs)[slot / 8])(cs, buf, handle);
        return a ? a[0] : ~0ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ~0ull;
    }
}
uint64_t ComponentActor(void *cs, uint32_t handle) { return DescField(cs, handle, 0x1e0); }
uint64_t ComponentId(void *cs, uint32_t handle) { return DescField(cs, handle, 0x1f0); }

// The handle of `actor`'s component `id` among the heal bubble system's records, or ~0:
// slot i's handle is (u16 [cs+D8h][i]) | its generation (u16 at [cs+C8h] + index * 4 + 2) << 16.
uint32_t FindBubble(void *cs, uint64_t actor, uint64_t id) {
    __try {
        uint8_t *c = (uint8_t *)cs;
        uint16_t count = *(uint16_t *)(c + 0xf4);
        uint16_t *index = *(uint16_t **)(c + 0xd8);
        uint16_t *table = *(uint16_t **)(c + 0xc8);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t idx = index[i];
            uint32_t h = idx | (uint32_t)table[idx * 2 + 1] << 16;
            if (ComponentActor(cs, h) == actor && ComponentId(cs, h) == id) return h;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0xffffffffu;
}

uint64_t *__fastcall Hooked_Slot(void *cs, uint64_t *out, uint32_t handle) {
    uint64_t *r = Real_Slot(cs, out, handle);
    g_equip_cs = cs;
    __try {
        uint8_t *rec = *(uint8_t **)((uint8_t *)cs + 0xf8) +
                       (*(uint16_t **)((uint8_t *)cs + 0xc8))[(handle & 0xffff) * 2] * 0x58ull;
        if (*(uint32_t *)(rec + 0x10)) {                   // this machine's player
            uint64_t weapon = *(uint64_t *)(rec + 0x18);
            int arm = *(int *)(rec + 0x30);
            uint64_t actor = ComponentActor(cs, handle);
            if (weapon != g_local_weapon || arm != g_local_arm || actor != g_local_actor)
                Log("this player: weapon %016llx%s, arm %d, actor %016llx (key %04x)", (unsigned long long)weapon,
                    weapon == MEDIGUN ? " (MEDIGUN)" : "", arm, (unsigned long long)actor, KeyOf(actor));
            uint64_t w0 = *(uint64_t *)(rec + 0x38), w1 = *(uint64_t *)(rec + 0x40);
            if (w0 != g_weapon_actor[0] || w1 != g_weapon_actor[1])
                Log("this player's weapon actor: %016llx %016llx", (unsigned long long)w0, (unsigned long long)w1);
            g_weapon_actor[0] = w0;
            g_weapon_actor[1] = w1;
            g_local_weapon = weapon;
            g_local_arm = arm;
            g_local_actor = actor;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#ifdef ECHOCOMBAT_LOG
    // Which code reads a MEDIGUN slot, and which hand each MEDIGUN player has here (a left-handed
    // MEDIGUN showed in the right hand on other machines, 2026-10-27).
    if (r && *r == MEDIGUN) {
        static uintptr_t seen[16];
        static int n = 0;
        uintptr_t ret = (uintptr_t)_ReturnAddress() - (uintptr_t)g_base;
        bool known = false;
        for (int i = 0; i < n; ++i) known |= seen[i] == ret;
        if (!known && n < 16) { seen[n++] = ret; LogDebug("MEDIGUN slot read from echovr+%llx", (unsigned long long)ret); }
        __try {
            uint8_t *rec = *(uint8_t **)((uint8_t *)cs + 0xf8) +
                           (*(uint16_t **)((uint8_t *)cs + 0xc8))[(handle & 0xffff) * 2] * 0x58ull;
            static uint64_t arms[16];                       // actor -> arm + 1, as logged
            uint64_t actor = ComponentActor(cs, handle);
            int arm = *(int *)(rec + 0x30), i = 0;
            for (; i < 16 && arms[i] && (arms[i] >> 8) != (actor & 0xffffffffffffffull); ++i) {}
            if (i < 16 && (arms[i] & 0xff) != (uint64_t)(arm + 1)) {
                arms[i] = (actor & 0xffffffffffffffull) << 8 | (uint8_t)(arm + 1);
                LogDebug("MEDIGUN player %016llx: arm %d here (%s), gun actor %016llx", (unsigned long long)actor, arm,
                    *(uint32_t *)(rec + 0x10) ? "this machine's player" : "another player",
                    (unsigned long long)*(uint64_t *)(rec + 0x40));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
#endif
    if (r && *r == MEDIGUN && (!g_medi_allowed || (uintptr_t)_ReturnAddress() == (uintptr_t)(g_base + MANAGER_RET_RVA)))
        *r = SCOUT;                                        // the enable manager: COMET's gun
    else if (r && PartyGun(*r) && (uintptr_t)_ReturnAddress() == (uintptr_t)(g_base + MANAGER_RET_RVA))
        *r = ASSAULT;                                      // the enable manager: the Pulsar
    return r;
}

void SendMedigun(bool on) {
    if (g_local_slot < 0) return;
    uint16_t key = (uint16_t)g_local_slot;
    uint32_t id = key | (on ? 1u << 16 : 0) | (++g_seq & 0x7fff) << 17;
    using send_fn2 = void(__cdecl *)(uint32_t);
    static send_fn2 send = nullptr;
    if (!send)
        if (HMODULE veilnet = GetModuleHandleW(L"VeilNet.dll")) send = (send_fn2)GetProcAddress(veilnet, "VeilNet_Medigun");
    if (send) send(id);
    AcquireSRWLockExclusive(&g_medi_lock);
    MediOwner *slot = &g_owners[0];
    for (MediOwner &o : g_owners) {
        if (o.key == key) { slot = &o; break; }
        if (o.at < slot->at) slot = &o;
    }
    *slot = { key, on, GetTickCount64() };
    ReleaseSRWLockExclusive(&g_medi_lock);
}

// Is the owner `key`'s beam on, as far as this machine knows?
bool OwnerOn(uint16_t key, ULONGLONG now) {
    bool on = false;
    AcquireSRWLockShared(&g_medi_lock);
    for (const MediOwner &o : g_owners)
        if (o.at && o.key == key) on = o.on && now - o.at < 2500;
    ReleaseSRWLockShared(&g_medi_lock);
    return on;
}

// A MEDIGUN bubble's record now, or null when its handle no longer names a MEDIGUN bubble.
uint8_t *MediRecord(const MediBubble &b) {
    if (!b.cs) return nullptr;
    __try {
        if (ComponentId(b.cs, b.handle) != MEDI_BUBBLE) return nullptr;
        return BubbleRecord(b.cs, b.handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int MediCone(uint8_t *bubble, float off, float dist, float range, int angle, void *dcs, uint32_t index) {
    ULONGLONG now = GetTickCount64();
    int key = -1;
    AcquireSRWLockShared(&g_medi_lock);
    for (const MediBubble &b : g_medi_bubbles)
        if (b.cs && MediRecord(b) == bubble) key = b.key;
    ReleaseSRWLockShared(&g_medi_lock);
    if (key < 0 || !OwnerOn((uint16_t)key, now)) return 0;
    BubbleLinks &bl = BubbleFor(bubble, now);
    bool mine = bl.count && bl.linked[0].dcs == dcs && (bl.linked[0].index & 0xffff) == (index & 0xffff);
    bool ok;
    if (mine) {                                            // the one it heals: kept a little past the cone
        ok = off <= angle + g_medi.slack && dist <= range;
        if (!ok) bl.count = 0;                             // gone: the next teammate in the cone can link
    } else if (!bl.count && off <= angle && dist <= range) {
        bl.linked[0] = { dcs, index, true, false, 0 };
        bl.count = 1;
        ok = true;
    } else {
        ok = false;
    }
    if (ok != mine) LogDebug("MEDIGUN %04x: %s %p/%u (%.1f m, %.0f deg)", key, ok ? "heals" : "lets go of", dcs,
                        index & 0xffff, dist, off);
    return ok;
}

// The heal bubble system's network activation handler (cs, msg): msg +10h -> payload { +0 game
// space id, +8 component id, +10h u16 player slot / handle, +12h generation, +14h kind }.
// Diagnostics only: does another machine's MEDIGUN activation arrive, and does it match?
constexpr uintptr_t NET_ACTIVATE_RVA = 0xd11eb0;
constexpr uint8_t NET_ACTIVATE_BYTES[] = { 0x4c, 0x8b, 0xdc, 0x56, 0x57, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00, 0x48 };
using net_activate_fn = void(__fastcall *)(void *cs, void *msg);
net_activate_fn Real_NetActivate = nullptr;

void __fastcall Hooked_NetActivate(void *cs, void *msg) {
    static volatile LONG told = 0;
    if (InterlockedIncrement(&told) <= 60) {
        __try {
            uint8_t *payload = *(uint8_t **)((uint8_t *)msg + 0x10);
            uint64_t here = *(uint64_t *)(*(uint8_t **)((uint8_t *)cs + 0x80) + 0x58);
            LogDebug("heal bubble activation from the network: space %016llx (this system's %016llx%s), component %016llx, "
                "slot %u gen %u, kind %d", (unsigned long long)*(uint64_t *)payload, (unsigned long long)here,
                here == *(uint64_t *)payload ? "" : ", NOT THIS ONE", (unsigned long long)*(uint64_t *)(payload + 8),
                *(uint16_t *)(payload + 0x10), *(uint16_t *)(payload + 0x12), *(int *)(payload + 0x14));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    Real_NetActivate(cs, msg);
}

void __fastcall Hooked_ActivateLocal(void *cs, uint32_t handle) {
    g_bubble_cs = cs;
    __try {
        uint64_t actor = ComponentActor(cs, handle);
        static volatile LONG told = 0;
        if (InterlockedIncrement(&told) <= 60)
            LogDebug("heal bubble %08x activated here: component %016llx, actor %016llx", handle,
                (unsigned long long)ComponentId(cs, handle), (unsigned long long)actor);
        if (ComponentId(cs, handle) == MEDI_BUBBLE) {
            int owner_slot = SlotOfBubble(cs, handle);
            uint16_t key = owner_slot >= 0 ? (uint16_t)owner_slot : 0xffff;
            ULONGLONG now = GetTickCount64();
            AcquireSRWLockExclusive(&g_medi_lock);
            MediBubble *slot = &g_medi_bubbles[0];
            for (MediBubble &b : g_medi_bubbles) {
                if (b.cs == cs && (b.handle & 0xffff) == (handle & 0xffff)) { slot = &b; break; }
                if (b.at < slot->at) slot = &b;
            }
            *slot = { cs, handle, key, now, actor, nullptr, 0xffffffffu, 0 };
            // heard of before its owner's packet: on until the owner says otherwise
            bool known = false;
            for (MediOwner &o : g_owners) known |= o.at && o.key == key;
            if (!known)
                for (MediOwner &o : g_owners)
                    if (!o.at || now - o.at > 10000) { o = { key, true, now }; break; }
            ReleaseSRWLockExclusive(&g_medi_lock);
            Log("MEDIGUN bubble of %016llx (player slot %d) activated here", (unsigned long long)actor, owner_slot);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Real_ActivateLocal(cs, handle);
}

// COMET's heat bar shows MEDIGUN's heat. COMET's script keeps its heat in script variables
// (heat_current, heat_max at its variable block +208h / +238h); the engine's SetExternalScriptReal
// sets one by name:
//   +0x1055c0  the game space's systems by name (space, CSymbol64("ncaScript")) -> { systems*, .., +30h count }
//   +0x468ee0  script system SetReal(cs, handle, variable, value): record (0xb8) +18h = the instance
// This player's COMET script: the script component (id "ncaScript", as every gun script's) whose actor
// is this player's weapon actor: equipment record +38h = the game space it lives in (by id, through
// +0x4f3700), +40h = the actor.
constexpr uintptr_t SPACE_BY_ID_RVA = 0x4f3700;
constexpr uint8_t SPACE_BY_ID_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x79 };
using space_fn = void *(__fastcall *)(void *space, uint64_t id);
space_fn SpaceById = nullptr;
constexpr uintptr_t SYSTEMS_FIND_RVA = 0x1055c0;
constexpr uint8_t SYSTEMS_FIND_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b };
constexpr uintptr_t SET_REAL_RVA = 0x468ee0;
constexpr uint8_t SET_REAL_BYTES[] = { 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x0f, 0x28, 0xd3, 0x48, 0x8b };
constexpr uint64_t NCA_SCRIPT = 0x6aea99eeb2bab6c4ull;    // CSymbol64("ncaScript")
constexpr uint64_t HEAT_CURRENT = 0x168d20ef5d0036a2ull;  // CSymbol64("heat_current")
using systems_fn = uint64_t *(__fastcall *)(void *space, uint64_t name);
using set_real_fn = void(__fastcall *)(void *cs, uint32_t handle, uint64_t name, float value);
systems_fn SystemsFind = nullptr;
set_real_fn SetScriptReal = nullptr;
float g_heat = 0;                                          // 0..1: MEDIGUN's heat
void *g_comet_cs = nullptr;
uint32_t g_comet_handle = 0xffffffffu;
ULONGLONG g_comet_search = 0;

// This player's COMET script (system, handle), searched at most twice a second.
bool FindComet(void *heal_cs, ULONGLONG now) {
    if (g_comet_cs && DescField(g_comet_cs, g_comet_handle, 0x1f0) == NCA_SCRIPT &&
        DescField(g_comet_cs, g_comet_handle, 0x1e0) == g_weapon_actor[1])
        return true;
    g_comet_cs = nullptr;
    if (!SystemsFind || now - g_comet_search < 500) return false;
    g_comet_search = now;
    __try {
        void *space = *(void **)((uint8_t *)heal_cs + 0x80);
        if (space && SpaceById && g_weapon_actor[0] != ~0ull) space = SpaceById(space, g_weapon_actor[0]);
        uint64_t *list = space ? SystemsFind(space, NCA_SCRIPT) : nullptr;
        static int told = 0;
        if (told < 5) {
            ++told;
            Log("MEDIGUN: gun space %p (id %016llx): %s", space, (unsigned long long)g_weapon_actor[0],
                list ? "has script systems" : "NO script systems");
        }
        if (!list) return false;
        void **systems = (void **)list[0];
        uint64_t n = list[6];
        static int logged = 0;
        for (uint64_t s = 0; s < n && s < 8; ++s) {
            uint8_t *c = (uint8_t *)systems[s];
            uint16_t count = *(uint16_t *)(c + 0xf4);
            uint16_t *index = *(uint16_t **)(c + 0xd8);
            uint16_t *table = *(uint16_t **)(c + 0xc8);
            for (uint16_t i = 0; i < count; ++i) {
                uint32_t h = index[i] | (uint32_t)table[index[i] * 2 + 1] << 16;
                uint64_t actor = DescField(c, h, 0x1e0);
                if (actor != g_weapon_actor[1] || DescField(c, h, 0x1f0) != NCA_SCRIPT) continue;
                if (logged < 40) {
                    ++logged;
                    LogDebug("COMET script %p/%08x on actor %016llx (weapon actor %016llx %016llx)", c, h,
                        (unsigned long long)actor, (unsigned long long)g_weapon_actor[0], (unsigned long long)g_weapon_actor[1]);
                }
                if (actor != ~0ull && actor == g_weapon_actor[1]) {
                    g_comet_cs = c;
                    g_comet_handle = h;
                    Log("MEDIGUN: this player's COMET script is %p/%08x: its heat bar shows the beam's", c, h);
                    return true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogWarning("MEDIGUN: looking for COMET's script faulted");
    }
    return false;
}

// The gun of a MEDIGUN bubble's owner: their equipment record (+38h its game space, +40h the gun
// actor) -> the gun's root transform ("ncaTransform") in that space -> its world pose
// (CTransformCS::get, +0x3c4470: rotation x y z w, then position). Looked up again each second.
constexpr uintptr_t TRANSFORM_GET_RVA = 0x3c4470;
constexpr uint8_t TRANSFORM_GET_BYTES[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00 };
constexpr uint64_t NCA_TRANSFORM = 0xabfe651c8d260515ull;   // CSymbol64("ncaTransform")
struct Pose { float q[4]; float p[3]; float s; };
using transform_get_fn = Pose *(__fastcall *)(void *cs, Pose *out, uint32_t handle);
transform_get_fn TransformGet = nullptr;

bool FindGunTransform(uint64_t owner, void **tcs, uint32_t *th) {
    void *equip = g_equip_cs, *heal = g_bubble_cs;
    if (!equip || !heal || !SpaceById || !SystemsFind) return false;
    __try {
        uint8_t *c = (uint8_t *)equip;
        uint16_t count = *(uint16_t *)(c + 0xf4);
        uint16_t *index = *(uint16_t **)(c + 0xd8);
        uint16_t *table = *(uint16_t **)(c + 0xc8);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t idx = index[i];
            uint32_t h = idx | (uint32_t)table[idx * 2 + 1] << 16;
            if (ComponentActor(c, h) != owner) continue;
            uint8_t *rec = *(uint8_t **)(c + 0xf8) + table[idx * 2] * 0x58ull;
            uint64_t space_id = *(uint64_t *)(rec + 0x38), gun = *(uint64_t *)(rec + 0x40);
            void *space = SpaceById(*(void **)((uint8_t *)heal + 0x80), space_id);
            uint64_t *list = space ? SystemsFind(space, NCA_TRANSFORM) : nullptr;
            if (!list) return false;
            for (uint64_t k = 0; k < list[6] && k < 8; ++k) {
                uint32_t t = FindBubble(((void **)list[0])[k], gun, NCA_TRANSFORM);
                if (t != 0xffffffffu) { *tcs = ((void **)list[0])[k]; *th = t; return true; }
            }
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

#ifdef ECHOCOMBAT_LOG
// Diagnostics: every transform in the MEDIGUN owner's gun space within 1.2 m of their head, seen
// from the head (head axes), to find one on the gun hand -- the one FindGunTransform finds is on the
// off hand (2026-10-27). Every 3 s while the beam's cone runs.
void DiagGunSpace(uint8_t *bubble, const float *hp, const float *hq) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 3000 || !TransformGet || !SpaceById || !SystemsFind || !g_bubble_cs) return;
    last = now;
    uint64_t owner = ~0ull;
    for (MediBubble &m : g_medi_bubbles)
        if (m.cs && MediRecord(m) == bubble) owner = m.owner;
    uint64_t space_id, gun;
    int arm = -1;
    if (owner == ~0ull || !GunOf(owner, &space_id, &gun, nullptr, &arm)) return;
    __try {
        void *space = SpaceById(*(void **)((uint8_t *)g_bubble_cs + 0x80), space_id);
        uint64_t *list = space ? SystemsFind(space, NCA_TRANSFORM) : nullptr;
        if (!list) return;
        LogDebug("gun space %016llx of %016llx (arm %d, equipment gun %016llx): transforms near the head:",
            (unsigned long long)space_id, (unsigned long long)owner, arm, (unsigned long long)gun);
        float inv[4] = { -hq[0], -hq[1], -hq[2], hq[3] };
        int shown = 0;
        for (uint64_t k = 0; k < list[6] && k < 8; ++k) {
            uint8_t *c = ((uint8_t **)list[0])[k];
            uint16_t count = *(uint16_t *)(c + 0xf4);
            uint16_t *index = *(uint16_t **)(c + 0xd8);
            uint16_t *table = *(uint16_t **)(c + 0xc8);
            for (uint16_t i = 0; i < count && shown < 40; ++i) {
                uint32_t h = index[i] | (uint32_t)table[index[i] * 2 + 1] << 16;
                Pose p;
                Pose *r = TransformGet(c, &p, h);
                if (!r) continue;
                float d[3] = { r->p[0] - hp[0], r->p[1] - hp[1], r->p[2] - hp[2] }, l[3];
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1.44f) continue;
                Rotate(inv, d, l);
                ++shown;
                LogDebug("  transform %p/%08x actor %016llx: %.2f %.2f %.2f from the head%s", c, h,
                    (unsigned long long)DescField(c, h, 0x1e0), l[0], l[1], l[2],
                    DescField(c, h, 0x1e0) == gun ? " (the equipment's gun)" : "");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogDebug("gun space survey faulted");
    }
}
#else
void DiagGunSpace(uint8_t *, const float *, const float *) {}
#endif

bool GunPose(uint8_t *bubble, float pos[3], float q[4]) {
    if (!TransformGet) return false;
    ULONGLONG now = GetTickCount64();
    MediBubble *b = nullptr;
    for (MediBubble &m : g_medi_bubbles)                   // (game thread: the map only changes here)
        if (m.cs && MediRecord(m) == bubble) b = &m;
    if (!b) return false;
    if (now - b->looked >= 1000) {                         // (again each second: a respawn can bring another gun)
        b->looked = now;
        void *tcs;
        uint32_t th;
        if (FindGunTransform(b->owner, &tcs, &th)) {
            if (tcs != b->tcs || th != b->th) LogDebug("MEDIGUN %04x: aims from its gun's transform %p/%08x", b->key, tcs, th);
            b->tcs = tcs;
            b->th = th;
        }
    }
    if (!b->tcs) return false;
    __try {
        Pose p;
        Pose *r = TransformGet(b->tcs, &p, b->th);
        if (!r) return false;
        memcpy(q, r->q, sizeof(float) * 4);
        memcpy(pos, r->p, sizeof(float) * 3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        b->tcs = nullptr;
        return false;
    }
}

// The gun of any heal bubble's owner (OVERCHARGE's): the bubble's record -> its handle among the
// heal bubble system's records -> the owner's actor reference -> FindGunTransform. Cached per
// record and looked up again each second, as GunPose does.
struct AimGun { uint8_t *record; uint64_t owner; void *tcs; uint32_t th; ULONGLONG looked; };
AimGun g_aim_guns[8];

bool OwnerGunPose(uint8_t *bubble, float pos[3], float q[4]) {
    void *cs = g_bubble_cs;
    if (!TransformGet || !cs) return false;
    ULONGLONG now = GetTickCount64();
    AimGun *a = nullptr, *oldest = &g_aim_guns[0];
    for (AimGun &g : g_aim_guns) {
        if (g.record == bubble) { a = &g; break; }
        if (g.looked < oldest->looked) oldest = &g;
    }
    if (!a) { a = oldest; *a = { bubble, ~0ull, nullptr, 0, 0 }; }
    if (now - a->looked >= 1000) {
        a->looked = now;
        a->tcs = nullptr;
        __try {
            uint8_t *c = (uint8_t *)cs;
            uint16_t count = *(uint16_t *)(c + 0xf4);
            uint16_t *index = *(uint16_t **)(c + 0xd8);
            uint16_t *table = *(uint16_t **)(c + 0xc8);
            for (uint16_t i = 0; i < count; ++i) {
                uint32_t h = index[i] | (uint32_t)table[index[i] * 2 + 1] << 16;
                if (BubbleRecord(cs, h) == bubble) { a->owner = ComponentActor(cs, h); break; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a->owner = ~0ull;
        }
        void *tcs;
        uint32_t th;
        if (a->owner != ~0ull && FindGunTransform(a->owner, &tcs, &th)) { a->tcs = tcs; a->th = th; }
        static int told = 0;
        if (told < 20) {
            ++told;
            LogDebug("OVERCHARGE bubble %p: owner %016llx, %s", bubble, (unsigned long long)a->owner,
                a->tcs ? "aims from its gun" : "no gun found: aims from the head");
        }
    }
    if (!a->tcs) return false;
    __try {
        Pose p;
        Pose *r = TransformGet(a->tcs, &p, a->th);
        if (!r) return false;
        memcpy(q, r->q, sizeof(float) * 4);
        memcpy(pos, r->p, sizeof(float) * 3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a->tcs = nullptr;
        return false;
    }
}

// COMET drawn? Its script's holstered_current / retracted_current (0 out .. 1 away): holding an
// ordnance in the gun hand retracts it, and then the trigger is the ordnance's.
constexpr uintptr_t GET_REAL_RVA = 0x44e8b0;
constexpr uint8_t GET_REAL_BYTES[] = { 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x44, 0x0f, 0xb7, 0xca, 0x42, 0x0f };
constexpr uint64_t HOLSTERED_CURRENT = 0xe6c90eff22a7052cull;
constexpr uint64_t RETRACTED_CURRENT = 0x294d24efe4682344ull;
using get_real_fn = float(__fastcall *)(void *cs, uint32_t handle, uint64_t name);
get_real_fn GetScriptReal = nullptr;

void UpdateGunOut() {
    if (!GetScriptReal || !g_comet_cs) { g_gun_out = true; return; }
    float holstered = 0, retracted = 0;
    __try {
        holstered = GetScriptReal(g_comet_cs, g_comet_handle, HOLSTERED_CURRENT);
        retracted = GetScriptReal(g_comet_cs, g_comet_handle, RETRACTED_CURRENT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_gun_out = true;
        return;
    }
    bool out = holstered < 0.5f && retracted < 0.5f;
    static float lh = -1, lr = -1;
    if (fabsf(holstered - lh) > 0.25f || fabsf(retracted - lr) > 0.25f || out != g_gun_out) {
        LogDebug("COMET holstered %.2f retracted %.2f: %s", holstered, retracted, out ? "drawn" : "away (trigger left to the game)");
        lh = holstered;
        lr = retracted;
    }
    g_gun_out = out;
}

// COMET's manual vent (its vent button): the script sets `venting` and drains heat_current at its
// venting rate until it is empty. Writing heat_current every frame kept it from ever emptying (the
// vent effect never ended) while the beam carried on. While COMET vents -- `venting` set, or its
// heat below what was written last frame -- the beam is off, nothing is written, and MEDIGUN's heat
// follows COMET's down; the beam can come back once the vent is over.
constexpr uint64_t VENTING = 0x2fc18a914a7d598dull;       // CSymbol64("venting")
constexpr uint64_t HEAT_MAX = 0x3ee8950316f2857eull;      // CSymbol64("heat_max")
volatile bool g_venting = false;

void ShowHeat(void *heal_cs, ULONGLONG now) {
    if (!SetScriptReal || !FindComet(heal_cs, now)) return;
    static float heat_max = 100.0f;
    static float written = -1.0f;
    static ULONGLONG written_at = 0;
    static int faults = 0;
    __try {
        if (GetScriptReal) {
            float m = GetScriptReal(g_comet_cs, g_comet_handle, HEAT_MAX);
            if (m > 1.0f && m < 10000.0f) heat_max = m;
            float cur = GetScriptReal(g_comet_cs, g_comet_handle, HEAT_CURRENT);
            // (a drop faster than VENT_RATE per second: COMET's own cooling is 20/s, its manual vent 50/s)
            constexpr float VENT_RATE = 35.0f;
            float dt = written_at && now > written_at ? (now - written_at) / 1000.0f : 0.0f;
            bool dropping = written >= 0 && dt > 0 && dt < 0.25f && (written - cur) / dt > VENT_RATE;
            bool venting = GetScriptReal(g_comet_cs, g_comet_handle, VENTING) > 0.5f ||
                           (g_venting ? cur < written - 0.01f : dropping && !g_overheated);
            written_at = now;
            if (venting != g_venting) Log("MEDIGUN: COMET %s (heat %.1f, written %.1f)", venting ? "venting" : "vent over", cur, written);
            g_venting = venting;
            if (venting) {                                // COMET's vent runs its course
                g_heat = cur / heat_max;
                written = cur;
                return;
            }
        }
        written = g_heat * heat_max;
        SetScriptReal(g_comet_cs, g_comet_handle, HEAT_CURRENT, written);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (faults++ < 5) LogWarning("MEDIGUN: setting COMET's heat faulted (%08x)", GetExceptionCode());
        g_comet_cs = nullptr;
        return;
    }
    static ULONGLONG logged = 0;
    if (now - logged > 2000 && g_heat > 0) {
        logged = now;
        LogDebug("MEDIGUN heat %.2f -> COMET heat_current %.1f", g_heat, g_heat * heat_max);
    }
}

// The beam leaves the gun hand. Repair Matrix's beam effect is attached to its player's model at
// the off hand's joint (picked at run time). Before the engine resolves that attachment
// (+0x1286d30, CreateParticleEffect node: (node ref, data)), a MEDIGUN player's beam is moved to
// the other hand, the one holding the gun.
constexpr uintptr_t RESOLVE_ATTACH_RVA = 0x1286d30;
constexpr uint8_t RESOLVE_ATTACH_BYTES[] = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
constexpr uint64_t NCA_MODEL = 0x38ee951a26fb816aull;
constexpr uint64_t HEAL_BEAM = 0x39424707bb2df316ull;    // pfx_test_jason_cbt_offhand_heal_beam_plr
constexpr uint64_t BLUE_BEAM = 0xaccda1a9cfb868c1ull;    // fx_medigun_beam: MEDIGUN's blue copy (fix_medigun)
constexpr uint64_t HAND_L1 = 0xd3009aa0bc77fd03ull;      // EXP_L1_Hand1
constexpr uint64_t HAND_R1 = 0xcd009aa0bc77fd03ull;      // EXP_R1_Hand1
constexpr uint64_t TOOL_L1 = 0x46ff0ac8fa842b21ull;      // EXP_L1_ToolAttach1 (fix_medigun's gun bone)
constexpr uint64_t TOOL_R1 = 0xeb587bb25bc85873ull;      // EXP_R1_ToolAttach1
constexpr uint64_t COMET_MUZZLE = 0x17b85ed52e0ec3beull;  // the ref point COMET's muzzle flash uses
using resolve_fn = void(__fastcall *)(void *node, uint8_t *data);
resolve_fn Real_Resolve = nullptr;

// A player's (by actor reference) gun: equipment record +38h space id, +40h actor reference.
bool GunOf(uint64_t player, uint64_t *space_id, uint64_t *gun, uint64_t *weapon, int *arm);

// Does `shooter` (a player actor) hold a party gun? (no damage from their shots)
bool PartyShooter(uint64_t shooter) {
    uint64_t space_id, gun, weapon = 0;
    return GunOf(shooter, &space_id, &gun, &weapon) && PartyGun(weapon);
}

bool GunOf(uint64_t player, uint64_t *space_id, uint64_t *gun, uint64_t *weapon, int *arm) {
    void *equip = g_equip_cs;
    if (!equip) return false;
    __try {
        uint8_t *c = (uint8_t *)equip;
        uint16_t count = *(uint16_t *)(c + 0xf4);
        uint16_t *index = *(uint16_t **)(c + 0xd8);
        uint16_t *table = *(uint16_t **)(c + 0xc8);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t idx = index[i];
            uint32_t h = idx | (uint32_t)table[idx * 2 + 1] << 16;
            if (ComponentActor(c, h) != player) continue;
            uint8_t *rec = *(uint8_t **)(c + 0xf8) + table[idx * 2] * 0x58ull;
            *space_id = *(uint64_t *)(rec + 0x38);
            *gun = *(uint64_t *)(rec + 0x40);
            if (weapon) *weapon = *(uint64_t *)(rec + 0x18);
            if (arm) *arm = *(int *)(rec + 0x30);
            return *gun != ~0ull && *space_id != ~0ull;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Is `player` (this machine's actor reference) holding MEDIGUN? Its equipment says so, or it has
// a MEDIGUN bubble active here (another machine's player: their equipment may not be known here).
// The beam's start in the gun hand bone's space: PowerBoost.ini [medigun] hand_offset=x,y,z (metres),
// read again every second and applied every frame, so it can be tuned while the beam is on. (The
// gun's own transform can't place a muzzle: it sat on the left hand with the gun in the right.)
volatile float g_hand_offset[3] = { 0, 0, 0 };
volatile float g_hand_offset_left[3] = { 0, 0, 0 };   // [medigun] hand_offset_left: the left hand's (its bone is mirrored)

void ReadOffset(const char *key, const char *def, volatile float *out, const char *what) {
    char v[64];
    GetPrivateProfileStringA("medigun", key, def, v, sizeof(v), g_ini_path);
    float x = 0, y = 0, z = 0;
    if (sscanf_s(v, "%f,%f,%f", &x, &y, &z) == 3 && (x != out[0] || y != out[1] || z != out[2])) {
        out[0] = x;
        out[1] = y;
        out[2] = z;
        Log("MEDIGUN %s: %.3f %.3f %.3f", what, x, y, z);
    }
}

void ReadBeamBone();

void ReadHandOffset() {
    ReadOffset("hand_offset", "0,0,0", g_hand_offset, "beam offset from the gun hand bone");
    char dv[64];
    sprintf_s(dv, "%g,%g,%g", g_hand_offset[0], g_hand_offset[1], g_hand_offset[2]);
    ReadOffset("hand_offset_left", dv, g_hand_offset_left, "beam offset from the left gun hand bone");
    ReadBeamBone();
}

bool MedigunPlayer(uint64_t player) {
    uint64_t space_id, gun, weapon = 0;
    if (GunOf(player, &space_id, &gun, &weapon) && weapon == MEDIGUN) return true;
    ULONGLONG now = GetTickCount64();
    bool found = false;
    AcquireSRWLockShared(&g_medi_lock);
    for (const MediBubble &b : g_medi_bubbles)
        if (b.cs && (b.owner & 0xffff) == (player & 0xffff) && now - b.at < 60000) found = true;   // (index: upper bits vary)
    ReleaseSRWLockShared(&g_medi_lock);
    return found;
}

// The node's own handler (+0x10aba30, thread: +18h state, +B0h data): its state 1 creates the
// effect with the local transform in data (+18h position), then resolves the attachment. The
// muzzle offset has to be in data before the effect is created.
constexpr uintptr_t PFX_NODE_RVA = 0x10aba30;
constexpr uint8_t PFX_NODE_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c };
using pfx_node_fn = uint64_t(__fastcall *)(int64_t *thread);
pfx_node_fn Real_PfxNode = nullptr;

// The attachment's world pose: the node's own lookup (+0x12357b0: (data + 230h, &out, data) ->
// &out, rotation x y z w then position), which state 1 calls for an effect spawned in world space.
constexpr uintptr_t ATTACH_POSE_RVA = 0x12357b0;
constexpr uint8_t ATTACH_POSE_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57 };
using attach_pose_fn = Pose *(__fastcall *)(uint8_t *node, Pose *out, uint8_t *data);
attach_pose_fn AttachPose = nullptr;

// In state 2 (every frame) the node writes data +18h (position), +28h.. (rotation, degrees) and
// +30h.. (scale) into the effect as its offset from the joint it follows (+60h, set once, when the
// effect is first attached: Hooked_Resolve). The game reuses a beam effect, so after a hand switch
// it still followed the old hand's bone (tested 2026-10-27: switched to the right, beam on the left).
// So every frame, while a MEDIGUN player's weapon arm differs from the bone the beam follows, the
// offset carries it from that bone to the weapon arm's: local = joint^-1 * (arm bone - joint).
struct BeamHand { uint8_t *data; uint64_t player; bool medigun; ULONGLONG checked; int arm; ULONGLONG told; };
BeamHand g_beams[8];

uint64_t ArmBone(int arm) { return arm == 1 ? TOOL_R1 : TOOL_L1; }

// The offset from the attached bone to `bone`, in the attached bone's space.
bool BoneOffset(uint8_t *data, uint64_t bone, float local[3]) {
    if (!AttachPose) return false;
    uint64_t joint = *(uint64_t *)(data + 0x60);
    __try {
        alignas(16) Pose at = {}, to = {};
        Pose *j = AttachPose(data + 0x230, &at, data);
        *(uint64_t *)(data + 0x60) = bone;
        Pose *t = AttachPose(data + 0x230, &to, data);
        *(uint64_t *)(data + 0x60) = joint;
        if (!j || !t) return false;
        float d[3] = { t->p[0] - j->p[0], t->p[1] - j->p[1], t->p[2] - j->p[2] };
        float inv[4] = { -j->q[0], -j->q[1], -j->q[2], j->q[3] };
        Rotate(inv, d, local);
        return local[0] * local[0] + local[1] * local[1] + local[2] * local[2] < 4.0f;   // (hands under 2 m apart)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *(uint64_t *)(data + 0x60) = joint;
        return false;
    }
}

// The party gun (or 0) of the player whose gun actor is `gun` or one of its first children (the
// muzzle flash is attached to the gun's model, the gun actor + 1): their equipment record's +40h.
// *local: that player is this machine's.
uint64_t PartyWeaponOfGun(uint64_t gun, bool *local) {
    *local = false;
    uint8_t *c = (uint8_t *)g_equip_cs;
    if (!c || gun == ~0ull) return 0;
    __try {
        uint16_t count = *(uint16_t *)(c + 0xf4);
        uint16_t *index = *(uint16_t **)(c + 0xd8);
        uint16_t *table = *(uint16_t **)(c + 0xc8);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t idx = index[i];
            uint8_t *rec = *(uint8_t **)(c + 0xf8) + table[idx * 2] * 0x58ull;
            uint64_t g = *(uint64_t *)(rec + 0x40);
            if ((g >> 16) != (gun >> 16) || (gun & 0xffff) < (g & 0xffff) || (gun & 0xffff) - (g & 0xffff) > 4) continue;
            uint64_t w = *(uint64_t *)(rec + 0x18);
            *local = *(uint32_t *)(rec + 0x10) != 0;
            return PartyGun(w) ? w : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// The muzzle flash's team tint (the gun script hands it each team's pair): more blue than red = blue.
// Its place in the node data is looked for once: a colour triple (r, g, b) in [0, 1.5] whose
// largest part is 1, as both teams' stock tints are.
bool BlueTint(const uint8_t *data, bool *known) {
    *known = false;
    for (int o = 0x60; o + 12 <= 0x230; o += 4) {
        const float *f = (const float *)(data + o);
        bool range = true;
        for (int k = 0; k < 3; ++k) range &= f[k] >= 0.0f && f[k] <= 1.5f;
        if (!range) continue;
        float m = f[0] > f[1] ? (f[0] > f[2] ? f[0] : f[2]) : (f[1] > f[2] ? f[1] : f[2]);
        if (m < 0.95f || m > 1.05f || (f[0] < 0.9f && f[2] < 0.9f)) continue;
        if (f[0] > 0.9f && f[2] > 0.9f) continue;          // white / pink: not a team tint
        *known = true;
        return f[2] > f[0];
    }
    return false;
}

// The Pulsar script's muzzle-flash node asks this (tools/fix_party_fx.py, RULES mailbox +38h) which
// effect to spawn: gun = the gun actor (script instance +3B0h), stock = the muzzle flash, inst = the
// script's instance data (+220h the team's begin colour). Not a party gun: the stock flash. A party
// gun: the team's confetti for a shot, at most one burst per PARTY_BURST_MS per gun; else 0 (the
// node spawns nothing and finishes: a new effect from the node replaces its last one, so anything
// spawned on the frames after a shot killed the confetti at once).
constexpr ULONGLONG PARTY_BURST_MS = 2000;
struct BurstAt { uint64_t gun; ULONGLONG at; };
BurstAt g_burst_at[32];
uint64_t __fastcall PartyEffect(uint64_t gun, uint64_t stock, const uint8_t *inst) {
    __try {
        bool local = false;
        uint64_t weapon = PartyWeaponOfGun(gun, &local);
        static volatile LONG asked = 0;
        if (InterlockedIncrement(&asked) <= 20)
            Log("PARTY GUN: muzzle node of %016llx asks (party weapon %016llx, %s, last shot %llu ms ago)",
                (unsigned long long)gun, (unsigned long long)weapon, local ? "local" : "remote",
                g_party_shot_at ? (unsigned long long)(GetTickCount64() - g_party_shot_at) : 0ull);
        if (!weapon) return stock;
        // this machine's own gun runs the node every trigger frame, shot or not: only the one right
        // after a shot let through (Hooked_GunFire) gets the confetti
        ULONGLONG now = GetTickCount64();
        if (local) {
            ULONGLONG shot = g_party_shot_at;
            if (!shot || now - shot > 300) return 0;
            g_party_shot_at = 0;
        }
        BurstAt *b = &g_burst_at[(gun ^ (gun >> 16)) & 31];  // every gun: one burst per PARTY_BURST_MS
        if (b->gun == gun && now - b->at < PARTY_BURST_MS) return 0;
        *b = { gun, now };
        const float *tint = (const float *)(inst + 0x220);   // blue #194bff, orange #ff7d52
        bool blue = tint[2] > tint[0] + 0.2f;
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 40)
            Log("PARTY GUN: %016llx's shot -> %s confetti (%s, tint %.2f %.2f %.2f)", (unsigned long long)gun,
                blue ? "blue" : "orange", local ? "local" : "remote", tint[0], tint[1], tint[2]);
        return blue ? CONFETTI_BLUE : CONFETTI_ORANGE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return stock;
    }
}

// The Pulsar script's shot sound, flame and muzzle light nodes ask this (RULES mailbox +40h): is
// `gun` (the gun actor) a party-gun owner's? Then no sound, no flame, no light. Looked up at most every 100 ms per gun.
struct GunCache { uint64_t gun; ULONGLONG at; bool party; };
GunCache g_gun_cache[32];
int __fastcall PartyGunActor(uint64_t gun) {
    __try {
        ULONGLONG now = GetTickCount64();
        GunCache *c = &g_gun_cache[(gun ^ (gun >> 16)) & 31];
        if (c->gun != gun || now - c->at > 100) {
            bool local = false;
            *c = { gun, now, PartyWeaponOfGun(gun, &local) != 0 };
        }
        return c->party;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

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

// At CreateParticleEffect's state 1 (the spawn data built): the party effect PartyEffect picked gets
// [party] rot / scale.
void PartySwap(int64_t *thread) {
    uint8_t *data = (uint8_t *)thread[0x16];
#ifdef ECHOCOMBAT_LOG
    if (data) {                                            // every effect id / state seen, once each
        static uint64_t seen[64];
        static int nseen = 0;
        uint64_t key = *(uint64_t *)data ^ (uint64_t)thread[3];
        bool known = false;
        for (int i = 0; i < nseen; ++i) known |= seen[i] == key;
        if (!known && nseen < 64) {
            seen[nseen++] = key;
            Log("PFX node: effect %016llx state %lld attached to %016llx", (unsigned long long)*(uint64_t *)data,
                (long long)thread[3], (unsigned long long)*(uint64_t *)(data + 0x48));
        }
    }
#endif
    if (thread[3] != 1 || !data) return;
    uint64_t fx = *(uint64_t *)data;
    float *sc = (float *)(data + 0x30);
    if (fx != CONFETTI_BLUE && fx != CONFETTI_ORANGE) return;
    // its direction: [party] rot = the effect's rotation (degrees x y z, node data +24h; the burst
    // fires along its own +Y, the muzzle flash along the barrel); its size: [party] scale (live),
    // only where all three read 1, so a wrong guess of the layout changes nothing
    char v[32];
    GetPrivateProfileStringA("party", "rot", "90,0,0", v, sizeof(v), g_ini_path);
    float r[3] = { 0, 0, 0 };
    if (sscanf_s(v, "%f,%f,%f", &r[0], &r[1], &r[2]) == 3) memcpy(data + 0x24, r, sizeof(r));
    GetPrivateProfileStringA("party", "scale", "1", v, sizeof(v), g_ini_path);
    float k = (float)atof(v);
    bool scaled = k > 0.0f && sc[0] == 1.0f && sc[1] == 1.0f && sc[2] == 1.0f;
    if (scaled) sc[0] = sc[1] = sc[2] = k;
    static volatile LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 20)
        Log("PARTY GUN: confetti spawning, rot %.0f %.0f %.0f, %s", r[0], r[1], r[2],
            scaled ? "scaled" : "scale left (not 1 1 1 at +30h)");
}

// CREATEPARTICLEEFFECTNODE's own handler (+0xdda7f0; read from a loaded script's nodefunction slot):
// the gun scripts' muzzle flashes go through it, not through +0x10aba30. Same thread layout: +18h
// state (0 defaults, 1 creates the effect from data, 2 runs), +B0h data (+0 effect, +30h scale,
// +48h the actor it is attached to).
constexpr uintptr_t CREATE_PFX_RVA = 0xdda7f0;
constexpr uint8_t CREATE_PFX_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c };
pfx_node_fn Real_CreatePfx = nullptr;
uint64_t __fastcall Hooked_CreatePfx(int64_t *thread) {
    __try {
        PartySwap(thread);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return Real_CreatePfx(thread);
}

uint64_t __fastcall Hooked_PfxNode(int64_t *thread) {
    __try {
        PartySwap(thread);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        uint8_t *data = (uint8_t *)thread[0x16];
        int64_t state = thread[3];
        if ((state == 1 || state == 2) && data && (*(uint64_t *)data == HEAL_BEAM || *(uint64_t *)data == BLUE_BEAM) &&
            *(uint64_t *)(data + 0x58) == NCA_MODEL) {
            uint64_t player = *(uint64_t *)(data + 0x48);
            ULONGLONG now = GetTickCount64();
            BeamHand *b = nullptr, *oldest = &g_beams[0];
            for (BeamHand &x : g_beams) {
                if (x.data == data && x.player == player) { b = &x; break; }
                if (x.checked < oldest->checked) oldest = &x;
            }
            if (!b || state == 1) {                         // a new beam (node data is reused)
                b = b ? b : oldest;
                *b = { data, player, MedigunPlayer(player), 0, -1, 0 };
            }
            if (b->medigun) {
                if (now - b->checked >= 250) {              // the weapon arm, four times a second
                    b->checked = now;
                    uint64_t space_id, gun;
                    int arm = -1;
                    if (GunOf(player, &space_id, &gun, nullptr, &arm) && (arm == 0 || arm == 1)) b->arm = arm;
                }
                float *local = (float *)(data + 0x18);
                uint64_t joint = *(uint64_t *)(data + 0x60);
                float l[3] = { g_hand_offset[0], g_hand_offset[1], g_hand_offset[2] };
                if (state == 2 && b->arm >= 0 && (joint == TOOL_L1 || joint == TOOL_R1) && joint != ArmBone(b->arm)) {
                    bool ok = BoneOffset(data, ArmBone(b->arm), l);
                    if (now - b->told >= 2000) {
                        b->told = now;
                        LogDebug("MEDIGUN beam of %016llx follows %s, arm %d: %s (%.2f %.2f %.2f)", (unsigned long long)player,
                            joint == TOOL_L1 ? "EXP_L1" : "EXP_R1", b->arm, ok ? "carried to the arm's bone" : "carry failed",
                            l[0], l[1], l[2]);
                    }
                    if (!ok) l[0] = l[1] = l[2] = 0;
                }
                local[0] = l[0];
                local[1] = l[1];
                local[2] = l[2];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return Real_PfxNode(thread);
}

// Hand bones a MEDIGUN beam can start from ([medigun] beam_bone=<name>, default ToolAttach1),
// as (name, EXP_L1_ id, EXP_R1_ id). The beam's own spawn position is ignored (tested 2026-10-27:
// an offset of 10 along the bone moved nothing), so the bone is the only way to move its start.
struct HandBone { const char *name; uint64_t left, right; };
constexpr HandBone HAND_BONES[] = {
    { "Arm2", 0x1dba667218d878d8ull, 0x1da4667218d878d8ull },
    { "Hand1", 0xd3009aa0bc77fd03ull, 0xcd009aa0bc77fd03ull },
    { "ToolAttach1", 0x46ff0ac8fa842b21ull, 0xeb587bb25bc85873ull },
    { "HandPistol1", 0x5be79dc0cfff02c7ull, 0xf640ecba6eb37195ull },
    { "HandRifle1", 0xb7f94fe89f8c5149ull, 0x5a99e26370afb77dull },
    { "HandBFG1", 0xecaac5c1a63c2003ull, 0x072ed019bfb27561ull },
    { "HandBlade1", 0xb7f95fed98845149ull, 0x5a99f26677a7b77dull },
    { "HandGrenade1", 0xb47f7f59709351ffull, 0x084d6bbb9bae9e2dull },
    { "Index3", 0xb23913097310a6adull, 0xa6b4914deda46021ull },
    { "Middle3", 0x1d80c42770df3307ull, 0x88f47cc12f3ec40full },
    { "Thumb3", 0xb2390e0f6218bcadull, 0xa6b48c4bfcac7a21ull },
    { "ArmGameIkHand1", 0xb70c2c451eca4a9dull, 0x896997442a84446bull },
    { "ArmGameIkOppHand1", 0x8ebf4ebd516f0af1ull, 0xce2a3ea19217d85full },
    { "ArmGameIkOppHandItem1", 0xcfda725f29f5211dull, 0xacf456a754b13c1full },
    { "TouchController1", 0x5ecb29288b902243ull, 0xf56b29e33f276567ull },
    { "PalmPad1", 0xf4aac7c8b43b2303ull, 0x1f2ed210adb57661ull },
    { "Hand1Stretch1", 0x39227ec34ea78225ull, 0x8ebeb10d05c8683dull },
};
volatile int g_beam_bone = 2;                              // HAND_BONES index (ToolAttach1)

void ReadBeamBone() {
    char v[64];
    GetPrivateProfileStringA("medigun", "beam_bone", "ToolAttach1", v, sizeof(v), g_ini_path);
    for (int i = 0; i < (int)(sizeof(HAND_BONES) / sizeof(HAND_BONES[0])); ++i)
        if (_stricmp(v, HAND_BONES[i].name) == 0 && i != g_beam_bone) {
            g_beam_bone = i;
            Log("MEDIGUN beam bone: %s", HAND_BONES[i].name);
        }
}

// Where each hand bone is, ahead of Hand1 along the forearm (Arm2 -> Hand1) and off that line,
// for the side the beam is on: which bone is nearest the muzzle. Logged for the first beams.
void SurveyBones(void *node, uint8_t *data, bool right) {
    uint64_t joint = *(uint64_t *)(data + 0x60);
    alignas(16) Pose arm = {}, hand = {}, b = {};
    auto at = [&](uint64_t bone, Pose *out) {
        *(uint64_t *)(data + 0x60) = bone;
        return AttachPose((uint8_t *)node, out, data) != nullptr;
    };
    bool ok = at(right ? HAND_BONES[0].right : HAND_BONES[0].left, &arm) && at(right ? HAND_BONES[1].right : HAND_BONES[1].left, &hand);
    if (ok) {
        float f[3] = { hand.p[0] - arm.p[0], hand.p[1] - arm.p[1], hand.p[2] - arm.p[2] };
        float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (n > 1e-4f) {
            for (int k = 0; k < 3; ++k) f[k] /= n;
            for (const HandBone &hb : HAND_BONES) {
                if (!at(right ? hb.right : hb.left, &b)) { LogDebug("  bone %s: no pose", hb.name); continue; }
                float d[3] = { b.p[0] - hand.p[0], b.p[1] - hand.p[1], b.p[2] - hand.p[2] };
                float ahead = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
                float off = sqrtf(fmaxf(0.0f, d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - ahead * ahead));
                LogDebug("  bone %-22s %s: %.3f m ahead of the hand, %.3f m off the forearm line", hb.name, right ? "R" : "L", ahead, off);
            }
        }
        // the beam's bone's own axes, in the forearm's frame: forward (Arm2 -> Hand1), up (toward
        // the thumb, off the forearm line), right = forward x up. hand_offset is in these axes.
        alignas(16) Pose th = {}, jb = {};
        if (n > 1e-4f && at(right ? HAND_BONES[10].right : HAND_BONES[10].left, &th) && at(joint, &jb)) {
            float u[3] = { th.p[0] - hand.p[0], th.p[1] - hand.p[1], th.p[2] - hand.p[2] };
            float al = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
            for (int k = 0; k < 3; ++k) u[k] -= al * f[k];
            float un = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            if (un > 1e-4f) {
                for (float &x : u) x /= un;
                float r[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
                static const float ax[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
                for (int i = 0; i < 3; ++i) {
                    float w[3];
                    Rotate(jb.q, ax[i], w);
                    LogDebug("  beam bone %c axis = %.2f forward, %.2f up, %.2f right", "xyz"[i],
                        w[0] * f[0] + w[1] * f[1] + w[2] * f[2], w[0] * u[0] + w[1] * u[1] + w[2] * u[2],
                        w[0] * r[0] + w[1] * r[1] + w[2] * r[2]);
                }
            }
        }
    } else {
        LogDebug("  bone survey: no pose for the arm / hand");
    }
    *(uint64_t *)(data + 0x60) = joint;
}

// The beam's start past its bone. Its spawn position moves nothing and no bone sits at the muzzle
// (fingertip: far enough but low; thumb: high enough but short -- tested 2026-10-27), but the
// effect instance's own offset from its bone (+338h) does move it. Set every frame to [medigun]
// hand_offset (x, y, z metres in the bone's space, reread every second): a fixed offset keeps the
// beam rigidly on the gun. (Working the muzzle out each frame from other bones' poses jumped about:
// those poses don't all come from the same frame -- the beam glitched, tested 2026-10-27.)
struct LiveBeam { uint8_t *data; uint8_t *inst; bool right; ULONGLONG told; };
LiveBeam g_live[4];

void PlaceBeams(ULONGLONG now) {
    for (LiveBeam &b : g_live) {
        if (!b.data) continue;
        __try {
            uint8_t *inst = *(uint8_t **)(b.data + 0x240);
            if (*(uint64_t *)b.data != BLUE_BEAM || !inst || inst != b.inst) { b.data = nullptr; continue; }
            const volatile float *o = b.right ? g_hand_offset : g_hand_offset_left;
            float local[3] = { o[0], o[1], o[2] };
            memcpy(inst + 0x338, local, sizeof(local));
            if (now - b.told >= 5000) {
                b.told = now;
                LogDebug("MEDIGUN beam %p at %.3f %.3f %.3f from its bone", b.data, local[0], local[1], local[2]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            b.data = nullptr;
        }
    }
}

void __fastcall Hooked_Resolve(void *node, uint8_t *data) {
    // data (logged 2026-10-19): +0 effect id, +40h actor (space id, actor reference, prefab id),
    // +58h component (ncaModel), +60h joint (the off hand: EXP_L1_Hand1, picked at run time),
    // +70h attach type (2 joint). Attaching to the gun itself fails (it lives in another game space,
    // the beam shows nowhere), so the beam moves to the gun hand's joint on the same model.
    __try {
        if (*(uint64_t *)data == HEAL_BEAM && *(uint64_t *)(data + 0x58) == NCA_MODEL) {
            uint64_t player = *(uint64_t *)(data + 0x48), joint = *(uint64_t *)(data + 0x60);
            bool medigun = MedigunPlayer(player);
            static volatile LONG told = 0;
            if (InterlockedIncrement(&told) <= 30)
                LogDebug("heal beam of %016llx (joint %016llx): %s", (unsigned long long)player, (unsigned long long)joint,
                    medigun ? "MEDIGUN -> the gun hand" : "not a MEDIGUN player, left alone");
            if (medigun && (joint == HAND_L1 || joint == HAND_R1))
                *(uint64_t *)(data + 0x60) = joint == HAND_L1 ? HAND_R1 : HAND_L1;   // the other hand: the gun's
        }
        // fix_medigun hangs MEDIGUN's effects on EXP_L1_ToolAttach1. EXP_L1_* is the player's left
        // hand and EXP_R1_* the right (tested 2026-10-27: left-handed on L1 -> the gun hand; moved
        // to R1 -> the empty right hand; right-handed on L1 -> the empty left hand), so the bone
        // follows the equipment's weapon arm (+30h: 0 left, 1 right). (The gun's own transform is
        // no guide: it sat on EXP_L1 with the gun in the right hand.)
        uint64_t joint = *(uint64_t *)(data + 0x60);
        if ((joint == TOOL_L1 || joint == TOOL_R1) && *(uint64_t *)(data + 0x58) == NCA_MODEL) {
            uint64_t player = *(uint64_t *)(data + 0x48), space_id, gun;
            int arm = -1;
            bool medigun = MedigunPlayer(player);
            if (medigun && GunOf(player, &space_id, &gun, nullptr, &arm) && (arm == 0 || arm == 1)) {
                const HandBone &hb = HAND_BONES[g_beam_bone];
                *(uint64_t *)(data + 0x60) = arm == 1 ? hb.right : hb.left;
                static volatile LONG surveyed = 0;
                if (*(uint64_t *)data == BLUE_BEAM && AttachPose && InterlockedIncrement(&surveyed) <= 3) {
                    LogDebug("MEDIGUN beam on %s_%s; hand bones:", arm == 1 ? "EXP_R1" : "EXP_L1", hb.name);
                    SurveyBones(node, data, arm == 1);
                }
            }
            // (the beam's offset from the bone is the script's: fix_medigun.beam_offset)
            static volatile LONG told = 0;
            if (InterlockedIncrement(&told) <= 60)
                LogDebug("MEDIGUN effect %016llx of %016llx: arm %d -> %s", (unsigned long long)*(uint64_t *)data,
                    (unsigned long long)player, arm, arm == 1 ? "EXP_R1_ToolAttach1 (right)" :
                    arm == 0 ? "EXP_L1_ToolAttach1 (left)" : "hand not known here, left alone");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Real_Resolve(node, data);
    __try {                                                  // a MEDIGUN beam: placed at the muzzle each frame
        uint64_t joint = *(uint64_t *)(data + 0x60);
        uint8_t *inst = *(uint8_t **)(data + 0x240);
        bool right = false, mine = false;
        for (const HandBone &hb : HAND_BONES) {
            if (joint == hb.right) { right = true; mine = true; }
            if (joint == hb.left) mine = true;
        }
        if (*(uint64_t *)data == BLUE_BEAM && mine && inst) {
            LiveBeam *slot = &g_live[0];
            for (LiveBeam &b : g_live) {
                if (b.data == data) { slot = &b; break; }
                if (!b.data) slot = &b;
            }
            *slot = { data, inst, right, 0 };
            Log("MEDIGUN beam %p attached (instance %p, %s hand)", data, inst, right ? "right" : "left");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The owner's side, on the game's thread (the heal bubble system's update).
void MedigunTick(void *cs, ULONGLONG now) {
    if (g_server) return;
    bool medigun = g_local_weapon == MEDIGUN && g_medi_allowed;
    float t = now - g_trigger_at < 500 ? g_trigger : 0.0f;
    static bool pressed = false;
    pressed = medigun && (pressed ? t > 0.35f : t > 0.55f);
    // Heat: fills over beam_seconds of beam; full -> overheated for overheat_seconds (it drains
    // over them); otherwise it cools over cool_seconds, starting a second after the beam stops.
    static ULONGLONG last_tick = 0, beam_end = 0;
    float dt = last_tick && now > last_tick ? (now - last_tick) / 1000.0f : 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    last_tick = now;
    if (g_overheated) {
        if (now >= g_overheat_until) {
            g_overheated = false;
            g_heat = 0;
            Log("MEDIGUN overheat over");
        } else {
            g_heat = (g_overheat_until - now) / (g_medi.overheat_seconds * 1000.0f);
        }
    } else if (g_beam) {
        g_heat += dt / g_medi.beam_seconds;
    } else if (now - beam_end > 1000) {
        g_heat -= dt / g_medi.cool_seconds;
    }
    if (g_heat < 0) g_heat = 0;
    bool want = pressed && !g_overheated && !g_venting && g_local_actor && g_gun_out;
    // A beam effect is attached to a hand once, when the link makes it, and stays on that hand:
    // after a hand switch it carried on from the empty one. A change of weapon arm drops the beam
    // for a frame, so the next link makes a new one on the new gun hand.
    static int beam_arm = -1;
    if (g_beam && beam_arm >= 0 && g_local_arm != beam_arm) {
        Log("MEDIGUN: weapon arm %d -> %d with the beam on: restarting it on the new hand", beam_arm, (int)g_local_arm);
        want = false;
    }
    beam_arm = g_local_arm;
    if (g_beam && g_heat >= 1.0f) {
        g_heat = 1.0f;
        g_overheated = true;
        g_overheat_until = now + (ULONGLONG)(g_medi.overheat_seconds * 1000);
        want = false;
        Log("MEDIGUN overheated after %.1f s of beam: off for %.0f s", (now - g_beam_start) / 1000.0, g_medi.overheat_seconds);
    }
    if (medigun) {
        ShowHeat(cs, now);
        UpdateGunOut();
    } else {
        g_gun_out = true;
    }
    if (want && !g_beam) {
        uint32_t h = FindBubble(cs, g_local_actor, MEDI_BUBBLE);
        if (h == 0xffffffffu) {
            static ULONGLONG warned;
            if (now - warned > 5000) {
                warned = now;
                LogWarning("MEDIGUN: no medigun bubble on this player's actor %016llx (Repair Matrix's: %08x, %u bubbles)",
                    (unsigned long long)g_local_actor, FindBubble(cs, g_local_actor, 0xb1fda2a83f3debd1ull),
                    *(uint16_t *)((uint8_t *)cs + 0xf4));
            }
            return;
        }
        __try {
            uint8_t *rec = BubbleRecord(cs, h);
            *(float *)(rec + 0x30) = g_medi.heal_per_second * g_medi.beam_seconds;   // heal over duration
            *(float *)(rec + 0x34) = g_medi.beam_seconds;
            *(float *)(rec + 0x38) = floorf(g_medi.range) + (g_medi.angle + CONE_TAG) / 100000.0f;
            *(float *)(rec + 0x3c) = 0.0f;                                         // no self heal
            *(float *)(rec + 0x40) = 0.0f;                                         // no cap
            *(float *)(rec + 0x48) = 1.0f;
            *(float *)(rec + 0x4c) = 0.0f;
            *(uint8_t *)(rec + 0x660) = 1;                                         // send the params
            g_medi_handle = h;
            g_medi_record = rec;
            g_local_slot = SlotOfBubble(cs, h);
            Log("MEDIGUN: activating bubble %08x in space %016llx", h,
                (unsigned long long)*(uint64_t *)(*(uint8_t **)((uint8_t *)cs + 0x80) + 0x58));
            ActivateBubble(cs, h);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogWarning("MEDIGUN: activating the bubble faulted");
            return;
        }
        g_beam = true;
        g_beam_start = now;
        g_next_resend = now + 1000;
        SendMedigun(true);
        Log("MEDIGUN on (trigger %.2f)", t);
    } else if (g_beam && !want) {
        g_beam = false;
        beam_end = now;
        SendMedigun(false);
        Log("MEDIGUN off after %.1f s", (now - g_beam_start) / 1000.0);
    } else if (g_beam && now >= g_next_resend) {
        g_next_resend = now + 1000;
        SendMedigun(true);
    }
}

// Another player's MEDIGUN beam came on (VeilNet): their bubble is activated here too, with
// MEDIGUN's tuning. The game's own activation message reaches the server but its handler there
// never activates the bubble (logged 2026-10-19: "slot 1 gen 0, kind 0", no activation), and it
// only relays to the other clients after it does.
struct PendingOn { uint16_t key; ULONGLONG at; };
PendingOn g_pending[8];

void WriteMediTuning(uint8_t *rec) {
    *(float *)(rec + 0x30) = g_medi.heal_per_second * g_medi.beam_seconds;
    *(float *)(rec + 0x34) = g_medi.beam_seconds;
    *(float *)(rec + 0x38) = floorf(g_medi.range) + (g_medi.angle + CONE_TAG) / 100000.0f;
    *(float *)(rec + 0x3c) = 0.0f;
    *(float *)(rec + 0x40) = 0.0f;
    *(float *)(rec + 0x48) = 1.0f;
    *(float *)(rec + 0x4c) = 0.0f;
}

void __fastcall Hooked_ActivateLocal(void *cs, uint32_t handle);

void ActivateRemote(void *cs, ULONGLONG now) {
    for (PendingOn &p : g_pending) {
        if (!p.at) continue;
        if (now - p.at > 3000) { p.at = 0; continue; }
        __try {
            uint32_t h = SlotBubble(cs, p.key);
            if (h != 0xffffffffu && !(!g_server && p.key == g_local_slot)) {
                WriteMediTuning(BubbleRecord(cs, h));
                Log("MEDIGUN slot %u: its bubble %08x activated here from VeilNet", p.key, h);
                Hooked_ActivateLocal(cs, h);
                p.at = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            p.at = 0;
        }
    }
}

void __fastcall Hooked_BubbleUpdate(void *cs) {
    g_bubble_cs = cs;
    ULONGLONG now = GetTickCount64();
    static ULONGLONG offset_read = 0;
    if (now - offset_read >= 1000) {
        offset_read = now;
        ReadHandOffset();
    }
    UpdateMedigunAllowed(now);
    MedigunTick(cs, now);
    ActivateRemote(cs, now);
    SelfDamageTick(now);
    if (!g_server) PlaceBeams(now);
    // A MEDIGUN bubble whose owner's beam is off ends now (this frame's update sees it past its duration).
    MediBubble list[16];
    AcquireSRWLockShared(&g_medi_lock);
    memcpy(list, g_medi_bubbles, sizeof(list));
    ReleaseSRWLockShared(&g_medi_lock);
    for (const MediBubble &b : list) {
        if (!b.cs || OwnerOn(b.key, now)) continue;
        uint8_t *rec = MediRecord(b);
        if (!rec) continue;
        __try {
            float duration = *(float *)(rec + 0x34);
            if (*(float *)(rec + 0x50) < duration) *(float *)(rec + 0x50) = duration;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    Real_BubbleUpdate(cs);
}

// LibOVR: the gun hand's index trigger, taken from the game while MEDIGUN is equipped.
thread_local bool t_party_fire = false;     // a party gun's shot is being fired on this thread
using fire_fn = void(__fastcall *)(void *cs, uint32_t handle, float charge, void *a4, uint64_t a5, uint64_t a6,
                                   uint64_t a7, uint64_t a8);
fire_fn Real_GunFire = nullptr;

void __fastcall Hooked_GunFire(void *cs, uint32_t handle, float charge, void *a4, uint64_t a5, uint64_t a6,
                               uint64_t a7, uint64_t a8) {
    if (!g_server && g_local_weapon == MEDIGUN && g_medi_allowed) {
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 20) LogDebug("MEDIGUN: COMET's shot swallowed");
        return;
    }
    if (!g_server && PartyGun(g_local_weapon)) {          // a party gun's shot (the trigger is shaped in Hooked_Input)
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        // no trigger readings (another runtime): a time gap instead -- but a dropped shot stays
        // queued in the Pulsar script and goes out later, by itself (tested 2026-11-05)
        if (now - g_party_input_at >= 1000 && now - last < PARTY_SHOT_MS) return;
        last = now;
        Log("PARTY GUN: shot (%016llx), last trigger reading %llu ms ago, last pull let through %llu ms ago",
            (unsigned long long)g_local_weapon, g_party_input_at ? (unsigned long long)(now - g_party_input_at) : 0ull,
            g_party_pulse_at ? (unsigned long long)(now - g_party_pulse_at) : 0ull);
        g_party_shot_at = now;                             // its muzzle flash becomes the party effect
        g_party_bullet_until = now + 300;                  // and its bullet (spawned after the fire call) is dropped
        t_party_fire = true;                               // ... with no bullet (Hooked_BulletSpawn)
        Real_GunFire(cs, handle, charge, a4, a5, a6, a7, a8);
        t_party_fire = false;
        return;
    }
    Real_GunFire(cs, handle, charge, a4, a5, a6, a7, a8);
}

// The bullet system's spawn (+0xce2020, from FireBulletLocal; DroneFix hooks it too): none while a
// party gun's shot is being fired on this thread.
constexpr uintptr_t BULLET_SPAWN_RVA = 0xce2020;
constexpr uint8_t BULLET_SPAWN_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
using bullet_spawn_fn = void(__fastcall *)(uint8_t *, uint64_t, void *, float, const void *, const float *, int, uint32_t);
bullet_spawn_fn Real_BulletSpawn = nullptr;
void __fastcall Hooked_BulletSpawn(uint8_t *sys, uint64_t a2, void *a3, float a4, const void *pose, const float *v, int a7, uint32_t a8) {
    // [party] drop_bullet (live, default 1). (Keeping the bullet did not stop the Pulsar firing by
    // itself, tested 2026-11-05.)
    if ((t_party_fire || (!g_server && GetTickCount64() < g_party_bullet_until)) &&
        GetPrivateProfileIntA("party", "drop_bullet", 1, g_ini_path)) {
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 10) Log("PARTY GUN: bullet not spawned");
        return;
    }
    Real_BulletSpawn(sys, a2, a3, a4, pose, v, a7, a8);
}

using input_fn = int(__cdecl *)(void *session, unsigned type, void *state);
input_fn Real_Input = nullptr;

int __cdecl Hooked_Input(void *session, unsigned type, void *state) {
    int r = Real_Input(session, type, state);
#ifdef ECHOCOMBAT_LOG
    if (r >= 0 && state && g_local_weapon == MEDIGUN) {           // which call / field the game uses
        static ULONGLONG last;
        static LONG calls;
        ++calls;
        ULONGLONG now = GetTickCount64();
        const float *f = (const float *)state;
        if (now - last >= 1000 && (f[4] > 0.1f || f[5] > 0.1f)) {
            LogDebug("ovr input: type %x, %ld calls/s, ctl %x | trig %.2f %.2f | nodz %.2f %.2f | raw %.2f %.2f | buttons %08x touches %08x",
                type, calls, *(const uint32_t *)((const uint8_t *)state + 0x30), f[4], f[5], f[13], f[14], f[21], f[22],
                *(const uint32_t *)((const uint8_t *)state + 8), *(const uint32_t *)((const uint8_t *)state + 12));
            last = now;
            calls = 0;
        }
    }
#endif
    if (r >= 0 && state && PartyGun(g_local_weapon)) {    // party guns: one short pull per real pull, 3 s apart
        int hand = g_local_arm == 0 ? 0 : 1;
        if (type & (hand ? 2u : 1u)) {
            float *trigger = (float *)((uint8_t *)state + 0x10);
            float v = trigger[hand];
            ULONGLONG now = GetTickCount64();
            static bool held = false;
            static ULONGLONG pulse_until = 0, pulse_at = 0;
            if (v >= 0.55f && !held) {                         // a new pull
                held = true;
                bool through = now - pulse_at >= PARTY_SHOT_MS;
                if (through) {
                    pulse_at = now;
                    pulse_until = now + PARTY_PULSE_MS;
                    g_party_pulse_at = now;
                }
                Log("PARTY GUN: pull (%.2f) %s", v, through ? "let through" : "ignored (too soon)");
            } else if (v < 0.3f && held) {
                held = false;
                Log("PARTY GUN: release");
            }
            if (now >= pulse_until) {                           // the game sees the trigger let go
                trigger[hand] = 0.0f;
                static const size_t kMore[] = { 0x34, 0x54 };   // IndexTriggerNoDeadzone, IndexTriggerRaw
                for (size_t at : kMore) {
                    float *f = (float *)((uint8_t *)state + at) + hand;
                    if (*f >= 0.0f && *f <= 1.0001f) *f = 0.0f;
                }
            }
            g_party_input_at = now;
        }
    }
    if (r >= 0 && state && g_local_weapon == MEDIGUN && g_medi_allowed && g_gun_out) {
        int hand = g_medi.hand >= 0 ? g_medi.hand : g_local_arm == 0 ? 0 : 1;
        if (type & (hand ? 2u : 1u)) {
            float *trigger = (float *)((uint8_t *)state + 0x10);
            float v = trigger[hand];
            g_trigger = v;
            g_trigger_at = GetTickCount64();
            trigger[hand] = 0.0f;
            // IndexTriggerNoDeadzone (+34h) and IndexTriggerRaw (+54h): the game's input reads
            // these (they move with the finger, so this SDK's struct has them; logged 2026-10-19)
            static const size_t kMore[] = { 0x34, 0x54 };
            for (size_t at : kMore) {
                float *f = (float *)((uint8_t *)state + at) + hand;
                if (*f >= 0.0f && *f <= 1.0001f) *f = 0.0f;
            }
        }
    }
    return r;
}

void AttachTracked(PVOID *real, PVOID hook);

DWORD WINAPI HookOvr(LPVOID) {
    for (int i = 0; i < 600; ++i) {                        // LibOVR loads with the headset, a little later
        HMODULE ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
        if (ovr) {
            Real_Input = (input_fn)GetProcAddress(ovr, "ovr_GetInputState");
            if (!Real_Input) break;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            AttachTracked(&(PVOID &)Real_Input, Hooked_Input);
            LONG rc = DetourTransactionCommit();
            if (rc == NO_ERROR) Log("MEDIGUN: reading the trigger from LibOVR");
            else LogWarning("MEDIGUN: hooking LibOVR failed");
            return 0;
        }
        Sleep(500);
    }
    Log("MEDIGUN: no LibOVR (not the Oculus client?): the trigger can't drive the beam");
    return 0;
}

void ReadMedigunIni(const char *ini) {
    char v[32];
    auto real = [&](const char *key, float def) {
        char d[32];
        sprintf_s(d, "%g", def);
        GetPrivateProfileStringA("medigun", key, d, v, sizeof(v), ini);
        return (float)atof(v);
    };
    g_medi.heal_per_second = real("heal_per_second", 8.075f);
    g_medi.beam_seconds = real("beam_seconds", 40);
    g_medi.overheat_seconds = real("overheat_seconds", 3);
    g_medi.cool_seconds = real("cool_seconds", 5);
    if (g_medi.cool_seconds < 0.1f) g_medi.cool_seconds = 0.1f;
    g_medi.range = real("range", 20);
    g_medi.slack = real("cone_slack", 5);
    g_medi.angle = GetPrivateProfileIntA("medigun", "cone_angle", 20, ini);
    if (g_medi.angle < 1 || g_medi.angle > 90) g_medi.angle = 20;
    GetPrivateProfileStringA("medigun", "gun_axis", "+z", v, sizeof(v), ini);
    {
        int i = v[1] == 'x' || v[1] == 'X' ? 0 : v[1] == 'y' || v[1] == 'Y' ? 1 : 2;
        g_gun_axis[0] = g_gun_axis[1] = g_gun_axis[2] = 0;
        g_gun_axis[i] = v[0] == '-' ? -1.0f : 1.0f;
    }
    GetPrivateProfileStringA("medigun", "hand", "auto", v, sizeof(v), ini);
    g_medi.hand = _stricmp(v, "left") == 0 ? 0 : _stricmp(v, "auto") == 0 ? -1 : 1;
    GetPrivateProfileStringA("medigun", "aim", "head", v, sizeof(v), ini);
    g_medi_aim_gun = _stricmp(v, "gun") == 0;
    Log("MEDIGUN: %.3f hp/s for %.0f s, %.0f s overheat, one teammate within %d deg / %.0f m, %s hand", g_medi.heal_per_second,
        g_medi.beam_seconds, g_medi.overheat_seconds, g_medi.angle, g_medi.range, g_medi.hand == 0 ? "left" : g_medi.hand > 0 ? "right" : "weapon arm's");
}

bool Check(uint8_t *at, const uint8_t *bytes, size_t n, const char *name);

// MEDIGUN's hooks, as attached (some only when their prologue checks out, one from LibOVR's
// thread), so NvrPluginShutdown detaches exactly those.
struct Tracked { PVOID *real; PVOID hook; };
Tracked g_tracked[32];
volatile LONG g_ntracked = 0;

void AttachTracked(PVOID *real, PVOID hook) {
    if (DetourAttach(real, hook) != NO_ERROR) return;
    LONG i = InterlockedIncrement(&g_ntracked) - 1;
    if (i < 32) g_tracked[i] = { real, hook };
}

void InstallMedigun(uint8_t *base) {
    g_base = base;
    if (!Check(base + SLOT_RVA, SLOT_BYTES, sizeof(SLOT_BYTES), "GetWeaponSlot") ||
        !Check(base + MANAGER_RET_RVA - 5, MANAGER_CALL_BYTES, sizeof(MANAGER_CALL_BYTES), "enable manager's slot call") ||
        !Check(base + BUBBLE_UPDATE_RVA, BUBBLE_UPDATE_BYTES, sizeof(BUBBLE_UPDATE_BYTES), "heal bubble update") ||
        !Check(base + ACTIVATE_LOCAL_RVA, ACTIVATE_LOCAL_BYTES, sizeof(ACTIVATE_LOCAL_BYTES), "ActivateHealBubbleLocally") ||
        !Check(base + ACTIVATE_RVA, ACTIVATE_BYTES, sizeof(ACTIVATE_BYTES), "ActivateHealBubble"))
        return;
    if (Check(base + GUN_FIRE_RVA, GUN_FIRE_BYTES, sizeof(GUN_FIRE_BYTES), "FireIntegratedGunLocal")) {
        Real_GunFire = (fire_fn)(base + GUN_FIRE_RVA);
        AttachTracked(&(PVOID &)Real_GunFire, Hooked_GunFire);
        if (base[BULLET_SPAWN_RVA] == 0xe9 ||               // DroneFix's hook already there: chain on it
            Check(base + BULLET_SPAWN_RVA, BULLET_SPAWN_BYTES, sizeof(BULLET_SPAWN_BYTES), "bullet spawn")) {
            Real_BulletSpawn = (bullet_spawn_fn)(base + BULLET_SPAWN_RVA);
            AttachTracked(&(PVOID &)Real_BulletSpawn, Hooked_BulletSpawn);
        }
    }
    ReadMedigunIni(g_ini_path);
    if (Check(base + SYSTEMS_FIND_RVA, SYSTEMS_FIND_BYTES, sizeof(SYSTEMS_FIND_BYTES), "systems by name") &&
        Check(base + SET_REAL_RVA, SET_REAL_BYTES, sizeof(SET_REAL_BYTES), "script SetReal")) {
        SystemsFind = (systems_fn)(base + SYSTEMS_FIND_RVA);
        if (Check(base + GET_REAL_RVA, GET_REAL_BYTES, sizeof(GET_REAL_BYTES), "script GetReal"))
            GetScriptReal = (get_real_fn)(base + GET_REAL_RVA);
        // DroneFix.dll may have detoured it already (a jmp in front): calling through that is the same
        if (base[TRANSFORM_GET_RVA] == 0xe9 ||
            Check(base + TRANSFORM_GET_RVA, TRANSFORM_GET_BYTES, sizeof(TRANSFORM_GET_BYTES), "CTransformCS::get"))
            TransformGet = (transform_get_fn)(base + TRANSFORM_GET_RVA);
        if (Check(base + SPACE_BY_ID_RVA, SPACE_BY_ID_BYTES, sizeof(SPACE_BY_ID_BYTES), "game space by id"))
            SpaceById = (space_fn)(base + SPACE_BY_ID_RVA);
        SetScriptReal = (set_real_fn)(base + SET_REAL_RVA);
    }
    Real_Slot = (slot_fn)(base + SLOT_RVA);
    Real_BubbleUpdate = (update_fn)(base + BUBBLE_UPDATE_RVA);
    Real_ActivateLocal = (activate_fn)(base + ACTIVATE_LOCAL_RVA);
    ActivateBubble = (activate_fn)(base + ACTIVATE_RVA);
    AttachTracked(&(PVOID &)Real_Slot, Hooked_Slot);
    if (Check(base + PURCHASED_RVA, PURCHASED_BYTES, sizeof(PURCHASED_BYTES), "item purchased status")) {
        Real_Purchased = (item_fn)(base + PURCHASED_RVA);
        AttachTracked(&(PVOID &)Real_Purchased, Hooked_Purchased);
    }
    {
        item_fn hooks[3] = { Hooked_Unlock0, Hooked_Unlock1, Hooked_Unlock2 };
        for (int k = 0; k < 3; ++k)
            if (Check(base + UNLOCK_RVAS[k], UNLOCK_BYTES, sizeof(UNLOCK_BYTES), "item unlock level")) {
                Real_Unlock[k] = (item_fn)(base + UNLOCK_RVAS[k]);
                AttachTracked(&(PVOID &)Real_Unlock[k], hooks[k]);
            }
    }
    AttachTracked(&(PVOID &)Real_BubbleUpdate, Hooked_BubbleUpdate);
    AttachTracked(&(PVOID &)Real_ActivateLocal, Hooked_ActivateLocal);
    if (Check(base + RESOLVE_ATTACH_RVA, RESOLVE_ATTACH_BYTES, sizeof(RESOLVE_ATTACH_BYTES), "particle effect attachment")) {
        Real_Resolve = (resolve_fn)(base + RESOLVE_ATTACH_RVA);
        AttachTracked(&(PVOID &)Real_Resolve, Hooked_Resolve);
        if (Check(base + PFX_NODE_RVA, PFX_NODE_BYTES, sizeof(PFX_NODE_BYTES), "particle effect node")) {
            Real_PfxNode = (pfx_node_fn)(base + PFX_NODE_RVA);
            AttachTracked(&(PVOID &)Real_PfxNode, Hooked_PfxNode);
            if (Check(base + CREATE_PFX_RVA, CREATE_PFX_BYTES, sizeof(CREATE_PFX_BYTES), "CreateParticleEffect node")) {
                Real_CreatePfx = (pfx_node_fn)(base + CREATE_PFX_RVA);
                AttachTracked(&(PVOID &)Real_CreatePfx, Hooked_CreatePfx);
            }
            if (Check(base + ATTACH_POSE_RVA, ATTACH_POSE_BYTES, sizeof(ATTACH_POSE_BYTES), "attachment pose")) {
                AttachPose = (attach_pose_fn)(base + ATTACH_POSE_RVA);
            }
        }
    }
    if (uint32_t mb = RulesMailboxRva(base)) {             // the Pulsar script asks PartyEffect / PartyGun
        *(void *volatile *)(base + mb + PARTY_FN) = (void *)PartyEffect;
        *(void *volatile *)(base + mb + PARTY_GUN_FN) = (void *)PartyGunActor;
        Log("PARTY GUN: PartyEffect / PartyGunActor at RULES mailbox +%#x / +%#x", PARTY_FN, PARTY_GUN_FN);
    }
    if (Check(base + NET_ACTIVATE_RVA, NET_ACTIVATE_BYTES, sizeof(NET_ACTIVATE_BYTES), "heal bubble network activation")) {
        Real_NetActivate = (net_activate_fn)(base + NET_ACTIVATE_RVA);
        AttachTracked(&(PVOID &)Real_NetActivate, Hooked_NetActivate);
    }
    if (!g_server)
        if (HANDLE t = CreateThread(nullptr, 0, HookOvr, nullptr, 0, nullptr)) CloseHandle(t);
}

void ReadIni() {
    char axis[16];
    const char *ini = g_ini_path;
    GetPrivateProfileStringA("cone", "axis", "+z", axis, sizeof(axis), ini);
    int i = axis[1] == 'x' || axis[1] == 'X' ? 0 : axis[1] == 'y' || axis[1] == 'Y' ? 1 : 2;
    g_axis[0] = g_axis[1] = g_axis[2] = 0;
    g_axis[i] = axis[0] == '-' ? -1.0f : 1.0f;
    g_rez_grace_ms = GetPrivateProfileIntA("rez", "grace_ms", 1500, ini);
    g_rez_window_ms = GetPrivateProfileIntA("rez", "seconds", 5, ini) * 1000u;
    g_link_window_ms = GetPrivateProfileIntA("cone", "link_window_ms", 500, ini);
    char bonus[32];
    GetPrivateProfileStringA("buff", "damage_bonus", "0", bonus, sizeof(bonus), ini);
    g_damage_bonus = (float)atof(bonus);
    GetPrivateProfileStringA("buff", "body_bonus", "1", bonus, sizeof(bonus), ini);
    g_body_bonus = (float)atof(bonus);
    GetPrivateProfileStringA("buff", "head_bonus", "2", bonus, sizeof(bonus), ini);
    g_head_bonus = (float)atof(bonus);
    g_head_part = GetPrivateProfileIntA("buff", "head_part", 5, ini);
    GetPrivateProfileStringA("buff", "self_damage", "2", bonus, sizeof(bonus), ini);
    g_self_damage = (float)atof(bonus);
    g_self_tick_ms = GetPrivateProfileIntA("buff", "self_damage_tick_ms", 1000, ini);
    if (g_self_tick_ms < 50) g_self_tick_ms = 50;
    g_self_lethal = GetPrivateProfileIntA("buff", "self_damage_lethal", 0, ini) != 0;
    GetPrivateProfileStringA("cone", "aim", "head", bonus, sizeof(bonus), ini);
    g_aim_gun = _stricmp(bonus, "gun") == 0;
    Log("cone aim from the %s (head axis %s); cheat death for %u s from each teammate's first link; boosted hits +%.1f body / "
        "+%.1f head (part %d); the user takes %.1f every %u ms%s", g_aim_gun ? "gun, head if it has none" : "head", axis,
        g_rez_window_ms / 1000, g_body_bonus, g_head_bonus, g_head_part, g_self_damage, g_self_tick_ms,
        g_self_lethal ? "" : " (never the last point)");
}

bool Check(uint8_t *at, const uint8_t *bytes, size_t n, const char *name) {
    if (memcmp(at, bytes, n) == 0) return true;
    LogError("prologue mismatch at %s -- wrong echovr build, not hooking", name);
    return false;
}

bool g_hooked = false, g_send_hooked = false, g_apply_hooked = false;

bool Install(bool server) {
    if (g_hooked) return true;
    auto base = (uint8_t *)GetModuleHandleW(nullptr);
    if (!Check(base + HEAL_RVA, HEAL_BYTES, sizeof(HEAL_BYTES), "HealServer") ||
        !Check(base + DAMAGE_RVA, DAMAGE_BYTES, sizeof(DAMAGE_BYTES), "ProcessDamageServer") ||
        !Check(base + SEND_RVA, SEND_BYTES, sizeof(SEND_BYTES), "SendDamageDealt"))
        return false;
    SendDamageDealt = (send_fn)(base + SEND_RVA);
    if (Check(base + RADIUS_RVA, RADIUS_BYTES, sizeof(RADIUS_BYTES), "heal bubble radius check") &&
        Check(base + OWNER_HANDLE_RVA, OWNER_HANDLE_BYTES, sizeof(OWNER_HANDLE_BYTES), "owner handle") &&
        Check(base + OWNER_OBJECT_RVA, OWNER_OBJECT_BYTES, sizeof(OWNER_OBJECT_BYTES), "owner object")) {
        OwnerHandle = (handle_fn)(base + OWNER_HANDLE_RVA);
        OwnerObject = (object_fn)(base + OWNER_OBJECT_RVA);
        ReadIni();
        if (InstallCone(base)) Log("POWER BOOST link: cone on the heal bubble radius check");
        else LogWarning("cone patch failed");
    }
    g_server = server;
    Real_Heal = (heal_fn)(base + HEAL_RVA);
    Real_Damage = (damage_fn)(base + DAMAGE_RVA);
    Real_Send = (send_fn)(base + SEND_RVA);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_Heal, Hooked_Heal);
    DetourAttach(&(PVOID &)Real_Damage, Hooked_Damage);
    g_send_hooked = !g_server;
    if (g_send_hooked) DetourAttach(&(PVOID &)Real_Send, Hooked_Send);
    if (Check(base + SLOT_ACTOR_RVA, SLOT_ACTOR_BYTES, sizeof(SLOT_ACTOR_BYTES), "player slot actor"))
        SlotActorFn = (slot_actor_fn)(base + SLOT_ACTOR_RVA);   // (server: damage boosts; all: MEDIGUN owners)
    InstallMedigun(base);
    if (g_server && Check(base + APPLY_RVA, APPLY_BYTES, sizeof(APPLY_BYTES), "ApplyDamageAndNotify")) {
        Real_Apply = (apply_fn)(base + APPLY_RVA);
        DetourAttach(&(PVOID &)Real_Apply, Hooked_Apply);
        g_apply_hooked = true;
        if (Check(base + SHOOTER_SLOT_RVA, SHOOTER_SLOT_BYTES, sizeof(SHOOTER_SLOT_BYTES), "shooter slot"))
            ShooterSlotFn = (shooter_slot_fn)(base + SHOOTER_SLOT_RVA);
        g_player_symbol = (uint64_t *)(base + PLAYER_SYMBOL_RVA);
    }
    LONG rc = DetourTransactionCommit();
    if (rc == NO_ERROR) Log("hooked HealServer and ProcessDamageServer (%s)", g_server ? "server" : "client");
    else LogError("hook failed (%ld)", rc);
    g_hooked = rc == NO_ERROR;
    return g_hooked;
}

void Uninstall() {
    if (g_hooked) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID &)Real_Heal, Hooked_Heal);
        DetourDetach(&(PVOID &)Real_Damage, Hooked_Damage);
        if (g_send_hooked) DetourDetach(&(PVOID &)Real_Send, Hooked_Send);
        if (g_apply_hooked) DetourDetach(&(PVOID &)Real_Apply, Hooked_Apply);
        for (LONG i = 0; i < g_ntracked && i < 32; ++i) DetourDetach(g_tracked[i].real, g_tracked[i].hook);
        DetourTransactionCommit();
        g_hooked = g_send_hooked = g_apply_hooked = false;
        g_ntracked = 0;
    }
    if (g_radius_site) {                                // the cone's jump back to the radius check
        DWORD old;
        if (VirtualProtect(g_radius_site, sizeof(g_radius_saved), PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(g_radius_site, g_radius_saved, sizeof(g_radius_saved));
            VirtualProtect(g_radius_site, sizeof(g_radius_saved), old, &old);
            FlushInstructionCache(GetCurrentProcess(), g_radius_site, sizeof(g_radius_saved));
        }
        g_radius_site = nullptr;
    }
    Log("unhooked");
}

} // namespace

// VeilNet.dll hands over each MEDIGUN 'M' packet: the owner key (low 16 bits), on (bit 16).
extern "C" __declspec(dllexport) void PowerBoost_MedigunRemote(uint32_t id) {
    uint16_t key = (uint16_t)id;
    bool on = (id >> 16 & 1) != 0;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_medi_lock);
    MediOwner *slot = &g_owners[0];
    for (MediOwner &o : g_owners) {
        if (o.key == key && o.at) { slot = &o; break; }
        if (o.at < slot->at) slot = &o;
    }
    bool was = slot->key == key && slot->at && slot->on;
    *slot = { key, on, now };
    if (on && !was) {                                      // activate it here (Hooked_BubbleUpdate)
        PendingOn *q = &g_pending[0];
        for (PendingOn &p : g_pending) {
            if (p.at && p.key == key) { q = &p; break; }
            if (p.at < q->at) q = &p;
        }
        *q = { key, now };
    }
    ReleaseSRWLockExclusive(&g_medi_lock);
    if (was != on) Log("MEDIGUN %04x %s (from VeilNet)", key, on ? "on" : "off");
}

// CombatBots.dll finds the damageable system here (dead / respawn of its bots).
extern "C" __declspec(dllexport) void *PowerBoost_DamageableCS() { return g_damage_cs; }

// VeilNet.dll: this player's weapon slot (MEDIGUN stays in the list while it is selected).
extern "C" __declspec(dllexport) uint64_t PowerBoost_LocalWeapon() { return g_local_weapon; }

ECHOCOMBAT_EXPORT NvrPluginInfo NvrPluginGetInfo(void) {
    return EchoCombatPluginInfo("power_boost", "EchoCombat: OVERCHARGE (links, damage reduction, cheat death, damage bonus)");
}

ECHOCOMBAT_EXPORT uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

ECHOCOMBAT_EXPORT uint32_t NvrPluginGetCapabilities(void) {
    return NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY | NEVR_PLUGIN_CAP_HOOKS_ENGINE;
}

ECHOCOMBAT_EXPORT int NvrPluginInitEx(const NvrGameContext *ctx, const char *) {
    PluginLogBind(ctx);
    EchoCombatBesideDll(g_self, g_ini_path, sizeof(g_ini_path), "PowerBoost.ini");
    return Install(EchoCombatIsServer(ctx)) ? 0 : 1;
}

ECHOCOMBAT_EXPORT int NvrPluginInit(const NvrGameContext *ctx) { return NvrPluginInitEx(ctx, "{}"); }

ECHOCOMBAT_EXPORT void NvrPluginShutdown(void) { Uninstall(); }

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_self = self;
    }
    return TRUE;
}
