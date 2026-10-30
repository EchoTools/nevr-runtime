// Stands for another object that exports re33_ec_page: the source build's own libvrapi.so (loaded global), or the sentinel
// libovrplatformloader.so (src/quest/sentinel/ec_page.cpp), loaded as a dependency, not global.
#include <cstdint>
extern "C" {
alignas(4096) __attribute__((visibility("default"))) std::uint8_t re33_ec_page[4096] = {};
}
