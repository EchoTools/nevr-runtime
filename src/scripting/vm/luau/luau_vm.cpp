// Luau binding for the script VM seam (src/scripting/script_vm.h, #440).
//
// Error handling: the VM is built with LUA_USE_LONGJMP=1, so a Lua error is a
// longjmp, never a C++ exception. Every function below that can raise an error
// (anything that calls a lua_* or luaL_* function that allocates) therefore
// keeps no object with a destructor alive across such a call. All work that
// touches the VM from C++ runs inside lua_cpcall (setup) or lua_pcall (each
// callback, called directly), so an error can never reach the VM's
// unprotected-error path.
//
// Limits (script_vm.h): one lua_State per owner, with
//   - a safepoint and wall-clock budget checked from lua_callbacks()->interrupt,
//   - a byte cap enforced by the lua_Alloc handed to lua_newstate.
// A breach is sticky: it is recorded on the state and the interrupt raises an
// error at every later safepoint, so a script's pcall cannot swallow it.
//
// Two modes, chosen at configure time (NEVR_LUAU_SHARED_STATE, vm/luau/CMakeLists.txt):
//   0 (default)  one lua_State per script, as above.
//   1            one lua_State for the whole VM. Each script runs in its own
//                luaL_sandboxthread thread and its allocations are charged to its own
//                Luau memory category (lua_setmemcat); the cap applies to that category.
//                One VM-wide mutex serializes all script code. See SharedLuauVm.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "scripting/memory_policy.h"
#include "scripting/script_vm.h"

#ifndef NEVR_LUAU_SHARED_STATE
#define NEVR_LUAU_SHARED_STATE 0
#endif

namespace nevr_script {
namespace {

constexpr int kCallTag = 1;  // userdata tag of the call object `h`
constexpr int64_t kExactIntLimit = int64_t{1} << 53;  // a double holds every integer below this

#if NEVR_LUAU_SHARED_STATE
constexpr int kAnchorThread = 1;     // anchor table slots: the script's chunk thread,
constexpr int kAnchorRunThread = 2;  // the thread its callbacks run on,
constexpr int kAnchorCall = 3;       // the call object `h`
constexpr int kAnchorFirstCallback = 4;
constexpr int kMaxScripts = LUA_MEMORY_CATEGORIES - 1;  // categories 1..255; 0 is the shared part
#endif

struct State;

// One registered Lua callback; the pointer is the `user` the registry hands back.
struct CallbackRef {
  State* state = nullptr;
  int ref = LUA_NOREF;  // registry ref (one state per script) or slot in the script's anchor table (shared state)
  NevrHookPhase phase = NEVR_HOOK_PRE;
};

struct State {
  Registry* registry = nullptr;
  const NevrHostApi* api = nullptr;
  NevrOwner* owner = nullptr;
  VmLimits limits;
  lua_State* L = nullptr;    // the state: the script's own, or the VM's shared one
  lua_State* run = nullptr;  // the thread callbacks run on

  // Memory. One state per script: bytes the allocator has handed out and not got
  // back. Shared state: the script's memory category (UsedBytes), set by lua_setmemcat.
  std::atomic<size_t> used{0};
  std::atomic<bool> mem_failed{false};
  bool memory_breach = false;  // `breach` was set because an allocation hit the cap
  std::atomic<size_t> largest_refused{0};  // the biggest request CappedAlloc refused since the last check
  int garbage_refusals = 0;  // calls in a row that hit the cap with mostly garbage
  size_t live_after_gc = 0;  // bytes held right after the last full collection

  // Budget for the running top-level chunk or callback.
  bool active = false;
  uint64_t safepoints = 0;
  std::chrono::steady_clock::time_point started;
  std::string breach;  // non-empty once a limit was crossed; stays set

  // The Lua state is single threaded; hook points may fire on any game thread.
  // Shared state: the VM-wide mutex, which all scripts' code runs under.
  std::mutex own_mu;
  std::mutex* mu = &own_mu;

  // The running callback.
  NevrHookCall* call = nullptr;
  NevrHookPhase phase = NEVR_HOOK_PRE;
  bool skip = false;
  int call_object_ref = LUA_NOREF;
  int thread_ref = LUA_NOREF;  // the script's sandboxed thread, kept alive

#if NEVR_LUAU_SHARED_STATE
  // The registry ref of the script's anchor table, which lives in the script's
  // category and holds what must stay alive: its threads and call object (kAnchor*)
  // and its callbacks (from kAnchorFirstCallback).
  int anchor_ref = LUA_NOREF;
  int next_slot = kAnchorFirstCallback;
  int category = 0;   // 1..255; 0 is the shared part
#endif

  std::vector<std::unique_ptr<CallbackRef>> callbacks;

#if !NEVR_LUAU_SHARED_STATE
  ~State() {
    if (L) lua_close(L);
  }
#endif

  void BeginBudget() {
    active = true;
    safepoints = 0;
    started = std::chrono::steady_clock::now();
  }
  void EndBudget() { active = false; }

