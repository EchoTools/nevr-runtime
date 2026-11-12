// What test_map_channel and ic_map_channel share: the test minisign key, the folder bucket and its loopback server,
// the fake resource-set registry, the zip reader, and the MapChannel fixture over a temp root.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "core/base64.h"
#include "core/config_alias.h"
#include "core/logging.h"
#include "core/map_run.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nevr/symbol_hash.h"
#include "runtime/cargo/win_env.h"
#include "runtime/content/map_channel.h"
#include "runtime/content/map_session_pin.h"
#include "runtime/content/script_provider.h"

namespace {

using namespace nevr_map_channel::detail;
using json = nlohmann::json;
namespace maps = nevr_map_scripts;

// ---- keys and signatures (minisign "ED": Ed25519 over BLAKE2b-512 of the message) ----
struct TestKey {
  std::uint8_t secret[64];
  nevr_maps::PinnedKey pin;

  explicit TestKey(std::uint8_t seed_byte) {
    std::uint8_t seed[32];
    std::memset(seed, seed_byte, sizeof seed);
    crypto_ed25519_key_pair(secret, pin.key.pk.data(), seed);
    for (std::size_t i = 0; i < 8; ++i) pin.key.key_id[i] = static_cast<std::uint8_t>(0xA0 + seed_byte + i);
  }

  std::string PublicKeyFile(const std::string& comment) const {
    std::vector<std::uint8_t> blob = {'E', 'd'};
    blob.insert(blob.end(), pin.key.key_id.begin(), pin.key.key_id.end());
    blob.insert(blob.end(), pin.key.pk.begin(), pin.key.pk.end());
    return "untrusted comment: " + comment + "\n" + base64_encode(blob) + "\n";
  }

