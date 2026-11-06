// Stands for the source build's own libvrapi.so: another object that already exports re33_ec_page.
#include <cstdint>
extern "C" {
alignas(4096) __attribute__((visibility("default"))) std::uint8_t re33_ec_page[4096] = {};
}
