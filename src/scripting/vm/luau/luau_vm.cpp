// Luau binding for the script VM seam (src/scripting/script_vm.h, #440).
//
// Error handling: the VM is built with LUA_USE_LONGJMP=1, so a Lua error is a
// longjmp, never a C++ exception. Every function below that can raise an error
// (anything that calls a lua_* or luaL_* function that allocates) therefore
// keeps no object with a destructor alive across such a call. All work that
// touches the VM from C++ runs inside lua_cpcall, so an error can never reach
// the VM's unprotected-error path.
//
// Limits (script_vm.h): one lua_State per owner, with
//   - a safepoint and wall-clock budget checked from lua_callbacks()->interrupt,
//   - a byte cap enforced by the lua_Alloc handed to lua_newstate.
// A breach is sticky: it is recorded on the state and the interrupt raises an
// error at every later safepoint, so a script's pcall cannot swallow it.
#include <atomic>
#include <chrono>
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
#include "scripting/script_vm.h"

namespace nevr_script {
namespace {

constexpr int kCallTag = 1;  // userdata tag of the call object `h`
constexpr int64_t kExactIntLimit = int64_t{1} << 53;  // a double holds every integer below this

struct State;

// One registered Lua callback; the pointer is the `user` the registry hands back.
struct CallbackRef {
  State* state = nullptr;
  int ref = LUA_NOREF;
  NevrHookPhase phase = NEVR_HOOK_PRE;
};

struct State {
  Registry* registry = nullptr;
  const NevrHostApi* api = nullptr;
  NevrOwner* owner = nullptr;
  VmLimits limits;
  lua_State* L = nullptr;

  // Memory: bytes the allocator has handed out and not got back.
  std::atomic<size_t> used{0};
  std::atomic<bool> mem_failed{false};
  bool memory_breach = false;  // `breach` was set because an allocation hit the cap
  std::atomic<size_t> largest_refused{0};  // the biggest request CappedAlloc refused since the last check

  // Budget for the running top-level chunk or callback.
  bool active = false;
  uint64_t safepoints = 0;
  std::chrono::steady_clock::time_point started;
  std::string breach;  // non-empty once a limit was crossed; stays set

  // The Lua state is single threaded; hook points may fire on any game thread.
  std::mutex mu;

  // The running callback.
  NevrHookCall* call = nullptr;
  NevrHookPhase phase = NEVR_HOOK_PRE;
  bool skip = false;
  int call_object_ref = LUA_NOREF;
  int thread_ref = LUA_NOREF;  // the script's sandboxed thread, kept alive

  std::vector<std::unique_ptr<CallbackRef>> callbacks;

