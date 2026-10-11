// LuaJIT 2.1 binding of the ScriptVm seam (src/scripting/script_vm.h).
//
// One lua_State per owner, created with lua_newstate and the binding's own
// allocator: that is the memory cap, and it is possible on x64 and arm64 because
// both are LJ_GC64 targets (extern/luajit/src/lj_arch.h, LJ_TARGET_GC64; on a
// non-GC64 64-bit target lua_newstate refuses a custom allocator, lib_aux.c).
//
// Errors. Lua errors cross this file's C++ frames by upstream's external frame
// unwinding on both targets (lj_err.c, "EXT": LJ_ABI_WIN, and the Makefile's
// unwind-table probe for Android), so C++ destructors run when a Lua error passes
// through. The binding does not rely on that: every function that raises a Lua error
// holds only trivial locals at the point it raises, every call into Lua is a
// lua_pcall or lua_cpcall, and C++ exceptions never leave a callback (the
// trampoline catches std::exception).
#include "scripting/vm/luajit/luajit_vm.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lua.hpp"  // lua.h, lualib.h, lauxlib.h and the generated luajit.h, in extern "C"

namespace nevr_script {
namespace {

// Bytecodes between two calls of the budget hook.
constexpr int kStride = 1000;

struct Ctx;
NevrHookResult Trampoline(NevrHookCall* call, void* user);

struct CallbackRef {
  Ctx* ctx;
  int ref;  // registry reference of the Lua function
  NevrHookPhase phase;
};

#ifdef NEVR_LUAJIT_WATCHDOG
class Watchdog;
#endif

// Everything one owner's state needs. Reached from any lua_State of the state
// through the allocator's user data (lua_getallocf), so no upvalues are needed.
struct Ctx {
  Registry* registry = nullptr;
  const NevrHostApi* api = nullptr;
  VmLimits limits;
  NevrOwner* owner = nullptr;
  lua_State* L = nullptr;
  std::recursive_mutex mu;  // a state is not thread safe; hook points may fire on any game thread

  // Allocator accounting.
  size_t mem_used = 0;
  bool mem_refused = false;

  // Budget of the outermost call in progress.
  int depth = 0;
  uint64_t instructions = 0;
  std::chrono::steady_clock::time_point deadline;
  bool breached = false;
  std::string breach;
  bool budget_hook = true;

  // The callback in progress.
  NevrHookCall* call = nullptr;
  NevrHookPhase phase = NEVR_HOOK_PRE;
  bool skip = false;

