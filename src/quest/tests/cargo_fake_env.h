// A memory-backed nevr_cargo::Env for the host tests: files in a map that a "restart" can share, a scripted server, a stop switch
// that makes every mutating call after the Nth fail (a process killed at that point), real SHA-256, and the events as JSON lines and
// as records whose fields a test reads by key. The map channel's tests add a nevr_maps::MapFs over the same files
// (quest/tests/map_fake_fs.h), which reads the directories, file ids and write counts kept here.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "quest/data/sha256.h"

namespace cargo_test {

// A corrupted response has its first byte flipped by this mask, so its SHA-256 no longer matches.
constexpr char kCorruptMask = 0x5a;

struct Response {
  long status = 200;
  std::string body;
  std::size_t drop_after = std::string::npos;  // deliver this many bytes, then the connection drops (not completed)
  std::string extra;                           // bytes sent beyond the body (a server that sends more than it said)
};

struct Fs {
  std::map<std::string, std::string> files;
  int next_temp = 0;
  // What a file system says about its files beyond their bytes: the directories made, and per file an id that stays while it is
  // written and a count that moves with every write (a stamp's last-write time).
  std::set<std::string> dirs;
  std::map<std::string, std::uint64_t> ids;
  std::map<std::string, std::uint64_t> writes;
  std::uint64_t next_id = 0;
  std::uint64_t write_clock = 0;
  void Wrote(const std::string& path) {
    if (ids.find(path) == ids.end()) ids[path] = ++next_id;
    writes[path] = ++write_clock;
  }
};

class FakeHasher : public nevr_cargo::Hasher {
 public:
  explicit FakeHasher(std::uint64_t* hashed = nullptr) : hashed_(hashed) {}
  void Update(const void* data, std::size_t length) override {
    if (hashed_ != nullptr) *hashed_ += length;
    sha_.Update(data, length);
  }
  std::string Hex() override { return sha_.Hex(); }

 private:
  std::uint64_t* hashed_;
  nevr_quest::data::Sha256 sha_;
};

// One structured record, as Event received it.
struct Record {
  nevr_cargo::EventLevel level = nevr_cargo::EventLevel::kInfo;
  std::string name;
  std::vector<nevr_cargo::Field> fields;
  // The field's value as text (a number in decimal); empty when the record has no such field.
  std::string Get(const std::string& key) const {
    for (const nevr_cargo::Field& f : fields) {
      if (f.key == key) return f.is_number ? std::to_string(f.number) : f.text;
    }
    return std::string();
  }
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
  nevr_cargo::DeviceFacts device = DefaultDevice();  // what QueryDevice reports
  int device_queries = 0;
  std::size_t disk_bytes_left = std::string::npos;  // appends past this many bytes fail (a full disk)
  std::uint64_t blob_bytes_read = 0;                 // bytes read through ReadBlob: a boot that hashes shows here
  std::uint64_t hashed_bytes = 0;                    // bytes fed to every hasher NewHasher made
  std::set<std::string> fail_writes;                 // WriteFileAtomically refuses these paths (the process died before them)
  std::function<bool()> allow_get;                   // when set and false, a GET is cancelled before it starts (a shutdown)
  std::vector<Record> records;                       // every event, with its fields
  std::map<std::string, int> corrupt_times;          // by URL: the next N responses flip the body's first byte
  static nevr_cargo::DeviceFacts DefaultDevice() {
    nevr_cargo::DeviceFacts d;
    d.have_free = true;
    d.free_bytes = std::uint64_t{8} << 30;
    d.have_fds = true;
    d.fd_soft_limit = 1024;
    d.fds_open = 64;
    d.have_memory = true;
    d.memory_available = std::uint64_t{3} << 30;
    return d;
  }
  std::size_t open_blobs() const { return open_.size(); }
  std::vector<std::uint64_t> body_bytes_delivered;  // per GET

  bool Dead() const { return stop_after >= 0 && mutating_ops > stop_after; }

  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max) override {
    const auto it = fs_->files.find(path);
    if (it == fs_->files.end() || it->second.size() > max) return false;
    *out = it->second;
    return true;
  }
  bool MakeDirs(const std::string& dir) override {
    for (std::string d = dir; !d.empty(); d = d.substr(0, d.find_last_of('/') == std::string::npos ? 0 : d.find_last_of('/'))) {
      fs_->dirs.insert(d);
    }
    return true;
  }

  class FakeAppend : public nevr_cargo::AppendFile {
   public:
    FakeAppend(FakeEnv* env, std::string path) : env_(env), path_(std::move(path)) {
      if (env_->fs_->files.find(path_) == env_->fs_->files.end()) {
        env_->fs_->files[path_];  // created when absent
        env_->fs_->Wrote(path_);
      }
    }
    std::uint64_t Size() const override { return env_->fs_->files[path_].size(); }
    bool Append(const void* data, std::size_t length) override {
      if (!env_->Tick()) return false;
      if (env_->disk_bytes_left != std::string::npos) {
        if (length > env_->disk_bytes_left) return false;  // the disk is full
        env_->disk_bytes_left -= length;
      }
      env_->fs_->files[path_].append(static_cast<const char*>(data), length);
      env_->fs_->Wrote(path_);
      return true;
    }
    bool Truncate(std::uint64_t size) override {
      if (!env_->Tick()) return false;
      std::string& f = env_->fs_->files[path_];
      if (size > f.size()) return false;
      f.resize(static_cast<std::size_t>(size));
      env_->fs_->Wrote(path_);
      return true;
    }
    bool Sync() override { return env_->Tick(); }