  // Called from the interrupt at every safepoint. Records the first breach.
  void CheckBudget() {
    if (!active || !breach.empty()) return;
    if (mem_failed.load(std::memory_order_relaxed)) {
      breach = "memory limit exceeded: the script's state may not hold more than " +
               std::to_string(limits.memory_bytes) + " bytes";
      memory_breach = true;
      return;
    }
    if (++safepoints > limits.instructions_per_call) {
      breach = "instruction budget exceeded: more than " + std::to_string(limits.instructions_per_call) +
               " safepoints (loop iterations and calls) in one call";
      return;
    }
    // Reading the clock costs more than the counter; look every 64 safepoints.
    if ((safepoints & 63u) == 0) {
      const auto elapsed = std::chrono::steady_clock::now() - started;
      if (elapsed > std::chrono::milliseconds(limits.millis_per_call)) {
        breach = "time budget exceeded: more than " + std::to_string(limits.millis_per_call) +
                 " ms in one call";
      }
    }
  }
};

#if NEVR_LUAU_SHARED_STATE

// What every thread of the shared state reaches through lua_callbacks()->userdata.
// `running` is the script whose code runs now; it is only written under `mu`, and
// the allocator, the interrupt and the nevr.* functions read it on the thread that
// holds `mu`.
struct SharedHost {
  lua_State* main = nullptr;
  State* running = nullptr;
  std::mutex mu;
  std::atomic<size_t> allocated{0};  // bytes the allocator holds for the whole state
};

State* StateOf(lua_State* L) { return static_cast<SharedHost*>(lua_callbacks(L)->userdata)->running; }

// Bytes the script's own category holds, garbage included.
size_t UsedBytes(const State* s) { return lua_totalbytes(s->L, s->category); }
#else
State* StateOf(lua_State* L) { return static_cast<State*>(lua_callbacks(L)->userdata); }

size_t UsedBytes(const State* s) { return s->used.load(std::memory_order_relaxed); }
#endif

// ---- allocator: the memory cap -------------------------------------------------------------

#if NEVR_LUAU_SHARED_STATE
// One allocator for the whole state. It cannot see which category a block is
// charged to (lua_Alloc has no such argument), so the cap is checked against the
// category of the script that is running: every growth while a script runs is that
// script's, except what the VM itself keeps in category 0 (the string table, the
// registry), which can only be a few bytes per script. Luau updates
// memcatbytes after the allocator returns (lmem.cpp luaM_new_, luaM_realloc_), so
// the bytes the category holds now are what the request is added to.
void* SharedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  SharedHost* h = static_cast<SharedHost*>(ud);
  State* s = h->running;
  const size_t held = ptr ? osize : 0;
  if (nsize == 0) {
    std::free(ptr);
    h->allocated.fetch_sub(held, std::memory_order_relaxed);
    return nullptr;
  }
  if (s && h->main && nsize > held && UsedBytes(s) + (nsize - held) > s->limits.memory_bytes) {
    s->mem_failed.store(true, std::memory_order_relaxed);
    if (nsize > s->largest_refused.load(std::memory_order_relaxed)) {
      s->largest_refused.store(nsize, std::memory_order_relaxed);
    }
    return nullptr;
  }
  void* block = std::realloc(ptr, nsize);
  if (!block) {
    if (s) s->mem_failed.store(true, std::memory_order_relaxed);
    return nullptr;
  }
  h->allocated.fetch_add(nsize, std::memory_order_relaxed);
  h->allocated.fetch_sub(held, std::memory_order_relaxed);
  return block;
}
#else
void* CappedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  State* s = static_cast<State*>(ud);
  const size_t held = ptr ? osize : 0;
  const size_t used = s->used.load(std::memory_order_relaxed);
  if (nsize == 0) {
    std::free(ptr);
    s->used.store(used - held, std::memory_order_relaxed);
    return nullptr;
  }
  if (nsize > held && used - held + nsize > s->limits.memory_bytes) {
    s->mem_failed.store(true, std::memory_order_relaxed);
    if (nsize > s->largest_refused.load(std::memory_order_relaxed)) {
      s->largest_refused.store(nsize, std::memory_order_relaxed);
    }
    return nullptr;
  }
  void* block = std::realloc(ptr, nsize);
  if (!block) {
    s->mem_failed.store(true, std::memory_order_relaxed);
    return nullptr;
  }
  s->used.store(used - held + nsize, std::memory_order_relaxed);
  return block;
}
#endif

// ---- interrupt: the instruction and time budget --------------------------------------------

void Interrupt(lua_State* L, int gc) {
  if (gc >= 0) return;  // a GC step, not a safepoint of the running code
  State* s = StateOf(L);
  if (!s) return;
  // Keep garbage off the cap within the call: past half the cap, once a quarter
  // of the cap has been allocated since the last collection, collect here. The
  // VM runs its own GC steps inside the same VM_PROTECT that wraps this call
  // (lvmexecute.cpp VM_INTERRUPT, VM_PROTECT(luaC_checkGC(L))), and upstream's REPL
  // runs a full collection from running code (CLI/src/Repl.cpp lua_collectgarbage).
  // A large live set with no churn never pays for it, and its cost counts
  // against this call's time budget.
  const size_t cap = s->limits.memory_bytes;
  const size_t used = UsedBytes(s);
  if (s->active && used > cap / 2 && used - std::min(used, s->live_after_gc) >= cap / 4) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    s->live_after_gc = UsedBytes(s);
  }
  s->CheckBudget();
  if (!s->breach.empty()) luaL_error(L, "%s", s->breach.c_str());
}

void Panic(lua_State* L, int) {
  State* s = StateOf(L);
  if (!s) return;
  s->api->log(s->owner, NEVR_LOG_ERROR, "Luau raised an error outside any protected call; aborting");
}

// ---- script API: nevr.* --------------------------------------------------------------------

int PushFailure(lua_State* L, State* s, NevrStatus status) {
  lua_pushnil(L);
  lua_pushfstring(L, "%s: %s", s->api->status_name(status), s->api->last_error(s->owner));
  return 2;
}

int NevrOverride(lua_State* L) {
  State* s = StateOf(L);
  const char* key = luaL_checkstring(L, 1);
  NevrValue v{};
  switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
      v.type = NEVR_VALUE_BOOL;
      v.as.b = lua_toboolean(L, 2);
      break;
    case LUA_TNUMBER:
      v.type = NEVR_VALUE_FLOAT;
      v.as.f = lua_tonumber(L, 2);
      if (!std::isfinite(v.as.f)) luaL_error(L, "NEVR_ERR_INVALID_ARG: nevr.override('%s'): the value is not a finite number", key);
      break;
    case LUA_TSTRING:
      v.type = NEVR_VALUE_STRING;
      v.as.s = lua_tostring(L, 2);
      break;
    default:
      luaL_typeerror(L, 2, "boolean, number or string");
  }
  const NevrStatus status = s->api->override_set(s->owner, key, &v);
  if (status != NEVR_OK) return PushFailure(L, s, status);
  lua_pushboolean(L, 1);
  return 1;
}

