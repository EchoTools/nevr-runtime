#pragma once
// The types of the login-prerequisite hooks' thunks (login_prerequisites.h): 4 callbacks, 8 accessors,
// 4 requests. They live in a header so login_counters.cpp (buildable on the host) registers the same
// CallbackThunk instantiations login_prerequisites_install.cpp installs. The records and handlers stay in
// that file.

#include <cstdint>

#include "quest/login/login_prerequisites.h"
#include "quest/sentinel/callback_thunk.h"

namespace nevr_quest_login {

// Callbacks: void (SCallbacks* / CNSOVRUser*, ovrMessage*). The mailbox proxies (0x2089d0, 0x208be0) call
// the registered member pointer with the object in x0 and the message in x1.
template <Prerequisite P>
struct CallbackTag {};
template <Prerequisite P>
using CallbackThunk = sentinel::CallbackThunk<CallbackTag<P>, void(void*, void*)>;

using OrgCallbackThunk = CallbackThunk<Prerequisite::OrgScopedId>;
using UserCallbackThunk = CallbackThunk<Prerequisite::LoggedInUser>;
using TokenCallbackThunk = CallbackThunk<Prerequisite::AccessToken>;
using ProofCallbackThunk = CallbackThunk<Prerequisite::UserProof>;

// Accessors. Types from the callers in libpnsovr: IsError is tested with `tbz w0,#0` (0x1ece7c); the
// handle accessors feed x0 straight into the next accessor (0x1ecf08-0x1ecf0c, 0x1ed0f0-0x1ed0f4,
// 0x1ed70c-0x1ed710); GetID's x0 is stored as a 64-bit id (0x1ecf18); the strings are read as char const*
// (0x1ed330-0x1ed35c, 0x1ed0f4-0x1ed114, 0x1ed710-0x1ed91c).
struct IsErrorTag {};
struct GetStringTag {};
struct GetOrgScopedIdTag {};
struct OrgScopedIdGetIdTag {};
struct GetUserTag {};
struct UserGetOculusIdTag {};
struct GetUserProofTag {};
struct UserProofGetNonceTag {};
using IsErrorThunk = sentinel::CallbackThunk<IsErrorTag, bool(const void*)>;
using GetStringThunk = sentinel::CallbackThunk<GetStringTag, const char*(const void*)>;
using GetOrgScopedIdThunk = sentinel::CallbackThunk<GetOrgScopedIdTag, const void*(const void*)>;
using OrgScopedIdGetIdThunk = sentinel::CallbackThunk<OrgScopedIdGetIdTag, std::uint64_t(const void*)>;
using GetUserThunk = sentinel::CallbackThunk<GetUserTag, const void*(const void*)>;
using UserGetOculusIdThunk = sentinel::CallbackThunk<UserGetOculusIdTag, const char*(const void*)>;
using GetUserProofThunk = sentinel::CallbackThunk<GetUserProofTag, const void*(const void*)>;
using UserProofGetNonceThunk = sentinel::CallbackThunk<UserProofGetNonceTag, const char*(const void*)>;

// Requests: ovr_User_GetOrgScopedID takes the user id in x0 (0x2069b4-0x2069bc, 0x1ec990-0x1ec99c); the other
// three take nothing. All four return the 64-bit request id the game keys the callback by (0x1ec9b0,
// 0x1eca90, 0x1ecce0, 0x1ece20).
template <Prerequisite P>
struct RequestTag {};
using OrgRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::OrgScopedId>, std::uint64_t(std::uint64_t)>;
using UserRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::LoggedInUser>, std::uint64_t()>;
using TokenRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::AccessToken>, std::uint64_t()>;
using ProofRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::UserProof>, std::uint64_t()>;

// The message-level imports the local answers hook (#411). PopMessage returns the next message or null;
// GetType / GetRequestID read a message; FreeMessage releases one. A synthetic handle is answered here and
// never reaches the SDK (login_prerequisites.h, namespace local).
struct PopMessageTag {};
struct MessageGetTypeTag {};
struct MessageGetRequestIdTag {};
struct FreeMessageTag {};
using PopMessageThunk = sentinel::CallbackThunk<PopMessageTag, const void*()>;
using MessageGetTypeThunk = sentinel::CallbackThunk<MessageGetTypeTag, int(const void*)>;
using MessageGetRequestIdThunk = sentinel::CallbackThunk<MessageGetRequestIdTag, std::uint64_t(const void*)>;
using FreeMessageThunk = sentinel::CallbackThunk<FreeMessageTag, void(void*)>;

// The entitlement request (#411): takes nothing, returns the request id (0x206824, 0x206ae4 discard it).
struct EntitlementRequestTag {};
using EntitlementRequestThunk = sentinel::CallbackThunk<EntitlementRequestTag, std::uint64_t()>;

// Its handler: answers with request id 0 and never calls `original`, so the request never reaches the
// Platform SDK and no answer is ever queued. Never logs (a hook on the game's call path).
std::uint64_t OnEntitlementRequest(EntitlementRequestThunk::Fn original) noexcept;

}  // namespace nevr_quest_login
