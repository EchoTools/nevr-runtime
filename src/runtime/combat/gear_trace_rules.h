#pragma once
// [NEVR.COMBAT] gear ... records: what a PC client's tac mod and ordnance settings are at startup, at each balance push
// from a server, and at each session join and leave, compared with the values the client loaded at startup. Measure
// only: the hooks in gear_trace.cpp read and forward every call unchanged. The decisions and the line text are here, so
// test_gear_trace.cpp drives them with no game.
//
// Where the values live (echovr.exe):
//   - mp_weapon_settings (CSymbol64 0x11a8523ed4724ca7) is read by path through ResolveConfigHash (0x140d0b170) and the
//     CJson float getter 0x1405fca80 (json, CSymbol64(path), fallback), as UpdateServer (0x140d86790) reads
//     "grenade_properties|%s|cooldown". The getter returns the fallback when the path is absent.
//   - ResolveConfigHash looks first in CR15NetBalanceSettingsCS's table (the pointer at 0x1420d3a68, entries of 0x18
//     bytes: id, then the CJson; count at +0x30), else in the base resource (ResolveConfigHashAlt 0x1404b4340).
//   - The table is made by the CS's init (0x140cfe650), freed and the pointer zeroed by its teardown (vslot 43,
//     0x140d4cd70). A server's R15NetBalanceSettingsUpdate reaches the client's listener 0x140d49950 (registered at
//     0x140cfee4a), which parses the pushed JSON over the table entry of that id and logs "[BALANCE] Reloaded".
//   - The gear tables (gear_table weapons, ordnances, tacmods: lists at 0x1420d3420/28/30, entries of 0x88 bytes with
//     the name first) are rebuilt by 0x140c4eba0, called from InitAndLoadGearTables (0x140c4e2a0, at startup; return
//     address 0x140c4e391) and from SettingsUpdateCB (0x1401b2a90; return address 0x1401b2df4).
//
// A tac mod's charge is ability_properties|<tac>|required_charge (POINT: charge_required) and its duration
// ability_properties|<tac>|duration; an ordnance's is grenade_properties|<ord>|cooldown. The names are the stock
// mp_weapon_settings sections.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "nevr/symbol_hash.h"

