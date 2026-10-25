#!/usr/bin/env python3
"""
gen_symbol_corpus.py — Reusable CSymbol64 / SNS message hash→name corpus generator.

Reads candidate symbol name strings from stdin (or a file), hashes each with both
the CSymbol64 pipeline (CRC64, polynomial 0x95ac9329ac4bc9b5) and the SNS message
pipeline (CMatSym::Hash + SMatSymData::HashA with seed 0x6d451003fb4b172e), then
emits a generated C++ source/header pair containing a sorted reverse-lookup table.

Usage:
    python3 tools/gen_symbol_corpus.py < candidates.txt  [--audit-log audit.log]

Input format: one candidate string per line. Lines starting with '#' are comments.
Empty lines and whitespace-only lines are skipped. Lines starting with '@' are
pre-computed hash→name mappings (for internal labels): @0xHEXHASH name

Output: writes to stdout a combined .h/.cpp pair (suitable for redirection).
To split, use --split-dir PATH to write symbol_corpus.h and symbol_corpus.cpp.

The generated table maps uint64_t hash → const char* name via binary search.
Unresolved hashes return nullptr; the caller formats a hex fallback.

Pre-computed hash entries (@ lines) are included as-is in the table so internal
label names from symbols.h and messages.h propagate into the corpus even when the
label name itself does not hash to the stored value.

Re-run this script whenever the game binary is updated to refresh the string pool.
"""

import sys
import os
import re
import argparse
from typing import Dict, List, Tuple, Set

# ============================================================================
# CSymbol64 Hash (CRC64 with polynomial 0x95ac9329ac4bc9b5)
# ============================================================================
POLYNOMIAL = 0x95ac9329ac4bc9b5


def gen_cs64_table() -> List[int]:
    table = [0] * 256
    for i in range(256):
        crc = 0
        if i & 0x80:
            crc = 0x2b5926535897936a
        elif i & 0x40:
            crc = 0x95ac9329ac4bc9b5
        if (i & 0x40) and (i & 0x80):
            crc = 0xbef5b57af4dc5adf
        for bit in [0x20, 0x10, 0x08, 0x04, 0x02, 0x01]:
            crc = (crc * 2) & 0xFFFFFFFFFFFFFFFF
            if i & bit:
                crc ^= POLYNOMIAL
        table[i] = (crc * 2) & 0xFFFFFFFFFFFFFFFF
    return table


CS64_TABLE = gen_cs64_table()


def csymbol64_hash(s: str, seed: int = 0xFFFFFFFFFFFFFFFF) -> int:
    for ch in s:
        c = ord(ch)
        if 0x41 <= c <= 0x5A:
            c += 0x20
        seed = c ^ CS64_TABLE[(seed >> 56) & 0xFF] ^ ((seed << 8) & 0xFFFFFFFFFFFFFFFF)
    return seed


# ============================================================================
# CMatSym::Hash + SMatSymData::HashA (SNS message pipeline)
# ============================================================================

# MATSYM_TABLE: byte-expand table (byte i → 64-bit value with 0xFF at bit positions of i)
MATSYM_TABLE = [0] * 256
for i in range(256):
    val = 0
    for b in range(8):
        if i & (1 << b):
            val |= (0xFF << (b * 8))
    MATSYM_TABLE[i] = val