CallbackRef* NewCallbackRef(State* s, NevrHookPhase phase) {
  s->callbacks.push_back(std::make_unique<CallbackRef>());
  CallbackRef* ref = s->callbacks.back().get();
  ref->state = s;
  ref->phase = phase;
  return ref;
}

NevrHookResult HookTrampoline(NevrHookCall* call, void* user);

// Keeps the function on top of the stack alive for the script's lifetime and
// returns the handle PushCallback and ReleaseCallback take. One state per script:
// a registry ref. Shared state: a slot of the script's anchor table, so the
// function and the table's growth are charged to the script's category (the
// registry belongs to the shared part). Can raise (allocation).
int AnchorCallback(lua_State* L, State* s) {
#if NEVR_LUAU_SHARED_STATE
  const int slot = s->next_slot++;
  lua_getref(L, s->anchor_ref);
  lua_pushvalue(L, -2);
  lua_rawseti(L, -2, slot);
  lua_pop(L, 1);
  return slot;
#else
  (void)s;
  return lua_ref(L, -1);
#endif
}

void ReleaseCallback(lua_State* L, State* s, int handle) {
#if NEVR_LUAU_SHARED_STATE
  lua_getref(L, s->anchor_ref);
  lua_pushnil(L);
  lua_rawseti(L, -2, handle);
  lua_pop(L, 1);
#else
  (void)s;
  lua_unref(L, handle);
#endif
}

int NevrHook(lua_State* L) {
  State* s = StateOf(L);
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  static const char* const kKeys[2] = {"pre", "post"};
  static const NevrHookPhase kPhases[2] = {NEVR_HOOK_PRE, NEVR_HOOK_POST};
  int wanted = 0;
  for (int i = 0; i < 2; ++i) {
    lua_getfield(L, 2, kKeys[i]);
    const int t = lua_type(L, -1);
    if (t != LUA_TNIL && t != LUA_TFUNCTION) luaL_argerror(L, 2, "pre and post must be functions");
    wanted += t == LUA_TFUNCTION;
    lua_pop(L, 1);
  }
  if (wanted == 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "NEVR_ERR_INVALID_ARG: nevr.hook needs a pre or a post function");
    return 2;
  }
  for (int i = 0; i < 2; ++i) {
    lua_getfield(L, 2, kKeys[i]);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
      lua_pop(L, 1);
      continue;
    }
    CallbackRef* ref = NewCallbackRef(s, kPhases[i]);
    ref->ref = AnchorCallback(L, s);
    lua_pop(L, 1);
    const NevrStatus status = s->api->hook_add(s->owner, name, kPhases[i], HookTrampoline, ref);
    if (status != NEVR_OK) {
      ReleaseCallback(L, s, ref->ref);
      ref->ref = LUA_NOREF;
      return PushFailure(L, s, status);
    }
  }
  lua_pushboolean(L, 1);
  return 1;
}

int NevrLog(lua_State* L) {
  State* s = StateOf(L);
  const char* level = luaL_checkstring(L, 1);
  const char* message = luaL_checkstring(L, 2);
  NevrLogLevel lv = NEVR_LOG_INFO;
  if (std::strcmp(level, "debug") == 0) lv = NEVR_LOG_DEBUG;
  else if (std::strcmp(level, "info") == 0) lv = NEVR_LOG_INFO;
  else if (std::strcmp(level, "warn") == 0) lv = NEVR_LOG_WARNING;
  else if (std::strcmp(level, "error") == 0) lv = NEVR_LOG_ERROR;
  else luaL_argerror(L, 1, "level must be debug, info, warn or error");
  s->api->log(s->owner, lv, message);
  return 0;
}

int ScriptPrint(lua_State* L) {
  State* s = StateOf(L);
  luaL_Strbuf buf;
  luaL_buffinit(L, &buf);
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) luaL_addlstring(&buf, "\t", 1);
    luaL_tolstring(L, i, nullptr);
    luaL_addvalue(&buf);
  }
  luaL_pushresult(&buf);
  s->api->log(s->owner, NEVR_LOG_INFO, lua_tostring(L, -1));
  return 0;
}

int CollectGarbage(lua_State* L) {
  const char* what = luaL_optstring(L, 1, "collect");
  if (std::strcmp(what, "count") != 0) luaL_error(L, "collectgarbage: only \"count\" is available");
  lua_pushnumber(L, lua_gc(L, LUA_GCCOUNT, 0) + lua_gc(L, LUA_GCCOUNTB, 0) / 1024.0);
  return 1;
}

// ---- script API: the call object h ---------------------------------------------------------

NevrHookCall* RequireCall(lua_State* L, State* s) {
  luaL_checkudatatagged(L, 1, kCallTag);
  if (!s->call) luaL_error(L, "the call object is only valid inside its callback");
  return s->call;
}

