// Host test for the first-run prefetch (src/core/cargo_prefetch.*; docs/adr/0045-cdn-texture-delivery.md, the first-run hold). A scripted
// server and an in-memory store (cargo_fake_env.h) run the six ways a first run can go: full, resumed after a kill, 404, stalled,
// hash-mismatch and disk-full. Each run's events are checked by name and reason.
//
// Built and run by `just test-quest-data` (the cargo_prefetch_test target of src/quest/host_tests/groups/data.cmake).

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/cargo_prefetch.h"
#include "core/cargo_store.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
using cargo_test::FakeEnv;
using cargo_test::Response;
using cargo_test::Sha256Of;

constexpr const char* kBase = "https://cargo.echo.taxi/";
constexpr const char* kDir = "/cache/cargo";

// An object's path in the test store: the store is the fake filesystem under kDir, one file a name (the prefetch's seam is PrefetchStore, so
// the layout is the test's own; the real store's is cargo_store.h's).
std::string BlobPath(const std::string& dir, const std::string& sha256) { return dir + "/" + sha256; }

// An object being written: bytes go to a temp file in the fake filesystem and are moved to their name at Commit. Every write and the
// commit count as the env's mutating operations, so its stop switch (a killed process) fails them, as a real write would fail.
class TestTemp {
 public:
  TestTemp(FakeEnv* env, std::string path) : env_(env), path_(std::move(path)) { env_->fs_->files[path_] = std::string(); }
  ~TestTemp() {
    if (!done_ && !env_->Dead()) env_->fs_->files.erase(path_);
  }
  bool Write(const void* data, std::size_t length) {
    if (!env_->Tick()) return false;
    env_->fs_->files[path_].append(static_cast<const char*>(data), length);
    return true;
  }
  bool Commit(const std::string& final_path) {
    if (!env_->Tick()) return false;
    env_->fs_->files[final_path] = env_->fs_->files[path_];
    env_->fs_->files.erase(path_);
    done_ = true;
    return true;
  }
  void Abort() {
    if (!env_->Dead()) env_->fs_->files.erase(path_);
    done_ = true;
  }

 private:
  FakeEnv* env_;
  std::string path_;
  bool done_ = false;
};

std::unique_ptr<TestTemp> CreateTemp(FakeEnv* env, const std::string& dir) {
  return std::unique_ptr<TestTemp>(new TestTemp(env, dir + "/.tmp" + std::to_string(env->fs_->next_temp++)));
}

std::string UrlOf(const std::string& sha) { return std::string(kBase) + "blobs/sha256/" + sha; }

nc::Entry EntryOf(const std::string& label, std::uint64_t name, const std::string& bytes) {
  nc::Entry e;
  e.label = label;
  e.platform = "android-arm64";
  e.type = 0xe2efe7289d5985b8ULL;
  e.name = name;
  e.sha256 = Sha256Of(bytes);
  e.size = bytes.size();
  e.bytes = bytes.size();
  e.align = 16;
  e.stock_size = bytes.size();
  return e;
}

// The store over the fake filesystem. Writes go through the env's temp files, so a kill (the env's stop switch) fails them too.
// `fail_open` names the 1-based object whose writes report no space (the disk-full case).
class MemStore : public nc::PrefetchStore {
 public:
  explicit MemStore(FakeEnv* env) : env_(env) {}

  int fail_open = -1;
  int refuse_open = -1;   // the Nth Open is refused (no sink, an I/O error)
  bool fail_commit = false;
  int opened = 0;
  int body_writes = 0;    // successful body writes, over every object

  bool Has(const std::string& sha256) const override {
    return env_->fs_->files.count(BlobPath(kDir, sha256)) != 0;
  }
  // The fake store keeps one size per object (the stored frame's), so the decompressed size is not checked here.
  bool HoldsSized(const std::string& sha256, std::uint64_t size, std::uint64_t) const override {
    const auto it = env_->fs_->files.find(BlobPath(kDir, sha256));
    return it != env_->fs_->files.end() && it->second.size() == size;
  }

  std::unique_ptr<nc::ObjectSink> Open(const std::string& sha256, std::uint64_t, nc::StoreStatus* status) override;

  FakeEnv* env_;
};

