// Built into a test shared object together with src/quest/plugins/q15t/src/ec.cpp under the plugin's own flags (-fvisibility=hidden): the only symbols
// it exports besides re33_ec_page are these two, so the test can drive PublishEcPage from outside as a plugin host would.
#include "ec.h"

extern "C" __attribute__((visibility("default"))) int shim_publish() { return static_cast<int>(q15t::PublishEcPage()); }
extern "C" __attribute__((visibility("default"))) void* shim_page() { return q15t::EcPage(); }
