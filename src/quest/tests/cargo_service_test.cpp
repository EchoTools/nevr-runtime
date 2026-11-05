// Host test for the Quest side of the CDN texture delivery (src/quest/cargo/; docs/adr/0045-cdn-texture-delivery.md): the POSIX Env on
// real files in a scratch directory (temp and rename, held read handles, hashing, atomic writes, the events as sentinel JSON lines),
// the settings the config file changes, and the whole first launch and second launch through the shared worker over a scripted
// transport. The handle a mounted blob is given reads the bytes that were hashed, even when the file at its path is replaced.
//
// Built and run by `just test-quest-data`.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/cargo/cargo_service.h"
#include "quest/cargo/posix_env.h"
#include "quest/data/sha256.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
namespace qc = nevr_quest::cargo;
using json = nlohmann::ordered_json;
using nevr_quest::data::Sha256Hex;

std::mutex g_logMutex;
std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_lines.emplace_back(line);
}
std::vector<json> Events(const char* name) {
  std::vector<json> out;
  std::lock_guard<std::mutex> lock(g_logMutex);
  for (const std::string& l : g_lines) {
    const json j = json::parse(l, nullptr, false);
    QCHECK(!j.is_discarded());  // every line parses
    if (!j.is_discarded() && j.value("event", "") == name) out.push_back(j);
  }
  return out;
}
std::string EventOrder() {
  std::string out;
  std::lock_guard<std::mutex> lock(g_logMutex);
  for (const std::string& l : g_lines) {
    const json j = json::parse(l, nullptr, false);
    if (j.is_discarded()) continue;
    const std::string e = j.value("event", "");
    if (e.rfind("cargo_", 0) != 0) continue;
    if (!out.empty()) out += ",";
    out += e;
  }
  return out;
}
void ClearLog() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_lines.clear();
}

struct Response {
  long status = 200;
  std::string body;
};

class Scripted : public nevr_quest::data::Transport {
 public:
  std::map<std::string, Response> server;
  std::vector<std::string> requests;
  nevr_quest::data::TransportResult GetRange(const std::string& url, std::uint64_t offset, const nevr_quest::data::BodyFn& on_body,
                                             const nevr_quest::data::KeepGoingFn& keep_going) override {
    requests.push_back(url);
    QCHECK(offset == 0);  // the cargo fetch always starts at the first byte
    nevr_quest::data::TransportResult r;
    const auto it = server.find(url);
    const Response resp = it != server.end() ? it->second : Response{404, ""};
    r.http_status = resp.status;
    std::size_t at = 0;
    while (at < resp.body.size()) {
      if (!keep_going()) {
        r.interrupted = true;
        return r;
      }
      const std::size_t n = std::min<std::size_t>(1000, resp.body.size() - at);
      if (!on_body(resp.status, resp.body.data() + at, n)) return r;
      at += n;
    }
    if (resp.body.empty() && !on_body(resp.status, "", 0)) return r;
    r.completed = true;
    return r;
  }
};

std::string Bytes(std::size_t n, char seed) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(seed + static_cast<char>(i % 23));
  return s;
}