class MemSink : public nc::ObjectSink {
 public:
  MemSink(std::unique_ptr<TestTemp> temp, std::string final_path, bool no_space, MemStore* owner)
      : temp_(std::move(temp)), final_(std::move(final_path)), no_space_(no_space), owner_(owner) {}
  nc::StoreStatus Write(const void* data, std::size_t length) override {
    if (no_space_) return nc::StoreStatus::kNoSpace;
    if (!temp_->Write(data, length)) return nc::StoreStatus::kIoError;
    ++owner_->body_writes;
    return nc::StoreStatus::kOk;
  }
  nc::StoreStatus Commit() override {
    if (owner_->fail_commit) return nc::StoreStatus::kIoError;
    return temp_->Commit(final_) ? nc::StoreStatus::kOk : nc::StoreStatus::kIoError;
  }
  void Abort() override { temp_->Abort(); }

 private:
  std::unique_ptr<TestTemp> temp_;
  std::string final_;
  bool no_space_;
  MemStore* owner_;
};

std::unique_ptr<nc::ObjectSink> MemStore::Open(const std::string& sha256, std::uint64_t, nc::StoreStatus* status) {
  ++opened;
  *status = nc::StoreStatus::kOk;
  if (opened == refuse_open) {
    *status = nc::StoreStatus::kIoError;
    return nullptr;
  }
  return std::unique_ptr<nc::ObjectSink>(
      new MemSink(CreateTemp(env_, kDir), BlobPath(kDir, sha256), opened == fail_open, this));
}

// A transport that never delivers a byte: it asks keep_going over and over, as a curl progress callback does, and gives up when told.
class StallEnv : public FakeEnv {
 public:
  using FakeEnv::FakeEnv;
  std::uint64_t NowMs() override { return clock += 10000; }
  nc::HttpResult Get(const std::string& url, const nc::HttpBody&, const std::function<bool()>& keep_going) override {
    requests.push_back(url);
    nc::HttpResult r;
    for (int i = 0; i < 1000; ++i) {
      if (!keep_going()) {
        r.cancelled = true;
        return r;
      }
    }
    return r;
  }
};

const std::string kA(300, 'a');
const std::string kB(500, 'b');
const std::string kC(120, 'c');

nc::PrefetchConfig ConfigOf(const std::vector<nc::Entry>& objects) {
  nc::PrefetchConfig c;
  c.base_url = kBase;
  c.objects = objects;
  c.stall_ms = 30000;
  return c;
}

bool Held(const FakeEnv& env, const std::string& sha256) { return env.fs_->files.count(BlobPath(kDir, sha256)) != 0; }

int CountRequests(const FakeEnv& env, const std::string& sha256) {
  int n = 0;
  for (const std::string& url : env.requests) n += url == UrlOf(sha256) ? 1 : 0;
  return n;
}

// The first event named `name`, or "" when there is none.
std::string FirstEvent(const FakeEnv& env, const std::string& name) {
  const std::vector<std::string> found = env.EventsNamed(name);
  return found.empty() ? std::string() : found.front();
}

void TestFull() {
  auto fs = std::make_shared<cargo_test::Fs>();
  FakeEnv env(fs);
  const nc::Entry a = EntryOf("lobby_posters", 1, kA);
  const nc::Entry b = EntryOf("lobby_posters", 2, kB);
  const nc::Entry c = EntryOf("boot", 3, kC);
  env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
  env.server[UrlOf(b.sha256)] = Response{200, kB, std::string::npos, std::string()};
  env.server[UrlOf(c.sha256)] = Response{200, kC, std::string::npos, std::string()};
  MemStore store(&env);

  const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a, b, c}));

  QCHECK(r.status == nc::PrefetchStatus::kComplete);
  QCHECK(r.objects == 3);
  QCHECK(r.fetched == 3);
  QCHECK(r.held == 0);
  QCHECK(r.bytes_fetched == kA.size() + kB.size() + kC.size());
  QCHECK(env.requests.size() == 3);
  QCHECK(Held(env, a.sha256) && Held(env, b.sha256) && Held(env, c.sha256));
  QCHECK(fs->files[BlobPath(kDir, a.sha256)] == kA);
  QCHECK(fs->files[BlobPath(kDir, c.sha256)] == kC);
  // A test-store check, not an on-disk index: MemStore::Has reads the fake filesystem, so this shows only that the
  // lookup names every object of the set with its stored size, and does not name an absent one. The on-disk index is P3's.
  for (const nc::Entry* e : {&a, &b, &c}) {
    QCHECK(store.Has(e->sha256));
    QCHECK(store.HoldsSized(e->sha256, e->size, e->bytes));
  }
  QCHECK(!store.Has(Sha256Of("never fetched")));
  QCHECK(env.EventsNamed("cargo_prefetch_object").size() == 3);
  // The level of each record: a stored object is Debug, the run's summary is Info.
  for (const std::string& line : env.EventsNamed("cargo_prefetch_object")) QCHECK(FakeEnv::Value(line, "level") == "debug");
  QCHECK(FakeEnv::Value(FirstEvent(env, "cargo_prefetch_done"), "level") == "info");
  QCHECK(env.EventsNamed("cargo_prefetch_failed").empty());
  QCHECK(!FirstEvent(env, "cargo_prefetch_done").empty());
  std::printf("full: status=%s fetched=%zu held=%zu bytes=%llu requests=%zu\n", nc::PrefetchStatusName(r.status), r.fetched,
              r.held, static_cast<unsigned long long>(r.bytes_fetched), env.requests.size());
}