int CallGet(lua_State* L) {
  State* s = StateOf(L);
  NevrHookCall* call = RequireCall(L, s);
  const char* field = luaL_checkstring(L, 2);
  NevrValue v{};
  const NevrStatus status = s->api->call_get(call, field, &v);
  if (status != NEVR_OK) {
    luaL_error(L, "%s: h:get('%s') on hook '%s'", s->api->status_name(status), field,
               s->api->call_hook_name(call));
  }
  switch (v.type) {
    case NEVR_VALUE_BOOL: lua_pushboolean(L, v.as.b != 0); break;
    case NEVR_VALUE_INT:
      // A script number is a double: past ±2^53 it would hand the script a
      // nearby integer, and a get/set round trip would change the field.
      if (v.as.i <= -kExactIntLimit || v.as.i >= kExactIntLimit) {
        luaL_error(L, "NEVR_ERR_TYPE_MISMATCH: h:get('%s') on hook '%s': %lld does not fit a script number exactly",
                   field, s->api->call_hook_name(call), static_cast<long long>(v.as.i));
      }
      lua_pushnumber(L, static_cast<double>(v.as.i));
      break;
    case NEVR_VALUE_FLOAT: lua_pushnumber(L, v.as.f); break;
    case NEVR_VALUE_STRING: lua_pushstring(L, v.as.s); break;
    default: lua_pushnil(L); break;
  }
  return 1;
}

int CallSet(lua_State* L) {
  State* s = StateOf(L);
  NevrHookCall* call = RequireCall(L, s);
  const char* field = luaL_checkstring(L, 2);
  NevrValue v{};
  switch (lua_type(L, 3)) {
    case LUA_TBOOLEAN:
      v.type = NEVR_VALUE_BOOL;
      v.as.b = lua_toboolean(L, 3);
      break;
    case LUA_TNUMBER:
      // The registry converts to the field's type and refuses a non-integral
      // number, or one past ±2^53, for an INT field (host_registry.cpp CoerceTo).
      v.type = NEVR_VALUE_FLOAT;
      v.as.f = lua_tonumber(L, 3);
      // No NaN or infinity into a game field (Spritz, on #458).
      if (!std::isfinite(v.as.f)) {
        luaL_error(L, "NEVR_ERR_INVALID_ARG: h:set('%s') on hook '%s': the value is not a finite number", field,
                   s->api->call_hook_name(call));
      }
      break;
    case LUA_TSTRING:
      v.type = NEVR_VALUE_STRING;
      v.as.s = lua_tostring(L, 3);
      break;
    default:
      luaL_typeerror(L, 3, "boolean, number or string");
  }
  const NevrStatus status = s->api->call_set(call, field, &v);
  if (status != NEVR_OK) {
    luaL_error(L, "%s: h:set('%s') on hook '%s' (%s callback)", s->api->status_name(status), field,
               s->api->call_hook_name(call), s->phase == NEVR_HOOK_PRE ? "pre" : "post");
  }
  return 0;
}

int CallSkip(lua_State* L) {
  State* s = StateOf(L);
  RequireCall(L, s);
  if (s->phase != NEVR_HOOK_PRE) luaL_error(L, "h:skip() is only valid in a pre callback");
  s->skip = true;
  return 0;
}

// ---- state setup ---------------------------------------------------------------------------

void OpenLib(lua_State* L, const char* name, lua_CFunction open) {
  lua_pushcfunction(L, open, nullptr);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

void DropGlobal(lua_State* L, const char* name) {
  lua_pushnil(L);
  lua_setglobal(L, name);
}

// The argument the binding is about to hand a lua_cpcall thunk. A thunk refuses
// any other: once on a stack, a C function can be reached and called by script
// code with arguments of its choosing (the verification of #458 crashed the host
// this way through debug.info). Scripts no longer see debug, and callbacks are
// called without a thunk; this keeps the remaining two harmless regardless.
thread_local const void* t_thunk_arg = nullptr;

template <class T>
T* ThunkArg(lua_State* L) {
  if (lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_tolightuserdata(L, 1) != t_thunk_arg || !t_thunk_arg) {
    luaL_error(L, "this function is internal to the host and cannot be called from a script");
  }
  return static_cast<T*>(lua_tolightuserdata(L, 1));
}

int ProtectedCall(lua_State* L, lua_CFunction thunk, void* arg) {
  t_thunk_arg = arg;
  const int status = lua_cpcall(L, thunk, arg);
  t_thunk_arg = nullptr;
  return status;
}

// Builds the sandboxed environment. Runs inside lua_cpcall; `ud` is the State.
int SetupThunk(lua_State* L) {
#if NEVR_LUAU_SHARED_STATE
  ThunkArg<SharedHost>(L);  // the shared host; each script's call object is made in ScriptThreadThunk
#else
  State* s = ThunkArg<State>(L);
#endif

  // Whitelist: only the libraries script_vm.h names. luaL_openlibs would also
  // open bit32, buffer, vector and integer, which the contract does not offer.
  OpenLib(L, "", luaopen_base);
  OpenLib(L, LUA_COLIBNAME, luaopen_coroutine);
  OpenLib(L, LUA_TABLIBNAME, luaopen_table);
  OpenLib(L, LUA_OSLIBNAME, luaopen_os);
  OpenLib(L, LUA_STRLIBNAME, luaopen_string);
  OpenLib(L, LUA_MATHLIBNAME, luaopen_math);
  OpenLib(L, LUA_UTF8LIBNAME, luaopen_utf8);
  // No debug library (Spritz, on #458): introspection reached host internals,
  // and errors already carry chunk:line. os keeps clock, date and time only.
  lua_getglobal(L, LUA_OSLIBNAME);
  lua_pushnil(L);
  lua_setfield(L, -2, "difftime");
  lua_pop(L, 1);

  // What Luau's base library still exposes beyond script_vm.h's list.
  DropGlobal(L, "getfenv");
  DropGlobal(L, "setfenv");
  DropGlobal(L, "newproxy");
  DropGlobal(L, "gcinfo");

  lua_getglobal(L, "table");
  lua_getfield(L, -1, "unpack");
  lua_setglobal(L, "unpack");  // Lua 5.1 spelling, as the contract lists `unpack`
  lua_pop(L, 1);

  lua_pushcfunction(L, CollectGarbage, "collectgarbage");
  lua_setglobal(L, "collectgarbage");
  lua_pushcfunction(L, ScriptPrint, "print");
  lua_setglobal(L, "print");

  // getmetatable('') must not hand out the string metatable.
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1)) {
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  // nevr.*
  lua_newtable(L);
  lua_pushcfunction(L, NevrOverride, "nevr.override");
  lua_setfield(L, -2, "override");
  lua_pushcfunction(L, NevrHook, "nevr.hook");
  lua_setfield(L, -2, "hook");
  lua_pushcfunction(L, NevrLog, "nevr.log");
  lua_setfield(L, -2, "log");
  lua_setreadonly(L, -1, 1);
  lua_setglobal(L, "nevr");

  // The call object h: one userdata per script, valid only while its callback runs.
  lua_newtable(L);  // metatable
  lua_newtable(L);  // methods
  lua_pushcfunction(L, CallGet, "h:get");
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, CallSet, "h:set");
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, CallSkip, "h:skip");
  lua_setfield(L, -2, "skip");
  lua_setreadonly(L, -1, 1);
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "nevr.call");
  lua_setfield(L, -2, "__type");
  lua_pushliteral(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_setreadonly(L, -1, 1);
  lua_setuserdatametatable(L, kCallTag);
#if !NEVR_LUAU_SHARED_STATE
  lua_newuserdatataggedwithmetatable(L, sizeof(void*), kCallTag);
  s->call_object_ref = lua_ref(L, -1);
  lua_pop(L, 1);
#endif

  // Upstream's sandbox, last: every library table and the globals table become
  // read-only and the globals get `safeenv` (linit.cpp luaL_sandbox). The script
  // itself runs in a thread made by luaL_sandboxthread (see ScriptThreadThunk).
  luaL_sandbox(L);
  return 0;
}

