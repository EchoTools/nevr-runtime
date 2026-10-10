// Host test for the per-process login stand-ins (src/quest/login/login_standin.{h,cpp}).
// Built -fno-exceptions like the production unit.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/login/login_standin.h"
#include "quest/tests/test_check.h"

namespace SI = QuestLogin::StandIn;

int main() {
  // Before Generate: everything is empty / 0 and no predicate matches.
  SI::ResetForTest();
  QCHECK(SI::OrgId() == 0);
  QCHECK(SI::AccessToken()[0] == '\0');
  QCHECK(!SI::IsOrgId(0) && !SI::IsOrgId(~std::uint64_t{0}));
  QCHECK(!SI::IsAccessToken(""));

  // Generated values are never 0 or -1, are the documented shapes, and are recognised again.
  SI::Generate();
  const std::uint64_t org = SI::OrgId();
  QCHECK(org != 0 && org != ~std::uint64_t{0});
  QCHECK(SI::IsOrgId(org));
  QCHECK(!SI::IsOrgId(org ^ 1));  // a different id is not recognised

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

  // Two independent generations differ: the values are not a shared constant. Collisions are
  // astronomically unlikely, so an equal draw is a bug.
  SI::ResetForTest();
  SI::Generate();
  QCHECK(SI::OrgId() != org);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_standin_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_standin_test: all checks passed\n");
  return 0;
}