  std::string Minisig(const std::string& message, const std::string& trusted_comment) const {
    std::uint8_t digest[64];
    crypto_blake2b(digest, sizeof digest, reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    std::vector<std::uint8_t> blob = {'E', 'D'};
    blob.insert(blob.end(), pin.key.key_id.begin(), pin.key.key_id.end());
    std::uint8_t signature[64];
    crypto_ed25519_sign(signature, secret, digest, sizeof digest);
    blob.insert(blob.end(), signature, signature + 64);
    std::vector<std::uint8_t> global_message(signature, signature + 64);
    global_message.insert(global_message.end(), trusted_comment.begin(), trusted_comment.end());
    std::uint8_t global[64];
    crypto_ed25519_sign(global, secret, global_message.data(), global_message.size());
    return "untrusted comment: signature from a test key\n" + base64_encode(blob) + "\ntrusted comment: " +
           trusted_comment + "\n" + base64_encode(global, sizeof global) + "\n";
  }
};

std::string Sha(const std::string& bytes) {
  std::unique_ptr<nevr_cargo::Env> env = nevr_cargo::MakeWinEnv("");
  std::unique_ptr<nevr_cargo::Hasher> h = env->NewHasher();
  h->Update(bytes.data(), bytes.size());
  return h->Hex();
}

std::string Hex16(std::uint64_t v) {
  char text[17];
  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
  return text;
}
std::string Hex64(std::uint64_t v) { return "0x" + Hex16(v); }

void WriteFile(const std::string& path, const std::string& bytes) {
  const std::wstring wide = Wide(path);
  for (std::size_t at = wide.find_first_of(L"\\/", 3); at != std::wstring::npos; at = wide.find_first_of(L"\\/", at + 1)) {
    CreateDirectoryW(wide.substr(0, at).c_str(), nullptr);
  }
  std::ofstream out(std::filesystem::path(wide), std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) ADD_FAILURE() << "could not write " << path << " (held open without write sharing?)";
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(std::filesystem::path(Wide(path)), std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

void RemoveTree(const std::wstring& dir) {
  // Only a full path below a drive root: an empty one turns L"\\*" into the current drive's root, which is
  // how a failed SetUp wiped D:\b on 2026-11-12.
  if (dir.size() < 4 || dir[1] != L':' || dir[2] != L'\\') return;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring path = dir + L"\\" + name;
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        RemoveTree(path);
      } else {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
      }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  RemoveDirectoryW(dir.c_str());
}

// ---- a map's package: one set, one script, one rules alias ----
constexpr std::uint64_t kScriptHash = 0x5ca1ab1e00c0ffeeULL;

struct MapFiles {
  std::string id, version, set, script, index;
  std::uint64_t level = 0, alias_key = 0, alias = 0;
};

// ---- the bucket, published into a folder ----
struct Bucket {
  std::string dir;
  const TestKey& key;
  json maps = json::object();

  json Blob(const std::string& bytes) {
    const std::string sha = Sha(bytes);
    WriteFile(dir + "/blobs/sha256/" + sha, bytes);
    return {{"sha256", sha}, {"size", bytes.size()}};
  }
  void Publish(const MapFiles& m) {
    Blob(m.set);
    Blob(m.script);
    maps[m.id] = {{"title", "Title " + m.id}, {"author", "tester"}, {"thumbnail", Blob("thumb " + m.id)}};
    maps[m.id]["platforms"][nevr_maps::kPlatformPc] = {{"version", m.version},
                                                      {"index", Blob(m.index)},
                                                      {"level", Hex64(m.level)},
                                                      {"level_name", m.id},
                                                      {"game_build", "echovr.exe test"},
                                                      {"bytes", m.set.size() + m.script.size()},
                                                      {"scripts", 1}};
  }
  void Sign(std::uint64_t serial, const std::vector<std::string>& revoked_ids = {}) {
    const json body = {{"schema", "nevr-maps-manifest"}, {"schema_version", 1},      {"channel", "echo-test"},
                       {"serial", serial},                {"expires", "2099-01-01T00:00:00Z"}, {"retired", json::object()},
                       {"revoked_ids", revoked_ids},      {"revoked_sha256", json::array()}, {"maps", maps}};
    const std::string text = body.dump();
    const std::string sig = key.Minisig(text, "timestamp:1762819200");
    WriteFile(dir + "/v1/maps/manifest.signed", sig.substr(0, sig.size()) + "--\n" + text);
  }
};

// ---- a loopback server for a folder: GET /<path> answers the file or 404 ----
class FolderServer {
 public:
  explicit FolderServer(std::string root) : root_(std::move(root)) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    listen(listener_, 16);
    int len = sizeof addr;
    getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }
  ~FolderServer() {
    closesocket(listener_);
    thread_.join();
    WSACleanup();
  }
  std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }
  std::size_t Gets() const { return gets_.load(); }
  // Every request path, in arrival order.
  std::vector<std::string> Paths() const {
    std::lock_guard<std::mutex> lock(paths_mutex_);
    return paths_;
  }