namespace nevr_gear_trace {

inline constexpr std::uint64_t kImageBase = 0x140000000ULL;
inline constexpr std::uint64_t kWeaponSettingsId = 0x11a8523ed4724ca7ULL;  // CSymbol64("mp_weapon_settings")
inline constexpr std::uint64_t kStartupReturnVA = 0x140C4E391;              // after InitAndLoadGearTables' call
inline constexpr std::uint64_t kSettingsUpdateReturnVA = 0x1401B2DF4;       // after SettingsUpdateCB's call

struct Key {
  const char* group;  // "tacmod" or "ordnance"
  const char* name;   // the mp_weapon_settings section
  const char* field;  // the value read
  std::uint64_t hash; // CSymbol64 of "<properties>|<name>|<field>", the path the game's getter takes
};

#define NEVR_GEAR_KEY(group, props, name, field) \
  Key{group, name, field, EchoVR::CSymbol64Hash(props "|" name "|" field)}

inline constexpr std::array<Key, 19> kKeys = {
    NEVR_GEAR_KEY("tacmod", "ability_properties", "sensor", "required_charge"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "heal", "required_charge"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "buff", "required_charge"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "shield", "required_charge"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "smoke", "required_charge"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "wraith", "required_charge"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "point", "charge_required"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "heal", "heal_duration"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "buff", "dr_duration"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "shield", "duration"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "smoke", "duration"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "wraith", "duration"),
    NEVR_GEAR_KEY("tacmod", "ability_properties", "point", "charge_up_time"),
    NEVR_GEAR_KEY("ordnance", "grenade_properties", "det", "cooldown"),
    NEVR_GEAR_KEY("ordnance", "grenade_properties", "arc", "cooldown"),
    NEVR_GEAR_KEY("ordnance", "grenade_properties", "stun", "cooldown"),
    NEVR_GEAR_KEY("ordnance", "grenade_properties", "burst", "cooldown"),
    NEVR_GEAR_KEY("ordnance", "grenade_properties", "drone", "cooldown"),
    NEVR_GEAR_KEY("ordnance", "grenade_properties", "dot", "cooldown"),
};

#undef NEVR_GEAR_KEY

/// The values of kKeys at one moment. `readable` is false when mp_weapon_settings could not be resolved at all; a
/// value the getter did not find is NaN (the getter is called with a NaN fallback).
struct Snapshot {
  bool taken = false;
  bool readable = false;
  bool fromTable = false;  // the CS's balance table was present (else the base resource answered)
  std::array<float, kKeys.size()> values{};
};

enum class Caller { kStartup, kSettingsUpdate, kOther };

inline Caller ClassifyCaller(std::uint64_t returnVa) {
  if (returnVa == kStartupReturnVA) return Caller::kStartup;
  if (returnVa == kSettingsUpdateReturnVA) return Caller::kSettingsUpdate;
  return Caller::kOther;
}

inline const char* CallerName(Caller c) {
  switch (c) {
    case Caller::kStartup: return "startup";
    case Caller::kSettingsUpdate: return "settings_update";
    case Caller::kOther: return "other";
  }
  return "other";
}

// NetGameState numbers (abi/echovr.h): 5 Lobby, 7 LoadingLevel, 8 ReadyForGame, 9 InGame.
inline const char* SessionEvent(std::int32_t from, std::int32_t to) {
  if (to == 9 && from != 9) return "match_join";
  if (to == 5 && from != 5) return "lobby_join";
  if ((from == 9 || from == 8) && to != 9 && to != 8) return "match_leave";
  if (from == 5 && to != 5 && to != 6 && to != 7) return "lobby_leave";
  return "state";
}

inline bool Same(float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

inline std::string Num(float v) {
  if (std::isnan(v)) return "absent";
  char b[32];
  std::snprintf(b, sizeof(b), "%g", static_cast<double>(v));
  return b;
}

/// One group's values against the startup values: `heal.required_charge=35 (stock 35, equal)`, and for a value that
/// differs the last record's value too, so a carried-over value shows where it came from.
inline std::string FormatGroup(const char* group, const Snapshot& now, const Snapshot& stock, const Snapshot& prev,
                               std::size_t* equal, std::size_t* differs) {
  std::string out;
  for (std::size_t i = 0; i < kKeys.size(); ++i) {
    if (std::string(kKeys[i].group) != group) continue;
    if (!out.empty()) out += "; ";
    out += kKeys[i].name;
    out += '.';
    out += kKeys[i].field;
    out += '=';
    out += Num(now.values[i]);
    if (!stock.taken || !stock.readable) {
      out += " (stock unknown)";
      continue;
    }
    if (Same(now.values[i], stock.values[i])) {
      out += " (stock " + Num(stock.values[i]) + ", equal)";
      ++*equal;
    } else {
      out += " (stock " + Num(stock.values[i]) + ", differs";
      if (prev.taken && prev.readable) out += ", last record " + Num(prev.values[i]);
      out += ")";
      ++*differs;
    }
  }
  return out;
}

/// The record of the values in force at `at` (startup, a push, a join or leave).
inline std::string FormatValues(const char* at, const Snapshot& now, const Snapshot& stock, const Snapshot& prev) {
  if (!now.readable)
    return std::string("[NEVR.COMBAT] gear values at=") + at +
           " readable=false (mp_weapon_settings did not resolve through the game's ResolveConfigHash; no value is "
           "known, none is guessed)";
  std::size_t equal = 0, differs = 0;
  const std::string tac = FormatGroup("tacmod", now, stock, prev, &equal, &differs);
  const std::string ord = FormatGroup("ordnance", now, stock, prev, &equal, &differs);
  char head[256];
  std::snprintf(head, sizeof(head),
                "[NEVR.COMBAT] gear values at=%s source=%s equal_to_stock=%zu differ_from_stock=%zu%s", at,
                now.fromTable ? "balance_table" : "base_resource", equal, differs,
                stock.taken ? "" : " (no startup values yet)");
  std::string line = head;
  line += " tacmods: " + tac + " | ordnances: " + ord;
  if (differs > 0)
    line += " (a value that differs from stock is in force: this server's push, or a value kept from an earlier one)";
  return line;
}

/// The keys a push changed: `heal.required_charge 35->5`, or `none`.
inline std::string FormatChanges(const Snapshot& before, const Snapshot& after, std::size_t* count) {
  std::string out;
  *count = 0;
  if (!before.readable || !after.readable) return "unreadable";
  for (std::size_t i = 0; i < kKeys.size(); ++i) {
    if (Same(before.values[i], after.values[i])) continue;
    if (!out.empty()) out += ", ";
    out += std::string(kKeys[i].name) + "." + kKeys[i].field + " " + Num(before.values[i]) + "->" +
           Num(after.values[i]);
    ++*count;
  }
  return out.empty() ? "none" : out;
}

}  // namespace nevr_gear_trace
