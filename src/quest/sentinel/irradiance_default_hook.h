/* The default irradiance texture is grey, not black (#2091, #1737 unit U25).
 *
 * Why. Converted combat maps have areas outside every irradiance volume, and they come out black with the stock engine. The
 * reference combat build uploads the "Default 3D Texture" with the texel 0xff6b6b6b (R, G and B 0x6b, A 0xff; grey, about 0.42)
 * where CGEngineResources::Initialize uploads one zeroed texel, and its maps are lit grey there (#1737 plan; #2091). That the
 * engine reads this texture for those areas is the reference build's observed behaviour, not something this file re-derives from
 * the shaders. This module does the same upload with two hooks, and changes nothing else.
 *
 * What the game does (store libr15, sha-256 8dd9a961...; each address is the disassembly's, the file vaddr plus 0x100000):
 *   CGEngineResources::Initialize (0x193e52c) calls the image upload helper FUN_019daf90 once per default texture. For the
 *   "Default 3D Texture" it passes x0 = this + 0x1458 (the SGTexture), x2 = 9 (a four-byte texel format), x5 = sp + 0x45c (the
 *   texel, zeroed at 0x193efd0) and x6 = the name (the call is at 0x193f2d0, its arguments set from 0x193f28c). The helper copies the texel into its staging buffer
 *   (CMemory::Copy of one texel of format size) before it returns, so a pointer that is valid for the call is enough.
 *
 * Two hooks.
 *   1. A GOT hook on CGEngineResources::Initialize's slot (JUMP_SLOT 0x36fba08; libr15 reaches the function only through its PLT
 *      stub). It records `this` and marks the call in progress, calls the original, clears the mark, and logs one line.
 *   2. The row `irradiance_default.upload` of the build-time patch table (docs/adr/0021): an inline row at the helper's entry,
 *      whose handler replaces the texel pointer with this module's grey texel when, and only when, a call is in progress, the
 *      texture is `this + 0x1458`, the format is 9 and the texel is not null. Every other call of the helper (it has 25 `bl`
 *      sites, in Initialize and in the Create* helpers) is passed to the original untouched. No return-address test, one
 *      trampoline, initialization only.
 *
 * Owner (docs/adr/0039). This file owns the default 3D texture's texel. Its input is nothing: the texel is a constant, not a
 * setting. The substitution is one atomic load and a compare on the other 24 call sites.
 *
 * Log lines (JSON, event "irradiance_default"), none on the helper's call path except the one written after Initialize returns:
 *   {"event":"irradiance_default","status":"installed",...}              the install, once
 *   {"event":"irradiance_default","status":"substituted",...}           Info, once: Initialize uploaded the grey texel
 *   {"event":"irradiance_default","status":"not_reached",...}           Warn, once: Initialize returned without substituting
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"

namespace nevr_quest::irradiance_default {

// CGEngineResources + 0x1458: the "Default 3D Texture" SGTexture (pinned_targets.h kDefault3dTextureOffset).
inline constexpr std::size_t kDefault3dTextureOffset = 0x1458;
// The upload helper's format argument for the default 3D texture: a four-byte texel (the size table at 0x2c658b8 holds 4).
inline constexpr std::uint32_t kTexelFormat = 9;
// The texel as the reference build uploads it: 0xff6b6b6b as a little-endian word, the bytes 6b 6b 6b ff.
inline constexpr std::uint32_t kGreyTexel = 0xff6b6b6bU;

// int CGEngineResources::Initialize(): `this` in x0, the status in w0 (0 when every default resource was made).
struct InitializeTag {};
using InitializeSig = std::int32_t(void* resources);
using InitializeThunk = sentinel::CallbackThunk<InitializeTag, InitializeSig>;

// The image upload helper (disassembly FUN_019daf90): texture (x0, an SGTexture*), the image description (x1), the format (w2),
// two ints (w3, w4) the handler does not read, the texel data (x5) and the debug name (x6). It returns nothing.
struct UploadTag {};
using UploadSig = void(void* texture, const void* image, std::uint32_t format, std::int32_t arg3, std::int32_t arg4,
                       const void* texel, const char* name);
using UploadThunk = sentinel::CallbackThunk<UploadTag, UploadSig>;

// ---- irradiance_default_hook.cpp: the handlers and the GOT hook ---------------------------------------------------------

// Arms the Initialize handler and installs it on `initialize`'s slot through GotHook. Logs nothing itself; the status is the
// GotHook's. Called by InstallAt.
sentinel::GotStatus InstallInitializeHook(const sentinel::GotTarget& initialize, sentinel::ImageLookup lookup) noexcept;

// Arms the upload handler (the row's detour). Called by InstallUploadRow before the row is activated.
void ArmUploadHandler() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Arms the Initialize handler alone (the install path of the row arms the upload handler itself), or both handlers, without touching a
// GOT slot or a row.
void ArmInitializeForTest() noexcept;
void ArmForTest() noexcept;
// Forgets the in-progress mark, the counts and the logged flags.
void ResetForTest() noexcept;

}  // namespace nevr_quest::irradiance_default