  ~State() {
    if (L) lua_close(L);
  }

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

State* StateOf(lua_State* L) { return static_cast<State*>(lua_callbacks(L)->userdata); }

// ---- allocator: the memory cap -------------------------------------------------------------

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

// ---- interrupt: the instruction and time budget --------------------------------------------

void Interrupt(lua_State* L, int gc) {
  if (gc >= 0) return;  // a GC step, not a safepoint of the running code
  State* s = StateOf(L);
  s->CheckBudget();
  if (!s->breach.empty()) luaL_error(L, "%s", s->breach.c_str());
}

void Panic(lua_State* L, int) {
  State* s = StateOf(L);
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
    ref->ref = lua_ref(L, -1);
    lua_pop(L, 1);
    const NevrStatus status = s->api->hook_add(s->owner, name, kPhases[i], HookTrampoline, ref);
    if (status != NEVR_OK) {
      lua_unref(L, ref->ref);
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

// Builds the sandboxed environment. Runs inside lua_cpcall; `ud` is the State.
int SetupThunk(lua_State* L) {
  State* s = static_cast<State*>(lua_tolightuserdata(L, 1));

  // Whitelist: only the libraries script_vm.h names. luaL_openlibs would also
  // open bit32, buffer, vector and integer, which the contract does not offer.
  OpenLib(L, "", luaopen_base);
  OpenLib(L, LUA_COLIBNAME, luaopen_coroutine);
  OpenLib(L, LUA_TABLIBNAME, luaopen_table);
  OpenLib(L, LUA_OSLIBNAME, luaopen_os);
  OpenLib(L, LUA_STRLIBNAME, luaopen_string);
  OpenLib(L, LUA_MATHLIBNAME, luaopen_math);
  OpenLib(L, LUA_DBLIBNAME, luaopen_debug);
  OpenLib(L, LUA_UTF8LIBNAME, luaopen_utf8);

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

  // The call object h: one userdata per state, valid only while s->call is set.
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
  lua_newuserdatataggedwithmetatable(L, sizeof(void*), kCallTag);
  s->call_object_ref = lua_ref(L, -1);
  lua_pop(L, 1);

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
int ScriptThreadThunk(lua_State* L) {
  ScriptThread* c = static_cast<ScriptThread*>(lua_tolightuserdata(L, 1));
  lua_State* T = lua_newthread(L);
  c->state->thread_ref = lua_ref(L, -1);
  lua_pop(L, 1);
  luaL_sandboxthread(T);
  c->thread = T;
  c->load_status = luau_load(T, c->chunkname, c->bytecode, c->size, 0);
  return 0;
}

struct RunCallback {
  State* state;
  const CallbackRef* ref;
};

int RunCallbackThunk(lua_State* L) {
  const RunCallback* c = static_cast<const RunCallback*>(lua_tolightuserdata(L, 1));
  lua_getref(L, c->ref->ref);
  lua_getref(L, c->state->call_object_ref);
  lua_call(L, 1, 0);
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
// the cap, collect first and judge the live set: if it is under half the cap and
// no single refused request was over half the cap, the failure was garbage
// outrunning the collector; that call failed, but the script is not disabled.
// One request past half the cap (string.rep of 64 MiB) is a breach whatever is
// live. Between calls, a state past half its cap is collected so garbage does
// not carry over.
void ApplyBreach(State* s) {
  const size_t cap = s->limits.memory_bytes;
  if (s->mem_failed.load() && (s->breach.empty() || s->memory_breach)) {
    lua_gc(s->L, LUA_GCCOLLECT, 0);
    const size_t live = s->used.load();
    const size_t refused = s->largest_refused.exchange(0);
    if (live <= cap / 2 && refused <= cap / 2) {
      s->mem_failed.store(false);
      s->breach.clear();
      s->memory_breach = false;
      const std::string note = "an allocation reached the memory cap of " + std::to_string(cap) +
                               " bytes while most of it was garbage (" + std::to_string(live) +
                               " bytes live after collection); that call failed, the script continues";
      s->api->log(s->owner, NEVR_LOG_WARNING, note.c_str());
      return;
    }
    s->breach = "memory limit exceeded: " + std::to_string(live) + " bytes live after collection, largest refused request " +
                std::to_string(refused) + " bytes; the cap is " + std::to_string(cap) + " bytes";
  }
  if (!s->breach.empty()) {
    s->registry->DisableOwner(s->owner, s->breach);
    return;
  }
  if (s->used.load() > cap / 2) lua_gc(s->L, LUA_GCCOLLECT, 0);
}

NevrHookResult HookTrampoline(NevrHookCall* call, void* user) {
  const CallbackRef* ref = static_cast<const CallbackRef*>(user);
  State* s = ref->state;
  if (s->owner->disabled.load()) return NEVR_HOOK_CONTINUE;
  std::lock_guard<std::mutex> lock(s->mu);
  s->call = call;
  s->phase = ref->phase;
  s->skip = false;
  s->BeginBudget();
  RunCallback run{s, ref};
  const int base = lua_gettop(s->L);
  const int status = lua_cpcall(s->L, RunCallbackThunk, &run);
  std::string error;
  if (status != LUA_OK) error = PopError(s->L);
  lua_settop(s->L, base);
  s->EndBudget();
  s->call = nullptr;
  ApplyBreach(s);
  if (status != LUA_OK) return s->api->call_fail(call, error.c_str());
  return s->skip ? NEVR_HOOK_SKIP_ORIGINAL : NEVR_HOOK_CONTINUE;
}

// ---- the VM --------------------------------------------------------------------------------

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
    lua_Callbacks* cb = lua_callbacks(s->L);
    cb->userdata = s;
    cb->interrupt = Interrupt;
    cb->panic = Panic;
    {
      std::lock_guard<std::mutex> lock(map_mu_);
      states_[owner] = std::move(state);
    }

    std::lock_guard<std::mutex> lock(s->mu);
    if (lua_cpcall(s->L, SetupThunk, s) != LUA_OK) {
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
    int status = lua_cpcall(s->L, ScriptThreadThunk, &run);
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
    std::lock_guard<std::mutex> lock(gone->mu);  // wait out a callback still running
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

}  // namespace

std::unique_ptr<ScriptVm> CreateScriptVm(Registry& registry, const VmLimits& limits) {
  return std::make_unique<LuauVm>(registry, limits);
}

}  // namespace nevr_script
