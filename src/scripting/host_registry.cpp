#include "scripting/host_registry.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace nevr_script {
namespace {

const char* StatusName(NevrStatus status) {
  switch (status) {
    case NEVR_OK: return "NEVR_OK";
    case NEVR_ERR_INVALID_ARG: return "NEVR_ERR_INVALID_ARG";
    case NEVR_ERR_CONFLICT: return "NEVR_ERR_CONFLICT";
    case NEVR_ERR_UNKNOWN_HOOK: return "NEVR_ERR_UNKNOWN_HOOK";
    case NEVR_ERR_UNKNOWN_FIELD: return "NEVR_ERR_UNKNOWN_FIELD";
    case NEVR_ERR_TYPE_MISMATCH: return "NEVR_ERR_TYPE_MISMATCH";
    case NEVR_ERR_READ_ONLY: return "NEVR_ERR_READ_ONLY";
    case NEVR_ERR_NOT_FOUND: return "NEVR_ERR_NOT_FOUND";
    case NEVR_ERR_DISABLED: return "NEVR_ERR_DISABLED";
    default: return "NEVR_ERR_UNKNOWN_STATUS";
  }
}

bool ValidValue(const NevrValue* v) {
  if (!v) return false;
  switch (v->type) {
    case NEVR_VALUE_BOOL:
    case NEVR_VALUE_INT:
    case NEVR_VALUE_FLOAT: return true;
    case NEVR_VALUE_STRING: return v->as.s != nullptr;
    default: return false;
  }
}

std::string Describe(const NevrValue& v) {
  switch (v.type) {
    case NEVR_VALUE_BOOL: return v.as.b ? "true" : "false";
    case NEVR_VALUE_INT: return std::to_string(v.as.i);
    case NEVR_VALUE_FLOAT: return std::to_string(v.as.f);
    case NEVR_VALUE_STRING: return std::string("\"") + v.as.s + "\"";
    default: return "none";
  }
}

// ---- the C ABI table: every entry dispatches through the owner's or call's registry ----

NevrStatus ApiOverrideSet(NevrOwner* owner, const char* key, const NevrValue* value) {
  if (!owner) return NEVR_ERR_INVALID_ARG;
  return owner->registry->OverrideSet(owner, key, value);
}

NevrStatus ApiOverrideGet(NevrOwner* owner, const char* key, NevrValue* out) {
  if (!owner) return NEVR_ERR_INVALID_ARG;
  return owner->registry->OverrideGet(owner, key, out);
}

NevrStatus ApiHookAdd(NevrOwner* owner, const char* hook, NevrHookPhase phase, NevrHookFn fn,
                      void* user) {
  if (!owner) return NEVR_ERR_INVALID_ARG;
  return owner->registry->HookAdd(owner, hook, phase, fn, user);
}

NevrStatus ApiCallGet(const NevrHookCall* call, const char* field, NevrValue* out) {
  if (!call || !field || !out) return NEVR_ERR_INVALID_ARG;
  const int index = call->hook->FieldIndex(field);
  if (index < 0) return NEVR_ERR_UNKNOWN_FIELD;
  *out = call->fields[index];
  return NEVR_OK;
}

NevrStatus ApiCallSet(NevrHookCall* call, const char* field, const NevrValue* value) {
  if (!call || !field || !ValidValue(value)) return NEVR_ERR_INVALID_ARG;
  const int index = call->hook->FieldIndex(field);
  if (index < 0) return NEVR_ERR_UNKNOWN_FIELD;
  const FieldSpec& spec = call->hook->Fields()[static_cast<size_t>(index)];
  if (spec.type != value->type) return NEVR_ERR_TYPE_MISMATCH;
  const bool writable = call->phase == NEVR_HOOK_PRE ? spec.pre_writable : spec.post_writable;
  if (!writable) return NEVR_ERR_READ_ONLY;
  NevrValue& slot = call->fields[index];
  if (value->type == NEVR_VALUE_STRING) {
    if (!call->strings) call->strings.reset(new std::string[call->hook->Fields().size()]);
    std::string& text = call->strings[static_cast<size_t>(index)];
    text = value->as.s;
    slot = *value;
    slot.as.s = text.c_str();
  } else {
    slot = *value;
  }
  return NEVR_OK;
}

const char* ApiCallHookName(const NevrHookCall* call) {
  return call ? call->hook->Name().c_str() : "";
}

NevrHookResult ApiCallFail(NevrHookCall* call, const char* reason) {
  if (call) call->fail_reason = reason ? reason : "";
  return NEVR_HOOK_FAILED;
}

void ApiLog(NevrOwner* owner, NevrLogLevel level, const char* message) {
  if (owner) owner->registry->OwnerLog(owner, level, message ? message : "");
}

const char* ApiLastError(NevrOwner* owner) {
  return owner ? owner->last_error.c_str() : "";
}

const NevrHostApi kApi = {
    sizeof(NevrHostApi), NEVR_HOST_API_VERSION,
    ApiOverrideSet, ApiOverrideGet,
    ApiHookAdd, ApiCallGet, ApiCallSet, ApiCallHookName, ApiCallFail,
    ApiLog, StatusName, ApiLastError,
};

}  // namespace