void ThePosixEnvWorksOnRealFiles() {
  data_test::Scratch scratch;
  qc::PosixEnv env(nullptr);
  const std::string deep = scratch.Path("a/b/c");
  QCHECK(env.MakeDirs(deep) && env.MakeDirs(deep));  // twice is fine
  QCHECK(!env.MakeDirs("relative/path"));
  QCHECK(!env.MakeDirs(""));
  // Atomic whole-file write and the small-file reader.
  QCHECK(env.WriteFileAtomically(deep + "/index.json", "{\"a\":1}"));
  std::string text;
  QCHECK(env.ReadSmallFile(deep + "/index.json", &text, 100) && text == "{\"a\":1}");
  QCHECK(!env.ReadSmallFile(deep + "/index.json", &text, 3));          // over the cap
  QCHECK(!env.ReadSmallFile(deep + "/missing", &text, 100));
  QCHECK(::symlink((deep + "/index.json").c_str(), (deep + "/link").c_str()) == 0);
  QCHECK(!env.ReadSmallFile(deep + "/link", &text, 100));            // a link is not followed
  // A temp file: written, flushed and renamed in one step; two are different files.
  {
    std::unique_ptr<nc::TempFile> a = env.CreateTemp(deep), b = env.CreateTemp(deep);
    QCHECK(a && b);
    QCHECK(a->Write("hello ", 6) && a->Write("world", 5));
    QCHECK(b->Write("other", 5));
    QCHECK(a->Commit(deep + "/final.blob"));
    QCHECK(data_test::ReadFile(deep + "/final.blob") == "hello world");
    b->Abort();
  }
  // No temp file is left: the committed one was renamed, the aborted one deleted.
  {
    std::unique_ptr<nc::TempFile> t = env.CreateTemp(deep);
    QCHECK(t && t->Write("x", 1));
    // destroyed without Commit: the file goes
  }
  const auto listingOf = [](const std::string& dir) {
    std::string out;
    if (DIR* d = ::opendir(dir.c_str())) {
      while (const dirent* e = ::readdir(d)) out += std::string(e->d_name) + "\n";
      ::closedir(d);
    }
    return out;
  };
  std::string listing = listingOf(deep);
  QCHECK(listing.find(".tmp-") == std::string::npos);
  QCHECK(listing.find("final.blob") != std::string::npos && listing.find("index.json") != std::string::npos);
  // A commit onto a name in a directory that does not exist fails and leaves no temp file.
  {
    std::unique_ptr<nc::TempFile> t = env.CreateTemp(deep);
    QCHECK(t && t->Write("x", 1));
    QCHECK(!t->Commit(scratch.Path("no/such/dir/x.blob")));
  }
  QCHECK(listingOf(deep).find(".tmp-") == std::string::npos);  // the failed commit removed its temp file
  QCHECK(env.CreateTemp(scratch.Path("no/such/dir")) == nullptr);
}

void ABlobIsOpenedReadOnlyAndReadAtAnyOffset() {
  data_test::Scratch scratch;
  qc::PosixEnv env(nullptr);
  const std::string blob = Bytes(5000, 'b');
  data_test::WriteFile(scratch.Path("x.blob"), blob);
  std::int64_t handle = -1;
  std::uint64_t size = 0;
  QCHECK(env.OpenBlob(scratch.Path("x.blob"), &handle, &size) && handle >= 0 && size == 5000);
  char out[100];
  std::size_t got = 0;
  QCHECK(env.ReadBlob(handle, 1234, out, 100, &got) && got == 100 && std::memcmp(out, blob.data() + 1234, 100) == 0);
  QCHECK(env.ReadBlob(handle, 4950, out, 100, &got) && got == 50);   // short at the end of the file
  QCHECK(env.ReadBlob(handle, 5000, out, 100, &got) && got == 0);
  // Writing through the handle is not possible: it is read-only.
  QCHECK(::write(static_cast<int>(handle), "x", 1) < 0);
  env.CloseBlob(handle);
  QCHECK(!env.ReadBlob(handle, 0, out, 10, &got));                   // closed
  QCHECK(!env.OpenBlob(scratch.Path("missing"), &handle, &size));
  QCHECK(::mkdir(scratch.Path("dir").c_str(), 0700) == 0);
  QCHECK(!env.OpenBlob(scratch.Path("dir"), &handle, &size));        // not a regular file
  QCHECK(::symlink(scratch.Path("x.blob").c_str(), scratch.Path("lnk").c_str()) == 0);
  QCHECK(!env.OpenBlob(scratch.Path("lnk"), &handle, &size));        // a link is not followed
}

void TheHasherIsSha256() {
  qc::PosixEnv env(nullptr);
  std::unique_ptr<nc::Hasher> h = env.NewHasher();
  const std::string data = Bytes(1000, 'h');
  h->Update(data.data(), 400);
  h->Update(data.data() + 400, 600);
  QCHECK(h->Hex() == Sha256Hex(data.data(), data.size()));
}

void GetMapsTheTransportsAnswer() {
  Scripted t;
  t.server["https://h/a"] = Response{206, "abc"};
  qc::PosixEnv env(&t);
  std::string got;
  const nc::HttpResult r = env.Get("https://h/a", [&](long st, const void* d, std::size_t n) {
    QCHECK(st == 206);
    got.append(static_cast<const char*>(d), n);
    return true;
  }, [] { return true; });
  QCHECK(r.completed && r.status == 206 && !r.cancelled && got == "abc");
  const nc::HttpResult gone = env.Get("https://h/missing", [](long, const void*, std::size_t) { return true; }, [] { return true; });
  QCHECK(gone.status == 404);
  const nc::HttpResult stopped = env.Get("https://h/a", [](long, const void*, std::size_t) { return true; }, [] { return false; });
  QCHECK(stopped.cancelled && !stopped.completed);
  qc::PosixEnv none(nullptr);
  QCHECK(!none.Get("https://h/a", [](long, const void*, std::size_t) { return true; }, [] { return true; }).completed);
}