  std::vector<std::unique_ptr<CallbackRef>> callbacks;
#ifdef NEVR_LUAJIT_WATCHDOG
  Watchdog* watchdog = nullptr;
#endif
};

Ctx* CtxOf(lua_State* L) {
  void* ud = nullptr;
  lua_getallocf(L, &ud);
  return static_cast<Ctx*>(ud);
}

// ---- allocator: the per-state memory cap ------------------------------------------------------

void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  Ctx* c = static_cast<Ctx*>(ud);
  const size_t old = ptr ? osize : 0;  // osize is a type tag when ptr is null
  if (nsize == 0) {
    std::free(ptr);
    c->mem_used -= old;
    return nullptr;
  }
  if (nsize > old && c->mem_used - old + nsize > c->limits.memory_bytes) {
    c->mem_refused = true;
    return nullptr;
  }
  void* block = std::realloc(ptr, nsize);
  if (!block) {
    c->mem_refused = true;
    return nullptr;
  }
  c->mem_used = c->mem_used - old + nsize;
  return block;
}

// ---- budget -----------------------------------------------------------------------------------

std::string MemoryReason(const Ctx* c) {
  return "memory limit of " + std::to_string(c->limits.memory_bytes) + " bytes exceeded";
}

// Called from the count hook (and the watchdog's hook). Raises a Lua error once the budget is gone;
// it keeps raising on every later tick (the count is 1 by then), so a script that catches the error
// with pcall cannot keep running.
void BudgetHook(lua_State* L, lua_Debug*) {
  Ctx* c = CtxOf(L);
#ifndef NEVR_LUAJIT_WATCHDOG
  if (!c->breached) {
    c->instructions += static_cast<uint64_t>(kStride);
    if (c->instructions > c->limits.instructions_per_call) {
      c->breached = true;
      c->breach = "instruction budget of " + std::to_string(c->limits.instructions_per_call) + " exceeded";
    } else if (std::chrono::steady_clock::now() > c->deadline) {
      c->breached = true;
      c->breach = "time budget of " + std::to_string(c->limits.millis_per_call) + " ms exceeded";
    } else if (c->mem_refused) {
      c->breached = true;
      c->breach = MemoryReason(c);
    }
    if (c->breached) lua_sethook(L, BudgetHook, LUA_MASKCOUNT, 1);
  }
#endif
  if (c->breached) luaL_error(L, "%s", c->breach.c_str());
}

#ifdef NEVR_LUAJIT_WATCHDOG
// Experiment: no hook while the script runs. One thread sleeps until the earliest armed deadline and
// then installs a count-1 hook from outside the VM thread; lua_sethook is documented as callable
// asynchronously (lj_dispatch.c, "This function can be called asynchronously"), and a LuaJIT built
// with LUAJIT_ENABLE_CHECKHOOK makes compiled loops notice it (lj_record.c, "Regularly check for
// instruction/line hooks from compiled code").
class Watchdog {
 public:
  Watchdog() : thread_([this] { Run(); }) {}
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  void Arm(Ctx* c) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      armed_.push_back(c);
    }
    cv_.notify_all();
  }
  void Disarm(Ctx* c) {
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < armed_.size(); ++i) {
      if (armed_[i] == c) {
        armed_.erase(armed_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
      if (armed_.empty()) {
        cv_.wait(lock);
        continue;
      }
      auto next = armed_[0]->deadline;
      for (Ctx* c : armed_) {
        if (c->deadline < next) next = c->deadline;
      }
      if (cv_.wait_until(lock, next) == std::cv_status::timeout) {
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < armed_.size();) {
          Ctx* c = armed_[i];
          if (c->deadline <= now) {
            c->breach = "time budget of " + std::to_string(c->limits.millis_per_call) + " ms exceeded";
            c->breached = true;
            lua_sethook(c->L, BudgetHook, LUA_MASKCOUNT, 1);
            armed_.erase(armed_.begin() + static_cast<std::ptrdiff_t>(i));
          } else {
            ++i;
          }
        }
      }
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Ctx*> armed_;
  bool stop_ = false;
  std::thread thread_;
};
#endif

void BeginCall(Ctx* c) {
  if (c->depth++ > 0) return;
  c->instructions = 0;
  c->breached = false;
  c->breach.clear();
  c->mem_refused = false;
  c->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c->limits.millis_per_call);
#ifdef NEVR_LUAJIT_WATCHDOG
  c->watchdog->Arm(c);
#endif
}

// Ends the outermost call. `rc` is what lua_pcall returned; on rc != 0 the error value is on the
// stack and is popped here. Returns true when the call completed within every limit.
bool EndCall(Ctx* c, int rc, std::string* error) {
  std::string message;
  if (rc != 0) {
    lua_State* L = c->L;
    if (lua_type(L, -1) == LUA_TSTRING) {
      message = lua_tostring(L, -1);
    } else {
      message = std::string("(error object is a ") + luaL_typename(L, -1) + " value)";
    }
    lua_pop(L, 1);
  }
  if (--c->depth > 0) {
    if (rc != 0 && error) *error = message;
    return rc == 0;
  }
#ifdef NEVR_LUAJIT_WATCHDOG
  c->watchdog->Disarm(c);
  if (c->breached) lua_sethook(c->L, nullptr, 0, 0);
#endif
  std::string reason;
  if (c->breached) {
    reason = c->breach;
  } else if (c->mem_refused) {
    reason = MemoryReason(c);
  }
  if (!reason.empty()) {
    c->registry->DisableOwner(c->owner, reason);
    if (error) *error = message.empty() ? reason : message;
    return false;
  }
  if (rc != 0 && error) *error = message;
  return rc == 0;
}

// ---- script-visible functions -----------------------------------------------------------------
// None of these keeps a std::string alive across a call that can raise.

int LNevrOverride(lua_State* L) {
  Ctx* c = CtxOf(L);
  const char* key = luaL_checkstring(L, 1);
  NevrValue v{};
  switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
      v.type = NEVR_VALUE_BOOL;
      v.as.b = lua_toboolean(L, 2) ? 1 : 0;
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
      return luaL_argerror(L, 2, "boolean, number or string expected");
  }
  const NevrStatus st = c->api->override_set(c->owner, key, &v);
  if (st == NEVR_OK) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushnil(L);
  lua_pushfstring(L, "%s: %s", c->api->status_name(st), c->api->last_error(c->owner));
  return 2;
}

