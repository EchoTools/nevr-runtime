// Host test for the grey default irradiance texture (#2091, #1737 unit U25): the real handlers
// (src/quest/sentinel/irradiance_default_hook.cpp), built without exceptions as the sentinel builds it, driven through the two
// thunks' entries with a model of CGEngineResources::Initialize and of the image upload helper FUN_019daf90 as stand-ins. No GOT
// slot and no row is touched: got_pinned_test.cpp and the patcher's pinned test check both against the real library, and
// row_hooks_install_test.cpp activates the row on a synthetic image.
//
// What it pins: during Initialize, and only for the SGTexture at `this + 0x1458` with the four-byte format and a texel, the upload
// helper is given the grey texel (bytes 6b 6b 6b ff) instead of the black one; every other call of the helper (another texture of the
// same Initialize, the same address outside Initialize, another object's +0x1458, another format, a null texel, an unarmed hook) is
// passed to the original with the arguments it came with; Initialize's status comes back unchanged and the in-progress mark is
// cleared when it returns; the log carries one `substituted` line per process, and one `not_reached` warning when an Initialize
// finishes without substituting.
//
// Built and run by `just test-quest-hooks`.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "irradiance_default_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace idh = nevr_quest::irradiance_default;

idh::InitializeThunk::Fn InitializeEntry() { return reinterpret_cast<idh::InitializeThunk::Fn>(idh::InitializeThunk::EntryAddress()); }
idh::UploadThunk::Fn UploadEntry() { return reinterpret_cast<idh::UploadThunk::Fn>(idh::UploadThunk::EntryAddress()); }

// ---- the game's side ----------------------------------------------------------------------------------------------------

constexpr std::size_t kResourcesBytes = 0x2900;  // the extent Initialize touches (the reconstruction's static_assert)
constexpr std::size_t kShadowOffset = 0x14b0;    // "Default 3D Shadow Texture", the next upload of Initialize
constexpr std::uint32_t kShadowFormat = 0x12;

struct Seen {
  void* texture;
  const void* image;
  std::uint32_t format;
  std::int32_t arg3;
  std::int32_t arg4;
  const void* texel;
  const char* name;
  unsigned char texelBytes[4];  // what the pointer pointed at, when the helper was called
};
std::vector<Seen> g_seen;

// The helper: records every argument and the four bytes the texel pointer holds at the call (the real helper copies them at once).
void StandInUpload(void* texture, const void* image, std::uint32_t format, std::int32_t arg3, std::int32_t arg4, const void* texel,
                   const char* name) {
  Seen s{texture, image, format, arg3, arg4, texel, name, {0, 0, 0, 0}};
  if (texel != nullptr) std::memcpy(s.texelBytes, texel, sizeof(s.texelBytes));
  g_seen.push_back(s);
}

struct Step {
  std::size_t offset;  // from `this`
  std::uint32_t format;
  const void* texel;
  const char* name;
};
std::vector<Step> g_script;     // what the modelled Initialize uploads, in order
std::int32_t g_initStatus = 0;  // what the modelled Initialize returns
int g_initCalls = 0;
void* g_initThis = nullptr;

const unsigned char kBlack[4] = {0, 0, 0, 0};          // sp + 0x45c, zeroed at 0x193efd0
const unsigned char kShadow[2] = {0xff, 0xff};         // sp + 0x5a8, one 0xffff texel
const char* const kName3d = "Default 3D Texture";
const char* const kNameShadow = "Default 3D Shadow Texture";

// CGEngineResources::Initialize: each step is a call of the helper through its entry, as the game's `bl` reaches the row's stub.
std::int32_t StandInInitialize(void* self) {
  ++g_initCalls;
  g_initThis = self;
  for (const Step& step : g_script) {
    UploadEntry()(static_cast<unsigned char*>(self) + step.offset, nullptr, step.format, 0, 0, step.texel, step.name);
  }
  return g_initStatus;
}

alignas(16) unsigned char g_resources[kResourcesBytes];
alignas(16) unsigned char g_other[kResourcesBytes];

std::vector<std::string> g_lines;
std::vector<sentinel::LogLevel> g_levels;
void CaptureLine(sentinel::LogLevel level, const char* line) {
  g_levels.push_back(level);
  g_lines.emplace_back(line);
}