# MATSYM_CHAR_TABLE: character lookup table (256 entries at 0x1416d0120)
MATSYM_CHAR_TABLE = [
    0x2BDDA74BEAACABC3, 0x8869C65FEB52DB80, 0x5723C9F6CB908143, 0x8F32573790804CF3,
    0xFF1BA8836937AC9A, 0x7C20ECE554D76D16, 0x8AA7A9FFC26FB5CF, 0x09528283645E5DCE,
    0x6787C29065E52F19, 0x90940AD579EC8F19, 0x76E9C71B2825A262, 0x9B660273ADFA8368,
    0x0D911813E4519293, 0x35D55BB82066A822, 0x0B39689134C9A6B9, 0x21E83E1AF57281D8,
    0x2B3D6D235326E186, 0x3F2158C687D0AFC2, 0xBE8B2D251CC01E65, 0x8A376215C574365F,
    0x15D8D74207BE1E9F, 0xD7B19EFB7ACCE172, 0xB38F4E35F6F7A91E, 0x853FB06D889E2A48,
    0xEF4F0C88BE562058, 0x14E0B6E48D0C0F16, 0xCCDB5BBA150C1288, 0x71B40CD4946E0B37,
    0x50F9CBA1770A2B63, 0x3FAD46D8B8069923, 0x81A77F3B7678B640, 0xCB8AF474D7262810,
    0xF976A95598B999D2, 0xFDA3F52463B46C6E, 0xD64815138C09F229, 0xB3917CA22F79CEEE,
    0x5C34055F26A1E404, 0x1EF1E6511D3F69A6, 0x1CB54571B9D18AB6, 0xE2BED505BA1CAF02,
    0xE9B31894D4E6B04B, 0x3CFDA4E0033EAF63, 0xFED841D74060E331, 0x327CDC166506E07F,
    0xE3066B4B6A3BE136, 0x4CB1BC3021704523, 0x8C9333F52F505B76, 0xFC18ADB105F7C9B8,
    0x2327982451117F85, 0x613FCAECC73A627F, 0xE3C4FC8114B0A723, 0xBB0E6FD523ADB9E8,
    0x8F295BEBBD341415, 0xB43C7604F7A26CB7, 0xA4598BB726FA7417, 0x98DF018E1984CBB2,
    0xC4AE345C802A8B12, 0x18533BF7DD051A8F, 0xD855F543AA101890, 0x73F458FD3CA20A7B,
    0x8955DE42197DC029, 0xEBD567CD3F114D43, 0xCF0B7C4DBB7DA040, 0x97D0FBD6DEAF63D2,
    0x522FEBA0337365A7, 0xC81A5609BE7D6F06, 0xB1C5A91FFC13E604, 0x97095FDDACA8455C,
    0x04EC2A2962E5B834, 0xC98084064297AFDF, 0x0E8EA6F71750FFD2, 0x6D619B9823D2E22E,
    0xFC2B498AF86A2138, 0xB5FCEA5433B75939, 0x4ABECDC610426D45, 0xB1B934E29DE730D9,
    0x6285ED78715B3F07, 0x7E701E082D26A112, 0xFADA81CCA0C8AE8D, 0x0E434FF7FDA9B763,
    0x9BCFD0913CFDBA07, 0xB6CF83E36AF959C8, 0x2016340AD3FA438B, 0x6D451003FB4B172E,
    0x10FDEC124A68D757, 0xA3D3E7019A90604B, 0xDBC7F7C0C252D0B6, 0xE0AEBDCA8ED8DC54,
    0x7C7B5F50ED34705D, 0xB2F4D9EED340806F, 0xC40535DA19AAFB5D, 0xD68DF8913E6FF156,
    0x07C425CBC83DB596, 0xD66F5BF8FDA90371, 0xA1C3CFC5F3333289, 0x539EB082389AB5CF,
    0x0CC42B06C6863AA4, 0x772F252A54A59B21, 0x22728D0C714E87A7, 0xD884B1720C9611D9,
    0xDC9CA8015ECED8AB, 0xCFD2C804C9755DDE, 0x5E1F742B263D8987, 0x1704189301AC7B5E,
    0x3C49116D049646E0, 0x4BFA71804D096BAA, 0x97C52591E2A6AA59, 0x0326F5922F9661A9,
    0x354ADCB609C444FA, 0x5B9F9E2EDD3C7865, 0xF9F2AF7D32F47F93, 0x0748E8C03E247FB7,
    0x7EF957E2824216F6, 0xD4DD3A1B4C5D667A, 0x49FB6450FF733FA9, 0x1E841816D55276D2,
    0xBE027DFB60B17121, 0x76B5F668E338DFE6, 0x9A227690F7DE06C7, 0x7A6C5EB0C61146B6,
    0x84998179859FBB75, 0xF43BDED162334AF2, 0x3A48B9BCCDAB4931, 0x3E85CBA55F4CC097,
    0xCAFA73718DB08C1C, 0xADC8AFEEF9E9FF3E, 0x259C0765DE66D53F, 0x0D98C981AB0175F3,
    0x1BA1DF279F35AC76, 0xCF7CB75C9BFF472D, 0x3BEA02C1DA2E851A, 0xF1D9BB5E3B6652B1,
    0x9306AFB78A4C2E17, 0x5125987CFB8850D2, 0x536170A205F994FF, 0x88205660246BEAFB,
    0x52D6C91AF3219618, 0x4F69ADCF11689705, 0xC8951D5767F006F9, 0x6BAE37D19BCF2460,
    0x7FA3B054CABE3A0D, 0xD89C492E27163282, 0xF06E22169264E91D, 0x99DD47A11B4CE032,
    0x4B44937F43012E69, 0x05D1624DC1220E5F, 0x32313F6B4D585F89, 0xDDA93772245FAC4E,
    0x62AF119CF65CD083, 0x67F796EBDFC0460D, 0x10E6851DF123D3FE, 0x8C9145AD2A258E2B,
    0x0893EB45F31ECCDF, 0xF5C238E2CCBFAC98, 0xA5903D21BE4F634E, 0xC827185779EAC765,
    0x596C67B0FBA18AFD, 0x50F991751133ECBE, 0x01FD97A17E5BA8FF, 0x62C459D30742D2BF,
    0x5748D2F6CE89D41B, 0x68FF67608558A539, 0x52F10C6B519F2D47, 0xA17EF6D7DD6B528C,
    0x832E09F919F1D19E, 0x785190BBA6310917, 0x988A42CDB93C67D6, 0xC31B2938ED73BE43,
    0x85944E4CDE9D42A3, 0xE1F6E967296998F4, 0x494740298137B045, 0xC28F78B839450B6A,
    0xF72333153B288FD3, 0x6DE6379FDAE18892, 0xF019C911E7F19924, 0xC6F06AB11454E0C9,
    0xBF131AABFD47FEDD, 0xE02A3DCBD6966FA6, 0x074947D8A89DE9F5, 0x460CA87C88302739,
    0x8ADCED696F62A1AE, 0x49CFACB843FE08FD, 0x216F5EC66091E6D2, 0xE08EE158DDEE1C06,
    0x8DF91A42ADD00477, 0xFDF50E76F1F8BF51, 0x1D4FA3CAC632B586, 0x15F5725B8CBD2D27,
    0x9612CB0A58EA9007, 0x7B51A5DCFA4569A8, 0x27CE193BAA2B8362, 0x6815D48A2A4C63F3,
    0xB6544CB28955B87B, 0x754F0187175C21D9, 0x427D6207844CF5E2, 0xC5F5E96F95B3CC5B,
    0x885D4E3EE8C4AF20, 0x12CBCD1D6D93557C, 0x026CE1CE1E949953, 0xB930F3504FCE07BD,
    0x761808F9E8CBA37B, 0xAA60BDD29733E9A6, 0x2C246C5A77A17928, 0xE1C1C5B2157B2A03,
    0x4C74D71904A5FE47, 0xABB569FCCC0141DE, 0x16C32C4AAFCB351A, 0x5B5619F4E8B28EFE,
    0xC3834DB2B3D4D6C9, 0x4AE5B8232AF651B2, 0xB0C5456259B66BE3, 0x7DC6D2211AF523CB,
    0x2CC5671E366CEEBE, 0xE438C0E3D15740B6, 0x1A50932D6E65E887, 0x8BE246A968A14858,
    0x8B5AF58FD341FDE3, 0xB2C84F31D2AA0344, 0x93948F91456F0DCC, 0xAD5A6CC0A1EDA6CF,
    0xBF2251D9C991DBB5, 0x3489DFDC48210428, 0x22A1A7A8E1925F30, 0x069F879882A5A8E5,
    0x755E5F41E8316962, 0xB45F72E9DE033D28, 0xB2949D43199FEF6A, 0x4A8FA3EB16810D5B,
    0x4DB6F7714F230F9F, 0x8206955348CD08E4, 0x2876B475F229EC78, 0xEFE55A85A593595D,
    0x21D9173F0A753007, 0xFCC8CD013AE5097F, 0xCC3C2B3A61CFEF84, 0x05AFC8DB7FE68212,
    0x9E29D99A6C8FC33B, 0x407C63999243A965, 0x8A2017B26B84FB95, 0xE2CAF6992A6EB7C7,
    0x195A02F6E09E6144, 0x539A2C55AFFB863B, 0x4C80150CF9E32E85, 0xDB23C6E07CBD5B51,
    0x70D3AE4A5D84EB3B, 0xF798993A2EA01074, 0xF9558D6BF258ECBC, 0x6F57CCCF61E437B6,
    0x33E1F9EAD361CDA8, 0xA86F7843AE7AC258, 0x9ECB9228D3638B83, 0xFF18D674DB5F9534,
    0x4BDE2F948F9E3638, 0xA596AC3C03504B9D, 0x3A7DA230FD7F2C6A, 0xC8AA7D11932F9CBE,
]


