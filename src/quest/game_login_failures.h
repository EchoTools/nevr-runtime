#pragma once
// The game's own local login-failure texts: the exact strings libpnsovr.so / libr15.so pass to
// CNSUser::LogInFailed when a login fails on the device, before or instead of a server answer.
// Measured on the pinned build (docs/adr/0003, "Sign-in prompt in the headset"):
//   kPrerequisitesMissing  libpnsovr 0x556b40, CNSOVRUser::UpdateInternal (code 500)
//   kFailedToGetUserProof  libpnsovr 0x556a5a, CNSOVRUser::GotUserProofCB
//   kClientError           libpnsovr 0x556abf, CNSOVRUser::GotUserProofCB
//   kCryptographyError     libpnsovr 0x5569ab, CNSOVRUser::LogInInternal and GotUserProofCB
//   kServiceUnavailable    libpnsovr 0x584c42 / libr15 0x31737d7, CNSUser::SendLogInRequest and
//                          ConnectFailedCB
// The sign-in prompt hook (src/quest/sentinel/login_prompt_hook.h) shows the prompt only in place
// of one of these, never in place of a server-sent text. Code that fails the game's login itself
// (CNSOVRUser::LogInFailed) and wants the player to see the sign-in prompt passes one of these, as
// is: kPrerequisitesMissing for "no NEVR identity yet". Each is one line of at most 63 characters,
// so the game stores it whole as the first line of its error block.
//
// Plain constants only: included by translation units built with and without exceptions.

namespace nevr_quest::game_login_failures {

inline constexpr char kPrerequisitesMissing[] = "Log in request failed: One or more prerequisites are missing";
inline constexpr char kFailedToGetUserProof[] = "Log in request failed: Failed to get user proof";
inline constexpr char kClientError[] = "Log in request failed: Client error";
inline constexpr char kCryptographyError[] = "Log in request failed: Cryptography error";
inline constexpr char kServiceUnavailable[] = "Log in request failed: Service unavailable";

inline constexpr const char* kAll[] = {kPrerequisitesMissing, kFailedToGetUserProof, kClientError,
                                       kCryptographyError, kServiceUnavailable};

static_assert(sizeof(kPrerequisitesMissing) - 1 <= 63, "fits the game's first error line");
static_assert(sizeof(kFailedToGetUserProof) - 1 <= 63, "fits the game's first error line");
static_assert(sizeof(kServiceUnavailable) - 1 <= 63, "fits the game's first error line");

}  // namespace nevr_quest::game_login_failures
