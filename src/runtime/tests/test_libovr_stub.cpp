// The stub LibOVRPlatform64_1.dll (src/libovr-stub): pnsovr.dll resolves seven init functions with GetProcAddress and imports
// the rest through its IAT, so a stub that lacks one of them stops the game at load. This test loads the built DLL under Wine,
// holds its export table to exports.def (the list the build links against, copied beside the test), and holds the answers the
// callers branch on: sync init returns 0 (success), async calls return distinct non-zero request ids, there is never a message
// to pop, and the fake user is non-zero.

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>

namespace {

constexpr const char* kStubDll = "LibOVRPlatform64_1.dll";
constexpr const char* kExportsDef = "libovr_stub_exports.def";
// The fake user the stub reports (stub.cpp: "non-zero so callers don't treat as no user").
constexpr std::uint64_t kStubUserId = 1000;
constexpr std::uint64_t kStubAppId = 1;

using Sync64 = std::uint64_t (*)(std::uint64_t);
using NoArg64 = std::uint64_t (*)();
using NoArgInt = int (*)();
using PtrArgPtr = void* (*)(void*);
using PtrArgStr = const char* (*)(void*);
using PtrArgInt = int (*)(void*);
using PtrArg64 = std::uint64_t (*)(void*);
using TokenInit = std::uint64_t (*)(std::uint64_t, const char*);
using TokenInitOpts = std::uint64_t (*)(std::uint64_t, const char*, void*);
using StandaloneInit = std::uint64_t (*)(std::uint64_t, const char*, const char*);
using OrgInit = std::uint64_t (*)(std::uint64_t, const char*);
using FromString = void (*)(std::uint64_t*, const char*);

// The directory of this test program, where the build puts the stub DLL and the copy of the def file.
std::string ExeDirectory() {
  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
  const std::string full(path, length);
  return full.substr(0, full.find_last_of('\\') + 1);
}

// The names listed under EXPORTS in exports.def: indented, not a comment, not blank.
std::set<std::string> DefNames() {
  std::set<std::string> names;
  std::ifstream in(ExeDirectory() + kExportsDef);
  bool inExports = false;
  std::string line;
  while (std::getline(in, line)) {
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    const std::string text = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (text[0] == ';') continue;
    if (text == "EXPORTS") {
      inExports = true;
      continue;
    }
    if (inExports) names.insert(text);
  }
  return names;
}

// The names the loaded image's export directory carries.
std::set<std::string> ImageExports(HMODULE module) {
  std::set<std::string> names;
  const auto* base = reinterpret_cast<const unsigned char*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0) return names;
  const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  const auto* nameRvas = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
  for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
    names.insert(reinterpret_cast<const char*>(base + nameRvas[i]));
  }
  return names;
}

class LibOvrStub : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { module_ = LoadLibraryA((ExeDirectory() + kStubDll).c_str()); }
  static void TearDownTestSuite() {
    if (module_ != nullptr) FreeLibrary(module_);
    module_ = nullptr;
  }
  template <typename Fn>
  static Fn Get(const char* name) {
    FARPROC proc = module_ != nullptr ? GetProcAddress(module_, name) : nullptr;
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(proc));
  }
  static HMODULE module_;
};
HMODULE LibOvrStub::module_ = nullptr;

TEST_F(LibOvrStub, TheBuiltDllLoads) { ASSERT_NE(module_, nullptr) << kStubDll << " did not load, error " << GetLastError(); }

TEST_F(LibOvrStub, ItExportsEveryNameOfTheDefFileAndNothingElse) {
  ASSERT_NE(module_, nullptr);
  const std::set<std::string> wanted = DefNames();
  const std::set<std::string> image = ImageExports(module_);
  ASSERT_FALSE(wanted.empty()) << kExportsDef << " lists no export (is it beside the test?)";
  for (const std::string& name : wanted) {
    EXPECT_NE(image.count(name), 0u) << "not exported: " << name;
    EXPECT_NE(GetProcAddress(module_, name.c_str()), nullptr) << "GetProcAddress fails for " << name;
  }
  for (const std::string& name : image) EXPECT_NE(wanted.count(name), 0u) << "exported but not in the def file: " << name;
}

