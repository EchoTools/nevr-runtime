// #447: the plugin-side resource-override wrappers resolve their exports from the module the runtime
// is deployed as (BugSplat64.dll), falling back to the legacy dbgcore.dll, and say so when neither
// exports them. The module lookup is injected, so no real DLL has to be loaded.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "resource_registry.h"

namespace {

void FakeRegister(uint64_t, uint64_t, const void*, uint64_t, const char*) {}
void FakeDeregister(const void*, const void*) {}
void FakeReset() {}
void OtherRegister(uint64_t, uint64_t, const void*, uint64_t, const char*) {}

// A fake process: module name -> (export name -> function address). A module listed with an empty
// export table is "loaded but exports nothing".
using Exports = std::map<std::string, void*>;
using Modules = std::map<std::string, Exports>;

Exports FullExports(void (*registerFn)(uint64_t, uint64_t, const void*, uint64_t, const char*) = FakeRegister) {
    return {{"NEVR_RegisterResourceOverride", reinterpret_cast<void*>(registerFn)},
            {"NEVR_DeregisterResourceOverrides", reinterpret_cast<void*>(&FakeDeregister)},
            {"NEVR_ResetResourceOverrides", reinterpret_cast<void*>(&FakeReset)}};
}

nevr::detail::ResourceExports Resolve(const Modules& modules) {
    // HMODULE is an opaque handle: point it at the fake module's export table.
    return nevr::detail::ResolveResourceExportsFrom(
        [&](const char* name) -> HMODULE {
            const auto it = modules.find(name);
            return it == modules.end() ? nullptr : reinterpret_cast<HMODULE>(const_cast<Exports*>(&it->second));
        },
        [](HMODULE module, const char* name) -> void* {
            const auto& exports = *reinterpret_cast<const Exports*>(module);
            const auto it = exports.find(name);
            return it == exports.end() ? nullptr : it->second;
        });
}

TEST(ResourceRegistry, ResolvesFromBugSplat64WhenThatIsTheDeployedName) {
    const Modules modules = {{"BugSplat64.dll", FullExports()}};
    const auto found = Resolve(modules);
    EXPECT_EQ(found.registerFn, &FakeRegister);
    EXPECT_EQ(found.deregisterFn, &FakeDeregister);
    EXPECT_EQ(found.resetFn, &FakeReset);
}

TEST(ResourceRegistry, StillResolvesFromTheLegacyDbgcoreName) {
    const Modules modules = {{"dbgcore.dll", FullExports()}};
    const auto found = Resolve(modules);
    EXPECT_EQ(found.registerFn, &FakeRegister);
}

TEST(ResourceRegistry, BugSplat64WinsWhenBothAreLoaded) {
    const Modules modules = {{"dbgcore.dll", FullExports(&FakeRegister)},
                             {"BugSplat64.dll", FullExports(&OtherRegister)}};
    EXPECT_EQ(Resolve(modules).registerFn, &OtherRegister);
}

TEST(ResourceRegistry, ALoadedModuleWithoutTheExportsIsSkipped) {
    // The real dbgcore.dll proxy may be loaded alongside the runtime; it exports none of ours.
    const Modules modules = {{"BugSplat64.dll", Exports{}}, {"dbgcore.dll", FullExports()}};
    EXPECT_EQ(Resolve(modules).registerFn, &FakeRegister);
}

TEST(ResourceRegistry, NeitherModuleLoadedResolvesNothing) {
    const auto found = Resolve({});
    EXPECT_EQ(found.registerFn, nullptr);
    EXPECT_EQ(found.deregisterFn, nullptr);
    EXPECT_EQ(found.resetFn, nullptr);
}

TEST(ResourceRegistry, AModuleExportingOnlySomeOfThemIsNotUsed) {
    Exports partial = FullExports();
    partial.erase("NEVR_ResetResourceOverrides");
    const Modules modules = {{"BugSplat64.dll", partial}};
    const auto found = Resolve(modules);
    EXPECT_EQ(found.registerFn, nullptr);  // no half-resolved state
}

// With no runtime module in the process (this test executable), the real resolution writes one
// warning that names both modules, and the wrappers keep returning false.
TEST(ResourceRegistry, FailureWritesOneNamedWarningAndTheWrappersReportIt) {
    const char* temp = std::getenv("TEMP");
    const std::string path = std::string(temp != nullptr ? temp : ".") + "\\resource_registry_test.txt";
    ASSERT_NE(std::freopen(path.c_str(), "w", stderr), nullptr);
    nevr::detail::ResolveResourceExports();
    std::fflush(stderr);
    std::FILE* in = std::fopen(path.c_str(), "r");
    ASSERT_NE(in, nullptr);
    char buf[512] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, in);
    std::fclose(in);
    std::remove(path.c_str());
    const std::string line(buf, n);
    EXPECT_NE(line.find("[NEVR.PLUGIN] WARNING"), std::string::npos) << line;
    EXPECT_NE(line.find("BugSplat64.dll"), std::string::npos) << line;
    EXPECT_NE(line.find("dbgcore.dll"), std::string::npos) << line;
    EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1) << line;
    EXPECT_FALSE(nevr::RegisterResourceOverride(1, 2, nullptr, 0, "t"));
}

}  // namespace