struct ScriptThread {
  State* state;
  const char* chunkname;  // "=mod.lua": '=' makes Luau use the rest verbatim
  const char* bytecode;
  size_t size;
  lua_State* thread = nullptr;
  int load_status = 0;  // luau_load's result: 0 loaded, else the message is on `thread`
};

// Makes the script's own thread with its own global table (luaL_sandboxthread:
// a fresh globals table whose __index is the sandboxed main globals, so the
// script's global writes land in its own table), anchors it in the registry and
// loads the compiled chunk into it. Runs inside lua_cpcall.
//
// Shared state: the caller has set the state's active category to the script's
// (lua_setmemcat), and a thread takes the category of the thread that creates it
// (lstate.cpp luaE_newthread: luaC_init and `L1->activememcat = L->activememcat`),
// so the anchor table, both threads, the call object, the globals table
// luaL_sandboxthread makes and the loaded function are all charged to the script.
// The chunk runs on `T`; callbacks run on a second thread of the script, because
// a chunk that yielded at top level leaves `T` suspended.
int ScriptThreadThunk(lua_State* L) {
  ScriptThread* c = ThunkArg<ScriptThread>(L);
#if NEVR_LUAU_SHARED_STATE
  State* s = c->state;
  lua_newtable(L);  // index 2: the anchor
  s->anchor_ref = lua_ref(L, 2);
  lua_State* T = lua_newthread(L);
  lua_rawseti(L, 2, kAnchorThread);
  s->run = lua_newthread(L);
  lua_rawseti(L, 2, kAnchorRunThread);
  lua_newuserdatataggedwithmetatable(L, sizeof(void*), kCallTag);
  lua_rawseti(L, 2, kAnchorCall);
#else
  lua_State* T = lua_newthread(L);
  c->state->thread_ref = lua_ref(L, -1);
  lua_pop(L, 1);
#endif
  luaL_sandboxthread(T);
  c->thread = T;
  c->load_status = luau_load(T, c->chunkname, c->bytecode, c->size, 0);
  return 0;
}

// Pops the error object that lua_cpcall left and returns it as text.
std::string PopError(lua_State* L) {
  std::string text;
  if (lua_type(L, -1) == LUA_TSTRING) {
    text = lua_tostring(L, -1);
  } else {
    text = std::string("(error object is a ") + luaL_typename(L, -1) + " value)";
  }
  lua_pop(L, 1);
  return text;
}

// After a run, outside the VM: a crossed limit disables the owner, whatever the
// script did with the error.
//
// The cap counts what the allocator has handed out, garbage included, and Luau
// has no emergency collection inside the allocator. So when an allocation hit
// the cap, collect first and judge it with JudgeCapHit (memory_policy.h): the
// call merely failed when the live set and every refused request are within
// half the cap and the cap has not been reached on kCapHitsInARowLimit calls in
// a row; otherwise the owner is disabled. The collection here is not timed
// against the call's budget, which is why repeats count. The collector is tuned
// and assisted within the call (Load, Interrupt), so a script that is not trying
// should not get here at all.
void ApplyBreach(State* s) {
  const size_t cap = s->limits.memory_bytes;
  if (!s->mem_failed.load()) s->garbage_refusals = 0;
  if (s->mem_failed.load() && (s->breach.empty() || s->memory_breach)) {
    lua_gc(s->L, LUA_GCCOLLECT, 0);
    const size_t live = UsedBytes(s);
    s->live_after_gc = live;
    const size_t refused = s->largest_refused.exchange(0);
    const int hits = ++s->garbage_refusals;
    const CapVerdict verdict = JudgeCapHit(CapHit{live, refused, cap, hits});
    if (verdict == CapVerdict::kGarbage) {
      s->mem_failed.store(false);
      s->breach.clear();
      s->memory_breach = false;
      const std::string note = "an allocation reached the memory cap of " + std::to_string(cap) +
                               " bytes while most of it was garbage (" + std::to_string(live) +
                               " bytes live after collection); that call failed, the script continues";
      s->api->log(s->owner, NEVR_LOG_WARNING, note.c_str());
      return;
    }
    s->breach = std::string("memory limit exceeded: ") + CapVerdictReason(verdict) + " (" + std::to_string(live) +
                " bytes live after collection, largest refused request " + std::to_string(refused) + " bytes, " +
                std::to_string(hits) + " call(s) in a row at the cap; the cap is " + std::to_string(cap) + " bytes)";
  }
  if (!s->breach.empty()) s->registry->DisableOwner(s->owner, s->breach);
}

