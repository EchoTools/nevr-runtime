// Host test for the hardware dump's libr15 hook handlers (#335): each passes every argument through to the
// original unchanged (floats bit for bit), returns exactly what the original returned, records the query,
// and never reads past a buffer the game handed it. Built with -fno-exceptions, like the sentinel's copy.
#include "quest/diag/hwdump_handlers.h"
#include "quest/diag/hwdump_records.h"

#include <sys/utsname.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

namespace hw = nevr_quest::hwdump;
using hw::OvrJava;
using hw::QueryFn;

std::uint32_t Bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

// The record for (fn, id, name), or nullptr.
const hw::RecordSnapshot* Find(const hw::RecordSnapshot* snaps, std::size_t n, QueryFn fn, std::int64_t id,
                               const char* name = nullptr) {
  for (std::size_t i = 0; i < n; ++i) {
    if (snaps[i].fn != fn || snaps[i].id != id) continue;
    if (name != nullptr && std::string(snaps[i].name) != name) continue;
    return &snaps[i];
  }
  return nullptr;
}

// What the fake originals saw.
struct Seen {
  const void* p0 = nullptr;
  int i0 = 0, i1 = 0, i2 = 0;
  float f0 = 0;
  int calls = 0;
} g_seen;

const OvrJava kJava = {reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222),
                       reinterpret_cast<void*>(0x3333)};
// A float whose bit pattern an int-typed thunk would not preserve: a NaN payload.
const float kOddFloat = [] {
  std::uint32_t u = 0x7fc12345U;
  float f;
  std::memcpy(&f, &u, sizeof f);
  return f;
}();

int FakeGetInt(const OvrJava* java, int id) {
  g_seen = {};
  g_seen.p0 = java;
  g_seen.i0 = id;
  return 0x5eed0000 + id;
}
float FakeGetFloat(const OvrJava* java, int id) {
  g_seen.p0 = java;
  g_seen.i0 = id;
  return kOddFloat;
}
int FakeFloatArray(const OvrJava* java, int id, float* values, int capacity) {
  g_seen.p0 = java;
  g_seen.i0 = id;
  g_seen.i1 = capacity;
  for (int i = 0; i < 5 && i < capacity; ++i) values[i] = 60.0F + 10.0F * static_cast<float>(i);
  return 5;
}
int FakeGetPropertyInt(const OvrJava* java, int id, int* out) {
  g_seen.p0 = java;
  g_seen.i0 = id;
  if (id == 0x20) {
    *out = 77;
    return 1;  // bit 0: the out value is set
  }
  return 0;
}
int FakeExtensions(char* names, std::uint32_t* size) {
  // Writes the names and reports their length, past which the caller's buffer holds a guard.
  std::memcpy(names, "VK_KHR_a VK_KHR_b", 17);
  *size = 17;
  return 0;
}
int FakeSetPropertyInt(const OvrJava* java, int id, int value) {
  g_seen.p0 = java;
  g_seen.i0 = id;
  g_seen.i1 = value;
  return -7;
}
int FakeSetRefreshRate(void* ovr, float rate) {
  g_seen.p0 = ovr;
  g_seen.f0 = rate;
  return 0x77;
}
int FakeClockLevels(void* ovr, int cpu, int gpu) {
  g_seen.p0 = ovr;
  g_seen.i0 = cpu;
  g_seen.i1 = gpu;
  return 3;
}
int FakeLatency(void* ovr, int mode) {
  g_seen.p0 = ovr;
  g_seen.i0 = mode;
  return 9;
}
int FakeInitialize(const hw::OvrInitParms* parms) {
  g_seen.p0 = parms;
  return 0;
}
int FakePropertyGet(const char* name, char* value) {
  g_seen.p0 = name;
  std::strcpy(value, "Quest 2");
  return 7;
}
long FakeSysconf(int name) {
  g_seen.i0 = name;
  return 8;
}
int FakeUname(struct utsname* buf) {
  std::memset(buf, 0, sizeof *buf);
  std::strcpy(buf->sysname, "Linux");
  std::strcpy(buf->machine, "aarch64");
  return 0;
}
int FakeGethostname(char* name, std::size_t len) {
  // Fills the whole buffer with no terminator, as gethostname may when the name is exactly `len` bytes.
  std::memset(name, 'h', len);
  return 0;
}
void FakeGetLanguage(void*, char* out) {
  out[0] = 'e';
  out[1] = 'n';
}