TEST_F(LibOvrStub, TheSevenGetProcAddressInitFunctionsReportSuccessTheWayPnsovrReadsIt) {
  ASSERT_NE(module_, nullptr);
  // pnsovr: "if (result != 0) return false" for the synchronous initializers.
  EXPECT_EQ(Get<Sync64>("ovr_PlatformInitializeWindows")(kStubAppId), 0u);
  EXPECT_EQ(Get<TokenInit>("ovr_PlatformInitializeWithAccessToken")(kStubAppId, "token"), 0u);
  EXPECT_EQ(Get<TokenInit>("ovr_PlatformInitializeWithAccessToken")(kStubAppId, nullptr), 0u);
  EXPECT_EQ(Get<TokenInitOpts>("ovr_PlatformInitializeWithAccessTokenAndOptions")(kStubAppId, "token", nullptr), 0u);
  EXPECT_NE(Get<NoArgInt>("ovr_IsPlatformInitialized")(), 0);
  EXPECT_EQ(Get<NoArg64>("ovr_PopMessage")(), 0u);  // never a message to pop: the game's poll loop must end
}

TEST_F(LibOvrStub, AsyncCallsReturnDistinctNonZeroRequestIds) {
  ASSERT_NE(module_, nullptr);
  std::set<std::uint64_t> ids;
  ids.insert(Get<Sync64>("ovr_PlatformInitializeWindowsAsynchronous")(kStubAppId));
  ids.insert(Get<StandaloneInit>("ovr_PlatformInitializeStandaloneAccessToken")(kStubAppId, "token", "org"));
  ids.insert(Get<OrgInit>("ovr_Platform_InitializeStandaloneOculus")(kStubAppId, "org"));
  ids.insert(Get<NoArg64>("ovr_User_GetLoggedInUser")());
  ids.insert(Get<NoArg64>("ovr_User_GetAccessToken")());
  ids.insert(Get<NoArg64>("ovr_User_GetUserProof")());
  ids.insert(Get<Sync64>("ovr_User_Get")(kStubUserId));
  ids.insert(Get<Sync64>("ovr_User_GetOrgScopedID")(kStubUserId));
  constexpr size_t kCalls = 8;
  EXPECT_EQ(ids.size(), kCalls) << "a request id repeated: the caller cannot tell two requests apart";
  EXPECT_EQ(ids.count(0u), 0u) << "0 means the submission failed";
}

TEST_F(LibOvrStub, RequestIdsRiseWithEachCall) {
  ASSERT_NE(module_, nullptr);
  const auto next = Get<NoArg64>("ovr_User_GetLoggedInUser");
  const std::uint64_t first = next();
  const std::uint64_t second = next();
  EXPECT_EQ(second, first + 1);
}

TEST_F(LibOvrStub, PagingEndsAtOnceAndTheFakeUserIsNeverNone) {
  ASSERT_NE(module_, nullptr);
  EXPECT_EQ(Get<PtrArg64>("ovr_User_GetNextUserArrayPage")(nullptr), 0u);  // 0: no more pages
  EXPECT_EQ(Get<PtrArg64>("ovr_User_GetNextUserAndRoomArrayPage")(nullptr), 0u);
  EXPECT_EQ(Get<NoArg64>("ovr_GetLoggedInUserID")(), kStubUserId);
  EXPECT_EQ(Get<PtrArg64>("ovr_User_GetID")(nullptr), kStubUserId);
  EXPECT_STREQ(Get<PtrArgStr>("ovr_User_GetOculusID")(nullptr), "StubUser");
}

TEST_F(LibOvrStub, MessagesAreNeverErrorsAndCarryNothing) {
  ASSERT_NE(module_, nullptr);
  EXPECT_EQ(Get<PtrArg64>("ovr_Message_GetRequestID")(nullptr), 0u);
  EXPECT_EQ(Get<PtrArgInt>("ovr_Message_IsError")(nullptr), 0);
  EXPECT_EQ(Get<PtrArgInt>("ovr_Message_GetType")(nullptr), 0);
  EXPECT_STREQ(Get<PtrArgStr>("ovr_Message_GetString")(nullptr), "");
  EXPECT_EQ(Get<PtrArgPtr>("ovr_Message_GetUser")(nullptr), nullptr);
  EXPECT_EQ(Get<PtrArgInt>("ovr_Error_GetCode")(nullptr), 0);
  EXPECT_EQ(Get<PtrArgInt>("ovr_Error_GetHttpCode")(nullptr), 0);
  EXPECT_NE(std::string(Get<PtrArgStr>("ovr_Error_GetMessage")(nullptr)).find("no error"), std::string::npos);
  Get<void (*)(void*)>("ovr_FreeMessage")(nullptr);  // a no-op, and it must not crash on null
}

TEST_F(LibOvrStub, IdFromStringWritesZeroAndToleratesNull) {
  ASSERT_NE(module_, nullptr);
  const auto fromString = Get<FromString>("ovrID_FromString");
  std::uint64_t id = kStubUserId;
  fromString(&id, "123");
  EXPECT_EQ(id, 0u);
  fromString(nullptr, nullptr);  // must not write through, or read, a null
}

}  // namespace
