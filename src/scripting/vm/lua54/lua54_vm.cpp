// PUC-Rio Lua 5.4 binding for the script host seam (src/scripting/script_vm.h).
//
// Error discipline. Lua is built as C, so lua_error is a longjmp (extern/lua/ldo.c
// LUAI_THROW). A longjmp must never skip a C++ frame that owns something with a
// destructor. The rules that keep that true:
//   1. Every lua_CFunction below (the L_* functions, Hook, MsgHandler, Setup) holds
//      only trivially destructible locals: pointers, ints, NevrValue, char arrays.
//      Anything that needs std::string, std::vector or a lock lives in a plain C++
//      function that calls no Lua API able to raise, and the lua_CFunction calls
//      it before or after (never across) the Lua calls.
//   2. C++ code reaches Lua only through lua_pcall, lua_newstate (returns null on
//      failure) and luaL_loadbufferx (protected parser), plus API calls that
//      cannot raise (lua_gettop, lua_settop shrinking, lua_rawgeti, lua_rawgetp,
//      lua_pushlightuserdata, lua_pushcfunction, lua_tolstring on a string).
//      Everything else runs inside Setup or a pcall'd function.
//   3. The registry's C++ frames (Registry::Invoke, RunChain) call the trampoline,
//      which calls lua_pcall: no longjmp can pass through them.
// The binding therefore needs no C++ exceptions and is built with -fno-exceptions.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "scripting/script_vm.h"

namespace nevr_script {
namespace {

using Clock = std::chrono::steady_clock;

enum class Breach { kNone, kInstructions, kTime, kMemory };

constexpr int kHookStep = 1000;  // VM instructions between budget checks
const char kCallMeta[] = "nevr.call";
char g_env_key;   // registry key (address) of the sandbox environment table
char g_call_key;  // registry key of the reusable call object

struct OwnerVm;

struct Binding {
  OwnerVm* vm;
  int ref;  // registry reference of the Lua callback
  NevrHookPhase phase;
};

struct OwnerVm {
  Registry* registry = nullptr;
  const NevrHostApi* api = nullptr;
  NevrOwner* owner = nullptr;
  VmLimits limits;
  int step = kHookStep;
  lua_State* L = nullptr;
  std::mutex mu;  // a state is not thread-safe; callbacks can come from any game thread
  bool closed = false;

  // Allocator accounting (Alloc below).
  size_t used = 0;
  bool refusal_pending = false;  // an allocation was refused and its retry has not happened yet
  void* refused_ptr = nullptr;
  size_t refused_old = 0;
  size_t refused_new = 0;
  bool hard_oom = false;  // a refusal that Lua's emergency collection did not recover from

  // Budget of the running top-level chunk or callback.
  uint64_t executed = 0;
  Clock::time_point deadline;
  Breach breach = Breach::kNone;

  // The running callback, for h:get / h:set / h:skip.
  NevrHookCall* call = nullptr;
  NevrHookPhase phase = NEVR_HOOK_PRE;
  bool skip = false;

  std::vector<std::unique_ptr<Binding>> bindings;