void EventsAreOneParsableSentinelLineEachWithTypedFields() {
  ClearLog();
  qc::PosixEnv env(nullptr);
  env.Event(nc::EventLevel::kWarn, "cargo_blob", {{"result", "failed"}, {"size", 2000}, {"ms", 7}, {"name", "0x3fa6d25bc24a7b01"}});
  const std::vector<json> lines = Events("cargo_blob");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(lines[0]["level"] == "warn" && lines[0]["result"] == "failed" && lines[0]["name"] == "0x3fa6d25bc24a7b01");
  QCHECK(lines[0]["size"].is_number_integer() && lines[0]["size"] == 2000 && lines[0]["ms"] == 7);
}

void TheSettingsAreTheCodesDefaultsChangedByTheFile() {
  nevr_quest::AssetsChoice none;
  nc::RunConfig c = qc::MakeRunConfig(none, "/c");
  QCHECK(c.dir == "/c" && c.base_url == "https://cargo.echo.taxi/" && c.manifest_path == "v1/manifest.json");
  QCHECK(c.platform.name == std::string("android-arm64") && c.source == "default" && !c.policy.required_only && c.policy.overrides.empty());
  QCHECK(nc::StateOf("lobby_posters", c.policy) == nc::LabelState::kOn && nc::StateOf("anything_else", c.policy) == nc::LabelState::kOff);
  nevr_quest::AssetsChoice file;
  file.cdnUrl = "https://test.example/";
  file.manifest = "v1/test/manifest.json";
  file.requiredOnly = true;
  file.labels = {{"lobby_posters", "off"}, {"extra", "required"}, {"junk", "sideways"}};
  file.source = nevr_quest::Source::kFile;
  c = qc::MakeRunConfig(file, "/c");
  QCHECK(c.base_url == "https://test.example/" && c.manifest_path == "v1/test/manifest.json" && c.source == "config" && c.policy.required_only);
  QCHECK(nc::StateOf("lobby_posters", c.policy) == nc::LabelState::kOff && nc::StateOf("extra", c.policy) == nc::LabelState::kRequired);
  QCHECK(c.policy.overrides.count("junk") == 0);  // a word that is no state changes nothing
  QCHECK(std::string(qc::kDefaultCacheDir) == "/data/data/com.readyatdawn.r15/files/nevr/cache");
}

struct Rig {
  data_test::Scratch scratch;
  Scripted transport;
  std::string desc = Bytes(40, 'd'), pix = Bytes(2000, 'p');
  nc::RunConfig config;
  Rig() {
    config = qc::MakeRunConfig(nevr_quest::AssetsChoice(), scratch.Path("nevr/cache"));
    config.manifest_path = "v1/test/manifest.json";
    const json doc = {{"schema", "nevr-cargo-manifest"},
                      {"schema_version", 1},
                      {"labels", {{"lobby_posters", {{"entries", {Entry("0xe2efe7289d5985b8", desc, 248), Entry("0x489bb35d53ca50e9", pix, 8388608)}}}}}}};
    transport.server["https://cargo.echo.taxi/v1/test/manifest.json"] = Response{206, doc.dump()};
    transport.server["https://cargo.echo.taxi/blobs/sha256/" + Sha256Hex(desc.data(), desc.size())] = Response{206, desc};
    transport.server["https://cargo.echo.taxi/blobs/sha256/" + Sha256Hex(pix.data(), pix.size())] = Response{206, pix};
  }
  static json Entry(const char* type, const std::string& blob, std::uint64_t bytes) {
    return {{"platform", "android-arm64"}, {"type", type}, {"name", "0x3fa6d25bc24a7b01"}, {"format", "zstd-frame"},
            {"sha256", Sha256Hex(blob.data(), blob.size())}, {"size", blob.size()}, {"bytes", bytes}, {"align", 16}, {"stock_size", bytes}};
  }
};

std::vector<const nc::Mounted*> g_published;
void Publish(const nc::Mounted* m) { g_published.push_back(m); }