void Reset() {
  g_seen.clear();
  g_script.clear();
  g_lines.clear();
  g_levels.clear();
  g_initStatus = 0;
  g_initCalls = 0;
  g_initThis = nullptr;
  std::memset(g_resources, 0, sizeof(g_resources));
  std::memset(g_other, 0, sizeof(g_other));
  idh::ResetForTest();
  idh::InitializeThunk::Reset();
  idh::UploadThunk::Reset();
  *idh::InitializeThunk::OriginalOut() = reinterpret_cast<void*>(&StandInInitialize);
  *idh::UploadThunk::OriginalOut() = reinterpret_cast<void*>(&StandInUpload);
}

// The stock Initialize's two default 3D uploads: the irradiance texture with the black texel, then the shadow texture.
void ScriptTheStockInitialize() {
  g_script = {{idh::kDefault3dTextureOffset, idh::kTexelFormat, kBlack, kName3d},
              {kShadowOffset, kShadowFormat, kShadow, kNameShadow}};
}

bool IsGrey(const unsigned char (&bytes)[4]) { return bytes[0] == 0x6b && bytes[1] == 0x6b && bytes[2] == 0x6b && bytes[3] == 0xff; }

const Seen* Find(const char* name) {
  for (const Seen& s : g_seen) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

int CountEvents(const char* needle) {
  int n = 0;
  for (const std::string& line : g_lines) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

bool Has(const std::string& line, const char* needle) { return line.find(needle) != std::string::npos; }

// ---- tests --------------------------------------------------------------------------------------------------------------

// The texel the row hands the helper is the reference build's word, byte for byte.
void TheGreyTexelIsTheReferenceWord() {
  QCHECK(idh::kGreyTexel == 0xff6b6b6bU);
  QCHECK(idh::kDefault3dTextureOffset == 0x1458);
  QCHECK(idh::kTexelFormat == 9);
}

// The central case: the default 3D texture is uploaded with the grey texel, and the black one is not what the helper reads.
void TheDefault3dTextureIsUploadedGreyDuringInitialize() {
  Reset();
  idh::ArmForTest();
  ScriptTheStockInitialize();
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(g_initCalls == 1);
  const Seen* irradiance = Find(kName3d);
  QCHECK(irradiance != nullptr);
  if (irradiance == nullptr) return;
  QCHECK(irradiance->texture == g_resources + idh::kDefault3dTextureOffset);
  QCHECK(irradiance->format == idh::kTexelFormat);
  QCHECK(irradiance->texel != static_cast<const void*>(kBlack));
  QCHECK(IsGrey(irradiance->texelBytes));
}

// Every other upload of the same Initialize reaches the helper with the texel pointer it was given.
void EveryOtherUploadOfInitializeIsUntouched() {
  Reset();
  idh::ArmForTest();
  ScriptTheStockInitialize();
  g_script.push_back({0x1000, idh::kTexelFormat, kBlack, "Default 2D Texture"});  // same format and texel, another texture
  g_script.push_back({0x2000, idh::kTexelFormat, kBlack, "Default Cube Texture"});
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(g_seen.size() == 4);
  const Seen* shadow = Find(kNameShadow);
  QCHECK(shadow != nullptr && shadow->texel == static_cast<const void*>(kShadow));
  const Seen* twoD = Find("Default 2D Texture");
  QCHECK(twoD != nullptr && twoD->texel == static_cast<const void*>(kBlack));
  const Seen* cube = Find("Default Cube Texture");
  QCHECK(cube != nullptr && cube->texel == static_cast<const void*>(kBlack));
  int grey = 0;
  for (const Seen& s : g_seen) grey += IsGrey(s.texelBytes) ? 1 : 0;
  QCHECK(grey == 1);  // exactly the default 3D texture
}

// The arguments the substitution does not own are passed on as they came.
int g_image = 0;  // an image description the modelled Initialize below passes, by address
std::int32_t InitializeWithArguments(void* self) {
  UploadEntry()(static_cast<unsigned char*>(self) + idh::kDefault3dTextureOffset, &g_image, 9, 7, -3, kBlack, kName3d);
  return 0;
}

void OnlyTheTexelPointerChanges() {
  Reset();
  idh::ArmForTest();
  *idh::InitializeThunk::OriginalOut() = reinterpret_cast<void*>(&InitializeWithArguments);
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(g_seen.size() == 1);
  if (g_seen.empty()) return;
  const Seen& s = g_seen[0];
  QCHECK(s.texture == g_resources + idh::kDefault3dTextureOffset);
  QCHECK(s.image == &g_image);
  QCHECK(s.format == 9);
  QCHECK(s.arg3 == 7);
  QCHECK(s.arg4 == -3);
  QCHECK(s.name == kName3d);
  QCHECK(IsGrey(s.texelBytes));
}

// The same SGTexture address outside Initialize is not substituted: before the call, and after it returned.
void TheSameAddressOutsideInitializeIsUntouched() {
  Reset();
  idh::ArmForTest();
  void* const texture = g_resources + idh::kDefault3dTextureOffset;
  UploadEntry()(texture, nullptr, idh::kTexelFormat, 0, 0, kBlack, kName3d);  // before any Initialize
  QCHECK(g_seen.size() == 1 && g_seen[0].texel == static_cast<const void*>(kBlack));
  ScriptTheStockInitialize();
  QCHECK(InitializeEntry()(g_resources) == 0);
  g_seen.clear();
  UploadEntry()(texture, nullptr, idh::kTexelFormat, 0, 0, kBlack, kName3d);  // after it returned
  QCHECK(g_seen.size() == 1 && g_seen[0].texel == static_cast<const void*>(kBlack));
  QCHECK(!IsGrey(g_seen[0].texelBytes));
}

// With no Initialize in progress there is no `this`: a texture at the bare offset (a null `this`) is not the default texture either.
void ABareOffsetWithNoInitializeInProgressIsUntouched() {
  Reset();
  idh::ArmForTest();
  UploadEntry()(reinterpret_cast<void*>(idh::kDefault3dTextureOffset), nullptr, idh::kTexelFormat, 0, 0, kBlack, kName3d);
  QCHECK(g_seen.size() == 1 && g_seen[0].texel == static_cast<const void*>(kBlack));
}

// During Initialize, another object's +0x1458 is not the default texture.
void AnotherObjectsOffsetIsUntouched() {
  Reset();
  idh::ArmForTest();
  struct Local {
    static std::int32_t Init(void* self) {
      UploadEntry()(g_other + idh::kDefault3dTextureOffset, nullptr, idh::kTexelFormat, 0, 0, kBlack, kName3d);
      UploadEntry()(static_cast<unsigned char*>(self) + idh::kDefault3dTextureOffset - 8, nullptr, idh::kTexelFormat, 0, 0, kBlack,
                    kName3d);
      UploadEntry()(static_cast<unsigned char*>(self) + idh::kDefault3dTextureOffset + 8, nullptr, idh::kTexelFormat, 0, 0, kBlack,
                    kName3d);
      return 0;
    }
  };
  *idh::InitializeThunk::OriginalOut() = reinterpret_cast<void*>(&Local::Init);
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(g_seen.size() == 3);
  for (const Seen& s : g_seen) QCHECK(s.texel == static_cast<const void*>(kBlack));
}

// Only the four-byte format carries the four-byte texel; another format on that texture, and a null texel, are the game's own.
void AnotherFormatOrANullTexelIsUntouched() {
  Reset();
  idh::ArmForTest();
  g_script = {{idh::kDefault3dTextureOffset, kShadowFormat, kShadow, kName3d},
              {idh::kDefault3dTextureOffset, idh::kTexelFormat, nullptr, kNameShadow}};
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(g_seen.size() == 2);
  QCHECK(g_seen[0].texel == static_cast<const void*>(kShadow));
  QCHECK(g_seen[1].texel == nullptr);
}

// Initialize's status is the game's, and the mark ends with the call.
void TheStatusComesBackAndTheMarkEnds() {
  Reset();
  idh::ArmForTest();
  ScriptTheStockInitialize();
  g_initStatus = -7;
  QCHECK(InitializeEntry()(g_resources) == -7);
  g_seen.clear();
  UploadEntry()(g_resources + idh::kDefault3dTextureOffset, nullptr, idh::kTexelFormat, 0, 0, kBlack, kName3d);
  QCHECK(g_seen.size() == 1 && g_seen[0].texel == static_cast<const void*>(kBlack));
}

// An unarmed pair is the game: nothing is substituted.
void WithoutTheHooksArmedNothingIsChanged() {
  Reset();
  ScriptTheStockInitialize();
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(g_seen.size() == 2);
  for (const Seen& s : g_seen) QCHECK(!IsGrey(s.texelBytes));
  QCHECK(g_lines.empty());
}

// One `substituted` line per process, Info, naming the texture, the texel and the count; a second Initialize logs nothing.
void TheSubstitutionIsLoggedOnce() {
  Reset();
  idh::ArmForTest();
  ScriptTheStockInitialize();
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(CountEvents("\"event\":\"irradiance_default\"") == 1);
  QCHECK(CountEvents("\"status\":\"substituted\"") == 1);
  if (!g_lines.empty()) {
    const std::string& line = g_lines[0];
    QCHECK(Has(line, "\"texture\":\"Default 3D Texture\""));
    QCHECK(Has(line, "\"texel\":\"0xff6b6b6b\""));
    QCHECK(Has(line, "\"substitutions\":1"));
    QCHECK(Has(line, "\"initialize_status\":0"));
    QCHECK(g_levels[0] == sentinel::LogLevel::kInfo);
  }
  QCHECK(InitializeEntry()(g_resources) == 0);  // the engine re-initialised: substituted again, logged no more
  QCHECK(g_lines.size() == 1);
  QCHECK(g_seen.size() == 4);
}

// An Initialize that finishes without reaching the default 3D texture is a warning, once, with the status it returned.
void AnInitializeThatNeverReachesTheTextureIsAWarning() {
  Reset();
  idh::ArmForTest();
  g_script = {{kShadowOffset, kShadowFormat, kShadow, kNameShadow}};
  g_initStatus = 5;
  QCHECK(InitializeEntry()(g_resources) == 5);
  QCHECK(g_lines.size() == 1);
  if (!g_lines.empty()) {
    QCHECK(Has(g_lines[0], "\"status\":\"not_reached\""));
    QCHECK(Has(g_lines[0], "\"initialize_status\":5"));
    QCHECK(Has(g_lines[0], "\"substitutions\":0"));
    QCHECK(g_levels[0] == sentinel::LogLevel::kWarn);
  }
  QCHECK(InitializeEntry()(g_resources) == 5);  // the same failure again: not said again
  QCHECK(g_lines.size() == 1);
  g_script.clear();
  ScriptTheStockInitialize();
  g_initStatus = 0;
  QCHECK(InitializeEntry()(g_resources) == 0);  // a later success is still reported
  QCHECK(CountEvents("\"status\":\"substituted\"") == 1);
}

// The count belongs to one Initialize: after a call that substituted, one that does not reach the texture is still a warning (a count
// carried over from the earlier call would hide it).
void TheCountStartsOverWithEachInitialize() {
  Reset();
  idh::ArmForTest();
  ScriptTheStockInitialize();
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(CountEvents("\"status\":\"substituted\"") == 1);
  g_script = {{kShadowOffset, kShadowFormat, kShadow, kNameShadow}};
  QCHECK(InitializeEntry()(g_resources) == 0);
  QCHECK(CountEvents("\"status\":\"not_reached\"") == 1);
  QCHECK(g_lines.size() == 2);
}

// The helper is called outside any Initialize at the game's own rate, and the hook never logs on that path.
void TheUploadPathNeverLogs() {
  Reset();
  idh::ArmForTest();
  for (int i = 0; i < 100; ++i) UploadEntry()(g_resources + 0x40, nullptr, 9, 0, 0, kBlack, "Texture");
  QCHECK(g_lines.empty());
  QCHECK(g_seen.size() == 100);
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureLine);  // every line below lands in g_lines, none on stderr
  TheGreyTexelIsTheReferenceWord();
  TheDefault3dTextureIsUploadedGreyDuringInitialize();
  EveryOtherUploadOfInitializeIsUntouched();
  OnlyTheTexelPointerChanges();
  TheSameAddressOutsideInitializeIsUntouched();
  ABareOffsetWithNoInitializeInProgressIsUntouched();
  AnotherObjectsOffsetIsUntouched();
  AnotherFormatOrANullTexelIsUntouched();
  TheStatusComesBackAndTheMarkEnds();
  WithoutTheHooksArmedNothingIsChanged();
  TheSubstitutionIsLoggedOnce();
  AnInitializeThatNeverReachesTheTextureIsAWarning();
  TheCountStartsOverWithEachInitialize();
  TheUploadPathNeverLogs();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "irradiance_default_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("irradiance_default_test: all checks pass\n");
  return 0;
}
