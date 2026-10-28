// Control for row_fake_thunk.cpp: the supported way to activate a row compiles.
#include "common.h"

nevr_quest::libr15_patch::ActivateStatus Use() { return sentinel::InstallRowThunk<snippet::Thunk>("lobby_full.join"); }
