// The libr15 functions the FRIENDS panel calls (#1535), at their addresses in the pinned patched libr15
// (sentinel::pinned::LibR15PatchedBuildId) with their first four instructions, read from the file. A caller checks the
// four words at the load address before the first call and calls nothing when one differs.
//
// The canvas handle (CComponentHandle<CCanvasUICS>, 0x50 bytes) is laid out as the game's own CR15DynLoadTextureBehaviorNode::Enter
// (0x1fa5bb8) initializes it on its stack, and its two fields after Find and RefreshCache are the ones Enter reads: +0x40
// the CCanvasUICS component, +0x48 the canvas actor's pool handle. Enter then asks ElementID(component, handle, element)
// for the element's primitive id, FindBehavior(component, behaviour type, &{handle, id, 0, type}) for the behaviour (the
// SUIBehaviorID is 24 bytes, passed by reference), and calls LoadAndApplyTexture(behaviour, texture). The behaviour type is
// the CSymbol64 Enter loads from 0x2ba17a0 (read-only data).
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest_friends::pins {

struct Pin {
  const char* name;
  std::uint64_t vaddr;
  std::uint32_t code[4];
};

// CComponentHandle<CCanvasUICS>::Find(CncaGameSpaceMT*, CSymbol64 actor, CSymbol64 component): `this` the handle.
inline constexpr Pin kFind{"CComponentHandle<CCanvasUICS>::Find", 0x16fe628, {0xd10103ff, 0xa90253f5, 0xa9037bf3, 0xaa0003f3}};
// CComponentHandle<CCanvasUICS>::RefreshCache() const.
inline constexpr Pin kRefreshCache{"CComponentHandle<CCanvasUICS>::RefreshCache", 0x12da698,
                                   {0xd101c3ff, 0xa90363f9, 0xa9045bf7, 0xa90553f5}};
// CCanvasUICS::SetText(SCompactPoolHandle, CSymbol64, char const*): sentinel::pinned::kCanvasSetTextVaddr.
inline constexpr Pin kSetText{"CCanvasUICS::SetText", 0x139111c, {0xb4000243, 0xd10083ff, 0xa9017bf3, 0xaa0303f3}};
// CCanvasUICS::ElementID(SCompactPoolHandle, CSymbol64) const: the primitive id, ~0 for none (SUIPrimitiveID::Valid).
inline constexpr Pin kElementId{"CCanvasUICS::ElementID", 0x13931b0, {0xf81f0ffe, 0x92407c21, 0x910023e3, 0x97ede4c5}};
// CCanvasUICS::FindBehavior(CSymbol64 type, SUIBehaviorID).
inline constexpr Pin kFindBehavior{"CCanvasUICS::FindBehavior", 0x138ff74, {0xd100c3ff, 0xf90013fe, 0xf9400848, 0x3dc00040}};
// CR15DynLoadTextureBehavior::LoadAndApplyTexture(CSymbol64 texture).
inline constexpr Pin kLoadAndApplyTexture{"CR15DynLoadTextureBehavior::LoadAndApplyTexture", 0x20f31c8,
                                          {0xd10143ff, 0xa90207f6, 0xa90353f5, 0xa9047bf3}};
inline constexpr Pin kAll[] = {kFind, kRefreshCache, kSetText, kElementId, kFindBehavior, kLoadAndApplyTexture};

// The CR15DynLoadTextureBehavior type symbol Enter passes to FindBehavior, and its value in the pinned file.
inline constexpr std::uint64_t kBehaviourTypeVaddr = 0x2ba17a0;
inline constexpr std::uint64_t kBehaviourType = 0xd50027525038ec0aULL;

// SYM_CANVASUI: the canvas component of an actor (the PC's ui_text.cpp kSymCanvasUi).
inline constexpr std::uint64_t kSymCanvasUi = 0x35EC9A0128EC916FULL;

inline constexpr const char* kLibR15 = "libr15.so";  // sentinel::pinned::kLibR15

// The game space the menu's actors are found from, the one QuitOnError sends its events to: *(*(netgame + 8) + 0x7af0)
// (sentinel::pinned::game_layout::kGameOffset and kGameSpaceOffset; pinned_targets.h is for -fno-exceptions units only).
inline constexpr std::size_t kGameOffset = 8;
inline constexpr std::size_t kGameSpaceOffset = 0x7af0;

inline constexpr std::size_t kHandleBytes = 0x50;
inline constexpr std::size_t kHandleComponent = 0x40;
inline constexpr std::size_t kHandlePoolHandle = 0x48;

struct BehaviourId {
  std::uint32_t handle;
  std::uint32_t primitive;
  std::uint64_t zero;
  std::uint64_t type;
};
static_assert(sizeof(BehaviourId) == 24, "SUIBehaviorID is 24 bytes");

using FindFn = void (*)(void* handle, void* gameSpace, std::uint64_t actor, std::uint64_t component);
using RefreshCacheFn = void (*)(void* handle);
using SetTextFn = std::uint64_t (*)(void* canvas, std::uint32_t handle, std::uint64_t element, const char* text);
using ElementIdFn = std::uint32_t (*)(void* canvas, std::uint32_t handle, std::uint64_t element);
using FindBehaviorFn = void* (*)(void* canvas, std::uint64_t type, const BehaviourId* id);
using LoadAndApplyTextureFn = void (*)(void* behaviour, std::uint64_t texture);

}  // namespace nevr_quest_friends::pins
