#pragma once

// The social message level a nevr-runtime client declares at login ("nevr_social"): which of the
// server's newer social messages it parses. The server sends a newer social message only to a
// session that declared its level (nakama server/evr/login_request.go LoginProfile.SocialLevel;
// friend presence, recently met, the lobby tablet and party data all require level 1 or more).
// 1 = docs/design/2026-10-01-social-nakama-proposal.md.
//
// One definition for every client: the PCVR bridge (ws_bridge.cpp) and the Quest login rewrite
// (src/quest/login) both take it from here. It is a header of its own so the Quest build does
// not have to include the whole party facade for one constant.

namespace SocialParty {

constexpr int kSocialLevel = 1;

}  // namespace SocialParty
