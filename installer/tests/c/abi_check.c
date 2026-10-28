/* Drives the C ABI the way a Unity or JNI caller does: open, verify manifest bytes, pick a release, build the Quest plan,
 * end the run. Exits 0 only when every call returned NEVR_OK where it should. argv: config_json manifest sig */
#include "nevr_install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(20); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc((size_t)n);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(21);
  fclose(f);
  *len = (size_t)n;
  return b;
}

#define CHECK(call, want)                                                                     \
  do {                                                                                        \
    int rc_ = (call);                                                                         \
    if (rc_ != (want)) {                                                                      \
      fprintf(stderr, "%s returned %d, wanted %d: %s\n", #call, rc_, (want), s ? NevrLastError(s) : ""); \
      return 1;                                                                               \
    }                                                                                         \
  } while (0)

int main(int argc, char **argv) {
  if (argc != 4) return 2;
  NevrSession *s = NULL;
  CHECK(NevrInstallOpen(argv[1], &s), NEVR_OK);
  size_t ml, sl;
  uint8_t *m = slurp(argv[2], &ml), *g = slurp(argv[3], &sl);
  /* Tampered bytes are refused and the session stays usable. */
  m[10] ^= 1;
  CHECK(NevrManifestVerify(s, m, ml, g, sl), NEVR_FAILED);
  if (strstr(NevrLastError(s), "FAIL step 2") == NULL) { fprintf(stderr, "wrong error: %s\n", NevrLastError(s)); return 1; }
  m[10] ^= 1;
  CHECK(NevrManifestVerify(s, m, ml, g, sl), NEVR_OK);
  char *rel = NULL;
  CHECK(NevrReleasePick(s, "beta", &rel), NEVR_OK);
  printf("release=%s\n", rel);
  NevrStringFree(rel);
  char *plan = NULL;
  CHECK(NevrPlanBuild(s, "1.0.0", "{\"installed\":null,\"data\":{\"legacy\":true,\"media\":false}}", &plan), NEVR_OK);
  printf("plan=%s\n", plan);
  NevrStringFree(plan);
  CHECK(NevrPlanBuild(NULL, "1.0.0", "{}", &plan), NEVR_BAD_ARGUMENT);
  CHECK(NevrRunEnd(s, "bogus", NULL), NEVR_BAD_ARGUMENT);
  CHECK(NevrRunEnd(s, "unchanged", "abi check"), NEVR_OK);
  printf("core=%s\n", NevrCoreVersion());
  NevrClose(s);
  free(m);
  free(g);
  return 0;
}