void TestResumed() {
  auto fs = std::make_shared<cargo_test::Fs>();
  const nc::Entry a = EntryOf("lobby_posters", 1, kA);
  const nc::Entry b = EntryOf("lobby_posters", 2, kB);
  const nc::Entry c = EntryOf("boot", 3, kC);
  const std::vector<nc::Entry> set = {a, b, c};

  // The first run is killed after the first object is committed: its second object's first write fails.
  FakeEnv first(fs);
  first.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
  first.server[UrlOf(b.sha256)] = Response{200, kB, std::string::npos, std::string()};
  first.server[UrlOf(c.sha256)] = Response{200, kC, std::string::npos, std::string()};
  // Object A is 300 bytes: four body writes (97-byte chunks) and a commit are five mutating ops. The kill lands after two
  // of object B's six body writes, so B's temp is left behind and B is not committed.
  first.stop_after = 7;
  MemStore store1(&first);
  const nc::PrefetchResult r1 = nc::Prefetch(first, store1, ConfigOf(set));
  QCHECK(r1.status == nc::PrefetchStatus::kStoreFailed);
  QCHECK(r1.fetched == 1);
  QCHECK(Held(first, a.sha256));
  QCHECK(!Held(first, b.sha256));

  // The next launch: a new env on the same files. A is held and not requested again; B and C are fetched.
  FakeEnv second(fs);
  second.server = first.server;
  MemStore store2(&second);
  const nc::PrefetchResult r2 = nc::Prefetch(second, store2, ConfigOf(set));
  QCHECK(r2.status == nc::PrefetchStatus::kComplete);
  QCHECK(r2.held == 1);
  QCHECK(r2.fetched == 2);
  QCHECK(CountRequests(second, a.sha256) == 0);
  QCHECK(CountRequests(second, b.sha256) == 1);
  QCHECK(CountRequests(second, c.sha256) == 1);
  QCHECK(Held(second, b.sha256) && Held(second, c.sha256));
  QCHECK(fs->files[BlobPath(kDir, b.sha256)] == kB);
  std::printf("resumed: run1=%s fetched=%zu; run2=%s held=%zu fetched=%zu requests=%zu\n", nc::PrefetchStatusName(r1.status),
              r1.fetched, nc::PrefetchStatusName(r2.status), r2.held, r2.fetched, second.requests.size());
}

void TestNotFound() {
  auto fs = std::make_shared<cargo_test::Fs>();
  FakeEnv env(fs);
  const nc::Entry a = EntryOf("lobby_posters", 1, kA);
  const nc::Entry b = EntryOf("lobby_posters", 2, kB);
  const nc::Entry c = EntryOf("boot", 3, kC);
  env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
  env.server[UrlOf(b.sha256)] = Response{404, "<Error>NoSuchKey</Error>", std::string::npos, std::string()};
  env.server[UrlOf(c.sha256)] = Response{200, kC, std::string::npos, std::string()};
  MemStore store(&env);

  const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a, b, c}));

  QCHECK(r.status == nc::PrefetchStatus::kHttpFailed);
  QCHECK(r.failed_sha256 == b.sha256);
  QCHECK(r.detail == "404");
  QCHECK(Held(env, a.sha256));
  QCHECK(!Held(env, b.sha256));
  QCHECK(CountRequests(env, c.sha256) == 0);  // the run ends at the failure
  const std::string failed = FirstEvent(env, "cargo_prefetch_failed");
  QCHECK(FakeEnv::Value(failed, "reason") == "http_status");
  QCHECK(FakeEnv::Value(failed, "http_status") == "404");
  QCHECK(FakeEnv::Value(failed, "sha256") == b.sha256);
  QCHECK(FakeEnv::Value(FirstEvent(env, "cargo_prefetch_done"), "level") == "error");
  std::printf("404: status=%s detail=%s failed=%s\n", nc::PrefetchStatusName(r.status), r.detail.c_str(), failed.c_str());
}