// Every state whose script code this thread is running (each one's lock held),
// innermost last. A hook point or load that needs a lock this thread already
// holds is refused, since the lock is not recursive and its VM is mid-call: with
// one state per script that is the same script again (A -> A, A -> B -> A); with
// the shared state, where one lock serializes every script, it is any script
// nested inside another on one thread. Deeper nesting than the list holds is
// refused too.
constexpr int kMaxNestedScripts = 8;
thread_local const State* t_running_states[kMaxNestedScripts];
thread_local int t_running_depth = 0;

bool HoldsLockOnThisThread(const std::mutex* lock) {
  for (int i = 0; i < t_running_depth; ++i) {
    if (t_running_states[i]->mu == lock) return true;
  }
  return false;
}

struct RunningState {
  explicit RunningState(const State* s) { t_running_states[t_running_depth++] = s; }
  ~RunningState() { --t_running_depth; }
  RunningState(const RunningState&) = delete;
  RunningState& operator=(const RunningState&) = delete;
};

#if NEVR_LUAU_SHARED_STATE
// Marks `s` as the script whose code runs until the end of the scope (the VM-wide
// mutex is held by the caller). Cleared before ApplyBreach: its collection
// frees, it does not run script code.
class RunningScope {
 public:
  RunningScope(SharedHost& host, State* s) : host_(host) { host_.running = s; }
  ~RunningScope() { host_.running = nullptr; }
  RunningScope(const RunningScope&) = delete;
  RunningScope& operator=(const RunningScope&) = delete;

 private:
  SharedHost& host_;
};
#endif

NevrHookResult HookTrampoline(NevrHookCall* call, void* user) {
  const CallbackRef* ref = static_cast<const CallbackRef*>(user);
  State* s = ref->state;
  if (s->owner->disabled.load()) return NEVR_HOOK_CONTINUE;
  if (HoldsLockOnThisThread(s->mu)) {
    return s->api->call_fail(call, "re-entrant call into this script on the thread already running it; refused");
  }
  if (t_running_depth >= kMaxNestedScripts) {
    return s->api->call_fail(call, "scripts nested more than 8 deep on one thread; refused");
  }
  std::lock_guard<std::mutex> lock(*s->mu);
  const RunningState running(s);
  s->call = call;
  s->phase = ref->phase;
  s->skip = false;
  s->BeginBudget();
  lua_State* const T = s->run;
  const int base = lua_gettop(T);
  int status;
  std::string error;
  {
#if NEVR_LUAU_SHARED_STATE
    // The callback thread belongs to the script: its stack and every object the
    // callback creates are charged to the script's category.
    RunningScope running_scope(*static_cast<SharedHost*>(lua_callbacks(T)->userdata), s);
#endif
    // The callback and h are pushed and called directly: no C function of ours
    // sits on the stack under the script. Pushing them needs no allocation (an
    // idle thread keeps LUA_MINSTACK free slots), so nothing here can raise
    // outside the protected call.
#if NEVR_LUAU_SHARED_STATE
    lua_getref(T, s->anchor_ref);
    lua_rawgeti(T, -1, ref->ref);
    lua_rawgeti(T, -2, kAnchorCall);
    lua_remove(T, -3);
#else
    lua_getref(T, ref->ref);
    lua_getref(T, s->call_object_ref);
#endif
    status = lua_pcall(T, 1, 0, 0);
    if (status != LUA_OK) error = PopError(T);
    lua_settop(T, base);
    s->EndBudget();
  }
  s->call = nullptr;
  ApplyBreach(s);
  if (status != LUA_OK) return s->api->call_fail(call, error.c_str());
  return s->skip ? NEVR_HOOK_SKIP_ORIGINAL : NEVR_HOOK_CONTINUE;
}

// ---- the VM --------------------------------------------------------------------------------

#if !NEVR_LUAU_SHARED_STATE
class LuauVm final : public ScriptVm {
 public:
  LuauVm(Registry& registry, const VmLimits& limits) : registry_(registry), limits_(limits) {}

  const char* Name() const override { return "Luau " NEVR_LUAU_TAG; }