  bool Refused() const { return hard_oom || refusal_pending; }
  void BeginCall() {
    executed = 0;
    deadline = Clock::now() + std::chrono::milliseconds(limits.millis_per_call);
  }
  Binding* NewBinding(int ref, NevrHookPhase p) {
    bindings.push_back(std::unique_ptr<Binding>(new Binding{this, ref, p}));
    return bindings.back().get();
  }
};

NevrHookResult RunBinding(NevrHookCall* call, void* user);

OwnerVm* Self(lua_State* L) {
  void* ud = nullptr;
  lua_getallocf(L, &ud);
  return static_cast<OwnerVm*>(ud);
}

// ---- allocator: counts bytes, refuses past the cap ------------------------------------------
//
// Lua answers a refused allocation by running an emergency collection and retrying
// the identical request (extern/lua/lmem.c luaM_realloc_ -> tryagain). A refusal
// that the retry satisfies is not a breach. A refusal with no identical retry that
// succeeds is: `hard_oom`, or `refusal_pending` still set when the hook or the end
// of the call looks.
void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  OwnerVm* vm = static_cast<OwnerVm*>(ud);
  if (ptr == nullptr) osize = 0;  // then osize is a type tag, not a size
  if (nsize == 0) {
    std::free(ptr);
    vm->used -= osize;
    return nullptr;
  }
  bool retry = false;
  if (vm->refusal_pending) {
    retry = ptr == vm->refused_ptr && osize == vm->refused_old && nsize == vm->refused_new;
    if (!retry) {
      vm->hard_oom = true;
      vm->refusal_pending = false;
    }
  }
  void* block = nullptr;
  const bool grows = nsize > osize;
  if (!grows || vm->used - osize + nsize <= vm->limits.memory_bytes) {
    block = std::realloc(ptr, nsize);
    if (block == nullptr && !grows) block = ptr;  // a shrink must not fail
  }
  if (block == nullptr) {
    if (retry) {
      vm->hard_oom = true;
      vm->refusal_pending = false;
    } else {
      vm->refusal_pending = true;
      vm->refused_ptr = ptr;
      vm->refused_old = osize;
      vm->refused_new = nsize;
    }
    return nullptr;
  }
  if (retry) vm->refusal_pending = false;
  vm->used = vm->used - osize + nsize;
  return block;
}

// ---- budget hook ----------------------------------------------------------------------------
//
// Installed with LUA_MASKCOUNT on the main thread; coroutines inherit it
// (extern/lua/lstate.c lua_newthread copies hookmask, basehookcount and hook).
// After a breach the hook keeps raising, at every instruction of the thread it
// fires on, so a pcall inside the script cannot swallow the stop: the loop around
// the pcall executes instructions outside it and the hook raises there.
const char* BreachText(Breach b) {
  switch (b) {
    case Breach::kInstructions: return "instruction budget exceeded";
    case Breach::kTime: return "time budget exceeded";
    case Breach::kMemory: return "memory cap exceeded";
    default: return "stopped";
  }
}

void Hook(lua_State* L, lua_Debug*) {
  OwnerVm* vm = Self(L);
  if (vm->breach == Breach::kNone) {
    vm->executed += static_cast<uint64_t>(vm->step);
    if (vm->Refused()) {
      vm->breach = Breach::kMemory;
    } else if (vm->executed > vm->limits.instructions_per_call) {
      vm->breach = Breach::kInstructions;
    } else if (Clock::now() > vm->deadline) {
      vm->breach = Breach::kTime;
    }
  }
  if (vm->breach != Breach::kNone) {
    lua_sethook(L, Hook, LUA_MASKCOUNT, 1);  // this thread: check at every instruction from now on
    lua_pushstring(L, BreachText(vm->breach));
    lua_error(L);
  }
}

// ---- script-visible functions ---------------------------------------------------------------

int MsgHandler(lua_State* L) {
  if (lua_type(L, 1) == LUA_TSTRING) return 1;
  lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  return 1;
}

int L_override(lua_State* L) {
  OwnerVm* vm = Self(L);
  const char* key = luaL_checkstring(L, 1);
  NevrValue v{};
  switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
      v.type = NEVR_VALUE_BOOL;
      v.as.b = lua_toboolean(L, 2);
      break;
    case LUA_TNUMBER:  // the registry converts to the override point's type
      if (lua_isinteger(L, 2)) {
        v.type = NEVR_VALUE_INT;
        v.as.i = static_cast<int64_t>(lua_tointeger(L, 2));
      } else {
        v.type = NEVR_VALUE_FLOAT;
        v.as.f = lua_tonumber(L, 2);
      }
      break;
    case LUA_TSTRING:
      v.type = NEVR_VALUE_STRING;
      v.as.s = lua_tostring(L, 2);
      break;
    default: return luaL_argerror(L, 2, "boolean, number or string expected");
  }
  const NevrStatus st = vm->api->override_set(vm->owner, key, &v);
  if (st == NEVR_OK) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushnil(L);
  lua_pushfstring(L, "%s: %s", vm->api->status_name(st), vm->api->last_error(vm->owner));
  return 2;
}