def mix_function(p1, p2, p3, p4, p5, p6, p7, p8):
    p4 = ((p4 >> 32) ^ p4) & 0xFFFFFFFFFFFFFFFF
    p5 = ((p5 << 32) ^ p5) & 0xFFFFFFFFFFFFFFFF
    p3 = ((p3 >> 32) ^ p3) & 0xFFFFFFFFFFFFFFFF
    p8 = ((p8 << 32) ^ p8) & 0xFFFFFFFFFFFFFFFF
    p2 = ((p2 >> 32) ^ p2) & 0xFFFFFFFFFFFFFFFF
    p7 = ((p7 << 32) ^ p7) & 0xFFFFFFFFFFFFFFFF
    p1 = ((p1 >> 32) ^ p1) & 0xFFFFFFFFFFFFFFFF
    p6 = ((p6 << 32) ^ p6) & 0xFFFFFFFFFFFFFFFF
    p3 = ((p3 << 16) ^ p3) & 0xFFFFFFFFFFFFFFFF
    p8 = ((p8 << 16) ^ p8) & 0xFFFFFFFFFFFFFFFF
    p6 = ((p6 >> 16) ^ p6) & 0xFFFFFFFFFFFFFFFF
    p2 = ((p2 >> 16) ^ p2) & 0xFFFFFFFFFFFFFFFF
    p5 = ((p5 >> 16) ^ p5) & 0xFFFFFFFFFFFFFFFF
    p1 = ((p1 >> 16) ^ p1) & 0xFFFFFFFFFFFFFFFF
    p7 = ((p7 << 16) ^ p7) & 0xFFFFFFFFFFFFFFFF
    p4 = ((p4 << 16) ^ p4) & 0xFFFFFFFFFFFFFFFF

    return ((p7 & 0xFF000000000000) ^
            (((p7 & 0xFF000000000000FF) ^ ((p1 & 0xFF00) & 0xFFFFFFFF) ^
              (p3 & 0xFF000000) ^ (p5 & 0xFF0000000000)) >> 8) ^
            (((p2 & 0xFF) ^ ((p4 & 0xFF0000) & 0xFFFFFFFF) ^
              (p6 & 0xFF00000000)) << 8) ^
            ((((p8 << 8) ^ p8)) & 0xFF00000000000000) ^
            (p1 & 0xFF) ^
            ((p3 & 0xFF0000) & 0xFFFFFFFF) ^
            (p5 & 0xFF00000000) ^
            ((p2 & 0xFF00) & 0xFFFFFFFF) ^
            (p4 & 0xFF000000) ^
            (p6 & 0xFF0000000000)) & 0xFFFFFFFFFFFFFFFF


