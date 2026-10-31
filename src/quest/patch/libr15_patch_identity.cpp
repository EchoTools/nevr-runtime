#include "quest/patch/libr15_patch_identity.h"

#include <algorithm>
#include <cstdio>

#include "quest/data/sha256.h"

namespace nevr_quest::patch {
namespace {

const char* KindName(RowKind kind) {
  switch (kind) {
    case RowKind::kInlineHook: return "inline";
    case RowKind::kCreditsPair: return "credits";
    case RowKind::kWordPatch: return "word";
  }
  return "unknown";
}

std::string Hex(std::uint64_t value) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(value));
  return buf;
}

int Nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

bool ParseBuildIdHex(const std::string& hex, BuildIdBytes* out) {
  if (hex.size() != kBuildIdBytes * 2) return false;
  for (std::size_t i = 0; i < kBuildIdBytes; ++i) {
    const int hi = Nibble(hex[2 * i]);
    const int lo = Nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    (*out)[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return true;
}

std::string BuildIdHex(const BuildIdBytes& id) {
  std::string out;
  char buf[3];
  for (const std::uint8_t b : id) {
    std::snprintf(buf, sizeof buf, "%02x", b);
    out += buf;
  }
  return out;
}

std::string CanonicalTable(const Row* rows, std::size_t rowCount, const std::vector<WordPatchRow>& extra,
                           std::uint64_t slotArea) {
  std::string s = "layout|bias=" + Hex(kDisasmBias) + "|slot_area=" + Hex(slotArea) + "|slot_bytes=" + Hex(kSlotBytes) + "\n";
  for (std::size_t i = 0; i < rowCount; ++i) {
    const Row& row = rows[i];
    s += std::string("row|") + row.id + "|" + KindName(row.kind) + "|slot=" + Hex(row.slot);
    for (std::size_t site = 0; site < row.siteCount; ++site) {
      s += "|" + Hex(row.sites[site].disasmVa) + ":";
      for (std::size_t w = 0; w < row.sites[site].wordCount; ++w) {
        if (w != 0) s += ",";
        s += Hex(row.sites[site].words[w]);
      }
    }
    s += "\n";
  }
  if (extra.empty()) return s;
  std::vector<const WordPatchRow*> sorted;
  for (const WordPatchRow& r : extra) sorted.push_back(&r);
  std::sort(sorted.begin(), sorted.end(), [](const WordPatchRow* a, const WordPatchRow* b) {
    if (a->disasmVa != b->disasmVa) return a->disasmVa < b->disasmVa;
    return a->id < b->id;
  });
  s += "extra|" + Hex(sorted.size()) + "\n";
  auto put = [&s](std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) s += static_cast<char>((value >> (8 * i)) & 0xFF);
  };
  for (const WordPatchRow* r : sorted) {
    put(r->id.size(), 1);
    s += r->id;
    put(r->disasmVa, 8);
    put(r->edits.size(), 1);
    for (const WordEdit& e : r->edits) {
      put(e.offset, 2);
      for (const std::uint8_t b : e.word) put(b, 1);
      for (const std::uint8_t b : e.stock) put(b, 1);
    }
  }
  return s;
}

BuildIdBytes DeriveBuildId(const BuildIdBytes& stockBuildId, const std::string& canonicalTable) {
  data::Sha256 sha;
  sha.Update(kBuildIdDomain, std::char_traits<char>::length(kBuildIdDomain));
  sha.Update(stockBuildId.data(), stockBuildId.size());
  sha.Update(canonicalTable.data(), canonicalTable.size());
  const std::string digest = sha.Hex();
  BuildIdBytes full{};
  ParseBuildIdHex(digest.substr(0, kBuildIdBytes * 2), &full);
  return full;
}

std::string PatchedBuildId(const std::vector<WordPatchRow>& extra) {
  BuildIdBytes stock{};
  ParseBuildIdHex(kLibR15StockBuildId, &stock);
  return BuildIdHex(DeriveBuildId(stock, CanonicalTable(kRows, kRowCount, extra, kSlotAreaVaddr)));
}

}  // namespace nevr_quest::patch