 private:
  void Serve() {
    for (;;) {
      SOCKET c = accept(listener_, nullptr, nullptr);
      if (c == INVALID_SOCKET) return;
      std::string head;
      char buf[4096];
      while (head.find("\r\n\r\n") == std::string::npos) {
        const int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        head.append(buf, static_cast<std::size_t>(n));
      }
      ++gets_;
      const std::size_t from = head.find(' ') + 2, to = head.find(' ', from);
      const std::string path = head.substr(from, to - from);
      {
        std::lock_guard<std::mutex> lock(paths_mutex_);
        paths_.push_back(path);
      }
      const DWORD attrs = GetFileAttributesW(Wide(root_ + "/" + path).c_str());
      const bool found = path.find("..") == std::string::npos && attrs != INVALID_FILE_ATTRIBUTES &&
                         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
      const std::string body = found ? ReadFile(root_ + "/" + path) : std::string();
      const std::string reply = std::string(found ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
                                "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      send(c, reply.data(), static_cast<int>(reply.size()), 0);
      shutdown(c, SD_SEND);
      while (recv(c, buf, sizeof buf, 0) > 0) {
      }
      closesocket(c);
    }
  }

  std::string root_;
  SOCKET listener_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::thread thread_;
  std::atomic<std::size_t> gets_{0};
  mutable std::mutex paths_mutex_;
  std::vector<std::string> paths_;
};

// The types of stock mp_arena_rules and of the global_mp registry, read from the stock Win10 manifest 48037dc70b0ecab2.
constexpr std::uint64_t kStockJsonResourceWin10 = 0x607d858c90268f9aull;
constexpr std::uint64_t kStockJsonConfigCRWin10 = 0xcb3478c2a8cd87ccull;

// ---- the fake registry: takes every set but the refused owner's; the mount mounts what it took ----
struct FakeRegistry {
  std::map<std::string, std::int64_t> taken;  // owner -> handle
  std::map<std::string, std::uint64_t> levels, aliases;
  std::set<std::string> refuse_at_register, refuse_at_mount, withdrawn;
  std::uint64_t alias_type = kStockJsonResourceWin10;  // the type the sets hold their alias records under
  bool mounted = false;
};
FakeRegistry* g_registry = nullptr;

nevr_maps::Grant FakeRegister(const std::string& owner, std::int64_t held, const std::string&, const std::string&) {
  if (g_registry->refuse_at_register.count(owner) != 0) return nevr_maps::Grant{false, "collides"};
  g_registry->taken[owner] = held;
  return nevr_maps::Grant{true, ""};
}
void FakeWithdrawSet(const std::string& owner) {
  const auto it = g_registry->taken.find(owner);
  if (it == g_registry->taken.end()) return;
  CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(it->second)));
  g_registry->taken.erase(it);
  g_registry->withdrawn.insert(owner);
}
std::string FakeReport() {
  json sets = json::array();
  for (const auto& [owner, handle] : g_registry->taken) {
    if (g_registry->refuse_at_mount.count(owner) != 0) continue;
    sets.push_back({{"plugin", owner}, {"label", owner}, {"levels", json::array({Hex64(g_registry->levels[owner])})}});
  }
  return sets.dump();
}
bool FakeHoldsKey(std::uint64_t type, std::uint64_t name) {
  if (type != g_registry->alias_type) return false;
  for (const auto& [owner, alias] : g_registry->aliases) {
    if (alias == name && g_registry->taken.count(owner) != 0 && g_registry->refuse_at_mount.count(owner) == 0) return true;
  }
  return false;
}

bool NoPack(const wchar_t*) { return false; }

// The zip reader over bytes in memory: the directory, then every entry inflated.
bool ReadZipString(const std::string& bytes, std::vector<std::pair<std::string, std::string>>* entries,
                   std::string* problem) {
  const ReadAt read = [&](std::uint64_t offset, void* out, std::size_t length) {
    if (offset > bytes.size() || length > bytes.size() - offset) return false;
    std::memcpy(out, bytes.data() + offset, length);
    return true;
  };
  std::vector<ZipEntry> directory;
  if (!ReadZipDirectory(read, bytes.size(), &directory, problem)) return false;
  for (const ZipEntry& e : directory) {
    std::string data;
    const auto append = [&](const char* d, std::size_t n) {
      data.append(d, n);
      return true;
    };
    if (!InflateZipEntry(read, e, append, problem)) return false;
    entries->emplace_back(e.name, std::move(data));
  }
  return true;
}

