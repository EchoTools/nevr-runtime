// A memory-backed nevr_cargo::Env for the host tests: files in a map that a "restart" can share, a scripted server, a stop switch
// that makes every mutating call after the Nth fail (a process killed at that point), real SHA-256, and the events as JSON lines.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "quest/data/sha256.h"

namespace cargo_test {

struct Response {
  long status = 200;
  std::string body;
  std::size_t drop_after = std::string::npos;  // deliver this many bytes, then the connection drops (not completed)
  std::string extra;                           // bytes sent beyond the body (a server that sends more than it said)
};

struct Fs {
  std::map<std::string, std::string> files;
  int next_temp = 0;
};

class FakeHasher : public nevr_cargo::Hasher {
 public:
  void Update(const void* data, std::size_t length) override { sha_.Update(data, length); }
  std::string Hex() override { return sha_.Hex(); }

 private:
  nevr_quest::data::Sha256 sha_;
};

class FakeEnv : public nevr_cargo::Env {
 public:
  explicit FakeEnv(std::shared_ptr<Fs> fs = std::make_shared<Fs>()) : fs_(std::move(fs)) {}

  std::shared_ptr<Fs> fs_;
  std::map<std::string, Response> server;  // by URL
  std::vector<std::string> requests;       // every GET, in order
  std::vector<std::string> events;         // one JSON line each
  std::vector<std::string> event_levels;
  int mutating_ops = 0;                    // counts temp writes, commits and atomic writes
  int stop_after = -1;                     // -1: never; otherwise the ops after this many fail
  std::uint64_t clock = 1000;
  std::size_t open_blobs() const { return open_.size(); }
  std::vector<std::uint64_t> body_bytes_delivered;  // per GET

  bool Dead() const { return stop_after >= 0 && mutating_ops > stop_after; }

  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max) override {
    const auto it = fs_->files.find(path);
    if (it == fs_->files.end() || it->second.size() > max) return false;
    *out = it->second;
    return true;
  }
  bool MakeDirs(const std::string&) override { return true; }

  class Temp : public nevr_cargo::TempFile {
   public:
    Temp(FakeEnv* env, std::string path) : env_(env), path_(std::move(path)) { env_->fs_->files[path_] = std::string(); }
    ~Temp() override {
      if (!done_ && !env_->Dead()) env_->fs_->files.erase(path_);
    }
    bool Write(const void* data, std::size_t length) override {
      if (!env_->Tick()) return false;
      env_->fs_->files[path_].append(static_cast<const char*>(data), length);
      return true;
    }
    bool Commit(const std::string& final_path) override {
      if (!env_->Tick()) return false;
      env_->fs_->files[final_path] = env_->fs_->files[path_];
      env_->fs_->files.erase(path_);
      done_ = true;
      return true;
    }
    void Abort() override {
      if (!env_->Dead()) env_->fs_->files.erase(path_);
      done_ = true;
    }

   private:
    FakeEnv* env_;
    std::string path_;
    bool done_ = false;
  };

  std::unique_ptr<nevr_cargo::TempFile> CreateTemp(const std::string& dir) override {
    return std::unique_ptr<nevr_cargo::TempFile>(new Temp(this, dir + "/.tmp" + std::to_string(fs_->next_temp++)));
  }
  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    if (!Tick()) return false;
    fs_->files[path] = data;
    return true;
  }

  bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    const auto it = fs_->files.find(path);
    if (it == fs_->files.end()) return false;
    *handle = next_handle_++;
    open_[*handle] = path;
    *size = it->second.size();
    return true;
  }
  bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) override {
    const auto h = open_.find(handle);
    if (h == open_.end()) return false;
    const auto it = fs_->files.find(h->second);
    if (it == fs_->files.end() || offset > it->second.size()) return false;
    const std::size_t n = std::min<std::size_t>(length, it->second.size() - static_cast<std::size_t>(offset));
    std::memcpy(buffer, it->second.data() + offset, n);
    *got = n;
    return true;
  }
  void CloseBlob(std::int64_t handle) override { open_.erase(handle); }
  std::string PathOfHandle(std::int64_t handle) const {
    const auto it = open_.find(handle);
    return it == open_.end() ? std::string() : it->second;
  }

  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override { return std::unique_ptr<nevr_cargo::Hasher>(new FakeHasher()); }

  nevr_cargo::HttpResult Get(const std::string& url, const nevr_cargo::HttpBody& on_body,
                             const std::function<bool()>& keep_going) override {
    requests.push_back(url);
    body_bytes_delivered.push_back(0);
    nevr_cargo::HttpResult r;
    if (Dead()) return r;  // a killed process makes no request
    const auto it = server.find(url);
    const Response resp = it != server.end() ? it->second : Response{404, std::string(), std::string::npos, std::string()};
    std::string body = resp.body + resp.extra;
    const std::size_t limit = resp.drop_after != std::string::npos ? std::min(resp.drop_after, body.size()) : body.size();
    r.status = resp.status;
    std::size_t at = 0;
    while (at < limit) {
      if (!keep_going()) {
        r.cancelled = true;
        return r;
      }
      const std::size_t n = std::min<std::size_t>(97, limit - at);
      body_bytes_delivered.back() += n;
      if (!on_body(resp.status, body.data() + at, n)) return r;  // the caller ended the transfer
      at += n;
    }
    if (resp.drop_after != std::string::npos && resp.drop_after < body.size()) {
      r.code = 18;  // a dropped connection
      return r;
    }
    if (body.empty() && !on_body(resp.status, "", 0)) return r;
    r.completed = true;
    return r;
  }

  std::uint64_t NowMs() override { return clock += 3; }

  void Event(nevr_cargo::EventLevel level, const char* name, const std::vector<nevr_cargo::Field>& fields) override {
    std::string line = std::string("{\"level\":\"") + (level == nevr_cargo::EventLevel::kInfo ? "info" : level == nevr_cargo::EventLevel::kWarn ? "warn" : "error") +
                       "\",\"event\":\"" + name + "\"";
    for (const nevr_cargo::Field& f : fields) {
      line += ",\"" + f.key + "\":";
      line += f.is_number ? std::to_string(f.number) : "\"" + f.text + "\"";
    }
    line += "}";
    events.push_back(line);
  }

  std::vector<std::string> EventsNamed(const std::string& name) const {
    std::vector<std::string> out;
    for (const std::string& e : events) {
      if (e.find("\"event\":\"" + name + "\"") != std::string::npos) out.push_back(e);
    }
    return out;
  }
  // The order of the event names, e.g. "cargo_start,cargo_manifest".
  std::string EventOrder() const {
    std::string out;
    for (const std::string& e : events) {
      const std::size_t at = e.find("\"event\":\"") + 9;
      if (!out.empty()) out += ",";
      out += e.substr(at, e.find('"', at) - at);
    }
    return out;
  }
  static std::string Value(const std::string& line, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const std::size_t at = line.find(needle);
    if (at == std::string::npos) return "<absent>";
    const std::size_t begin = at + needle.size();
    if (line[begin] == '"') return line.substr(begin + 1, line.find('"', begin + 1) - begin - 1);
    return line.substr(begin, line.find_first_of(",}", begin) - begin);
  }

  bool Tick() {
    ++mutating_ops;
    return !Dead();
  }

 private:
  std::int64_t next_handle_ = 100;
  std::map<std::int64_t, std::string> open_;
};

inline std::string Sha256Of(const std::string& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

}  // namespace cargo_test
