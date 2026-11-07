// Built into a test shared object together with src/quest/sentinel/ec_page.cpp under the sentinel's own flags (-fvisibility=hidden): the only
// symbols it exports besides re33_ec_page are these two, so the test can drive PublishEcPage from outside as the sentinel's install would.
#include "quest/sentinel/ec_page.h"

extern "C" __attribute__((visibility("default"))) int shim_publish() {
  bool reopened = false;
  const void* seen = nullptr;
  return static_cast<int>(sentinel::PublishEcPage(&reopened, &seen));
}
extern "C" __attribute__((visibility("default"))) void* shim_page() { return sentinel::EcPage(); }