int HookPoint::FieldIndex(const char* field) const {
  for (size_t i = 0; i < fields_.size(); ++i) {
    if (fields_[i].name == field) return static_cast<int>(i);
  }
  return -1;
}

Registry::Registry(LogSink sink) : sink_(std::move(sink)) {}
Registry::~Registry() = default;

const NevrHostApi* Registry::Api() const { return &kApi; }

void Registry::Emit(NevrLogLevel level, const char* event, const NevrOwner* owner,
                    const char* other, const char* target, const std::string& detail) {
  if (!sink_) return;
  const LogRecord record{level, event, owner ? owner->name.c_str() : "", other ? other : "",
                         target ? target : "", detail.c_str()};
  sink_(record);
}

NevrStatus Registry::Fail(NevrOwner* owner, NevrStatus status, std::string why) {
  owner->last_error = std::move(why);
  return status;
}

NevrOwner* Registry::OpenOwner(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  auto owner = std::make_unique<NevrOwner>();
  owner->registry = this;
  owner->name = name;
  owner->order = static_cast<uint32_t>(owners_.size());
  owners_.push_back(std::move(owner));
  NevrOwner* opened = owners_.back().get();
  Emit(NEVR_LOG_DEBUG, "owner_opened", opened, nullptr, nullptr,
       "order " + std::to_string(opened->order));
  return opened;
}

size_t Registry::DropOwnerLocked(NevrOwner* owner) {
  size_t dropped = 0;
  for (auto it = overrides_.begin(); it != overrides_.end();) {
    if (it->second.owner == owner) {
      it = overrides_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  for (auto& entry : hooks_) {
    HookPoint& hook = *entry.second;
    for (std::shared_ptr<const HookPoint::Chain>* chain : {&hook.pre_, &hook.post_}) {
      auto next = std::make_shared<HookPoint::Chain>(**chain);
      const auto before = next->size();
      next->erase(std::remove_if(next->begin(), next->end(),
                                 [owner](const HookPoint::Callback& c) { return c.owner == owner; }),
                  next->end());
      if (next->size() != before) {
        dropped += before - next->size();
        std::atomic_store(chain, std::shared_ptr<const HookPoint::Chain>(std::move(next)));
      }
    }
  }
  return dropped;
}

void Registry::DisableOwner(NevrOwner* owner, const std::string& reason) {
  if (!owner || owner->disabled.exchange(true)) return;
  size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    dropped = DropOwnerLocked(owner);
  }
  Emit(NEVR_LOG_ERROR, "owner_disabled", owner, nullptr, nullptr,
       reason + " (" + std::to_string(dropped) + " override(s) and callback(s) removed)");
}

void Registry::ResetOwner(NevrOwner* owner) {
  if (!owner) return;
  size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    dropped = DropOwnerLocked(owner);
  }
  owner->disabled.store(false);
  owner->last_error.clear();
  Emit(NEVR_LOG_INFO, "owner_reset", owner, nullptr, nullptr,
       std::to_string(dropped) + " override(s) and callback(s) removed");
}

HookPoint* Registry::RegisterHookPoint(const std::string& name, std::vector<FieldSpec> fields) {
  std::lock_guard<std::mutex> lock(mu_);
  if (name.empty() || hooks_.count(name)) return nullptr;
  std::unique_ptr<HookPoint> hook(new HookPoint(name, std::move(fields)));
  HookPoint* raw = hook.get();
  hooks_.emplace(name, std::move(hook));
  return raw;
}

HookPoint* Registry::FindHookPoint(const char* name) const {
  if (!name) return nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = hooks_.find(name);
  return it == hooks_.end() ? nullptr : it->second.get();
}

