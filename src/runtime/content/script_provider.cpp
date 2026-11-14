#include "runtime/content/script_provider.h"

#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#include "core/log_text.h"
#include "core/logging.h"
#include "core/map_script_check.h"
#include "core/script_image_rules.h"
#include "quest/data/sha256.h"
#include "runtime/content/content_pack.h"

namespace nevr_map_scripts {

namespace {

// The widths the log lines and the path handling use, named so no literal sits in a call.
constexpr std::size_t kHash16Chars = 16;
constexpr std::size_t kLogTextBytes = 300;        // the most of a text one log field carries; the rest is cut and counted
constexpr std::size_t kFinalPathStartChars = 512;  // the first buffer for GetFinalPathNameByHandleW; grown when it is short
constexpr std::wstring_view kVolumePrefix = L"\\\\?\\";
constexpr std::wstring_view kUncPrefix = L"\\\\?\\UNC\\";

struct Entry {
  std::string owner;
  std::string sha256;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::wstring final_path;
  bool served = false;  // the rewriter has handed this file to the loader
};

struct State {
  bool configured = false;
  Inputs inputs;
  std::set<std::string> revoked;
  std::set<std::string> withdrawn;  // owners Withdraw dropped: their scripts are not taken again in this process
  std::map<std::uint64_t, Entry> held;
  std::unordered_map<std::wstring, std::uint64_t> by_path;  // CanonicalScriptPath -> hash
};

std::mutex g_mutex;
State g_state;

Outcome Refuse(const char* reason, std::string detail) {
  Outcome o;
  o.reason = reason;
  o.detail = std::move(detail);
  return o;
}

std::string Hash16(std::uint64_t hash) {
  char buf[kHash16Chars + 1];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
  return buf;
}

// A text from outside, made safe for one quoted log field (core/log_text.h: printable ASCII, '"' and '\\' written as '?').
std::string Printable(const std::string& text) { return nevr_log_text::Printable(text.c_str(), kLogTextBytes); }

// One record per Register or RegisterRekey: Info when the script is held, Warning with the token when it is refused.
void LogOutcome(const char* kind, const std::string& owner, std::uint64_t hash, const Outcome& o, const std::wstring& path) {
  if (o.ok()) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.MAPS] map script held kind=%s owner=%s hash=%s file=\"%s\" (the game's loads of scripts\\%s.dll map this file)",
        kind, Printable(owner).c_str(), Hash16(hash).c_str(), Printable(nevr_content::Narrow(path)).c_str(),
        Hash16(hash).c_str());
    return;
  }
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.MAPS] map script refused kind=%s owner=%s hash=%s reason=%s detail=\"%s\" (the map's script is not served; "
      "a level that needs it fails to load it)",
      kind, Printable(owner).c_str(), Hash16(hash).c_str(), o.reason.c_str(), Printable(o.detail).c_str());
}

bool FinalPathOfHandle(HANDLE file, std::wstring* out) {
  std::wstring buf(kFinalPathStartChars, L'\0');
  for (;;) {
    const DWORD n = GetFinalPathNameByHandleW(file, buf.data(), static_cast<DWORD>(buf.size()),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) return false;
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(static_cast<std::size_t>(n) + 1);
  }
  if (std::wstring_view(buf).substr(0, kUncPrefix.size()) == kUncPrefix) {
    // "\\?\UNC\server\share" is the UNC path "\\server\share": the backslash that ends the UNC prefix stays.
    buf = L"\\" + buf.substr(kUncPrefix.size() - 1);
  } else if (std::wstring_view(buf).substr(0, kVolumePrefix.size()) == kVolumePrefix) {
    buf.erase(0, kVolumePrefix.size());
  }
  *out = std::move(buf);
  return true;
}

