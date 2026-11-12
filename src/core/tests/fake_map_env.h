/* Fakes for the map channel core's test (core/map_run.h): one in-memory file system serving as both nevr_cargo::Env and
 * MapFs, a scripted HTTP server, a SHA-256 hasher that counts what it hashes, and a recording Mounter and Deriver.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/cargo_env.h"
#include "core/map_run.h"
#include "core/map_types.h"
#include "quest/data/sha256.h"

namespace nevr_maps_test {

inline std::string FakeSha(std::string_view bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

class FakeEnv : public nevr_cargo::Env, public nevr_maps::MapFs {
 public:
  struct File {
    std::string bytes;
    std::uint64_t id = 0, writes = 0;
  };
  struct Response {
    long status = 200;
    std::string body;
    int corrupt_times = 0;  // the next N responses flip the body's first byte
  };
  struct Logged {
    nevr_cargo::EventLevel level;
    std::string name;
    std::vector<nevr_cargo::Field> fields;
    std::string Get(const std::string& key) const {
      for (const nevr_cargo::Field& f : fields) {
        if (f.key == key) return f.is_number ? std::to_string(f.number) : f.text;
      }
      return std::string();
    }
  };

  std::map<std::string, File> files;
  std::set<std::string> dirs;
  std::map<std::int64_t, std::string> handles;
  std::map<std::string, Response> server;
  std::map<std::string, int> gets;
  std::vector<Logged> events;
  std::set<std::string> fail_writes;  // WriteFileAtomically refuses these paths: the process died before that write
  std::function<bool()> allow_get = [] { return true; };  // false: the GET is cancelled (the shutdown path)
  std::uint64_t now_ms = 0, hashed_bytes = 0;
  bool stamps_supported = true;

  static std::string Norm(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
  }
  static std::string Parent(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
  }
  static bool Under(const std::string& path, const std::string& dir) {
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
  }

  void Put(const std::string& path, const std::string& bytes) {
    File& f = files[Norm(path)];
    if (f.id == 0) f.id = ++next_id_;
    f.bytes = bytes;
    f.writes = ++write_clock_;
  }
  // Same bytes, a new last-write time: the stamp moves and the sha256 does not.
  void Touch(const std::string& path) { files.at(Norm(path)).writes = ++write_clock_; }
  bool Has(const std::string& path) const { return files.count(Norm(path)) != 0; }
  bool HasDir(const std::string& path) const { return dirs.count(Norm(path)) != 0; }
  std::size_t OpenHandles() const { return handles.size(); }
  int GetsUnder(const std::string& prefix) const {
    int n = 0;
    for (const auto& [url, count] : gets) {
      if (url.compare(0, prefix.size(), prefix) == 0) n += count;
    }
    return n;
  }
  const Logged* Find(const std::string& name, const std::string& key = "", const std::string& value = "") const {
    for (const Logged& e : events) {
      if (e.name == name && (key.empty() || e.Get(key) == value)) return &e;
    }
    return nullptr;
  }

  // nevr_cargo::Env
  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) override {
    const auto it = files.find(Norm(path));
    if (it == files.end() || it->second.bytes.size() > max_bytes) return false;
    *out = it->second.bytes;
    return true;
  }
  bool MakeDirs(const std::string& dir) override {
    for (std::string d = Norm(dir); !d.empty() && d.find('/') != std::string::npos; d = Parent(d)) {
      if (files.count(d)) return false;
      dirs.insert(d);
    }
    return true;
  }
  std::unique_ptr<nevr_cargo::TempFile> CreateTemp(const std::string& dir) override {
    if (!HasDir(dir)) return nullptr;
    return std::unique_ptr<nevr_cargo::TempFile>(new Temp(*this));
  }
  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    if (fail_writes.count(Norm(path)) || !HasDir(Parent(Norm(path)))) return false;
    Put(path, data);
    return true;
  }
  bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    const auto it = files.find(Norm(path));
    if (it == files.end()) return false;
    *handle = ++next_handle_;
    handles[*handle] = it->first;
    *size = it->second.bytes.size();
    return true;
  }
  bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length,
                std::size_t* got) override {
    const auto h = handles.find(handle);
    if (h == handles.end()) return false;
    const std::string& bytes = files.at(h->second).bytes;
    if (offset > bytes.size()) return false;
    *got = static_cast<std::size_t>(std::min<std::uint64_t>(length, bytes.size() - offset));
    std::copy_n(bytes.data() + offset, *got, static_cast<char*>(buffer));
    return true;
  }
  void CloseBlob(std::int64_t handle) override { handles.erase(handle); }
  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override {
    return std::unique_ptr<nevr_cargo::Hasher>(new Hasher(*this));
  }
  nevr_cargo::HttpResult Get(const std::string& url, const nevr_cargo::HttpBody& on_body,
                             const std::function<bool()>& keep_going) override {
    ++gets[url];
    nevr_cargo::HttpResult out;
    if (!allow_get() || !keep_going()) {
      out.cancelled = true;
      return out;
    }
    const auto it = server.find(url);
    if (it == server.end()) {
      out.completed = true;
      out.status = 404;
      return out;
    }
    Response& response = it->second;
    out.status = response.status;
    std::string body = response.body;
    if (response.corrupt_times > 0 && !body.empty()) {
      --response.corrupt_times;
      body[0] = static_cast<char>(body[0] ^ 0x5a);
    }
    for (std::size_t at = 0; at < body.size(); at += 4096) {
      if (!on_body(response.status, body.data() + at, std::min<std::size_t>(4096, body.size() - at))) return out;
    }
    out.completed = true;
    return out;
  }
  std::uint64_t NowMs() override { return now_ms; }
  void Event(nevr_cargo::EventLevel level, const char* name, const std::vector<nevr_cargo::Field>& fields) override {
    events.push_back(Logged{level, name, fields});
  }

  // nevr_maps::MapFs
  bool StampOf(std::int64_t handle, nevr_maps::FileStamp* out) override {
    const auto h = handles.find(handle);
    if (!stamps_supported || h == handles.end()) return false;
    const File& f = files.at(h->second);
    *out = nevr_maps::FileStamp();
    out->volume = 7;
    out->id_lo = f.id;
    out->size = f.bytes.size();
    out->last_write = f.writes;
    return true;
  }
  std::string FinalPathOf(std::int64_t handle) override {
    const auto h = handles.find(handle);
    return h == handles.end() ? std::string() : h->second;
  }
  bool OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    return OpenBlob(path, handle, size);
  }
  bool ListDir(const std::string& dir, std::vector<std::string>* names) override {
    const std::string d = Norm(dir);
    if (!dirs.count(d)) return false;
    std::set<std::string> found;
    for (const auto& [path, file] : files) {
      if (Parent(path) == d) found.insert(path.substr(d.size() + 1));
    }
    for (const std::string& sub : dirs) {
      if (Parent(sub) == d) found.insert(sub.substr(d.size() + 1));
    }
    names->assign(found.begin(), found.end());
    return true;
  }
  // As on Windows: what is not open goes, a file held open stays with its folders, and the call then says false.
  bool RemoveTree(const std::string& dir) override {
    const std::string d = Norm(dir);
    if (files.count(d)) return false;
    if (!dirs.count(d)) return true;
    std::set<std::string> held;
    for (const auto& [handle, path] : handles) {
      if (Under(path, d)) held.insert(path);
    }
    for (auto it = files.begin(); it != files.end();) {
      it = Under(it->first, d) && !held.count(it->first) ? files.erase(it) : std::next(it);
    }
    for (auto it = dirs.begin(); it != dirs.end();) {
      const std::string sub = *it;
      const bool keep = std::any_of(held.begin(), held.end(), [&](const std::string& h) { return Under(h, sub); });
      it = (sub == d || Under(sub, d)) && !keep ? dirs.erase(it) : std::next(it);
    }
    return held.empty();
  }
  bool RenameDir(const std::string& from, const std::string& to) override {
    const std::string f = Norm(from), t = Norm(to);
    if (!dirs.count(f) || dirs.count(t) || files.count(t) || !dirs.count(Parent(t))) return false;
    for (const auto& [handle, path] : handles) {
      if (Under(path, f)) return false;
    }
    std::map<std::string, File> moved;
    for (auto it = files.begin(); it != files.end();) {
      if (Under(it->first, f)) {
        moved[t + it->first.substr(f.size())] = it->second;
        it = files.erase(it);
      } else {
        ++it;
      }
    }
    files.insert(moved.begin(), moved.end());
    std::set<std::string> moved_dirs;
    for (auto it = dirs.begin(); it != dirs.end();) {
      if (*it == f || Under(*it, f)) {
        moved_dirs.insert(t + it->substr(f.size()));
        it = dirs.erase(it);
      } else {
        ++it;
      }
    }
    dirs.insert(moved_dirs.begin(), moved_dirs.end());
    return true;
  }

 private:
  class Temp : public nevr_cargo::TempFile {
   public:
    explicit Temp(FakeEnv& env) : env_(env) {}
    bool Write(const void* data, std::size_t length) override {
      bytes_.append(static_cast<const char*>(data), length);
      return true;
    }
    bool Commit(const std::string& final_path) override {
      if (!env_.HasDir(Parent(Norm(final_path)))) return false;
      env_.Put(final_path, bytes_);
      return true;
    }
    void Abort() override { bytes_.clear(); }

   private:
    FakeEnv& env_;
    std::string bytes_;
  };
  class Hasher : public nevr_cargo::Hasher {
   public:
    explicit Hasher(FakeEnv& env) : env_(env) {}
    void Update(const void* data, std::size_t length) override {
      env_.hashed_bytes += length;
      hash_.Update(data, length);
    }
    std::string Hex() override { return hash_.Hex(); }

   private:
    FakeEnv& env_;
    nevr_quest::data::Sha256 hash_;
  };

  std::int64_t next_handle_ = 100;
  std::uint64_t next_id_ = 0, write_clock_ = 0;
};

class FakeMounter : public nevr_maps::Mounter {
 public:
  explicit FakeMounter(FakeEnv& env) : env_(env) {}

  std::vector<std::string> calls;
  std::set<std::uint64_t> refuse_scripts;
  std::set<std::string> refuse_sets;              // owners whose Set is refused "collides"
  std::map<std::string, nevr_maps::Grant> alias;  // owner -> the StageAliases result (default ok)
  std::map<std::string, bool> verified;           // owner -> verified_stamp of its Set
  std::map<std::string, std::string> labels, set_sha;
  std::uint64_t set_cost_ms = 0;  // the clock moves this much per Set (the boot budget)

  nevr_maps::Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held,
                          const std::string&) override {
    calls.push_back("script " + owner + " " + std::to_string(hash));
    return Take(owner, held, !refuse_scripts.count(hash), "pe_rules");
  }
  nevr_maps::Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t, const std::string&) override {
    calls.push_back("rekey " + owner + " " + std::to_string(hash));
    return Ok(!refuse_scripts.count(hash), "rekey_mismatch");
  }
  nevr_maps::Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                       bool verified_stamp) override {
    calls.push_back("set " + owner);
    env_.now_ms += set_cost_ms;
    verified[owner] = verified_stamp;
    labels[owner] = label;
    set_sha[owner] = sha256;
    return Take(owner, held, !refuse_sets.count(owner), "collides");
  }
  nevr_maps::Grant StageAliases(const std::string& owner, const std::string&, std::uint64_t,
                                const std::vector<nevr_maps::ConfigAlias>&) override {
    calls.push_back("aliases " + owner);
    const auto it = alias.find(owner);
    return it == alias.end() ? Ok(true, "") : it->second;
  }
  nevr_maps::Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string&,
                                 const std::string&) override {
    calls.push_back("derived " + owner + " " + std::to_string(hash));
    return Take(owner, held, !refuse_scripts.count(hash), "pe_rules");
  }
  void Withdraw(const std::string& owner) override {
    calls.push_back("withdraw " + owner);
    for (std::int64_t handle : owned_[owner]) env_.CloseBlob(handle);
    owned_.erase(owner);
  }
  std::size_t Owned() const {
    std::size_t n = 0;
    for (const auto& [owner, held] : owned_) n += held.size();
    return n;
  }
  std::size_t Owned(const std::string& owner) const {
    const auto it = owned_.find(owner);
    return it == owned_.end() ? 0 : it->second.size();
  }
  bool Called(const std::string& call) const { return std::find(calls.begin(), calls.end(), call) != calls.end(); }

 private:
  static nevr_maps::Grant Ok(bool ok, const char* reason) {
    nevr_maps::Grant g;
    g.ok = ok;
    if (!ok) g.reason = reason;
    return g;
  }
  nevr_maps::Grant Take(const std::string& owner, std::int64_t held, bool ok, const char* reason) {
    if (ok) owned_[owner].push_back(held);
    return Ok(ok, reason);
  }

  FakeEnv& env_;
  std::map<std::string, std::vector<std::int64_t>> owned_;
};

// Rebuilds a recipe map the simple way: the merged set is the published set's bytes, and each rekeyed script with edits
// is the bytes the test registered for its hash. Every rebuild reads the same stock file twice, as PC10's read_from
// would.
class FakeDeriver : public nevr_maps::Deriver {
 public:
  explicit FakeDeriver(FakeEnv& env) : env_(env) {}

  bool current = true;
  std::string reason;
  int derive_calls = 0;
  std::vector<std::string> stock_checked;  // one path per StockCurrent call
  std::map<std::uint64_t, std::string> rebuilt_scripts;

  nevr_maps::DeriveOutput Derive(const nevr_maps::Package& package, const std::string& version_dir,
                                 const std::function<bool()>&) override {
    ++derive_calls;
    nevr_maps::DeriveOutput out;
    out.reason = reason;
    if (!reason.empty()) return out;
    nevr_maps::StockStamp stock;
    stock.path = "C:/game/_data/packages/stock0";
    stock.stamp.id_lo = 1;
    out.stock_stamps = {stock, stock};
    if (!package.derived.empty()) {
      std::string set;
      env_.ReadSmallFile(version_dir + "/set/windows-x86_64.nevrres", &set, std::size_t{1} << 30);
      out.set_path = version_dir + "/set/windows-x86_64.mounted.nevrres";
      env_.Put(out.set_path, set);
      out.set_sha256 = FakeSha(set);
    }
    env_.MakeDirs(version_dir + "/scripts");
    for (const nevr_maps::RekeyPin& r : package.rekeyed) {
      if (r.edits.empty()) continue;
      char name[32];
      std::snprintf(name, sizeof name, "scripts/%016llx.dll", static_cast<unsigned long long>(r.hash));
      env_.Put(version_dir + "/" + name, rebuilt_scripts[r.hash]);
      out.scripts.emplace_back(r.hash, name);
    }
    return out;
  }
  bool StockCurrent(const std::vector<nevr_maps::StockStamp>& stamps) override {
    for (const nevr_maps::StockStamp& s : stamps) stock_checked.push_back(s.path);
    return current;
  }

 private:
  FakeEnv& env_;
};

}  // namespace nevr_maps_test