int LNevrHook(lua_State* L) {
  Ctx* c = CtxOf(L);
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  static const char* const kPhaseKeys[] = {"pre", "post"};
  static const NevrHookPhase kPhases[] = {NEVR_HOOK_PRE, NEVR_HOOK_POST};
  for (int i = 0; i < 2; ++i) {
    lua_getfield(L, 2, kPhaseKeys[i]);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    if (lua_type(L, -1) != LUA_TFUNCTION) {
      return luaL_error(L, "nevr.hook: '%s' must be a function", kPhaseKeys[i]);
    }
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the function
    c->callbacks.push_back(std::unique_ptr<CallbackRef>(new CallbackRef{c, ref, kPhases[i]}));
    CallbackRef* cb = c->callbacks.back().get();
    const NevrStatus st = c->api->hook_add(c->owner, name, kPhases[i], Trampoline, cb);
    if (st != NEVR_OK) {
      luaL_unref(L, LUA_REGISTRYINDEX, ref);
      c->callbacks.pop_back();
      lua_pushnil(L);
      lua_pushfstring(L, "%s: %s", c->api->status_name(st), c->api->last_error(c->owner));
      return 2;
    }
  }
  lua_pushboolean(L, 1);
  return 1;
}

int LNevrLog(lua_State* L) {
  Ctx* c = CtxOf(L);
  const char* level = luaL_checkstring(L, 1);
  const char* message = luaL_checkstring(L, 2);
  NevrLogLevel lv = NEVR_LOG_INFO;
  if (std::strcmp(level, "debug") == 0) {
    lv = NEVR_LOG_DEBUG;
  } else if (std::strcmp(level, "info") == 0) {
    lv = NEVR_LOG_INFO;
  } else if (std::strcmp(level, "warn") == 0) {
    lv = NEVR_LOG_WARNING;
  } else if (std::strcmp(level, "error") == 0) {
    lv = NEVR_LOG_ERROR;
  } else {
    return luaL_argerror(L, 1, "level must be debug, info, warn or error");
  }
  c->api->log(c->owner, lv, message);
  return 0;
}

// print(...) = nevr.log("info", <args joined by tabs>), through the tostring saved at start.
int LPrint(lua_State* L) {
  Ctx* c = CtxOf(L);
  const int n = lua_gettop(L);
  if (n == 0) {
    c->api->log(c->owner, NEVR_LOG_INFO, "");
    return 0;
  }
  for (int i = 1; i <= n; ++i) {
    if (i > 1) lua_pushliteral(L, "\t");
    lua_getfield(L, LUA_REGISTRYINDEX, "nevr.tostring");
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "'tostring' must return a string to 'print'");
  }
  lua_concat(L, n * 2 - 1);
  c->api->log(c->owner, NEVR_LOG_INFO, lua_tostring(L, -1));
  return 0;
}

int LCollectGarbage(lua_State* L) {
  const char* what = luaL_optstring(L, 1, "collect");
  if (std::strcmp(what, "count") != 0) return luaL_error(L, "collectgarbage: only \"count\" is available");
  lua_pushnumber(L, static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNT, 0)) +
                        static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNTB, 0)) / 1024.0);
  return 1;
}

int RaiseStatus(lua_State* L, const Ctx* c, NevrStatus st, const char* what, const char* field) {
  return luaL_error(L, "%s: cannot %s field '%s'", c->api->status_name(st), what, field);
}

int LCallGet(lua_State* L) {
  Ctx* c = CtxOf(L);
  const char* field = luaL_checkstring(L, 2);
  if (!c->call) return luaL_error(L, "h used outside its callback");
  NevrValue v{};
  const NevrStatus st = c->api->call_get(c->call, field, &v);
  if (st != NEVR_OK) return RaiseStatus(L, c, st, "get", field);
  switch (v.type) {
    case NEVR_VALUE_BOOL: lua_pushboolean(L, v.as.b != 0); break;
    case NEVR_VALUE_INT: lua_pushnumber(L, static_cast<lua_Number>(v.as.i)); break;
    case NEVR_VALUE_FLOAT: lua_pushnumber(L, v.as.f); break;
    case NEVR_VALUE_STRING: lua_pushstring(L, v.as.s); break;
    default: return luaL_error(L, "field '%s' has an unsupported type", field);
  }
  return 1;
}

