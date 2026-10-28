/* Drives NevrPcStage the way the runtime's update worker does: open with "nevr_root", fetch the manifest, stage, end the run.
 * Prints the result JSON. Exits 0 only when every call returned what it should. argv: config_json nevr_root current_json */
#include "nevr_install.h"
#include <stdio.h>
#include <string.h>

#define CHECK(call, want)                                                                              \
  do {                                                                                                 \
    int rc_ = (call);                                                                                  \
    if (rc_ != (want)) {                                                                               \
      fprintf(stderr, "%s returned %d, wanted %d: %s\n", #call, rc_, (want), s ? NevrLastError(s) : ""); \
      return 1;                                                                                        \
    }                                                                                                  \
  } while (0)

int main(int argc, char **argv) {
  if (argc != 4) return 2;
  NevrSession *s = NULL;
  char *res = NULL;
  CHECK(NevrInstallOpen(argv[1], &s), NEVR_OK);
  CHECK(NevrPcStage(NULL, argv[2], argv[3], &res), NEVR_BAD_ARGUMENT);
  CHECK(NevrPcStage(s, NULL, argv[3], &res), NEVR_BAD_ARGUMENT);
  CHECK(NevrPcStage(s, argv[2], "not json", &res), NEVR_FAILED);
  CHECK(NevrManifestFetch(s), NEVR_OK);
  CHECK(NevrPcStage(s, argv[2], argv[3], &res), NEVR_OK);
  printf("stage=%s\n", res);
  NevrStringFree(res);
  CHECK(NevrRunEnd(s, "unchanged", "stage check"), NEVR_OK);
  NevrClose(s);
  return 0;
}