void VrapiHandlersPassEverythingThroughAndRecord() {
  hw::ResetCaptureForTest();
  CHECK(hw::HookVrapiGetSystemPropertyInt(&FakeGetInt, &kJava, 0x40) == 0x5eed0040);
  CHECK(g_seen.p0 == &kJava && g_seen.i0 == 0x40);
  CHECK(Bits(hw::HookVrapiGetSystemPropertyFloat(&FakeGetFloat, &kJava, 4)) == Bits(kOddFloat));
  CHECK(g_seen.i0 == 4);

  float rates[8] = {};
  CHECK(hw::HookVrapiGetSystemPropertyFloatArray(&FakeFloatArray, &kJava, 0x41, rates, 8) == 5);
  CHECK(g_seen.i0 == 0x41 && g_seen.i1 == 8 && rates[4] == 100.0F);

  CHECK(hw::HookVrapiGetSystemStatusInt(&FakeGetInt, &kJava, 1) == 0x5eed0001);
  int out = -1;
  CHECK(hw::HookVrapiGetPropertyInt(&FakeGetPropertyInt, &kJava, 0x20, &out) == 1);
  CHECK(g_seen.p0 == &kJava && g_seen.i0 == 0x20 && out == 77);  // the out pointer reached the original
  int untouched = -5;
  CHECK(hw::HookVrapiGetPropertyInt(&FakeGetPropertyInt, &kJava, 0x21, &untouched) == 0 && untouched == -5);
  struct {
    char names[17];
    char guard[8];
  } ext;
  std::memset(ext.guard, 'G', sizeof ext.guard);
  std::uint32_t size = sizeof ext.names;
  CHECK(hw::HookVrapiGetDeviceExtensionsVulkan(&FakeExtensions, ext.names, &size) == 0 && size == 17);
  CHECK(hw::HookVrapiSetPropertyInt(&FakeSetPropertyInt, &kJava, 0x1e, 3) == -7);
  CHECK(g_seen.i0 == 0x1e && g_seen.i1 == 3);

  void* ovr = reinterpret_cast<void*>(0x4444);
  CHECK(hw::HookVrapiSetDisplayRefreshRate(&FakeSetRefreshRate, ovr, kOddFloat) == 0x77);
  CHECK(g_seen.p0 == ovr && Bits(g_seen.f0) == Bits(kOddFloat));  // the rate arrives bit for bit
  CHECK(hw::HookVrapiSetClockLevels(&FakeClockLevels, ovr, 5, 4) == 3);
  CHECK(g_seen.i0 == 5 && g_seen.i1 == 4);
  CHECK(hw::HookVrapiSetExtraLatencyMode(&FakeLatency, ovr, 1) == 9);
  CHECK(g_seen.i0 == 1);

  hw::OvrInitParms parms = {{1, 2, 3, 4, 5, 0x40100}, kJava};
  CHECK(hw::HookVrapiInitialize(&FakeInitialize, &parms) == 0);
  CHECK(g_seen.p0 == &parms);

  // Captured: the VM and the activity, never the calling thread's JNIEnv.
  const hw::GameCapture cap = hw::Captured();
  CHECK(cap.vm == kJava.vm && cap.activity == kJava.activity);
  CHECK(cap.initialize_seen && cap.initialize_status == 0);

  hw::RecordSnapshot snaps[hw::kMaxRecords];
  const std::size_t n = hw::Records().Snapshot(snaps, hw::kMaxRecords);
  const hw::RecordSnapshot* fl = Find(snaps, n, QueryFn::kVrapiGetSystemPropertyFloat, 4);
  CHECK(fl != nullptr && fl->first.num_floats == 1 && Bits(fl->first.floats[0]) == Bits(kOddFloat));
  const hw::RecordSnapshot* arr = Find(snaps, n, QueryFn::kVrapiGetSystemPropertyFloatArray, 0x41);
  CHECK(arr != nullptr && arr->first.num_floats == 5 && arr->first.floats[0] == 60.0F && arr->first.ints[0] == 5);
  const hw::RecordSnapshot* init = Find(snaps, n, QueryFn::kVrapiInitialize, 0);
  CHECK(init != nullptr && init->first.num_ints == 7 && init->first.ints[5] == 0x40100 && init->first.ints[6] == 0);
  const hw::RecordSnapshot* rate = Find(snaps, n, QueryFn::kVrapiSetDisplayRefreshRate, 0);
  CHECK(rate != nullptr && Bits(rate->first.floats[0]) == Bits(kOddFloat) && rate->first.ints[0] == 0x77);
  const hw::RecordSnapshot* prop = Find(snaps, n, QueryFn::kVrapiGetPropertyInt, 0x20);
  CHECK(prop != nullptr && prop->first.num_ints == 2 && prop->first.ints[0] == 1 && prop->first.ints[1] == 77);
  const hw::RecordSnapshot* unset = Find(snaps, n, QueryFn::kVrapiGetPropertyInt, 0x21);
  CHECK(unset != nullptr && unset->first.num_ints == 1);  // bit 0 clear: the out value is not read
  const hw::RecordSnapshot* exts = Find(snaps, n, QueryFn::kVrapiGetDeviceExtensionsVulkan, 0);
  CHECK(exts != nullptr && std::string(exts->first.text) == "VK_KHR_a VK_KHR_b" && exts->first.ints[1] == 17);
  const hw::RecordSnapshot* set = Find(snaps, n, QueryFn::kVrapiSetPropertyInt, 0x1e);
  CHECK(set != nullptr && set->first.ints[0] == 3);
}