int LCallSet(lua_State* L) {
  Ctx* c = CtxOf(L);
  const char* field = luaL_checkstring(L, 2);
  if (!c->call) return luaL_error(L, "h used outside its callback");
  NevrValue current{};
  NevrStatus st = c->api->call_get(c->call, field, &current);
  if (st != NEVR_OK) return RaiseStatus(L, c, st, "set", field);
  NevrValue v{};
  switch (lua_type(L, 3)) {
    case LUA_TBOOLEAN:
      v.type = NEVR_VALUE_BOOL;
      v.as.b = lua_toboolean(L, 3) ? 1 : 0;
      break;
    case LUA_TNUMBER: {
      const lua_Number d = lua_tonumber(L, 3);
      if (current.type == NEVR_VALUE_INT) {
        if (!(d == std::floor(d)) || d < -9.2e18 || d > 9.2e18) {
          return luaL_error(L, "%s: field '%s' is an integer; %f is not integral",
                            c->api->status_name(NEVR_ERR_TYPE_MISMATCH), field, d);
        }
        v.type = NEVR_VALUE_INT;
        v.as.i = static_cast<int64_t>(d);
      } else {
        v.type = NEVR_VALUE_FLOAT;
        v.as.f = d;
      }
      break;
    }
    case LUA_TSTRING:
      v.type = NEVR_VALUE_STRING;
      v.as.s = lua_tostring(L, 3);
      break;
    default:
      return luaL_argerror(L, 3, "boolean, number or string expected");
  }
  st = c->api->call_set(c->call, field, &v);
  if (st != NEVR_OK) return RaiseStatus(L, c, st, "set", field);
  return 0;
}

int LCallSkip(lua_State* L) {
  Ctx* c = CtxOf(L);
  if (!c->call) return luaL_error(L, "h used outside its callback");
  if (c->phase != NEVR_HOOK_PRE) return luaL_error(L, "h:skip() is only valid in a pre callback");
  c->skip = true;
  return 0;
}

// ---- hook point trampoline --------------------------------------------------------------------

NevrHookResult Trampoline(NevrHookCall* call, void* user) {
  CallbackRef* cb = static_cast<CallbackRef*>(user);
  Ctx* c = cb->ctx;
  std::lock_guard<std::recursive_mutex> lock(c->mu);
  if (!c->L || c->owner->disabled.load()) return NEVR_HOOK_CONTINUE;
  try {
    lua_State* L = c->L;
    NevrHookCall* const saved_call = c->call;
    const NevrHookPhase saved_phase = c->phase;
    const bool saved_skip = c->skip;
    c->call = call;
    c->phase = cb->phase;
    c->skip = false;
    BeginCall(c);
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    lua_getfield(L, LUA_REGISTRYINDEX, "nevr.h");
    const int rc = lua_pcall(L, 1, 0, 0);
    std::string error;
    const bool ok = EndCall(c, rc, &error);
    const bool skip = c->skip;
    c->call = saved_call;
    c->phase = saved_phase;
    c->skip = saved_skip;
    if (!ok) return c->api->call_fail(call, error.c_str());
    return skip ? NEVR_HOOK_SKIP_ORIGINAL : NEVR_HOOK_CONTINUE;
  } catch (const std::exception& e) {
    return c->api->call_fail(call, e.what());
  }
}

// ---- state construction -----------------------------------------------------------------------