def smatsymdata_hash_a(param1, param2):
    v0 = MATSYM_TABLE[(param1 >> 0) & 0xFF] & param2
    v1 = MATSYM_TABLE[(param1 >> 8) & 0xFF] & param2
    v2 = MATSYM_TABLE[(param1 >> 16) & 0xFF] & param2
    v3 = MATSYM_TABLE[(param1 >> 24) & 0xFF] & param2
    v4 = MATSYM_TABLE[(param1 >> 32) & 0xFF] & param2
    v5 = MATSYM_TABLE[(param1 >> 40) & 0xFF] & param2
    v6 = MATSYM_TABLE[(param1 >> 48) & 0xFF] & param2
    v7 = MATSYM_TABLE[(param1 >> 56) & 0xFF] & param2
    return mix_function(v0, v1, v2, v3, v4, v5, v6, v7)


# Cache for CMatSym::Hash results to avoid deep recursion on long strings
_cmatsym_cache = {}


def cmatsym_hash(s):
    """CMatSym::Hash — recursive, processes string BACKWARDS."""
    if not s:
        return 0x8040201008040201
    if s in _cmatsym_cache:
        return _cmatsym_cache[s]
    result = cmatsym_hash(s[1:])
    char_value = MATSYM_CHAR_TABLE[ord(s[0])]
    result = smatsymdata_hash_a(char_value, result)
    _cmatsym_cache[s] = result
    return result


# Namespace seeds (verified from binary analysis)
SNS_MESSAGE_SEED = 0x6d451003fb4b172e   # SNS messages (registration functions)
NRADGAME_SEED = 0x6d451003fb4b172e      # NRadGame replicated events (same seed)


