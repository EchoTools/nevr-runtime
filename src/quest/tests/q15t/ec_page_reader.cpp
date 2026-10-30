// Stands for an EC script library: it has the sentinel stand-in as a dependency (as lib<hash16>.so -> libr15.so ->
// libovrplatformloader.so) and asks for the page the way the libraries do, dlsym with handle 0 (RTLD_DEFAULT) from inside itself.
#include <dlfcn.h>
extern "C" __attribute__((visibility("default"))) void* reader_page() { return dlsym(RTLD_DEFAULT, "re33_ec_page"); }