void OpenLib(lua_State* L, lua_CFunction open, const char* name) {
  lua_pushcfunction(L, open);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

// Sets to nil every field of the table at stack index `t` (positive) whose name is not in `keep`.
void KeepOnly(lua_State* L, int t, std::initializer_list<const char*> keep) {
  lua_pushnil(L);
  while (lua_next(L, t) != 0) {
    lua_pop(L, 1);  // the value; the key stays for lua_next
    bool wanted = false;
    if (lua_type(L, -1) == LUA_TSTRING) {
      const char* name = lua_tostring(L, -1);
      for (const char* k : keep) wanted = wanted || std::strcmp(k, name) == 0;
    }
    if (!wanted) {
      lua_pushvalue(L, -1);
      lua_pushnil(L);
      lua_rawset(L, t);
    }
  }
}

void KeepOnlyIn(lua_State* L, const char* global, std::initializer_list<const char*> keep) {
  lua_getglobal(L, global);
  KeepOnly(L, lua_gettop(L), keep);
  lua_pop(L, 1);
}

// Runs under lua_cpcall; argument 1 is the Ctx.
int InitState(lua_State* L) {
  Ctx* c = static_cast<Ctx*>(lua_touserdata(L, 1));
  lua_settop(L, 0);

  OpenLib(L, luaopen_base, "");
  OpenLib(L, luaopen_table, LUA_TABLIBNAME);
  OpenLib(L, luaopen_string, LUA_STRLIBNAME);
  OpenLib(L, luaopen_math, LUA_MATHLIBNAME);
  OpenLib(L, luaopen_os, LUA_OSLIBNAME);
  OpenLib(L, luaopen_debug, LUA_DBLIBNAME);
  OpenLib(L, luaopen_jit, LUA_JITLIBNAME);  // starts the JIT engine; the global is removed below
#ifdef NEVR_LUAJIT_JIT_OFF
  luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
#endif

  // Host-only copies, kept in the registry (scripts have no debug.getregistry).
  lua_getglobal(L, "tostring");
  lua_setfield(L, LUA_REGISTRYINDEX, "nevr.tostring");
  lua_getglobal(L, "jit");
  lua_setfield(L, LUA_REGISTRYINDEX, "nevr.jit");

  // The standard library, cut down to the safe subset of script_vm.h.
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  KeepOnly(L, lua_gettop(L),
           {"assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
            "rawset", "select", "setmetatable", "tonumber", "tostring", "type", "unpack", "xpcall", "_G",
            "_VERSION", "string", "table", "math", "coroutine", "os", "debug"});
  lua_pop(L, 1);
  KeepOnlyIn(L, "string", {"byte", "char", "find", "format", "gmatch", "gsub", "len", "lower", "match", "rep",
                           "reverse", "sub", "upper"});
  KeepOnlyIn(L, "table", {"concat", "insert", "maxn", "remove", "sort"});
  KeepOnlyIn(L, "os", {"clock", "time", "date"});
  KeepOnlyIn(L, "debug", {"traceback"});

  // getmetatable('') must not hand out the string metatable.
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1)) {
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  lua_pushcfunction(L, LCollectGarbage);
  lua_setglobal(L, "collectgarbage");
  lua_pushcfunction(L, LPrint);
  lua_setglobal(L, "print");

  lua_createtable(L, 0, 3);
  lua_pushcfunction(L, LNevrOverride);
  lua_setfield(L, -2, "override");
  lua_pushcfunction(L, LNevrHook);
  lua_setfield(L, -2, "hook");
  lua_pushcfunction(L, LNevrLog);
  lua_setfield(L, -2, "log");
  lua_setglobal(L, "nevr");

  lua_createtable(L, 0, 3);
  lua_pushcfunction(L, LCallGet);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, LCallSet);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, LCallSkip);
  lua_setfield(L, -2, "skip");
  lua_setfield(L, LUA_REGISTRYINDEX, "nevr.h");

#ifndef NEVR_LUAJIT_WATCHDOG
  if (c->budget_hook) lua_sethook(L, BudgetHook, LUA_MASKCOUNT, kStride);
#else
  (void)c;
#endif
  return 0;
}

int PanicHandler(lua_State*) { return 0; }