int L_hook(lua_State* L) {
  OwnerVm* vm = Self(L);
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  static const char* const kKeys[2] = {"pre", "post"};
  static const NevrHookPhase kPhases[2] = {NEVR_HOOK_PRE, NEVR_HOOK_POST};
  int refs[2] = {LUA_NOREF, LUA_NOREF};
  for (int i = 0; i < 2; ++i) {
    lua_getfield(L, 2, kKeys[i]);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
    } else if (!lua_isfunction(L, -1)) {
      return luaL_error(L, "nevr.hook: '%s' must be a function", kKeys[i]);
    } else {
      refs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
  }
  if (refs[0] == LUA_NOREF && refs[1] == LUA_NOREF) {
    return luaL_argerror(L, 2, "needs a 'pre' or a 'post' function");
  }
  for (int i = 0; i < 2; ++i) {
    if (refs[i] == LUA_NOREF) continue;
    Binding* b = vm->NewBinding(refs[i], kPhases[i]);
    const NevrStatus st = vm->api->hook_add(vm->owner, name, kPhases[i], RunBinding, b);
    if (st != NEVR_OK) {
      lua_pushnil(L);
      lua_pushfstring(L, "%s: %s", vm->api->status_name(st), vm->api->last_error(vm->owner));
      return 2;
    }
  }
  lua_pushboolean(L, 1);
  return 1;
}

int L_log(lua_State* L) {
  static const char* const kNames[] = {"debug", "info", "warn", "error", nullptr};
  static const NevrLogLevel kLevels[] = {NEVR_LOG_DEBUG, NEVR_LOG_INFO, NEVR_LOG_WARNING, NEVR_LOG_ERROR};
  OwnerVm* vm = Self(L);
  const int level = luaL_checkoption(L, 1, nullptr, kNames);
  const char* message = luaL_checkstring(L, 2);
  vm->api->log(vm->owner, kLevels[level], message);
  return 0;
}

int L_print(lua_State* L) {
  OwnerVm* vm = Self(L);
  const int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) luaL_addlstring(&b, "\t", 1);
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    luaL_addlstring(&b, s, len);
    lua_pop(L, 1);
  }
  luaL_pushresult(&b);
  vm->api->log(vm->owner, NEVR_LOG_INFO, lua_tostring(L, -1));
  return 0;
}

int L_collectgarbage(lua_State* L) {
  const char* opt = luaL_optstring(L, 1, "");
  if (std::strcmp(opt, "count") != 0) return luaL_error(L, "collectgarbage: only \"count\" is available");
  lua_pushnumber(L, static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNT)) +
                        static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNTB)) / 1024.0);
  return 1;
}

// setmetatable without __gc. Lua runs a finalizer with hooks off (extern/lua/lgc.c GCTM sets
// L->allowhook = 0), so the instruction budget cannot stop one that loops. A finalizer is
// only registered when the metatable has __gc at the moment of setmetatable
// (lgc.c luaC_checkfinalizer, a raw lookup), so refusing it here closes the route.
int L_setmetatable(lua_State* L) {
  const int t = lua_type(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  if (t != LUA_TNIL && t != LUA_TTABLE) return luaL_typeerror(L, 2, "nil or table");
  if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL) return luaL_error(L, "cannot change a protected metatable");
  if (t == LUA_TTABLE) {
    lua_pushliteral(L, "__gc");
    lua_rawget(L, 2);
    if (!lua_isnil(L, -1)) return luaL_error(L, "__gc is not available to scripts");
  }
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1;
}

