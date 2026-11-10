// Stands for an EC script library: it needs nothing of the sentinel's (no DT_NEEDED on it) and finds the page as the EC script libraries do,
// dlsym(RTLD_DEFAULT, "re33_ec_page") from its own code.
#include <dlfcn.h>

extern "C" __attribute__((visibility("default"))) void* script_find_page() { return dlsym(RTLD_DEFAULT, "re33_ec_page"); }