void TestStalled() {
  auto fs = std::make_shared<cargo_test::Fs>();
  StallEnv env(fs);
  const nc::Entry a = EntryOf("lobby_posters", 1, kA);
  MemStore store(&env);

  const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));

  QCHECK(r.status == nc::PrefetchStatus::kStalled);
  QCHECK(r.failed_sha256 == a.sha256);
  QCHECK(!Held(env, a.sha256));
  QCHECK(FakeEnv::Value(FirstEvent(env, "cargo_prefetch_failed"), "reason") == "stalled");
  std::printf("stalled: status=%s requests=%zu\n", nc::PrefetchStatusName(r.status), env.requests.size());
}

void TestHashMismatch() {
  auto fs = std::make_shared<cargo_test::Fs>();
  FakeEnv env(fs);
  const nc::Entry a = EntryOf("lobby_posters", 1, kA);
  const nc::Entry b = EntryOf("lobby_posters", 2, kB);
  const nc::Entry c = EntryOf("boot", 3, kC);
  const std::string wrong(kB.size(), 'x');  // the same length, other bytes
  env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
  env.server[UrlOf(b.sha256)] = Response{200, wrong, std::string::npos, std::string()};
  env.server[UrlOf(c.sha256)] = Response{200, kC, std::string::npos, std::string()};
  MemStore store(&env);

  const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a, b, c}));

  QCHECK(r.status == nc::PrefetchStatus::kHashMismatch);
  QCHECK(r.failed_sha256 == b.sha256);
  QCHECK(!Held(env, b.sha256));  // refused, not stored
  QCHECK(Held(env, a.sha256));
  QCHECK(CountRequests(env, c.sha256) == 0);
  const std::string refused = FirstEvent(env, "cargo_prefetch_refused");
  QCHECK(FakeEnv::Value(refused, "reason") == "sha256_mismatch");
  QCHECK(FakeEnv::Value(refused, "sha256") == b.sha256);
  QCHECK(FakeEnv::Value(refused, "name") == "0x2");
  QCHECK(FakeEnv::Value(refused, "level") == "warn");
  QCHECK(FakeEnv::Value(FirstEvent(env, "cargo_prefetch_failed"), "level") == "error");
  std::printf("hash-mismatch: status=%s refused=%s\n", nc::PrefetchStatusName(r.status), refused.c_str());
}

void TestDiskFull() {
  auto fs = std::make_shared<cargo_test::Fs>();
  FakeEnv env(fs);
  const nc::Entry a = EntryOf("lobby_posters", 1, kA);
  const nc::Entry b = EntryOf("lobby_posters", 2, kB);
  const nc::Entry c = EntryOf("boot", 3, kC);
  env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
  env.server[UrlOf(b.sha256)] = Response{200, kB, std::string::npos, std::string()};
  env.server[UrlOf(c.sha256)] = Response{200, kC, std::string::npos, std::string()};
  MemStore store(&env);
  store.fail_open = 2;  // the second object's writes find no room

  const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a, b, c}));

  QCHECK(r.status == nc::PrefetchStatus::kNoSpace);
  QCHECK(r.failed_sha256 == b.sha256);
  QCHECK(r.fetched == 1);
  QCHECK(Held(env, a.sha256));
  QCHECK(!Held(env, b.sha256));
  QCHECK(CountRequests(env, c.sha256) == 0);
  QCHECK(FakeEnv::Value(FirstEvent(env, "cargo_prefetch_failed"), "reason") == "no_space");
  QCHECK(FakeEnv::Value(FirstEvent(env, "cargo_prefetch_failed"), "level") == "error");
  std::printf("disk-full: status=%s fetched=%zu\n", nc::PrefetchStatusName(r.status), r.fetched);
}