class LuajitVm final : public ScriptVm {
 public:
  LuajitVm(Registry& registry, const VmLimits& limits, bool budget_hook)
      : registry_(registry), limits_(limits), budget_hook_(budget_hook) {
    name_ = std::string(LUAJIT_VERSION);
#ifdef NEVR_LUAJIT_JIT_OFF
    name_ += " (jit off)";
#endif
#ifdef NEVR_LUAJIT_WATCHDOG
    name_ += " (watchdog)";
    watchdog_.reset(new Watchdog());
#endif
  }
  ~LuajitVm() override {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& entry : live_) Close(entry.second.get());
  }

  const char* Name() const override { return name_.c_str(); }

  bool Load(NevrOwner* owner, const std::string& chunkname, const std::string& source,
            std::string* error) override {
    Unload(owner);
    std::unique_ptr<Ctx> ctx(new Ctx());
    Ctx* c = ctx.get();
    c->registry = &registry_;
    c->api = registry_.Api();
    c->limits = limits_;
    c->owner = owner;
    c->budget_hook = budget_hook_;
#ifdef NEVR_LUAJIT_WATCHDOG
    c->watchdog = watchdog_.get();
#endif
    {
      std::lock_guard<std::mutex> lock(mu_);
      live_[owner] = std::move(ctx);
    }
    std::lock_guard<std::recursive_mutex> lock(c->mu);
    c->L = lua_newstate(Alloc, c);
    if (!c->L) {
      registry_.DisableOwner(owner, MemoryReason(c));
      Fail(error, chunkname, "not enough memory to create the state");
      return false;
    }
    lua_atpanic(c->L, PanicHandler);
    if (lua_cpcall(c->L, InitState, c) != 0) {
      const std::string reason = c->mem_refused ? MemoryReason(c) : std::string("state setup failed");
      if (c->mem_refused) registry_.DisableOwner(owner, reason);
      Fail(error, chunkname, reason);
      return false;
    }
    lua_State* L = c->L;
    const std::string name = "=" + chunkname;
    int rc = luaL_loadbuffer(L, source.data(), source.size(), name.c_str());
    if (rc != 0) {
      std::string message = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "load failed";
      lua_pop(L, 1);
      if (c->mem_refused) {
        registry_.DisableOwner(owner, MemoryReason(c));
      }
      if (error) *error = message;
      return false;
    }
    BeginCall(c);
    rc = lua_pcall(L, 0, 0, 0);
    return EndCall(c, rc, error);
  }

  size_t MemoryBytes(const NevrOwner* owner) const override {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = live_.find(owner);
    return it == live_.end() ? 0 : it->second->mem_used;
  }

  void Unload(NevrOwner* owner) override {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = live_.find(owner);
    if (it == live_.end()) return;
    Close(it->second.get());
    // The registry may still hold callbacks that point at this Ctx until the caller resets the
    // owner, so the Ctx (now without a state) outlives the state.
    retired_.push_back(std::move(it->second));
    live_.erase(it);
  }

  LuajitInspection Inspect(const NevrOwner* owner) {
    LuajitInspection out;
    Ctx* c = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = live_.find(owner);
      if (it != live_.end()) c = it->second.get();
    }
    if (!c) {
      out.error = "no state for owner";
      return out;
    }
    std::lock_guard<std::recursive_mutex> lock(c->mu);
    lua_State* L = c->L;
    static const char kScript[] =
        "local jit, loader = ...\n"
        "local util = loader()\n"
        "local n = 0\n"
        "for i = 1, 1000 do if pcall(util.traceinfo, i) then n = n + 1 end end\n"
        "return (jit.status()), n\n";
    if (luaL_loadbuffer(L, kScript, sizeof(kScript) - 1, "=inspect") != 0) {
      out.error = lua_tostring(L, -1);
      lua_pop(L, 1);
      return out;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "nevr.jit");
    lua_getfield(L, LUA_REGISTRYINDEX, "_PRELOAD");
    lua_getfield(L, -1, "jit.util");
    lua_remove(L, -2);
    BeginCall(c);
    const int rc = lua_pcall(L, 2, 2, 0);
    if (rc == 0) {
      out.jit_on = lua_toboolean(L, -2) != 0;
      out.traces = static_cast<int>(lua_tonumber(L, -1));
      lua_pop(L, 2);
      out.ok = true;
    }
    std::string error;
    const bool finished = EndCall(c, rc, &error);
    if (!finished) out.error = error;
    return out;
  }

 private:
  static void Fail(std::string* error, const std::string& chunk, const std::string& why) {
    if (error) *error = chunk + ": " + why;
  }
  static void Close(Ctx* c) {
    std::lock_guard<std::recursive_mutex> lock(c->mu);
    if (c->L) {
      lua_close(c->L);
      c->L = nullptr;
    }
  }

  Registry& registry_;
  VmLimits limits_;
  bool budget_hook_;
  std::string name_;
  mutable std::mutex mu_;
  std::map<const NevrOwner*, std::unique_ptr<Ctx>> live_;
  std::vector<std::unique_ptr<Ctx>> retired_;
#ifdef NEVR_LUAJIT_WATCHDOG
  std::unique_ptr<Watchdog> watchdog_;
#endif
};

}  // namespace

std::unique_ptr<ScriptVm> CreateLuajitVm(Registry& registry, const VmLimits& limits, bool budget_hook) {
  return std::unique_ptr<ScriptVm>(new LuajitVm(registry, limits, budget_hook));
}

std::unique_ptr<ScriptVm> CreateScriptVm(Registry& registry, const VmLimits& limits) {
  return CreateLuajitVm(registry, limits, true);
}

LuajitInspection InspectLuajit(ScriptVm& vm, const NevrOwner* owner) {
  LuajitInspection none;
  none.error = "not a LuaJIT VM";
  LuajitVm* lj = dynamic_cast<LuajitVm*>(&vm);
  return lj ? lj->Inspect(owner) : none;
}

}  // namespace nevr_script