int L_traceback(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  if (msg == nullptr && !lua_isnoneornil(L, 1)) {
    lua_pushvalue(L, 1);  // not a string: returned untouched, as debug.traceback does
    return 1;
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}

NevrHookCall* RequireCall(lua_State* L) {
  OwnerVm* vm = Self(L);
  luaL_checkudata(L, 1, kCallMeta);
  if (vm->call == nullptr) luaL_error(L, "the call object is only valid inside its callback");
  return vm->call;
}

int L_call_get(lua_State* L) {
  OwnerVm* vm = Self(L);
  NevrHookCall* call = RequireCall(L);
  const char* field = luaL_checkstring(L, 2);
  NevrValue v{};
  const NevrStatus st = vm->api->call_get(call, field, &v);
  if (st != NEVR_OK) return luaL_error(L, "h:get('%s'): %s", field, vm->api->status_name(st));
  switch (v.type) {
    case NEVR_VALUE_BOOL: lua_pushboolean(L, v.as.b != 0); break;
    case NEVR_VALUE_INT: lua_pushinteger(L, static_cast<lua_Integer>(v.as.i)); break;
    case NEVR_VALUE_FLOAT: lua_pushnumber(L, static_cast<lua_Number>(v.as.f)); break;
    case NEVR_VALUE_STRING: lua_pushstring(L, v.as.s); break;
    default: lua_pushnil(L); break;
  }
  return 1;
}

int L_call_set(lua_State* L) {
  OwnerVm* vm = Self(L);
  NevrHookCall* call = RequireCall(L);
  const char* field = luaL_checkstring(L, 2);
  NevrValue v{};
  switch (lua_type(L, 3)) {
    case LUA_TBOOLEAN:
      v.type = NEVR_VALUE_BOOL;
      v.as.b = lua_toboolean(L, 3);
      break;
    case LUA_TNUMBER:  // the registry converts to the field's type, or answers TYPE_MISMATCH
      if (lua_isinteger(L, 3)) {
        v.type = NEVR_VALUE_INT;
        v.as.i = static_cast<int64_t>(lua_tointeger(L, 3));
      } else {
        v.type = NEVR_VALUE_FLOAT;
        v.as.f = lua_tonumber(L, 3);
      }
      break;
    case LUA_TSTRING:
      v.type = NEVR_VALUE_STRING;
      v.as.s = lua_tostring(L, 3);
      break;
    default: return luaL_argerror(L, 3, "boolean, number or string expected");
  }
  const NevrStatus st = vm->api->call_set(call, field, &v);
  if (st != NEVR_OK) return luaL_error(L, "h:set('%s'): %s", field, vm->api->status_name(st));
  return 0;
}

int L_call_skip(lua_State* L) {
  OwnerVm* vm = Self(L);
  RequireCall(L);
  if (vm->phase != NEVR_HOOK_PRE) return luaL_error(L, "h:skip() is only valid in a pre callback");
  vm->skip = true;
  return 0;
}

// ---- sandbox ----------------------------------------------------------------------------------

// Copies the named fields of the table at `src` into the table at `dst`; a name the
// library does not have is an error, so a Lua upgrade that drops one is loud.
void CopyFields(lua_State* L, int src, int dst, const char* const* names) {
  src = lua_absindex(L, src);
  dst = lua_absindex(L, dst);
  for (; *names != nullptr; ++names) {
    lua_getfield(L, src, *names);
    if (lua_isnil(L, -1)) luaL_error(L, "sandbox: the Lua build has no '%s'", *names);
    lua_setfield(L, dst, *names);
  }
}

// Opens one standard library (not into the globals), copies the allowed names into a
// fresh table and stores that table in the environment as `name`.
void AddLibrary(lua_State* L, int env, const char* name, lua_CFunction open, const char* const* allowed) {
  env = lua_absindex(L, env);
  luaL_requiref(L, name, open, 0);
  lua_newtable(L);
  CopyFields(L, -2, -1, allowed);
  lua_setfield(L, env, name);
  lua_pop(L, 1);
}

void ClearTable(lua_State* L, int index) {
  index = lua_absindex(L, index);
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    lua_pop(L, 1);  // the value; the key stays for lua_next
    lua_pushvalue(L, -1);
    lua_pushnil(L);
    lua_rawset(L, index);
  }
}