def compute_sns_message_hash(name: str) -> int:
    """Full SNS message hash: strip 'S' prefix, CMatSym::Hash, HashA with seed."""
    if name.startswith('S'):
        hash_input = name[1:]
    else:
        hash_input = name
    name_hash = cmatsym_hash(hash_input)
    return smatsymdata_hash_a(SNS_MESSAGE_SEED, name_hash)


def compute_nradgame_event_hash(name: str) -> int:
    """NRadGame replicated event hash: CMatSym::Hash, HashA with namespace seed."""
    name_hash = cmatsym_hash(name)
    return smatsymdata_hash_a(NRADGAME_SEED, name_hash)


# ============================================================================
# Parsing
# ============================================================================

def parse_candidates(lines: List[str]) -> Tuple[List[Tuple[int, str]], List[str]]:
    """
    Parse candidate input lines.
    Returns:
      precomputed: list of (hash_value, name) from @ lines
      strings: list of candidate strings to hash forward
    """
    precomputed = []
    strings = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('@'):
            # @0xHEX name
            m = re.match(r'@(0x[0-9A-Fa-f]{1,16})\s+(.+)', line)
            if m:
                h = int(m.group(1), 16)
                name = m.group(2).strip()
                precomputed.append((h, name))
            else:
                print(f"Warning: malformed @ line: {line}", file=sys.stderr)
        else:
            strings.append(line)
    return precomputed, strings


# ============================================================================
# Generation
# ============================================================================

H_HEADER = """// AUTO-GENERATED by tools/gen_symbol_corpus.py — DO NOT EDIT BY HAND.
// Re-run the generator when the game binary is updated to refresh the string pool.
//
// Maps uint64_t symbol hash values to human-readable names.
// Two hash pipelines are supported:
//   1. CSymbol64 (CRC64 with polynomial 0x95ac9329ac4bc9b5) — resource / level names
//   2. CMatSym::Hash + SMatSymData::HashA — SNS messages, NRadGame events
//
// LookupSymbolName() searches both tables and returns the first match.

#pragma once

#include <cstdint>

namespace EchoVR {

/// Look up a symbol name by its 64-bit hash value.
/// Returns the name string if found, or nullptr if unknown.
/// The caller should format a hex fallback when nullptr is returned.
const char* LookupSymbolName(uint64_t hash);

/// Format a symbol ID for display.
/// Writes at most maxLen bytes (including null terminator) to buf.
/// If the hash is known, writes the name; otherwise writes the hex value.
/// Returns the number of bytes that would have been written (excluding null).
int FormatSymbolId(char* buf, int maxLen, uint64_t hash);

}  // namespace EchoVR
"""

CPP_HEADER = """// AUTO-GENERATED by tools/gen_symbol_corpus.py — DO NOT EDIT BY HAND.
// Re-run the generator when the game binary is updated to refresh the string pool.

#include "runtime/hook/symbol_corpus.h"
#include <cstdio>

namespace EchoVR {

namespace {

struct SymbolEntry {
    uint64_t hash;
    const char* name;
};

"""

CPP_FOOTER = """
}  // namespace

const char* LookupSymbolName(uint64_t hash) {
    // Binary search in the combined sorted table
    size_t lo = 0, hi = kSymbolCorpusSize;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kSymbolCorpus[mid].hash < hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < kSymbolCorpusSize && kSymbolCorpus[lo].hash == hash) {
        return kSymbolCorpus[lo].name;
    }
    return nullptr;
}

int FormatSymbolId(char* buf, int maxLen, uint64_t hash) {
    const char* name = LookupSymbolName(hash);
    if (name != nullptr) {
        return snprintf(buf, static_cast<size_t>(maxLen), "%s", name);
    }
    return snprintf(buf, static_cast<size_t>(maxLen), "0x%016llx",
                    static_cast<unsigned long long>(hash));
}

}  // namespace EchoVR
"""


