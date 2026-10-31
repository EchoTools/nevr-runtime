// The `ec` module (docs/quest/q15t/modules/ec.md): EC's spare memory page `re33_ec_page` and the Quest side of its EC network client.
// Port of the source build's ec. GOT data-page swaps only (got_swap.h): recvfrom, sendto, NWriteLog::WriteLog of libr15
// (pinned slots) and of libpnsovr.so / libpnsrad.so (by name, content-checked), and the two equipment-config rebuild entries
// (the hide's gate). No code word is written.
//
// Always there: `re33_ec_page`, 4096 zeroed bytes, a default-visibility data symbol of this plugin. Nothing writes it unless the
// switch is on: a zeroed page = no token anywhere = every EC script decides as on a PC without the EC network client.
//
// `ec.off` in the plugin's data directory is a kill file: ec stays off, whatever else is set below.
// With the switch on (a file in the plugin's data directory, ec.on with the TEST words on its first line, or ec_preset1.on /
// ec_preset2.on / ec_preset3.on = the source build's by-name presets; or Q15T_EC_DEFAULT_WORDS at build time): the identity-free port of
// the source's Main thread runs every 5 ms (EcStep): use counters of the page -> 'T' / 'V' / 'P' / 'O' packets to the server,
// the busiest sendto destination becomes the server, 'H' hello every 2 s, the tac-mod list hide, the selected-tac-mod store.
//
// The loader opens a plugin with RTLD_LOCAL (docs/quest/q15t/modules/ec.md (a)); PublishEcPage asks for global scope and says what it got.
#pragma once

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>

#include "import_hook.h"