int Setup(lua_State* L) {
  static const char* const kBase[] = {"assert", "error", "getmetatable", "ipairs", "next", "pairs",
                                      "pcall", "rawequal", "rawget", "rawlen", "rawset", "select",
                                      "tonumber", "tostring", "type", "xpcall",
                                      "_VERSION", nullptr};
  static const char* const kString[] = {"byte", "char", "find", "format", "gmatch", "gsub", "len",
                                        "lower", "match", "pack", "packsize", "rep", "reverse", "sub",
                                        "unpack", "upper", nullptr};  // no "dump"
  static const char* const kTable[] = {"concat", "insert", "move", "pack", "remove", "sort", "unpack", nullptr};
  static const char* const kMath[] = {"abs", "acos", "asin", "atan", "ceil", "cos", "exp", "floor",
                                      "fmod", "huge", "log", "max", "maxinteger", "min", "mininteger",
                                      "modf", "pi", "random", "randomseed", "sin", "sqrt", "tan",
                                      "tointeger", "type", "ult", nullptr};
  static const char* const kUtf8[] = {"char", "charpattern", "codepoint", "codes", "len", "offset", nullptr};
  static const char* const kCoroutine[] = {"close", "create", "isyieldable", "resume", "running",
                                           "status", "wrap", "yield", nullptr};
  static const char* const kOs[] = {"clock", "date", "time", nullptr};  // no execute/remove/getenv/...

  lua_newtable(L);  // env
  const int env = lua_gettop(L);

  luaL_requiref(L, "_G", luaopen_base, 0);
  CopyFields(L, -1, env, kBase);
  lua_pop(L, 1);
  lua_pushvalue(L, env);
  lua_setfield(L, env, "_G");

  AddLibrary(L, env, "string", luaopen_string, kString);
  AddLibrary(L, env, "table", luaopen_table, kTable);
  AddLibrary(L, env, "math", luaopen_math, kMath);
  AddLibrary(L, env, "utf8", luaopen_utf8, kUtf8);
  AddLibrary(L, env, "coroutine", luaopen_coroutine, kCoroutine);
  AddLibrary(L, env, "os", luaopen_os, kOs);

  lua_getfield(L, env, "table");
  lua_getfield(L, -1, "unpack");
  lua_setfield(L, env, "unpack");
  lua_pop(L, 1);

  // The string metatable is shared by every string: point its __index at the sandboxed
  // string table (the real one has string.dump) and lock it, so getmetatable('') returns
  // a plain string and setmetatable cannot reach it.
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1) == 0) return luaL_error(L, "sandbox: strings have no metatable");
  lua_getfield(L, env, "string");
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 2);

  lua_pushcfunction(L, L_setmetatable);
  lua_setfield(L, env, "setmetatable");
  lua_pushcfunction(L, L_print);
  lua_setfield(L, env, "print");
  lua_pushcfunction(L, L_collectgarbage);
  lua_setfield(L, env, "collectgarbage");

  lua_newtable(L);  // debug: traceback only
  lua_pushcfunction(L, L_traceback);
  lua_setfield(L, -2, "traceback");
  lua_setfield(L, env, "debug");

  lua_newtable(L);  // nevr
  lua_pushcfunction(L, L_override);
  lua_setfield(L, -2, "override");
  lua_pushcfunction(L, L_hook);
  lua_setfield(L, -2, "hook");
  lua_pushcfunction(L, L_log);
  lua_setfield(L, -2, "log");
  lua_setfield(L, env, "nevr");

  // The call object `h`: one userdata, handed to every callback.
  if (luaL_newmetatable(L, kCallMeta) == 0) return luaL_error(L, "sandbox: call metatable exists");
  lua_newtable(L);
  lua_pushcfunction(L, L_call_get);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, L_call_set);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, L_call_skip);
  lua_setfield(L, -2, "skip");
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);
  lua_newuserdatauv(L, 1, 0);
  luaL_setmetatable(L, kCallMeta);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &g_call_key);

  // The full libraries stay only inside the registry's package-loaded table and the
  // real globals, which no script reaches (no package, no debug, no load). Empty both
  // so nothing is left to reach by accident.
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  ClearTable(L, -1);
  lua_pop(L, 1);
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  ClearTable(L, -1);
  lua_pop(L, 1);

  lua_pushvalue(L, env);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &g_env_key);
  return 0;
}