void TheFirstLaunchOnRealFilesPublishesAShadowWhoseHandlesReadTheHashedBytes() {
  ClearLog();
  g_published.clear();
  Rig rig;
  qc::PosixEnv env(&rig.transport);
  const nc::RunResult r = qc::RunOnce(env, rig.config, &Publish, [] { return true; });
  QCHECK(r.manifest_loaded && r.fetched == 2 && r.failed == 0 && r.published);
  QCHECK(rig.transport.requests.size() == 3);
  const std::string descSha = Sha256Hex(rig.desc.data(), rig.desc.size());
  const std::string pixSha = Sha256Hex(rig.pix.data(), rig.pix.size());
  QCHECK(data_test::ReadFile(rig.config.dir + "/" + descSha + ".blob") == rig.desc);
  QCHECK(data_test::ReadFile(rig.config.dir + "/" + pixSha + ".blob") == rig.pix);
  const json index = json::parse(data_test::ReadFile(rig.config.dir + "/index.json"), nullptr, false);
  QCHECK(!index.is_discarded() && index["version"] == 2 && index["entries"].size() == 2);
  QCHECK(EventOrder() == "cargo_start,cargo_manifest,cargo_plan,cargo_blob,cargo_blob,cargo_index,cargo_mounted,cargo_done");
  QCHECK(g_published.size() == 1 && g_published[0]->shadow != nullptr);
  if (g_published.size() != 1 || !g_published[0]->shadow) return;
  nevr_shadow::Found f;
  QCHECK(g_published[0]->shadow->Find(0x489bb35d53ca50e9ULL, 0x3fa6d25bc24a7b01ULL, &f));
  QCHECK(f.compressed == 2000 && f.decompressed == 8388608 && f.chunk >= nevr_shadow::kCargoChunkBase && f.file_offset == 0);
  // What the engine would do with the request: read `compressed` bytes at the chunk's file offset through the handle.
  std::string viaHandle(f.compressed, '\0');
  std::size_t got = 0;
  QCHECK(env.ReadBlob(f.handle, f.file_offset, &viaHandle[0], viaHandle.size(), &got) && got == 2000 && viaHandle == rig.pix);
  // The file at the path is replaced by other bytes (a rename over it); the held handle still reads what was hashed.
  const std::string path = rig.config.dir + "/" + pixSha + ".blob";
  data_test::WriteFile(path + ".new", Bytes(2000, 'Z'));
  QCHECK(::rename((path + ".new").c_str(), path.c_str()) == 0);
  std::string again(2000, '\0');
  QCHECK(env.ReadBlob(f.handle, 0, &again[0], again.size(), &got) && again == rig.pix);
}

void TheSecondLaunchMountsFromTheCacheAndMakesNoBlobRequest() {
  ClearLog();
  g_published.clear();
  Rig rig;
  {
    qc::PosixEnv env(&rig.transport);
    qc::RunOnce(env, rig.config, &Publish, [] { return true; });
  }
  rig.transport.requests.clear();
  g_published.clear();
  ClearLog();
  qc::PosixEnv env(&rig.transport);
  const nc::RunResult r = qc::RunOnce(env, rig.config, &Publish, [] { return true; });
  QCHECK(r.fetched == 0 && r.cached == 2 && r.failed == 0);
  QCHECK(rig.transport.requests.size() == 1);  // the manifest alone
  QCHECK(g_published.size() == 1);             // the boot mount; the worker finds nothing new to mount
  const std::vector<json> mounted = Events("cargo_mounted");
  QCHECK(mounted.size() == 1 && mounted[0]["source"] == "boot" && mounted[0]["records"] == 2 && mounted[0]["chunk_base"] == "0x01000000");
  QCHECK(EventOrder() == "cargo_mounted,cargo_start,cargo_manifest,cargo_plan,cargo_done");
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  ThePosixEnvWorksOnRealFiles();
  ABlobIsOpenedReadOnlyAndReadAtAnyOffset();
  TheHasherIsSha256();
  GetMapsTheTransportsAnswer();
  EventsAreOneParsableSentinelLineEachWithTypedFields();
  TheSettingsAreTheCodesDefaultsChangedByTheFile();
  TheFirstLaunchOnRealFilesPublishesAShadowWhoseHandlesReadTheHashedBytes();
  TheSecondLaunchMountsFromTheCacheAndMakesNoBlobRequest();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_service_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_service_test: all checks passed\n");
  return 0;
}
