// friends_panel_platform.h: what the FRIENDS panel (#1535, #1599) needs from the platform it runs on.
//
// The panel's logic is shared by the PC runtime and the Quest sentinel: menu_friends_pack.cpp (rows, select mode,
// clicks), menu_friends_profile.cpp (the friend profile pop-up) and menu_friends_avatars.cpp (the pictures). Everything
// that differs between echovr.exe and libr15 is behind the functions below, which each platform defines once:
// friends_panel_pc.cpp on PC, src/quest/friends/friends_platform_quest.cpp on the Quest. The UI text writer
// (ui_text_writer.h) is the third shared seam: each platform defines its functions too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nevr_friends_platform {

enum class Level { Info, Warning };

/// One log line with the panel's tag already in `format` ([NEVR.MENU]). Any thread. The PC formats with the GNU
/// conversions (%zu, %llu), which MinGW's vsnprintf takes; gnu_printf checks those.
#if defined(__MINGW32__)
void Log(Level level, const char* format, ...) __attribute__((format(gnu_printf, 2, 3)));
#else
void Log(Level level, const char* format, ...) __attribute__((format(printf, 2, 3)));
#endif

/// A monotonic clock in milliseconds.
std::uint64_t NowMs();

/// Copies `length` bytes of game memory at `address` into `out`; false when that memory cannot be read.
bool Read(std::uint64_t address, void* out, std::size_t length);

/// Writes one word of game memory (an event the game has not handled yet); false when it cannot be written.
bool WriteWord(std::uint64_t address, std::uint64_t value);

/// One request to the community service, or to an image host. `method` is GET, POST or DELETE; `bearer` empty sends no
/// Authorization header; `payload` is the JSON body of a POST. Worker threads only (it blocks).
struct HttpReply {
  bool ok = false;  // the transfer completed (any HTTP status)
  long status = 0;
  std::string body;
};
HttpReply Http(const char* method, const std::string& url, const std::string& bearer, const std::string& payload);

/// The community service's HTTP address (nevr_http_uri on PC, auth.http_uri on the Quest), "" when there is none.
std::string ServiceBase();

/// The player's token for the service, "" when there is none yet.
std::string Token();

// ---- pictures ----

/// The dynamic-texture behaviour that shows the canvas element `element` of the actor `actor`, and a generation that
/// changes when the element is made again (a new canvas instance starts on its default texture). `behaviour` is null
/// while the element does not exist. Game-loop thread.
struct Picture {
  void* behaviour = nullptr;
  std::uint64_t generation = 0;
};
Picture FindPicture(std::uint64_t actor, std::uint64_t element);

/// Makes `behaviour` load and show the texture named `texture`. Game-loop thread.
void ApplyTexture(void* behaviour, std::uint64_t texture);

/// The picture a friend's avatar becomes: kPictureSide x kPictureSide RGBA, row by row, already blended onto the avatar
/// slate and decoded from sRGB to linear (the engine gamma-encodes texture colour for display).
inline constexpr int kPictureSide = 128;

/// Serves `rgba` (kPictureSide squared RGBA pixels) as the payload of the texture `name` the next time the game reads
/// it. The platform encodes it in its texture format and keeps the bytes for the rest of the process. Game-loop thread.
void ServePicture(std::uint64_t name, const std::vector<std::uint8_t>& rgba, const std::string& label);

/// True once the platform can find and set pictures (its game functions matched).
bool PicturesReady();

}  // namespace nevr_friends_platform
