/* The content pack's hook on Quest (#2312, docs/reference/content-packs.md): the twin of the PC's CFile::Open detour
 * (runtime/content/content_pack_install.cpp).
 *
 * What it hooks (pinned_targets.h, LibR15CSysFileOpen): NRadEngine::CSysFile::Open(path, flags, long& handle), libr15 0xf85538,
 * through libr15's own GOT slot 0x36f6450. CPackageManifest::AddPackage opens every package through it and CFile::Open tail-calls
 * it for every manifest read (kCSysFileOpenContentSites). A GOT hook, because the sentinel writes no code page at run time (N151),
 * where the PC detours the function itself.
 *
 * When. Run is called once, from the data step (data_wiring.cpp, AfterStorage), after the bundled pack step and before the game's
 * first manifest read (CEngine::InitGlobals comes later on the same thread). It decides over `<media root>/nevr/content`
 * (quest/data/content_folder.h), and only for a pack that passed does it put the hook on. A pack is refused as loaded_too_late
 * when the game's resource manager already exists (kResourceManagerPtrVaddr), and as hook_failed when the slot does not take the
 * hook.
 *
 * The handler passes every open to the game's function unchanged except a read of a stock path the pack serves, which it hands
 * the pack's held file (/proc/self/fd/<n>) with the game's own flags and handle; the game's result is returned as it came. The
 * lines are written after the game's function returned: the decision and every served file are the PC's [NEVR.CONTENT] lines
 * (content_core.h), each one sentinel record {"event":"content","msg":"<the line>"}. The disk log writes its own `tag`
 * (NEVR-Sentinel), so the line's [NEVR.CONTENT] prefix is what names it.
 * Built with -fno-exceptions (callback_thunk.h).
 */
#pragma once

#include <cstdint>
#include <string>

#include "pinned_targets.h"

namespace nevr_quest::content_pack_hook {

using OpenThunk = sentinel::pinned::LibR15CSysFileOpenThunk;

// Decides over the installed pack for the engine root `engine_root` and, for a pack that passed, installs the hook. Logs the
// decision. A second call does nothing.
void Run(const std::string& engine_root) noexcept;

// Run's body over `content_dir`, without the once guard. Run calls it with <media root>/nevr/content.
void RunIn(const std::string& engine_root, const std::string& content_dir) noexcept;

// Whether the resource manager pointer of the libr15 mapped at `image_base` is set (kResourceManagerPtrVaddr).
bool ResourceManagerExists(std::uintptr_t image_base) noexcept;

// The package named by a /proc/<pid>/cmdline file at `path` (path_rewrite.h, PackageFromCmdline); "" when it does not read or
// names none. Run reads /proc/self/cmdline.
std::string PackageOfCmdlineFile(const char* path) noexcept;

// Test support: the handler's body, as the thunk calls it.
int HandleForTest(OpenThunk::Fn original, const char* path, unsigned flags, long* handle) noexcept;

}  // namespace nevr_quest::content_pack_hook