// The cases the six runs do not reach: a repeated object, a refused open, a failed commit, a body the server sends past or short of
// its size, a stop from the caller, a transfer that ends early, and a non-200 answer's body that must never reach the sink.
void TestEdges() {
  {  // a repeated object is fetched once
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    const nc::Entry b = EntryOf("lobby_posters", 2, kB);
    env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
    env.server[UrlOf(b.sha256)] = Response{200, kB, std::string::npos, std::string()};
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a, b, a}));
    QCHECK(r.status == nc::PrefetchStatus::kComplete);
    QCHECK(r.objects == 2);
    QCHECK(r.fetched == 2);
    QCHECK(CountRequests(env, a.sha256) == 1);
  }
  {  // the store refuses to open an object: an I/O error, not no_space
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
    MemStore store(&env);
    store.refuse_open = 1;
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kStoreFailed);
    QCHECK(r.detail == "io_error");
    QCHECK(!Held(env, a.sha256));
  }
  {  // the store refuses the commit: the object is not held and the run is a store failure
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
    MemStore store(&env);
    store.fail_commit = true;
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kStoreFailed);
    QCHECK(r.detail == "io_error");
    QCHECK(!Held(env, a.sha256));
  }
  {  // a 404 body never reaches the sink
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{404, "<Error>NoSuchKey</Error>", std::string::npos, std::string()};
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kHttpFailed);
    QCHECK(store.body_writes == 0);
  }
  {  // a body longer than the manifest's size is refused, not stored
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string("extra")};
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kSizeMismatch);
    QCHECK(r.detail == "longer than the manifest's size");
    QCHECK(!Held(env, a.sha256));
  }
  {  // a body shorter than the manifest's size is refused, not stored
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA.substr(0, 100), std::string::npos, std::string()};
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kSizeMismatch);
    QCHECK(r.detail == "shorter than the manifest's size");
    QCHECK(!Held(env, a.sha256));
  }
  {  // a transfer that ends early is a transport failure
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA, 50, std::string()};
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kTransport);
    QCHECK(r.detail == "code 18");
    QCHECK(!Held(env, a.sha256));
  }
  {  // the caller's stop is honoured at the next boundary
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
    MemStore store(&env);
    nc::PrefetchConfig c = ConfigOf({a});
    c.keep_going = []() { return false; };
    const nc::PrefetchResult r = nc::Prefetch(env, store, c);
    QCHECK(r.status == nc::PrefetchStatus::kCancelled);
    QCHECK(!Held(env, a.sha256));
  }
}

// A held object is checked against the manifest's size before it is skipped. Held at the wrong size: refused by name, and the
// stored bytes are left alone. Held at the right size: skipped, with no request.
void TestHeldSize() {
  {
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    env.server[UrlOf(a.sha256)] = Response{200, kA, std::string::npos, std::string()};
    fs->files[BlobPath(kDir, a.sha256)] = std::string(7, 'z');  // held, but not the manifest's 300 bytes
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(std::string(nc::PrefetchStatusName(r.status)) == "held_size_mismatch");
    QCHECK(r.failed_sha256 == a.sha256);
    QCHECK(CountRequests(env, a.sha256) == 0);
    QCHECK(fs->files[BlobPath(kDir, a.sha256)] == std::string(7, 'z'));
    const std::string refused = FirstEvent(env, "cargo_prefetch_refused");
    QCHECK(FakeEnv::Value(refused, "reason") == "held_size_mismatch");
    QCHECK(FakeEnv::Value(refused, "name") == "0x1");
    QCHECK(FakeEnv::Value(refused, "level") == "warn");
    std::printf("held-wrong-size: status=%s refused=%s\n", nc::PrefetchStatusName(r.status), refused.c_str());
  }
  {
    auto fs = std::make_shared<cargo_test::Fs>();
    FakeEnv env(fs);
    const nc::Entry a = EntryOf("lobby_posters", 1, kA);
    fs->files[BlobPath(kDir, a.sha256)] = kA;  // held at the manifest's size
    MemStore store(&env);
    const nc::PrefetchResult r = nc::Prefetch(env, store, ConfigOf({a}));
    QCHECK(r.status == nc::PrefetchStatus::kComplete);
    QCHECK(r.held == 1);
    QCHECK(r.fetched == 0);
    QCHECK(CountRequests(env, a.sha256) == 0);
    std::printf("held-right-size: status=%s held=%zu\n", nc::PrefetchStatusName(r.status), r.held);
  }
}

}  // namespace

int main() {
  TestFull();
  TestResumed();
  TestNotFound();
  TestStalled();
  TestHashMismatch();
  TestDiskFull();
  TestEdges();
  TestHeldSize();
  const int failures = quest_test::Failures();
  std::printf("cargo_prefetch_test: %s (%d failed checks)\n", failures == 0 ? "OK" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