// The whole file, from its start, through `file`: its size must pass the bound and the pinned size (0: none pins it).
Outcome ReadAll(HANDLE file, std::vector<std::uint8_t>* bytes, std::uint64_t pinned_bytes, std::uint64_t byte_limit) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) return Refuse("unreadable", "size error=" + std::to_string(GetLastError()));
  if (const char* token = nevr_map_script::SizeRefusal(static_cast<std::uint64_t>(size.QuadPart), pinned_bytes, byte_limit)) {
    return Refuse(token, std::to_string(size.QuadPart) + " bytes");
  }
  LARGE_INTEGER start{};
  if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
    return Refuse("unreadable", "seek error=" + std::to_string(GetLastError()));
  }
  bytes->resize(static_cast<std::size_t>(size.QuadPart));
  DWORD got = 0;
  if (!ReadFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &got, nullptr) || got != bytes->size()) {
    return Refuse("unreadable", "read " + std::to_string(got) + " of " + std::to_string(bytes->size()) +
                                    " error=" + std::to_string(GetLastError()));
  }
  return Outcome{};
}

// The checks that need no file, under g_mutex: the run, the device's limit, the arguments, then the claim's facts in the order
// the shared rule gives (nevr_map_script::ClaimRefusal). `official` is the manifest's fact: a signed-only run refuses only a
// script whose manifest is not official (ADR 0048 decision 7).
Outcome CheckClaimLocked(const std::string& owner, std::uint64_t hash, const std::string& sha256, bool official) {
  const State& s = g_state;
  if (!s.configured) return Refuse("not_configured", "Configure was not called");
  if (s.inputs.byte_limit == 0) return Refuse("not_configured", "no device fact gives the byte limit");
  if (const char* token = nevr_map_script::ArgumentRefusal(owner, hash, sha256)) {
    return Refuse(token, Printable(owner) + " " + Hash16(hash));
  }
  const std::wstring key = CanonicalScriptPath(s.inputs.exe_dir, hash);
  nevr_map_script::ClaimFacts facts;
  facts.withdrawn = s.withdrawn.count(owner) != 0;
  facts.signed_only = s.inputs.signed_only && !official;
  facts.revoked = s.revoked.count(sha256) != 0;
  facts.duplicate = s.held.count(hash) != 0;
  facts.stock = GetFileAttributesW(key.c_str()) != INVALID_FILE_ATTRIBUTES;
  facts.pack = s.inputs.pack_carries != nullptr && s.inputs.pack_carries(key.c_str());
  if (const char* token = nevr_map_script::ClaimRefusal(facts)) return Refuse(token, Hash16(hash));
  return Outcome{};
}

// Holds `file` under `hash`; the claim is checked again, since the file was read outside the lock.
Outcome HoldLocked(const std::string& owner, std::uint64_t hash, const std::string& sha256, bool official, HANDLE file,
                   std::wstring final_path) {
  const Outcome again = CheckClaimLocked(owner, hash, sha256, official);
  if (!again.ok()) return again;
  const std::wstring key = CanonicalScriptPath(g_state.inputs.exe_dir, hash);
  g_state.held[hash] = Entry{owner, sha256, file, std::move(final_path)};
  g_state.by_path[key] = hash;
  return Outcome{};
}

std::uint64_t ByteLimitLocked() { return g_state.inputs.byte_limit; }

}  // namespace

std::wstring CanonicalScriptPath(const std::wstring& exe_dir, std::uint64_t hash) {
  const std::string name = Hash16(hash);
  return nevr_content::Canonical(exe_dir + L"\\scripts\\" + std::wstring(name.begin(), name.end()) + L".dll");
}

void Configure(const Inputs& inputs) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.inputs = inputs;
  g_state.configured = true;
}