  bool Load(NevrOwner* owner, const std::string& chunkname, const std::string& source,
            std::string* error) override {
    Unload(owner);
    auto state = std::make_unique<State>();
    State* s = state.get();
    s->registry = &registry_;
    s->api = registry_.Api();
    s->owner = owner;
    s->limits = limits_;
    s->L = lua_newstate(CappedAlloc, s);
    if (!s->L) return Fail(s, "cannot create a Luau state within the memory limit", error);
    // Goal 150% and step multiplier 300%, the pair lua.h recommends for that
    // goal (lua_GCOp, LUA_GCSETGOAL): the heap stays nearer the live set and
    // the collector keeps up with large temporary allocations within one call,
    // so garbage reaches the memory cap far less often than at the defaults
    // (200%, 200%).
    lua_gc(s->L, LUA_GCSETGOAL, 150);
    lua_gc(s->L, LUA_GCSETSTEPMUL, 300);
    lua_Callbacks* cb = lua_callbacks(s->L);
    cb->userdata = s;
    cb->interrupt = Interrupt;
    cb->panic = Panic;
    s->run = s->L;
    {
      std::lock_guard<std::mutex> lock(map_mu_);
      states_[owner] = std::move(state);
    }

    if (t_running_depth >= kMaxNestedScripts) return Fail(s, chunkname + ": scripts nested more than 8 deep on one thread", error);
    std::lock_guard<std::mutex> lock(*s->mu);
    const RunningState running(s);
    if (ProtectedCall(s->L, SetupThunk, s) != LUA_OK) {
      const std::string why = PopError(s->L);
      ApplyBreach(s);
      return Fail(s, chunkname + ": " + why, error);
    }

    lua_CompileOptions options = {};
    options.optimizationLevel = 1;  // baseline: no inlining or unrolling
    options.debugLevel = 1;         // line info, which error messages need
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
    if (!bytecode) return Fail(s, chunkname + ": out of memory while compiling", error);

    const std::string luau_name = "=" + chunkname;
    ScriptThread run{s, luau_name.c_str(), bytecode, size};
    std::string why;
    int status = ProtectedCall(s->L, ScriptThreadThunk, &run);
    if (status != LUA_OK) {
      why = PopError(s->L);
    } else if (run.load_status != 0) {
      status = LUA_ERRSYNTAX;
      why = PopError(run.thread);
    } else {
      // lua_resume runs the chunk protected; an error leaves its message on the thread.
      s->BeginBudget();
      status = lua_resume(run.thread, nullptr, 0);
      if (status == LUA_YIELD) status = LUA_OK;  // a top-level yield is not an error
      if (status != LUA_OK) why = PopError(run.thread);
      s->EndBudget();
    }
    std::free(bytecode);
    ApplyBreach(s);
    if (status != LUA_OK) return Fail(s, why, error);
    return true;
  }

  size_t MemoryBytes(const NevrOwner* owner) const override {
    std::lock_guard<std::mutex> lock(map_mu_);
    const auto it = states_.find(owner);
    return it == states_.end() ? 0 : it->second->used.load();
  }

  size_t TotalMemoryBytes() const override {
    std::lock_guard<std::mutex> lock(map_mu_);
    size_t total = 0;
    for (const auto& entry : states_) total += entry.second->used.load();
    return total;
  }

  void Unload(NevrOwner* owner) override {
    std::unique_ptr<State> gone;
    {
      std::lock_guard<std::mutex> lock(map_mu_);
      const auto it = states_.find(owner);
      if (it == states_.end()) return;
      gone = std::move(it->second);
      states_.erase(it);
    }
    std::lock_guard<std::mutex> lock(*gone->mu);  // wait out a callback still running
  }

 private:
  static bool Fail(State* s, const std::string& message, std::string* error) {
    if (error) *error = message;
    (void)s;
    return false;
  }

  Registry& registry_;
  VmLimits limits_;
  mutable std::mutex map_mu_;
  std::unordered_map<const NevrOwner*, std::unique_ptr<State>> states_;
};
#endif

#if NEVR_LUAU_SHARED_STATE
// One lua_State for every script (NEVR_LUAU_SHARED_STATE). The libraries and the
// sandbox exist once, in memory category 0; each script is one State (the same
// struct the other mode uses) that owns a memory category, an anchor table and two
// threads, all charged to that category. Everything that touches the state, from
// any game thread, runs under host_.mu.
//
// Categories: Luau tags every object with the category active when it was created
// (lgc.h luaC_init) and counts bytes per category (lmem.cpp memcatbytes). A script's
// thread is created with its category active, and a thread inherits the category of
// the thread that creates it, so the script's globals, closures, tables, strings,
// coroutines and stacks are all its own. Two things are not exact, and the cap
// accepts them: an interned string belongs to whoever created it first even when
// another script later uses it, and VM-internal growth (the string table, the
// registry) is category 0.
class SharedLuauVm final : public ScriptVm {
 public:
  SharedLuauVm(Registry& registry, const VmLimits& limits) : registry_(registry), limits_(limits) {
    for (int category = kMaxScripts; category >= 1; --category) free_.push_back(category);
  }

  ~SharedLuauVm() override {
    if (host_.main) lua_close(host_.main);
  }

  const char* Name() const override { return "Luau " NEVR_LUAU_TAG " (shared state)"; }

  bool Load(NevrOwner* owner, const std::string& chunkname, const std::string& source,
            std::string* error) override {
    if (HoldsLockOnThisThread(&host_.mu)) return Fail(chunkname + ": cannot load while a script runs on this thread", error);
    if (t_running_depth >= kMaxNestedScripts) return Fail(chunkname + ": scripts nested more than 8 deep on one thread", error);
    Unload(owner);
    std::lock_guard<std::mutex> lock(host_.mu);
    std::string why;
    if (!EnsureState(&why)) return Fail(chunkname + ": " + why, error);
    const int category = AcquireCategory();
    if (category == 0) {
      return Fail(chunkname + ": cannot load: all " + std::to_string(kMaxScripts) +
                      " script memory categories (1.." + std::to_string(kMaxScripts) +
                      ") are in use or reserved; unload a script first",
                  error);
    }
    auto state = std::make_unique<State>();
    State* s = state.get();
    s->registry = &registry_;
    s->api = registry_.Api();
    s->owner = owner;
    s->limits = limits_;
    s->L = host_.main;
    s->mu = &host_.mu;
    s->category = category;
    scripts_[owner] = std::move(state);
    const RunningState running_on_thread(s);

    lua_CompileOptions options = {};
    options.optimizationLevel = 1;  // baseline: no inlining or unrolling
    options.debugLevel = 1;         // line info, which error messages need
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
    if (!bytecode) return Fail(chunkname + ": out of memory while compiling", error);

    const std::string luau_name = "=" + chunkname;
    ScriptThread run{s, luau_name.c_str(), bytecode, size};
    std::string error_text;
    int status;
    {
      RunningScope running(host_, s);
      lua_setmemcat(host_.main, category);
      status = ProtectedCall(host_.main, ScriptThreadThunk, &run);
      lua_setmemcat(host_.main, 0);
      if (status != LUA_OK) {
        error_text = PopError(host_.main);
      } else if (run.load_status != 0) {
        status = LUA_ERRSYNTAX;
        error_text = PopError(run.thread);
      } else {
        // lua_resume runs the chunk protected; an error leaves its message on the thread.
        s->BeginBudget();
        status = lua_resume(run.thread, nullptr, 0);
        if (status == LUA_YIELD) status = LUA_OK;  // a top-level yield is not an error
        if (status != LUA_OK) error_text = PopError(run.thread);
        s->EndBudget();
      }
    }
    std::free(bytecode);
    ApplyBreach(s);
    if (status != LUA_OK) return Fail(error_text, error);
    return true;
  }