namespace q15t {

// ELF vaddrs of the pinned build (readelf -rW, a source-build note not in this tree; ReVault VA = ELF vaddr + 0x100000).
constexpr std::uintptr_t kEcSlotRecvfrom = 0x36d3e88;        // R43 libc recvfrom JUMP_SLOT
constexpr std::uintptr_t kEcSlotSendto = 0x36db3d0;          // R44 libc sendto JUMP_SLOT
constexpr std::uintptr_t kEcSlotWriteLog = 0x36fd398;        // R41 NWriteLog::WriteLog(ELogLevel, ull, char const*, ...) JUMP_SLOT
constexpr std::uintptr_t kEcFnWriteLog = 0xf88c20;           // its symbol value in libr15 (ReVault 0x1088c20)
constexpr std::uintptr_t kEcSlotWriteLogV = 0x36cb1b0;       // WriteLogV's JUMP_SLOT (the 25-word body's `bl` goes through it)
constexpr std::uintptr_t kEcFnWriteLogV = 0xf88720;          // WriteLogV (ReVault 0x1088720)
constexpr std::uintptr_t kEcSlotReloadEquip = 0x36ea1b0;     // NR15::ReloadEquipmentConfig JUMP_SLOT
constexpr std::uintptr_t kEcFnReloadEquip = 0x20feda8;       // (ReVault 0x21feda8)
constexpr std::uintptr_t kEcSlotUnloadEquip = 0x36f1878;     // NR15::UnloadEquipmentConfig JUMP_SLOT
constexpr std::uintptr_t kEcFnUnloadEquip = 0x20ff7b4;       // (ReVault 0x21ff7b4)
constexpr std::uintptr_t kEcTacModList = 0x37af5f8;          // R42 .bss word: pointer to the tac-mod list { alloc, entries, count, cap }

enum class EcPageState {
  kUnchecked,
  kVisible,    // dlsym(RTLD_DEFAULT, "re33_ec_page") is this plugin's page: the script libs find it
  kInvisible,  // not in the global scope: the script libs decide as on a PC without the EC network client
  kOther,      // another object already exports a re33_ec_page: the scripts use that one
};

// The page: 4096 zeroed bytes. Other translation units reach it through this accessor, not a second declaration of the symbol
// (checked 2026-10-28: a bare `extern` declaration in another file does NOT hide the symbol, on g++ or NDK clang 17 with
// -fvisibility=hidden; the accessor is for tidiness and for the test seam).
std::uint8_t* EcPage();

// Re-opens this object with RTLD_GLOBAL | RTLD_NOLOAD and reports what dlsym(RTLD_DEFAULT) finds. Logs one line.
EcPageState PublishEcPage();

// Reads the switch files / default words; logs the page line always. No switch = nothing installed, reason "off".
// Otherwise the result of the recvfrom + sendto pair (both or none).
InstallResult InstallEc(std::uintptr_t base);
// The same with the words given (tests; the main seat's own policy). `words` = one line of the ec.on words.
InstallResult InstallEcWithWords(std::uintptr_t base, const char* words);

// True when <data directory>/<name> exists (the plugin's own files dir, same rule as the switch files). Used by ecfx.
bool EcDataFileExists(const char* name);
// The first line of <data directory>/<name> into `out` (newline stripped, NUL-terminated). False when there is no such file.
bool EcReadDataLine(const char* name, char* out, std::size_t n);

// One 5 ms pass of the source's Main loop. `now100` = CLOCK_MONOTONIC in 100 ns. The thread InstallEc starts calls it.
void EcStep(std::uint64_t now100);

// Registration for other modules (ecfx): called at the end of every recvfrom on the game's own socket made by the thread that
// received a datagram of the server on it; up to 4, in registration order; false = no free slot.
bool EcOnNetTick(void (*tick)());
// A first-time 'K' (a drone's health ran out on the server) goes to this handler, on the libr15 thread that called recvfrom.
void EcOnDroneKilled(void (*handler)(std::uint32_t index));

struct EcStats {
  std::uint64_t sends, passed, consumed;           // sendto calls, datagrams passed to the game, EC network datagrams consumed
  std::uint64_t answers, tokens, dropped, kills, medi;
  bool answered;                                   // the current server answers the EC network
  bool have_server;
  std::uint32_t server_addr;
  std::uint16_t server_port;
  int server_fd;
  bool hide_write, equip_store, list_on;
  std::uint32_t types;
};
EcStats EcStatsNow();

// ---- test seams ----
using EcRecvfromFn = ssize_t (*)(int, void*, size_t, int, sockaddr*, socklen_t*);
using EcSendtoFn = ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t);
void SetEcLibcForTest(EcRecvfromFn recv, EcSendtoFn send);
ssize_t EcRecvfromForTest(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen);
ssize_t EcSendtoForTest(int fd, const void* buf, size_t len, int flags, const sockaddr* to, socklen_t tolen);
void SetEcNoThreadForTest(bool on);
void SetEcClockForTest(std::uint64_t (*clock100)());  // the 100 ns clock RecvfromWrap and EcStatsNow read
void SetEcDirForTest(const char* dir);
void SetEcLogVForTest(int lib, std::uintptr_t fn);       // the WriteLogV the wrapper of lib 0 (libr15) / 1 (pnsovr) / 2 (pnsrad) calls
std::uintptr_t EcWriteLogWrapForTest(int lib);           // the wrapper's address
std::uintptr_t EcGearWrapForTest(int which);             // 0 Reload, 1 Unload wrapper
void SetEcGearRealForTest(int which, void (*fn)());
// Fakes for the by-name platform-lib path: find a loaded lib by name tail ("/libpnsovr.so") or, with a null tail, the lib that
// holds `addr` -> (load bias, PT_DYNAMIC address); is `fn` exactly the symbol `sym` (dladdr)?
struct EcLibInfo {
  std::uintptr_t base = 0, dyn = 0;
};
struct EcSelEnv {
  bool (*find_lib)(const char* tail, std::uintptr_t addr, EcLibInfo* out) = nullptr;
  bool (*named_as)(std::uintptr_t fn, const char* sym) = nullptr;
};
void SetEcSelEnvForTest(const EcSelEnv& env);
bool EcSelTriedForTest(int lib);
std::uint32_t EcGearGenForTest();
void EcResetForTest();

}  // namespace q15t