int PanicHandler(lua_State* L) {
  OwnerVm* vm = Self(L);
  vm->api->log(vm->owner, NEVR_LOG_ERROR, "lua panic: error outside a protected call");
  return 0;  // Lua then aborts
}

// ---- VM instance -----------------------------------------------------------------------------

struct Outcome {
  Breach breach = Breach::kNone;  // set when the code must be stopped and the owner disabled
  std::string message;            // the error text, or the disable reason
};

std::string LimitText(const OwnerVm& vm, Breach b) {
  switch (b) {
    case Breach::kInstructions:
      return "instruction budget of " + std::to_string(vm.limits.instructions_per_call) + " exceeded";
    case Breach::kTime: return "time budget of " + std::to_string(vm.limits.millis_per_call) + " ms exceeded";
    case Breach::kMemory: return "memory cap of " + std::to_string(vm.limits.memory_bytes) + " bytes exceeded";
    default: return "stopped";
  }
}

// Reads the result of a finished protected call. Stack is cleaned to `base`.
Outcome Classify(OwnerVm& vm, int status, int base, const std::string& where) {
  lua_State* L = vm.L;
  Outcome out;
  if (status != LUA_OK) {
    const char* text = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "unknown error";
    out.message = text;
  }
  lua_settop(L, base);
  Breach b = vm.breach;
  if (b == Breach::kNone && (status == LUA_ERRMEM || vm.Refused())) b = Breach::kMemory;
  if (b != Breach::kNone) {
    out.breach = b;
    out.message = LimitText(vm, b) + " in " + where;
  }
  return out;
}

class Lua54Vm final : public ScriptVm {
 public:
  Lua54Vm(Registry& registry, const VmLimits& limits) : registry_(registry), limits_(limits) {}
  ~Lua54Vm() override {
    for (auto& entry : states_) Close(*entry.second);
  }

  const char* Name() const override { return LUA_RELEASE; }

  bool Load(NevrOwner* owner, const std::string& chunkname, const std::string& source,
            std::string* error) override {
    OwnerVm* vm = nullptr;
    {
      std::lock_guard<std::mutex> lock(map_mu_);
      std::unique_ptr<OwnerVm>& slot = states_[owner];
      if (slot) {
        Close(*slot);
        graveyard_.push_back(std::move(slot));  // chains may still hold its Binding pointers
      }
      slot.reset(new OwnerVm());
      vm = slot.get();
    }
    vm->registry = &registry_;
    vm->api = registry_.Api();
    vm->owner = owner;
    vm->limits = limits_;
    if (vm->limits.instructions_per_call < static_cast<uint64_t>(kHookStep)) {
      vm->step = static_cast<int>(vm->limits.instructions_per_call > 0 ? vm->limits.instructions_per_call : 1);
    }
    std::lock_guard<std::mutex> lock(vm->mu);
    vm->BeginCall();
    vm->L = lua_newstate(Alloc, vm);
    if (vm->L == nullptr) return Fail(*vm, "memory cap exceeded while creating the state", error, chunkname);
    lua_State* L = vm->L;
    lua_atpanic(L, PanicHandler);
    lua_sethook(L, Hook, LUA_MASKCOUNT, vm->step);

    lua_pushcfunction(L, Setup);
    int status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
      Outcome out = Classify(*vm, status, 0, "sandbox setup");
      if (out.breach == Breach::kNone) out.message = "sandbox setup failed: " + out.message;
      return Report(*vm, out, error, chunkname);
    }