  size_t MemoryBytes(const NevrOwner* owner) const override {
    std::lock_guard<std::mutex> lock(host_.mu);
    const auto it = scripts_.find(owner);
    return it == scripts_.end() ? 0 : lua_totalbytes(host_.main, it->second->category);
  }

  // What the allocator holds for the whole state (libraries, every script, free
  // space in its pages): the same measure as the other mode's TotalMemoryBytes,
  // so the two compare like for like.
  size_t TotalMemoryBytes() const override { return host_.allocated.load(std::memory_order_relaxed); }

  void Unload(NevrOwner* owner) override {
    std::lock_guard<std::mutex> lock(host_.mu);  // waits out a callback still running
    const auto it = scripts_.find(owner);
    if (it == scripts_.end()) return;
    std::unique_ptr<State> gone = std::move(it->second);
    scripts_.erase(it);
    // Dropping the anchor drops both threads, the call object and every callback;
    // the registry has already stopped calling them (Registry::Quiesce).
    lua_unref(host_.main, gone->anchor_ref);
    lua_gc(host_.main, LUA_GCCOLLECT, 0);
    reserved_.push_back(gone->category);
    SettleReserved();
    if (std::find(reserved_.begin(), reserved_.end(), gone->category) != reserved_.end()) {
      registry_.Record(NEVR_LOG_WARNING, "category_reserved", owner, "",
                       std::to_string(lua_totalbytes(host_.main, gone->category)) +
                           " bytes are still charged to memory category " + std::to_string(gone->category) +
                           " after a full collection (objects another script still reaches); the category stays "
                           "reserved until they are freed");
    }
  }

 private:
  static bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
  }

  // Creates the shared state on first use: libraries, removals and luaL_sandbox
  // once, in category 0. Holds host_.mu.
  bool EnsureState(std::string* why) {
    if (host_.main) return true;
    host_.main = lua_newstate(SharedAlloc, &host_);
    if (!host_.main) {
      *why = "cannot create the Luau state";
      return false;
    }
    // Goal 150% and step multiplier 300%: see LuauVm::Load.
    lua_gc(host_.main, LUA_GCSETGOAL, 150);
    lua_gc(host_.main, LUA_GCSETSTEPMUL, 300);
    lua_Callbacks* cb = lua_callbacks(host_.main);
    cb->userdata = &host_;
    cb->interrupt = Interrupt;
    cb->panic = Panic;
    if (ProtectedCall(host_.main, SetupThunk, &host_) != LUA_OK) {
      *why = PopError(host_.main);
      lua_close(host_.main);
      host_.main = nullptr;
      return false;
    }
    return true;
  }

  // A category with no bytes left is free again. Holds host_.mu.
  void SettleReserved() {
    for (size_t i = 0; i < reserved_.size();) {
      if (lua_totalbytes(host_.main, reserved_[i]) == 0) {
        free_.push_back(reserved_[i]);
        reserved_.erase(reserved_.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  }

  // The most recently freed category, or 0 when all are taken. Holds host_.mu.
  int AcquireCategory() {
    if (free_.empty() && !reserved_.empty()) {
      lua_gc(host_.main, LUA_GCCOLLECT, 0);
      SettleReserved();
    }
    if (free_.empty()) return 0;
    const int category = free_.back();
    free_.pop_back();
    return category;
  }

  Registry& registry_;
  VmLimits limits_;
  mutable SharedHost host_;
  std::unordered_map<const NevrOwner*, std::unique_ptr<State>> scripts_;
  std::vector<int> free_;      // categories no script holds
  std::vector<int> reserved_;  // unloaded scripts' categories that still hold bytes
};
#endif

}  // namespace

#if NEVR_LUAU_TEST_HOOKS
// Test builds only (src/scripting/vm/luau/CMakeLists.txt NEVR_LUAU_TEST_HOOKS): calls
// each lua_cpcall thunk as a script would if one were ever reachable again, with
// no argument, nil, and a foreign light userdata. True when every call raised an
// error instead of touching memory, which is what ThunkArg guarantees on its own.
bool LuauTestThunksRefuseForeignArguments() {
  lua_State* L = luaL_newstate();
  if (!L) return false;
  static int foreign = 0;
  bool all_refused = true;
  const lua_CFunction thunks[] = {SetupThunk, ScriptThreadThunk};
  for (const lua_CFunction thunk : thunks) {
    for (int variant = 0; variant < 3; ++variant) {
      lua_pushcfunction(L, thunk, "thunk");
      if (variant == 1) lua_pushnil(L);
      if (variant == 2) lua_pushlightuserdata(L, &foreign);
      all_refused = all_refused && lua_pcall(L, variant == 0 ? 0 : 1, 0, 0) != LUA_OK;
      lua_settop(L, 0);
    }
  }
  lua_close(L);
  return all_refused;
}
#endif

std::unique_ptr<ScriptVm> CreateScriptVm(Registry& registry, const VmLimits& limits) {
#if NEVR_LUAU_SHARED_STATE
  return std::make_unique<SharedLuauVm>(registry, limits);
#else
  return std::make_unique<LuauVm>(registry, limits);
#endif
}

}  // namespace nevr_script