// The real Win32 env, counting the bytes its hashers are given.
class HashCountingEnv : public nevr_cargo::Env {
 public:
  explicit HashCountingEnv(nevr_cargo::Env& real) : real_(real) {}
  std::uint64_t hashed = 0;

  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) override {
    return real_.ReadSmallFile(path, out, max_bytes);
  }
  bool MakeDirs(const std::string& dir) override { return real_.MakeDirs(dir); }
  std::unique_ptr<nevr_cargo::TempFile> CreateTemp(const std::string& dir) override { return real_.CreateTemp(dir); }
  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    return real_.WriteFileAtomically(path, data);
  }
  bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    return real_.OpenBlob(path, handle, size);
  }
  bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) override {
    return real_.ReadBlob(handle, offset, buffer, length, got);
  }
  void CloseBlob(std::int64_t handle) override { real_.CloseBlob(handle); }
  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override {
    return std::make_unique<Counting>(real_.NewHasher(), &hashed);
  }
  nevr_cargo::HttpResult Get(const std::string& url, const nevr_cargo::HttpBody& on_body,
                             const std::function<bool()>& keep_going) override {
    return real_.Get(url, on_body, keep_going);
  }
  std::uint64_t NowMs() override { return real_.NowMs(); }
  void Event(nevr_cargo::EventLevel level, const char* name, const std::vector<nevr_cargo::Field>& fields) override {
    real_.Event(level, name, fields);
  }

 private:
  class Counting : public nevr_cargo::Hasher {
   public:
    Counting(std::unique_ptr<nevr_cargo::Hasher> real, std::uint64_t* total) : real_(std::move(real)), total_(total) {}
    void Update(const void* data, std::size_t length) override {
      *total_ += length;
      real_->Update(data, length);
    }
    std::string Hex() override { return real_->Hex(); }

   private:
    std::unique_ptr<nevr_cargo::Hasher> real_;
    std::uint64_t* total_;
  };
  nevr_cargo::Env& real_;
};