void SetRevoked(const std::vector<std::string>& revoked_sha256) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.revoked.insert(revoked_sha256.begin(), revoked_sha256.end());
  for (auto it = g_state.held.begin(); it != g_state.held.end();) {
    if (g_state.revoked.count(it->second.sha256) == 0) {
      ++it;
      continue;
    }
    g_state.by_path.erase(CanonicalScriptPath(g_state.inputs.exe_dir, it->first));
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map script dropped owner=%s hash=%s reason=revoked sha256=%s (the manifest revokes it; it is no longer "
        "served)",
        Printable(it->second.owner).c_str(), Hash16(it->first).c_str(), it->second.sha256.c_str());
    CloseHandle(it->second.file);
    it = g_state.held.erase(it);
  }
}

namespace {

// PC10 writes a rebuilt script as <version dir>/scripts/<hash16>.dll (core/map_derive.h); the final path has backslashes.
bool IsRebuiltScriptPath(const std::wstring& final_path, std::uint64_t hash) {
  const std::string name = "\\scripts\\" + Hash16(hash) + ".dll";
  const std::wstring tail(name.begin(), name.end());
  if (final_path.size() <= tail.size()) return false;
  const int n = static_cast<int>(tail.size());
  return CompareStringOrdinal(final_path.c_str() + (final_path.size() - tail.size()), n, tail.c_str(), n, TRUE) ==
         CSTR_EQUAL;
}

Outcome RegisterHeld(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                     bool rehash, bool rebuilt, std::uint64_t pinned_bytes, bool official, std::wstring* held_path) {
  std::uint64_t byte_limit = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Outcome claim = CheckClaimLocked(owner, hash, sha256, official);
    if (!claim.ok()) return claim;
    byte_limit = ByteLimitLocked();
  }
  HANDLE file = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(held_handle));
  if (file == INVALID_HANDLE_VALUE || file == nullptr) return Refuse("unreadable", "no handle");
  std::vector<std::uint8_t> bytes;
  const Outcome read = ReadAll(file, &bytes, pinned_bytes, byte_limit);
  if (!read.ok()) return read;
  if (rehash) {
    const std::string got = nevr_quest::data::Sha256Hex(bytes.data(), bytes.size());
    if (const char* token = nevr_map_script::DigestRefusal(got, sha256)) {
      return Refuse(token, "sha256=" + got + " want=" + sha256);
    }
  }
  const nevr_script_image::ImageVerdict verdict = nevr_script_image::CheckPeScript(bytes.data(), bytes.size());
  if (!verdict.ok()) return Refuse(nevr_map_script::ImageRefusalToken(verdict.reason), verdict.detail);
  std::wstring final_path;
  if (!FinalPathOfHandle(file, &final_path)) return Refuse("no_final_path", "error=" + std::to_string(GetLastError()));
  if (rebuilt && !IsRebuiltScriptPath(final_path, hash)) return Refuse("derived_path", nevr_content::Narrow(final_path));
  *held_path = final_path;
  std::lock_guard<std::mutex> lock(g_mutex);
  return HoldLocked(owner, hash, sha256, official, file, std::move(final_path));
}

Outcome RegisterStock(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                      const std::string& stock_sha256, bool official, std::wstring* held_path) {
  std::wstring stock_path;
  std::uint64_t byte_limit = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Outcome claim = CheckClaimLocked(owner, hash, stock_sha256, official);
    if (!claim.ok()) return claim;
    stock_path = CanonicalScriptPath(g_state.inputs.exe_dir, stock_source);
    byte_limit = ByteLimitLocked();
  }
  HANDLE file = CreateFileW(stock_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Refuse("stock_missing", Hash16(stock_source) + " error=" + std::to_string(GetLastError()));
  }
  std::vector<std::uint8_t> bytes;
  Outcome result = ReadAll(file, &bytes, /*pinned_bytes=*/0, byte_limit);
  if (result.ok()) {
    const std::string got = nevr_quest::data::Sha256Hex(bytes.data(), bytes.size());
    if (got != stock_sha256) result = Refuse("stock_sha_mismatch", Hash16(stock_source) + " sha256=" + got);
  }
  if (result.ok()) {
    // A rekeyed script passes the same image rules as a downloaded one (ADR 0048 decision 6).
    const nevr_script_image::ImageVerdict verdict = nevr_script_image::CheckPeScript(bytes.data(), bytes.size());
    if (!verdict.ok()) result = Refuse(nevr_map_script::ImageRefusalToken(verdict.reason), verdict.detail);
  }
  std::wstring final_path;
  if (result.ok() && !FinalPathOfHandle(file, &final_path)) {
    result = Refuse("no_final_path", "error=" + std::to_string(GetLastError()));
  }
  if (result.ok()) {
    *held_path = final_path;
    std::lock_guard<std::mutex> lock(g_mutex);
    result = HoldLocked(owner, hash, stock_sha256, official, file, std::move(final_path));
  }
  if (!result.ok()) CloseHandle(file);
  return result;
}

}  // namespace