    lua_pushcfunction(L, MsgHandler);
    const int handler = lua_gettop(L);
    const std::string luaname = "=" + chunkname;
    status = luaL_loadbufferx(L, source.data(), source.size(), luaname.c_str(), "t");  // text only
    if (status == LUA_OK) {
      lua_rawgetp(L, LUA_REGISTRYINDEX, &g_env_key);
      lua_setupvalue(L, -2, 1);  // the chunk's _ENV is the sandbox table
      vm->BeginCall();
      status = lua_pcall(L, 0, 0, handler);
    }
    const Outcome out = Classify(*vm, status, 0, "chunk " + chunkname);
    if (status == LUA_OK && out.breach == Breach::kNone) return true;
    return Report(*vm, out, error, chunkname);
  }

  size_t MemoryBytes(const NevrOwner* owner) const override {
    std::lock_guard<std::mutex> lock(map_mu_);
    const auto it = states_.find(owner);
    return it == states_.end() ? 0 : it->second->used;
  }

  void Unload(NevrOwner* owner) override {
    std::lock_guard<std::mutex> lock(map_mu_);
    const auto it = states_.find(owner);
    if (it != states_.end()) Close(*it->second);
  }

 private:
  // Closes the Lua state. The OwnerVm object stays (so a callback still registered in a
  // chain finds `closed` and returns) until the ScriptVm is destroyed.
  static void Close(OwnerVm& vm) {
    std::lock_guard<std::mutex> lock(vm.mu);
    if (vm.closed) return;
    vm.closed = true;
    if (vm.L != nullptr) {
      vm.BeginCall();
      lua_close(vm.L);
      vm.L = nullptr;
    }
  }

  bool Fail(OwnerVm& vm, const std::string& reason, std::string* error, const std::string& chunk) {
    vm.registry->DisableOwner(vm.owner, reason);
    if (error) *error = chunk + ": " + reason;
    return false;
  }

  bool Report(OwnerVm& vm, const Outcome& out, std::string* error, const std::string& chunk) {
    if (out.breach != Breach::kNone) vm.registry->DisableOwner(vm.owner, out.message);
    if (error) *error = out.breach != Breach::kNone ? chunk + ": " + out.message : out.message;
    return false;
  }

  Registry& registry_;
  VmLimits limits_;
  mutable std::mutex map_mu_;
  std::map<const NevrOwner*, std::unique_ptr<OwnerVm>> states_;
  std::vector<std::unique_ptr<OwnerVm>> graveyard_;
};

// The registry calls this on whatever thread fires the hook point.
NevrHookResult RunBinding(NevrHookCall* call, void* user) {
  Binding* b = static_cast<Binding*>(user);
  OwnerVm& vm = *b->vm;
  std::lock_guard<std::mutex> lock(vm.mu);
  if (vm.closed || vm.owner->disabled.load()) return NEVR_HOOK_CONTINUE;
  lua_State* L = vm.L;
  const int base = lua_gettop(L);
  vm.call = call;
  vm.phase = b->phase;
  vm.skip = false;
  vm.BeginCall();
  lua_pushcfunction(L, MsgHandler);
  lua_rawgeti(L, LUA_REGISTRYINDEX, b->ref);
  lua_rawgetp(L, LUA_REGISTRYINDEX, &g_call_key);
  const int status = lua_pcall(L, 1, 0, base + 1);
  vm.call = nullptr;
  const std::string where = std::string(b->phase == NEVR_HOOK_PRE ? "pre" : "post") + " callback on " +
                            vm.api->call_hook_name(call);
  const Outcome out = Classify(vm, status, base, where);
  if (out.breach != Breach::kNone) {
    vm.registry->DisableOwner(vm.owner, out.message);
    return vm.api->call_fail(call, out.message.c_str());
  }
  if (status != LUA_OK) return vm.api->call_fail(call, out.message.c_str());
  return vm.skip ? NEVR_HOOK_SKIP_ORIGINAL : NEVR_HOOK_CONTINUE;
}

}  // namespace

std::unique_ptr<ScriptVm> CreateScriptVm(Registry& registry, const VmLimits& limits) {
  return std::unique_ptr<ScriptVm>(new Lua54Vm(registry, limits));
}

}  // namespace nevr_script
