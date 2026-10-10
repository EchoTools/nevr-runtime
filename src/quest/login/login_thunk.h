#pragma once
// The type of the login hook's thunk: the GOT slot of CNSUser::SendLogInRequest (login_hook.h). It lives
// in a header so the unit that registers its counters (login_counters.cpp, buildable on the host) names
// the same CallbackThunk instantiation as the unit that installs it (login_hook.cpp, Android only).

#include "quest/sentinel/callback_thunk.h"

namespace nevr_quest_login {

struct LoginTag {};
using LoginThunk = sentinel::CallbackThunk<LoginTag, void(void*, void*)>;

}  // namespace nevr_quest_login