Outcome Register(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                 bool rehash, std::uint64_t pinned_bytes, bool official) {
  std::wstring path;
  const Outcome o = RegisterHeld(owner, hash, held_handle, sha256, rehash, false, pinned_bytes, official, &path);
  LogOutcome("map", owner, hash, o, path);
  return o;
}

Outcome RegisterDerived(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                        const std::string& stock_sha256, bool official) {
  std::wstring path;
  const Outcome o = nevr_map_script::IsSha256Digest(stock_sha256)
                        ? RegisterHeld(owner, hash, held_handle, sha256, true, true, 0, official, &path)
                        : Refuse("bad_sha256", "stock " + stock_sha256);
  LogOutcome("derived", owner, hash, o, path);
  return o;
}

Outcome RegisterRekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                      const std::string& stock_sha256, bool official) {
  std::wstring path;
  const Outcome o = RegisterStock(owner, hash, stock_source, stock_sha256, official, &path);
  LogOutcome("rekey", owner, hash, o, path);
  return o;
}

bool Holds(const std::string& owner, std::uint64_t name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_state.held.find(name);
  return it != g_state.held.end() && it->second.owner == owner;
}

bool Rewrite(const wchar_t* lower_path, std::wstring* load_path, std::string* owner, std::uint64_t* hash, bool* first) {
  if (lower_path == nullptr || load_path == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state.by_path.empty()) return false;
  const auto it = g_state.by_path.find(nevr_content::Canonical(lower_path));
  if (it == g_state.by_path.end()) return false;
  Entry& entry = g_state.held.at(it->second);
  if (first != nullptr) *first = !entry.served;
  entry.served = true;
  *load_path = entry.final_path;
  if (owner != nullptr) *owner = entry.owner;
  if (hash != nullptr) *hash = it->second;
  return true;
}

std::size_t Withdraw(const std::string& owner) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& [hash, entry] : g_state.held) {
    if (entry.owner != owner || !entry.served) continue;
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map script refused kind=withdraw owner=%s hash=%s reason=withdraw_after_load (the game has loaded "
        "this script; the map's scripts stay served)",
        Printable(owner).c_str(), Hash16(hash).c_str());
    return 0;
  }
  std::size_t dropped = 0;
  for (auto it = g_state.held.begin(); it != g_state.held.end();) {
    if (it->second.owner != owner) {
      ++it;
      continue;
    }
    g_state.by_path.erase(CanonicalScriptPath(g_state.inputs.exe_dir, it->first));
    CloseHandle(it->second.file);
    it = g_state.held.erase(it);
    ++dropped;
  }
  // Recorded whether or not the owner held anything: a set that failed before its first script keeps its later ones out too.
  g_state.withdrawn.insert(owner);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] map script withdrawn owner=%s dropped=%zu (the map did not mount; none of its scripts is served)",
      Printable(owner).c_str(), dropped);
  return dropped;
}

std::size_t HeldCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state.held.size();
}

void Forget() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& [hash, entry] : g_state.held) CloseHandle(entry.file);
  g_state = State{};
}

}  // namespace nevr_map_scripts
