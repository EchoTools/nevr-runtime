/*
 * host_api.h - the nEVR host API: data overrides and named hook points.
 *
 * A plain C ABI over named things. A native plugin and a script binding (Lua)
 * call the same function table, so the binding wraps it one to one (#440).
 * Nothing here is a pointer into the game: a hook callback reads and writes the
 * call's named fields, and an override is a key with a typed value.
 *
 *   Data overrides. An owner sets `key = value`. The keys are override points
 *   the runtime registers, each with a type and the engine value(s) it maps to
 *   on this build; the runtime owns the hook on the engine's reader and applies
 *   the effective value. A number is accepted for an INT key when it is
 *   integral and for a FLOAT key either way. The first
 *   owner to set a key keeps it (owners are registered in `plugins:` order); a
 *   second owner setting the same key gets NEVR_ERR_CONFLICT and the host logs
 *   one line naming both.
 *
 *   Hook points. The runtime installs the only detour on a named function and
 *   exposes it as a hook point with declared fields. Owners add pre and post
 *   callbacks; they run in registration order (`plugins:` order). A pre
 *   callback may set fields the hook point declares writable before the call
 *   and may ask to skip the original; a post callback may set the fields
 *   declared writable after it ("result"). No owner installs a detour of its
 *   own, so two owners on one function chain instead of colliding.
 *
 * Ownership: the host issues one NevrOwner per plugin or script. Every call
 * that changes state takes it, so the host can name who did what, remove all
 * of an owner's overrides and callbacks at once (disable, hot reload), and a
 * script cannot act under another owner's name.
 *
 * Threading: overrides and callbacks are added on the thread that loads
 * plugins. Hook points may be invoked from any game thread; a callback runs on
 * the invoking thread. NevrHookCall is valid only inside the callback it was
 * passed to.
 *
 * Versioning: a consumer checks api->size >= the size it was compiled against
 * before using a field, and api->version >= NEVR_HOST_API_VERSION for
 * semantics. Fields are only ever appended.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEVR_HOST_API_VERSION 1

typedef int32_t NevrStatus;
enum {
    NEVR_OK                 = 0,
    NEVR_ERR_INVALID_ARG    = 1, /* null or empty name, unknown phase, NONE value */
    NEVR_ERR_CONFLICT       = 2, /* another owner already set this override key */
    NEVR_ERR_UNKNOWN_HOOK   = 3, /* no hook point by that name on this build */
    NEVR_ERR_UNKNOWN_FIELD  = 4, /* the hook point declares no field by that name */
    NEVR_ERR_TYPE_MISMATCH  = 5, /* value type differs from the field's declared type */
    NEVR_ERR_READ_ONLY      = 6, /* the field is not writable in this phase */
    NEVR_ERR_NOT_FOUND      = 7, /* no override for this key */
    NEVR_ERR_DISABLED       = 8, /* the owner was disabled (budget breach, error policy) */
    NEVR_ERR_UNDECLARED     = 9, /* the owner's manifest does not declare this hook or key */
    NEVR_ERR_UNKNOWN_KEY    = 10 /* no override point by that name on this build */
};

typedef int32_t NevrValueType;
enum {
    NEVR_VALUE_NONE   = 0,
    NEVR_VALUE_BOOL   = 1,
    NEVR_VALUE_INT    = 2,
    NEVR_VALUE_FLOAT  = 3,
    NEVR_VALUE_STRING = 4
};

/* A typed value. On set, the host copies a STRING. On get, a STRING points
 * into host storage and stays valid until that key or field next changes, or
 * until the callback returns, whichever is first. */
typedef struct NevrValue {
    NevrValueType type;
    int32_t       reserved; /* zero */
    union {
        int32_t     b;
        int64_t     i;
        double      f;
        const char* s;
    } as;
} NevrValue;

typedef int32_t NevrHookPhase;
enum {
    NEVR_HOOK_PRE  = 0,
    NEVR_HOOK_POST = 1
};

/* What a callback returns. */
typedef int32_t NevrHookResult;
enum {
    NEVR_HOOK_CONTINUE      = 0,
    NEVR_HOOK_SKIP_ORIGINAL = 1, /* pre only: the original is not called; every pre
                                  * callback still runs, then the post chain */
    NEVR_HOOK_FAILED        = 2  /* the callback failed; the host logs it and
                                  * carries on as if it returned CONTINUE */
};

typedef int32_t NevrLogLevel;
enum {
    NEVR_LOG_DEBUG   = 0,
    NEVR_LOG_INFO    = 1,
    NEVR_LOG_WARNING = 2,
    NEVR_LOG_ERROR   = 3
};

typedef struct NevrOwner    NevrOwner;    /* issued by the host, one per plugin or script */
typedef struct NevrHookCall NevrHookCall; /* one invocation of a hook point */

typedef NevrHookResult (*NevrHookFn)(NevrHookCall* call, void* user);

typedef struct NevrHostApi {
    uint32_t size;    /* sizeof(NevrHostApi) as the host built it */
    uint32_t version; /* NEVR_HOST_API_VERSION of the host */

    /* Data overrides. */
    NevrStatus (*override_set)(NevrOwner* owner, const char* key, const NevrValue* value);
    /* The effective value of `key`, whoever set it. */
    NevrStatus (*override_get)(NevrOwner* owner, const char* key, NevrValue* out);

    /* Hook points. `user` is passed back to `fn` untouched. */
    NevrStatus (*hook_add)(NevrOwner* owner, const char* hook, NevrHookPhase phase,
                           NevrHookFn fn, void* user);
    NevrStatus (*call_get)(const NevrHookCall* call, const char* field, NevrValue* out);
    NevrStatus (*call_set)(NevrHookCall* call, const char* field, const NevrValue* value);
    /* The hook point this call belongs to. */
    const char* (*call_hook_name)(const NevrHookCall* call);
    /* Record why the running callback failed and return NEVR_HOOK_FAILED, so a
     * callback ends with `return api->call_fail(call, "why");`. The host logs one
     * callback_failed line carrying the reason. */
    NevrHookResult (*call_fail)(NevrHookCall* call, const char* reason);

    /* One log line attributed to the owner. */
    void (*log)(NevrOwner* owner, NevrLogLevel level, const char* message);

    /* A stable name for a status ("NEVR_ERR_CONFLICT"). */
    const char* (*status_name)(NevrStatus status);
    /* Why the owner's last failed call failed, in words (names the other owner
     * on a conflict). Valid until the owner's next call. Never null. */
    const char* (*last_error)(NevrOwner* owner);
} NevrHostApi;

#ifdef __cplusplus
}
#endif
