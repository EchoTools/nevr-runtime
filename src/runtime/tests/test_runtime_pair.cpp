// Pair loading for the BugSplat64/nevr split (step 5): the real nevr.dll, built
// side by side with the stable host, loads and exposes a well-formed v1 runtime
// API (core/runtime_abi.h).
//
// This proves the pair is present and the ABI descriptor is valid WITHOUT running
// the full runtime initialization, which hooks a real echovr.exe and therefore
// belongs to the step-8 nested-client run. resolve + validate only: no
// api->initialize() call, so nevr.dll's global constructors run but no game code
// is touched.

#include "core/runtime_abi.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <string>

namespace {

std::wstring SiblingDir() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  EXPECT_GT(length, 0u);
  EXPECT_LT(length, static_cast<DWORD>(MAX_PATH));
  std::wstring full(path, length);
  const std::size_t separator = full.find_last_of(L"\\/");
  EXPECT_NE(separator, std::wstring::npos);
  return full.substr(0, separator + 1);
}

}  // namespace

TEST(RuntimePair, RealNevrDllExportsAValidV1Api) {
  const std::wstring runtime_path = SiblingDir() + L"nevr.dll";
  HMODULE runtime = LoadLibraryW(runtime_path.c_str());
  ASSERT_NE(runtime, nullptr)
      << "nevr.dll did not load from " << std::string(runtime_path.begin(), runtime_path.end());

  const auto get_api = reinterpret_cast<NevrGetRuntimeApiFn>(
      GetProcAddress(runtime, NevrAbi::kGetRuntimeApiExport));
  ASSERT_NE(get_api, nullptr) << "nevr.dll does not export NEVR_GetRuntimeApi";

  const NevrRuntimeApi* api = get_api();
  ASSERT_NE(api, nullptr);
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(api), NevrAbi::Status::kOk);
  EXPECT_EQ(api->header.abi_major, NevrAbi::kMajor);
  EXPECT_EQ(api->header.abi_minor, NevrAbi::kMinor);
  EXPECT_EQ(api->header.struct_size, sizeof(NevrRuntimeApi));
  EXPECT_NE(api->initialize, nullptr);
  EXPECT_NE(api->preprocess_before, nullptr);
  EXPECT_NE(api->preprocess_after, nullptr);

  // The runtime was never initialized, so unloading it is safe (no hooks, no
  // threads). This is a test-only action; production never frees a live runtime.
  FreeLibrary(runtime);
}