def generate(entries: List[Tuple[int, str]], audit_hashes: Set[int] = None):
    """
    Generate the full .cpp file content.
    `entries` is a list of (hash, name) tuples, sorted by hash ascending.
    """
    parts = [CPP_HEADER]

    # Emit the sorted table
    parts.append(f"// {len(entries)} entries total\n")
    parts.append("constexpr SymbolEntry kSymbolCorpus[] = {\n")
    for h, name in entries:
        # Escape backslashes and quotes in names (unlikely but safe)
        safe_name = name.replace('\\', '\\\\').replace('"', '\\"')
        parts.append(f'    {{0x{h:016x}ULL, "{safe_name}"}},\n')
    parts.append("};\n\n")
    parts.append(f"constexpr size_t kSymbolCorpusSize = {len(entries)};\n")

    # Audit log coverage report as a comment
    if audit_hashes:
        resolved = sum(1 for h in audit_hashes if any(e[0] == h for e in entries))
        parts.append(f"\n// Audit log coverage: {resolved}/{len(audit_hashes)} "
                     f"({100.0*resolved/len(audit_hashes):.1f}%) — "
                     f"generated {len(entries)} entries\n")
        unresolved = [h for h in sorted(audit_hashes) if not any(e[0] == h for e in entries)]
        if unresolved:
            parts.append("// Unresolved audit log hashes:\n")
            for h in unresolved:
                parts.append(f"//   0x{h:016x}\n")

    parts.append(CPP_FOOTER)
    return ''.join(parts)


def main():
    parser = argparse.ArgumentParser(
        description="Generate CSymbol64 / SNS message hash→name corpus C++ source")
    parser.add_argument('--audit-log', help='Path to audit log for coverage report')
    parser.add_argument('--split-dir', help='Write .h and .cpp to this directory')
    parser.add_argument('--candidates', help='Read candidates from file (default: stdin)')
    args = parser.parse_args()

    # Read candidates
    if args.candidates:
        with open(args.candidates) as f:
            raw_lines = f.readlines()
    else:
        raw_lines = sys.stdin.readlines()

    precomputed, strings = parse_candidates(raw_lines)

    # Build entries: (hash, name)
    entries: Dict[int, str] = {}

    # 1. Pre-computed entries (from symbols.h internal labels)
    for h, name in precomputed:
        if h not in entries:
            entries[h] = name

    # 2. CSymbol64Hash of candidate strings
    for s in strings:
        h = csymbol64_hash(s)
        if h not in entries:
            entries[h] = s

    # 3. SNS message hash of candidate strings (try both with and without 'S' prefix)
    for s in strings:
        # Try as-is (may already have S prefix)
        h = compute_sns_message_hash(s)
        if h not in entries:
            entries[h] = f"SNS:{s}"

        # Try with SNS prefix
        if not s.startswith('SNS') and not s.startswith('SBroadcaster'):
            sns_name = f"SNS{s}"
            h = compute_sns_message_hash(sns_name)
            if h not in entries:
                entries[h] = sns_name

    # 4. NRadGame event hash (delegate_* names)
    for s in strings:
        if s.startswith('delegate_'):
            h = compute_nradgame_event_hash(s)
            if h not in entries:
                entries[h] = s

    # Sort by hash
    sorted_entries = sorted(entries.items())

    # Load audit hashes if provided
    audit_hashes = None
    if args.audit_log:
        audit_hashes = set()
        with open(args.audit_log) as f:
            for line in f:
                for m in re.finditer(r'0x([0-9A-Fa-f]{16})', line):
                    audit_hashes.add(int(m.group(1), 16))

    # Generate
    cpp_content = generate(sorted_entries, audit_hashes)

    if args.split_dir:
        os.makedirs(args.split_dir, exist_ok=True)
        with open(os.path.join(args.split_dir, 'symbol_corpus.h'), 'w') as f:
            f.write(H_HEADER)
        with open(os.path.join(args.split_dir, 'symbol_corpus.cpp'), 'w') as f:
            f.write(cpp_content)
        print(f"Wrote {args.split_dir}/symbol_corpus.{{h,cpp}} "
              f"({len(sorted_entries)} entries)", file=sys.stderr)
        if audit_hashes:
            resolved = sum(1 for h in audit_hashes if any(e[0] == h for e in sorted_entries))
            print(f"Audit log coverage: {resolved}/{len(audit_hashes)} "
                  f"({100.0*resolved/len(audit_hashes):.1f}%)", file=sys.stderr)
    else:
        # Output combined
        print(f"/* {len(sorted_entries)} entries */")
        print(H_HEADER)
        print()
        print(cpp_content)

    # Return stats
    return sorted_entries, audit_hashes


if __name__ == '__main__':
    entries, audit_hashes = main()