   private:
    FakeEnv* env_;
    std::string path_;
  };

  std::unique_ptr<nevr_cargo::AppendFile> OpenAppend(const std::string& path) override {
    if (Dead()) return nullptr;
    return std::unique_ptr<nevr_cargo::AppendFile>(new FakeAppend(this, path));
  }
  bool QueryDevice(const std::string&, nevr_cargo::DeviceFacts* out) override {
    ++device_queries;
    *out = device;
    return true;
  }
  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    if (fail_writes.count(path) != 0 || !Tick()) return false;
    fs_->files[path] = data;
    fs_->Wrote(path);
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
    blob_bytes_read += n;
    *got = n;
    return true;
  }
  void CloseBlob(std::int64_t handle) override { open_.erase(handle); }
  std::string PathOfHandle(std::int64_t handle) const {
    const auto it = open_.find(handle);
    return it == open_.end() ? std::string() : it->second;
  }
  // The paths of every handle open now.
  std::vector<std::string> OpenPaths() const {
    std::vector<std::string> out;
    for (const auto& h : open_) out.push_back(h.second);
    return out;
  }

  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override {
    return std::unique_ptr<nevr_cargo::Hasher>(new FakeHasher(&hashed_bytes));
  }

  nevr_cargo::HttpResult Get(const std::string& url, const nevr_cargo::HttpBody& on_body,
                             const std::function<bool()>& keep_going) override {
    requests.push_back(url);
    body_bytes_delivered.push_back(0);
    nevr_cargo::HttpResult r;
    if (Dead()) return r;  // a killed process makes no request
    if (allow_get && !allow_get()) {
      r.cancelled = true;
      return r;
    }
    const auto it = server.find(url);
    const Response resp = it != server.end() ? it->second : Response{404, std::string(), std::string::npos, std::string()};
    std::string body = resp.body + resp.extra;
    const auto corrupt = corrupt_times.find(url);
    if (corrupt != corrupt_times.end() && corrupt->second > 0 && !body.empty()) {
      --corrupt->second;
      body[0] = static_cast<char>(body[0] ^ kCorruptMask);
    }
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
    std::string line = std::string("{\"level\":\"") + (level == nevr_cargo::EventLevel::kDebug ? "debug" : level == nevr_cargo::EventLevel::kInfo ? "info" : level == nevr_cargo::EventLevel::kWarn ? "warn" : "error") +
                       "\",\"event\":\"" + name + "\"";
    for (const nevr_cargo::Field& f : fields) {
      line += ",\"" + f.key + "\":";
      line += f.is_number ? std::to_string(f.number) : "\"" + f.text + "\"";
    }
    line += "}";
    events.push_back(line);
    records.push_back(Record{level, name, fields});
  }

  // The first record named `name`, with `key` equal to `value` when a key is given; null when there is none.
  const Record* FindRecord(const std::string& name, const std::string& key = std::string(),
                           const std::string& value = std::string()) const {
    for (const Record& r : records) {
      if (r.name == name && (key.empty() || r.Get(key) == value)) return &r;
    }
    return nullptr;
  }
  std::size_t CountRecords(const std::string& name) const {
    std::size_t n = 0;
    for (const Record& r : records) n += r.name == name ? 1 : 0;
    return n;
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

// A stored frame that satisfies the validator: the zstd magic, a single-segment header that states `bytes` as its content size, then
// filler up to `size` bytes (the host tests never decompress). `size` must be at least the header's length.
inline std::string MakeFrame(std::uint64_t bytes, std::size_t size, char fill = 'z') {
  std::string f;
  f.push_back(static_cast<char>(0x28));
  f.push_back(static_cast<char>(0xB5));
  f.push_back(static_cast<char>(0x2F));
  f.push_back(static_cast<char>(0xFD));
  if (bytes <= 255) {
    f.push_back(static_cast<char>(0x20));  // single segment, content size in 1 byte
    f.push_back(static_cast<char>(bytes));
  } else if (bytes <= 65791) {
    f.push_back(static_cast<char>(0x60));  // single segment, 2 bytes (value - 256)
    const std::uint64_t v = bytes - 256;
    f.push_back(static_cast<char>(v & 0xff));
    f.push_back(static_cast<char>(v >> 8));
  } else {
    f.push_back(static_cast<char>(0xA0));  // single segment, 4 bytes
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<char>((bytes >> (8 * i)) & 0xff));
  }
  if (f.size() < size) f.append(size - f.size(), fill);
  return f;
}

inline std::string Sha256Of(const std::string& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

}  // namespace cargo_test