void LibcHandlersPassThroughAndStayInsideTheGamesBuffers() {
  char value[92] = {};
  CHECK(hw::HookSystemPropertyGet(&FakePropertyGet, "ro.product.model", value) == 7);
  CHECK(std::string(value) == "Quest 2");
  CHECK(hw::HookSysconf(&FakeSysconf, 0x60) == 8 && g_seen.i0 == 0x60);
  struct utsname u;
  CHECK(hw::HookUname(&FakeUname, &u) == 0);

  // A buffer the game owns, followed by a guard the handler must not read into the record.
  struct {
    char name[8];
    char guard[8];
  } host;
  std::memset(host.guard, 'G', sizeof host.guard);
  CHECK(hw::HookGethostname(&FakeGethostname, host.name, sizeof host.name) == 0);

  struct {
    char lang[2];
    char guard[6];
  } cfg;
  std::memset(cfg.guard, 'G', sizeof cfg.guard);
  hw::HookConfigGetLanguage(&FakeGetLanguage, nullptr, cfg.lang);
  CHECK(cfg.lang[0] == 'e' && cfg.lang[1] == 'n');

  hw::RecordSnapshot snaps[hw::kMaxRecords];
  const std::size_t n = hw::Records().Snapshot(snaps, hw::kMaxRecords);
  const hw::RecordSnapshot* prop = Find(snaps, n, QueryFn::kSystemPropertyGet, 0, "ro.product.model");
  CHECK(prop != nullptr && std::string(prop->first.text) == "Quest 2" && prop->first.ints[0] == 7);
  const hw::RecordSnapshot* sc = Find(snaps, n, QueryFn::kSysconf, 0x60);
  CHECK(sc != nullptr && sc->first.ints[0] == 8);
  const hw::RecordSnapshot* un = Find(snaps, n, QueryFn::kUname, 0);
  CHECK(un != nullptr && std::string(un->first.text) == "Linux||||aarch64");
  const hw::RecordSnapshot* hn = Find(snaps, n, QueryFn::kGethostname, 0);
  CHECK(hn != nullptr && std::string(hn->first.text) == "hhhhhhhh");  // 8 bytes, no guard bytes
  const hw::RecordSnapshot* lang = Find(snaps, n, QueryFn::kConfigGetLanguage, 0);
  CHECK(lang != nullptr && std::string(lang->first.text) == "en");  // exactly two bytes
}

}  // namespace

int main() {
  VrapiHandlersPassEverythingThroughAndRecord();
  LibcHandlersPassThroughAndStayInsideTheGamesBuffers();
  if (g_failures != 0) {
    std::fprintf(stderr, "hwdump_handlers_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("hwdump_handlers_test: all checks pass\n");
  return 0;
}
