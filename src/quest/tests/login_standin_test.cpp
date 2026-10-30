// Host test for the per-process login stand-ins (src/quest/login/login_standin.{h,cpp}).
// Built -fno-exceptions like the production unit.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/login/login_standin.h"
#include "quest/tests/test_check.h"

namespace SI = nevr_quest_login::StandIn;

int main() {
  // Before Generate: everything is empty / 0 and no predicate matches.
  SI::ResetForTest();
  QCHECK(SI::OrgId() == 0);
  QCHECK(SI::AccessToken()[0] == '\0');
  QCHECK(!SI::IsOrgId(0) && !SI::IsOrgId(~std::uint64_t{0}));
  QCHECK(!SI::IsAccessToken(""));
  QCHECK(SI::UserId() == 0 && !SI::IsUserId(0));

  // Generated values are never 0 or -1, are the documented shapes, and are recognised again.
  SI::Generate();
  const std::uint64_t org = SI::OrgId();
  QCHECK(org != 0 && org != ~std::uint64_t{0});
  QCHECK(SI::IsOrgId(org));
  QCHECK(!SI::IsOrgId(org ^ 1));  // a different id is not recognised

  // The logged-in user id stand-in (#944): nonzero, not the org id, a non-negative signed 64-bit value (the login's
  // "accountid" is a CJson integer), and recognised again.
  const std::uint64_t user = SI::UserId();
  QCHECK(user != 0 && user != org);
  QCHECK((user >> 63) == 0);
  QCHECK(SI::IsUserId(user) && !SI::IsUserId(user ^ 1) && !SI::IsUserId(org));
  QCHECK(!SI::IsOrgId(user));

  const char* token = SI::AccessToken();
  const char* nonce = SI::Nonce();
  const char* name = SI::OculusId();
  QCHECK(std::strlen(token) == 40);
  QCHECK(std::strlen(nonce) == 32);
  QCHECK(std::strncmp(name, "player-", 7) == 0 && std::strlen(name) == 7 + 8);
  QCHECK(SI::IsAccessToken(token) && !SI::IsAccessToken("nope"));
  QCHECK(SI::IsNonce(nonce) && !SI::IsNonce(token));  // token is not the nonce

  // IsOculusId matches whether the game's buffer is NUL-terminated or filled with no terminator.
  char padded[0x24];
  std::memset(padded, 0, sizeof(padded));
  std::memcpy(padded, name, std::strlen(name));
  QCHECK(SI::IsOculusId(padded, sizeof(padded)));
  char noterm[0x24];
  std::memset(noterm, 'x', sizeof(noterm));
  std::memcpy(noterm, name, std::strlen(name));  // name + 'x' padding, no terminator within capacity
  QCHECK(!SI::IsOculusId(noterm, sizeof(noterm)));  // the padding makes it a different string
  QCHECK(!SI::IsOculusId("player-00000000", sizeof(padded)));  // a different name

  // Generate is idempotent: a second call keeps the first values.
  const std::uint64_t org_again = SI::OrgId();
  SI::Generate();
  QCHECK(SI::OrgId() == org_again);
  QCHECK(SI::UserId() == user);

  // Two independent generations differ: the values are not a shared constant. Collisions are
  // astronomically unlikely, so an equal draw is a bug.
  SI::ResetForTest();
  SI::Generate();
  QCHECK(SI::OrgId() != org);
  QCHECK(SI::UserId() != user);
  QCHECK(!SI::IsUserId(user));

  // The top bit is cleared on every draw, not by luck: 64 fresh draws, each with a random top bit before the mask
  // (a missing mask fails this with probability 1 - 2^-64).
  for (int i = 0; i < 64; ++i) {
    SI::ResetForTest();
    SI::Generate();
    QCHECK((SI::UserId() >> 63) == 0 && SI::UserId() != 0 && SI::UserId() != SI::OrgId());
  }

  // The user id is drawn again while it equals the org id. The collision is 2^-63 by chance, so the draws are scripted:
  // org 5, then a user draw of 5 (redrawn), then 7. A Generate without the redraw keeps 5 and fails the first check.
  {
    static int draws = 0;
    static const std::uint64_t kScript[] = {5, 5, 7};
    SI::SetRandomForTest([](void* buf, std::size_t n) {
      if (n != sizeof(std::uint64_t)) {
        std::memset(buf, 0x5a, n);
        return;
      }
      const std::uint64_t value = kScript[draws < 2 ? draws : 2];
      ++draws;
      std::memcpy(buf, &value, sizeof(value));
    });
    SI::ResetForTest();
    draws = 0;
    SI::Generate();
    QCHECK(SI::OrgId() == 5);
    QCHECK(SI::UserId() == 7);  // the colliding 5 was drawn again
    QCHECK(SI::UserId() != SI::OrgId());
    QCHECK(draws == 3);         // three eight-byte draws: the org, the colliding user, the redraw
    SI::SetRandomForTest(nullptr);
    SI::ResetForTest();
  }

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_standin_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_standin_test: all checks passed\n");
  return 0;
}