NevrStatus Registry::OverrideSet(NevrOwner* owner, const char* key, const NevrValue* value) {
  if (owner->disabled.load()) return Fail(owner, NEVR_ERR_DISABLED, owner->name + " is disabled");
  if (!key || !*key || !ValidValue(value)) {
    return Fail(owner, NEVR_ERR_INVALID_ARG, "override needs a non-empty key and a typed value");
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = overrides_.find(key);
  if (it != overrides_.end() && it->second.owner != owner) {
    const NevrOwner* holder = it->second.owner;
    Emit(NEVR_LOG_ERROR, "override_conflict", owner, holder->name.c_str(), key,
         owner->name + " tried to set " + Describe(*value) + "; " + holder->name +
             " set it first and keeps it (" + Describe(it->second.value) + ")");
    return Fail(owner, NEVR_ERR_CONFLICT,
                std::string("override ") + key + " is already set by " + holder->name);
  }
  OverrideEntry& entry = overrides_[key];
  entry.owner = owner;
  entry.value = *value;
  if (value->type == NEVR_VALUE_STRING) {
    entry.text = value->as.s;
    entry.value.as.s = entry.text.c_str();
  }
  Emit(NEVR_LOG_INFO, "override_set", owner, nullptr, key, Describe(entry.value));
  return NEVR_OK;
}

NevrStatus Registry::OverrideGet(NevrOwner* owner, const char* key, NevrValue* out) {
  if (!key || !out) return Fail(owner, NEVR_ERR_INVALID_ARG, "override_get needs a key and an out value");
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = overrides_.find(key);
  if (it == overrides_.end()) return Fail(owner, NEVR_ERR_NOT_FOUND, std::string("no override for ") + key);
  *out = it->second.value;
  return NEVR_OK;
}

NevrStatus Registry::HookAdd(NevrOwner* owner, const char* hook, NevrHookPhase phase, NevrHookFn fn,
                             void* user) {
  if (owner->disabled.load()) return Fail(owner, NEVR_ERR_DISABLED, owner->name + " is disabled");
  if (!hook || !*hook || !fn || (phase != NEVR_HOOK_PRE && phase != NEVR_HOOK_POST)) {
    return Fail(owner, NEVR_ERR_INVALID_ARG, "hook_add needs a hook name, a phase and a function");
  }
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = hooks_.find(hook);
  if (it == hooks_.end()) {
    Emit(NEVR_LOG_WARNING, "hook_unknown", owner, nullptr, hook,
         "no hook point by this name on this build; the owner's feature on it stays off");
    return Fail(owner, NEVR_ERR_UNKNOWN_HOOK, std::string("no hook point named ") + hook);
  }
  HookPoint& point = *it->second;
  std::shared_ptr<const HookPoint::Chain>& chain = phase == NEVR_HOOK_PRE ? point.pre_ : point.post_;
  auto next = std::make_shared<HookPoint::Chain>(*chain);
  // Chained by owner order (plugins: order), then by registration within an owner.
  const auto pos = std::upper_bound(next->begin(), next->end(), owner->order,
                                    [](uint32_t order, const HookPoint::Callback& c) {
                                      return order < c.owner->order;
                                    });
  next->insert(pos, HookPoint::Callback{owner, fn, user});
  std::atomic_store(&chain, std::shared_ptr<const HookPoint::Chain>(std::move(next)));
  Emit(NEVR_LOG_INFO, "hook_added", owner, nullptr, hook, phase == NEVR_HOOK_PRE ? "pre" : "post");
  return NEVR_OK;
}

void Registry::OwnerLog(NevrOwner* owner, NevrLogLevel level, const char* message) {
  Emit(level, "owner_log", owner, nullptr, nullptr, message);
}

void Registry::RunChain(const HookPoint::Chain& chain, NevrHookCall& call) {
  for (const HookPoint::Callback& cb : chain) {
    if (cb.owner->disabled.load(std::memory_order_relaxed)) continue;
    call.current = cb.owner;
    call.fail_reason.clear();
    const NevrHookResult result = cb.fn(&call, cb.user);
    if (result == NEVR_HOOK_FAILED) {
      Emit(NEVR_LOG_ERROR, "callback_failed", cb.owner, nullptr, call.hook->Name().c_str(),
           std::string(call.phase == NEVR_HOOK_PRE ? "pre: " : "post: ") +
               (call.fail_reason.empty() ? "no reason given" : call.fail_reason));
    } else if (result == NEVR_HOOK_SKIP_ORIGINAL && call.phase == NEVR_HOOK_PRE) {
      call.skip_original = true;
    }
  }
  call.current = nullptr;
}

void Registry::Invoke(const HookPoint* hook, NevrValue* fields, OriginalFn original, void* ctx) {
  const std::shared_ptr<const HookPoint::Chain> pre = std::atomic_load(&hook->pre_);
  const std::shared_ptr<const HookPoint::Chain> post = std::atomic_load(&hook->post_);
  if (pre->empty() && post->empty()) {
    original(fields, ctx);
    return;
  }
  NevrHookCall call{hook, fields, nullptr, NEVR_HOOK_PRE, nullptr, false, {}};
  RunChain(*pre, call);
  if (!call.skip_original) original(fields, ctx);
  call.phase = NEVR_HOOK_POST;
  RunChain(*post, call);
}

}  // namespace nevr_script