class MapChannel : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    base_ = std::wstring(tmp) + L"nevr-map-channel-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
    root_ = Utf8(base_);
    std::replace(root_.begin(), root_.end(), '\\', '/');
    exe_dir_ = base_ + L"\\echovr\\bin\\win10";
    WriteFile(Utf8(exe_dir_) + "/scripts/.keep", "");
    ConfigureScripts();
    g_registry = &registry_;
    env_ = nevr_cargo::MakeWinEnv("[NEVR.MAPS]");
    fs_ = nevr_maps::MakeWinMapFs();
  }
  void TearDown() override {
    // Nothing mounted: every owner is withdrawn and no alias stays armed for the next test.
    ApplyMount(Plan(), "[]");
    nevr_config_alias::Reconcile(0, {});
    for (const auto& [owner, handle] : registry_.taken) env_->CloseBlob(handle);
    maps::Forget();
    g_registry = nullptr;
    RemoveTree(base_);
  }

  // What a process does at boot (SetPolicy): the provider starts empty and is configured for this exe folder.
  void ConfigureScripts() {
    maps::Forget();
    maps::Inputs in;
    in.exe_dir = exe_dir_;
    in.pack_carries = &NoPack;
    maps::Configure(in);
  }

  Registry Fakes() const { return Registry{&FakeRegister, &FakeReport, &FakeHoldsKey, &FakeWithdrawSet}; }
  MountPlan Plan() const {
    MountPlan plan;
    plan.expected = expected_;
    plan.pins = pins_;
    plan.registry = Fakes();
    return plan;
  }
  void Expect(const MapFiles& m) {
    expected_.push_back("map:" + m.id);
    registry_.levels["map:" + m.id] = m.level;
    registry_.aliases["map:" + m.id] = m.alias;
  }
  bool Served(const std::string& owner_expected, std::uint64_t script_hash = kScriptHash) {
    std::wstring lower = maps::CanonicalScriptPath(exe_dir_, script_hash);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::wstring load_path;
    std::string owner;
    std::uint64_t hash = 0;
    if (!maps::Rewrite(lower.c_str(), &load_path, &owner, &hash)) return false;
    EXPECT_EQ(owner, owner_expected);
    EXPECT_EQ(hash, script_hash);
    return true;
  }
  nevr_maps::ChannelConfig CdnConfig(const std::string& base_url, const TestKey& key) const {
    nevr_maps::ChannelConfig c;
    c.dir = root_ + "/cache/maps";
    c.base_url = base_url;
    c.subscribed = {"arena1"};
    c.keys = {key.pin};
    return c;
  }
  // The LOCAL folder: dev_key.pub, and <id>.pc.nevrmap with its .minisig.
  std::string LocalDir() const { return root_ + "/maps/local"; }
  void WriteLocal(const MapFiles& m, const TestKey& signer, const std::string& comment) {
    std::string zip = Zip({{"package.json", m.index},
                           {"set/windows-x86_64.nevrres", m.set},
                           {"scripts/" + Hex16(kScriptHash) + ".dll", m.script},
                           {"thumbnail.png", "png"}});
    WriteFile(LocalPath(m.id), zip);
    WriteFile(LocalPath(m.id) + ".minisig", signer.Minisig(zip, comment));
  }
  std::string LocalPath(const std::string& id) const { return LocalDir() + "/" + id + kLocalSuffix; }
  std::string LocalCache() const { return LocalDir() + ".cache"; }
  // The one extracted copy of `id`'s script: <cache>/<id>/<leaf>/scripts/<hash16>.dll.
  std::string CachedScript(const std::string& id) const {
    const std::wstring dir = Wide(LocalCache() + "/" + id);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    std::string leaf;
    if (find != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && name != L"." && name != L"..") leaf = Utf8(name);
      } while (FindNextFileW(find, &fd));
      FindClose(find);
    }
    return LocalCache() + "/" + id + "/" + leaf + "/scripts/" + Hex16(kScriptHash) + ".dll";
  }

  // A zip as E1 writes one: the set and scripts deflated, the rest stored.
  static std::string Zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, directory;
    auto u16 = [](std::string* s, std::uint32_t v) { s->push_back(static_cast<char>(v)); s->push_back(static_cast<char>(v >> 8)); };
    auto u32 = [&](std::string* s, std::uint32_t v) { u16(s, v & 0xffff); u16(s, v >> 16); };
    for (const auto& [name, bytes] : files) {
      const bool deflate = name != "package.json";
      std::string packed = bytes;
      if (deflate) {
        z_stream zs{};
        deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        packed.resize(deflateBound(&zs, static_cast<uLong>(bytes.size())));
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
        zs.avail_in = static_cast<uInt>(bytes.size());
        zs.next_out = reinterpret_cast<Bytef*>(&packed[0]);
        zs.avail_out = static_cast<uInt>(packed.size());
        ::deflate(&zs, Z_FINISH);
        packed.resize(zs.total_out);
        deflateEnd(&zs);
      }
      const std::uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uInt>(bytes.size()));
      const std::uint32_t at = static_cast<std::uint32_t>(out.size());
      const std::uint32_t method = deflate ? 8 : 0;
      u32(&out, 0x04034b50); u16(&out, 20); u16(&out, 0); u16(&out, method); u32(&out, 0); u32(&out, crc);
      u32(&out, static_cast<std::uint32_t>(packed.size())); u32(&out, static_cast<std::uint32_t>(bytes.size()));
      u16(&out, static_cast<std::uint32_t>(name.size())); u16(&out, 0);
      out += name + packed;
      u32(&directory, 0x02014b50); u16(&directory, 20); u16(&directory, 20); u16(&directory, 0); u16(&directory, method);
      u32(&directory, 0); u32(&directory, crc); u32(&directory, static_cast<std::uint32_t>(packed.size()));
      u32(&directory, static_cast<std::uint32_t>(bytes.size())); u16(&directory, static_cast<std::uint32_t>(name.size()));
      u16(&directory, 0); u16(&directory, 0); u16(&directory, 0); u16(&directory, 0); u32(&directory, 0); u32(&directory, at);
      directory += name;
    }
    const std::uint32_t cd_at = static_cast<std::uint32_t>(out.size());
    out += directory;
    u32(&out, 0x06054b50); u16(&out, 0); u16(&out, 0);
    u16(&out, static_cast<std::uint32_t>(files.size())); u16(&out, static_cast<std::uint32_t>(files.size()));
    u32(&out, static_cast<std::uint32_t>(directory.size())); u32(&out, cd_at); u16(&out, 0);
    return out;
  }

  std::wstring base_, exe_dir_;
  std::string root_;
  FakeRegistry registry_;
  std::unique_ptr<nevr_cargo::Env> env_;
  std::unique_ptr<nevr_maps::MapFs> fs_;
  std::vector<std::string> expected_;
  std::vector<nevr_map_session_pin::MapPin> pins_;
};

std::function<bool()> Always() {
  return [] { return true; };
}

}  // namespace
