// `re33_ec_page`: the EchoCombat script libraries' spare page (docs/quest/q15t/modules/ec.md, section 0 (a)).
//
// Every EC script library (lib<hash16>.so in the APK) reads this page with dlsym(RTLD_DEFAULT, "re33_ec_page") (handle 0:
// `mov x0, xzr` before each `bl dlsym@plt` in libac4e9374a146d3a4.so) and takes `page - 0x2104000` as echovr.exe's image
// base. Each of those libraries has libr15.so as DT_NEEDED, and libr15.so has this library (libovrplatformloader.so) as
// DT_NEEDED, so this object is in every script library's own dependency tree, the same place the source build's runtime
// library (libvrapi.so, also a DT_NEEDED of libr15.so) exports the page from. The q15t plugin is opened RTLD_LOCAL by the
// plugin loader and sits in no script library's tree, so a page exported by the plugin is not found there.
//
// The sentinel only owns the storage: 4096 zeroed bytes in .bss, never written by sentinel code. The q15t `ec` module finds
// it with dlopen("libovrplatformloader.so", RTLD_NOLOAD) + dlsym and writes the tokens, counters and the selected tac mod
// into it. Data only: no code page changes (N151). tests/quest TestExportAllowlist lists this symbol.
#include <cstdint>

extern "C" {
alignas(4096) __attribute__((visibility("default"), used)) std::uint8_t re33_ec_page[4096] = {};
}
