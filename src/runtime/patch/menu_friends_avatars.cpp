#include "runtime/patch/menu_friends_avatars.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "nevr/symbol_hash.h"
#include "runtime/compat/social_roster.h"
#include "runtime/patch/friends_panel_platform.h"

namespace nevr_menu_friends_avatars {
namespace {

using nevr_friends_platform::Level;
using nevr_friends_platform::Log;

constexpr std::uint32_t kRows = 5;
constexpr int kSide = nevr_friends_platform::kPictureSide;
constexpr std::uint64_t kRefetchMs = 60000;

// The panel actor (SEASON INFO) draws the rows; the pop-up actor (SUPPORT & SOCIAL) draws the profile.
constexpr std::uint64_t kPanelActor = 0x9E0BF5FE92ABA570ULL;
constexpr std::uint64_t kPopupActor = 0x3DC5205E08A99CA4ULL;

// The picture slots of the pictures set (friends_package.h): a, the placeholder; b and c, the pictures, alternating so
// the name the engine is asked for is one it has not loaded yet and is read again.
std::uint64_t SlotTexture(std::uint32_t row, std::uint32_t slot) {
  const std::string name = "nevr_friend_avatar_" + std::to_string(row) + "_" + static_cast<char>('a' + slot);
  return EchoVR::CSymbol64Hash(name.c_str());
}

// The profile pop-up's avatar is its banner sprite, which keeps its stock name.
constexpr std::uint64_t kProfileElement = EchoVR::CSymbol64Hash("social");

std::uint64_t ProfileTexture(std::uint32_t slot) {
  const std::string name = std::string("nevr_friend_profile_avatar_") + static_cast<char>('a' + slot);
  return EchoVR::CSymbol64Hash(name.c_str());
}

std::uint64_t AvatarElement(std::uint32_t row) {
  const std::string name = "friend" + std::to_string(row) + "_avatar";
  return EchoVR::CSymbol64Hash(name.c_str());
}

using Pixels = std::shared_ptr<const std::vector<std::uint8_t>>;

// ---- state ----

// What the pictures keep between frames; a function-local static (the Quest sentinel may carry no dynamic initializer).
struct State {
  std::mutex mutex;
  std::map<std::uint64_t, Pixels> byFriend;          // friend id -> picture, filled by the worker
  std::array<std::uint64_t, kRows> generation{};      // the element generation each row's state belongs to
  std::array<std::uint64_t, kRows> shownFriend{};     // the friend whose picture each row shows
  std::array<std::uint32_t, kRows> nextSlot{1, 1, 1, 1, 1};
  std::array<std::uint64_t, kRows> rowTexture{};      // the picture texture each row shows (0: the placeholder)
  // Whether the runtime has set each row's picture since its element was made: the canvas starts every picture on a
  // texture the level loads (friends_canvas.h), so the placeholder is set once at the start.
  std::array<bool, kRows> rowSet{};
  std::atomic<bool> working{false};
  std::uint64_t lastFetchMs = 0;
  std::string lastRosterKey;
  // The profile pop-up's avatar: the friend it should show and the texture it shows, for one element generation.
  std::uint64_t profileWanted = 0;
  std::uint64_t profileTexture = 0;
  std::uint64_t profileGeneration = 0;
};

State& S() {
  static State state;
  return state;
}

// ---- fetch ----

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A Discord avatar URL asked for as a 128 px PNG (animated and webp avatars included); other URLs as they are.
std::string AsPng128(std::string url) {
  if (url.find("discordapp") == std::string::npos && url.find("discord.com") == std::string::npos) return url;
  const std::size_t q = url.find('?');
  if (q != std::string::npos) url.resize(q);
  for (const char* ext : {".gif", ".webp", ".jpg", ".jpeg"}) {
    const std::size_t at = url.rfind(ext);
    if (at != std::string::npos && at + std::strlen(ext) == url.size()) url.replace(at, std::strlen(ext), ".png");
  }
  return url + "?size=128";
}

// ---- PNG to the picture ----

std::uint32_t Be32(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// 8-bit grey, RGB, palette, grey-alpha and RGBA, not interlaced: what Discord serves. Returns false for anything else.
bool DecodePng(const std::string& file, int* width, int* height, std::vector<std::uint8_t>* rgba) {
  const auto* b = reinterpret_cast<const std::uint8_t*>(file.data());
  static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (file.size() < 8 || std::memcmp(b, kSig, 8) != 0) return false;
  int w = 0, h = 0, depth = 0, colour = -1, interlace = 0;
  std::vector<std::uint8_t> idat, palette, trns;
  for (std::size_t at = 8; at + 12 <= file.size();) {
    const std::uint32_t len = Be32(b + at);
    if (at + 12 + len > file.size()) return false;
    const std::string type(reinterpret_cast<const char*>(b + at + 4), 4);
    const std::uint8_t* d = b + at + 8;
    if (type == "IHDR" && len >= 13) {
      w = static_cast<int>(Be32(d));
      h = static_cast<int>(Be32(d + 4));
      depth = d[8];
      colour = d[9];
      interlace = d[12];
    } else if (type == "PLTE") {
      palette.assign(d, d + len);
    } else if (type == "tRNS") {
      trns.assign(d, d + len);
    } else if (type == "IDAT") {
      idat.insert(idat.end(), d, d + len);
    } else if (type == "IEND") {
      break;
    }
    at += 12 + len;
  }
  const int channels = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1 : colour == 4 ? 2 : colour == 6 ? 4 : 0;
  if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || depth != 8 || channels == 0 || interlace != 0) return false;
  const std::size_t stride = static_cast<std::size_t>(w) * channels;
  std::vector<std::uint8_t> raw((stride + 1) * h);
  z_stream z{};
  if (inflateInit(&z) != Z_OK) return false;
  z.next_in = idat.data();
  z.avail_in = static_cast<uInt>(idat.size());
  z.next_out = raw.data();
  z.avail_out = static_cast<uInt>(raw.size());
  const int rc = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  if (rc != Z_STREAM_END || z.avail_out != 0) return false;
  std::vector<std::uint8_t> px(stride * h);
  for (int y = 0; y < h; ++y) {
    const std::uint8_t filter = raw[y * (stride + 1)];
    const std::uint8_t* src = &raw[y * (stride + 1) + 1];
    std::uint8_t* cur = &px[y * stride];
    const std::uint8_t* up = y > 0 ? &px[(y - 1) * stride] : nullptr;
    for (std::size_t x = 0; x < stride; ++x) {
      const int a = x >= static_cast<std::size_t>(channels) ? cur[x - channels] : 0;
      const int u = up != nullptr ? up[x] : 0;
      const int c = (up != nullptr && x >= static_cast<std::size_t>(channels)) ? up[x - channels] : 0;
      int v = src[x];
      switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += u; break;
        case 3: v += (a + u) / 2; break;
        case 4: {
          const int p = a + u - c, pa = std::abs(p - a), pb = std::abs(p - u), pc = std::abs(p - c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? u : c);
          break;
        }
        default: return false;
      }
      cur[x] = static_cast<std::uint8_t>(v);
    }
  }
  rgba->resize(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
    const std::uint8_t* s = &px[i * channels];
    std::uint8_t* o = &(*rgba)[i * 4];
    switch (colour) {
      case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
      case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
      case 3: {
        const std::size_t k = s[0];
        if (3 * k + 2 >= palette.size()) return false;
        o[0] = palette[3 * k]; o[1] = palette[3 * k + 1]; o[2] = palette[3 * k + 2];
        o[3] = k < trns.size() ? trns[k] : 255;
        break;
      }
      case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
      default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    }
  }
  *width = w;
  *height = h;
  return true;
}

// The engine reads the slot's pixels as linear colour and gamma-encodes them for display, so an sRGB picture written
// as it is shows washed out: each channel goes through the sRGB decode first.
std::uint8_t SrgbToLinear(unsigned c) {
  static const std::array<std::uint8_t, 256> table = [] {
    std::array<std::uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
      const double v = i / 255.0;
      const double l = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
      t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(l * 255.0));
    }
    return t;
  }();
  return table[std::min(c, 255u)];
}

// Box-filtered to kPictureSide square and blended onto the avatar slate (a transparent pixel shows the slate), each
// channel decoded from sRGB to linear: RGBA, row by row (friends_panel_platform.h, ServePicture).
std::vector<std::uint8_t> ToPicture(int w, int h, const std::vector<std::uint8_t>& rgba) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(kSide) * kSide * 4);
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      const int x0 = x * w / kSide, x1 = std::max(x0 + 1, (x + 1) * w / kSide);
      const int y0 = y * h / kSide, y1 = std::max(y0 + 1, (y + 1) * h / kSide);
      unsigned sum[4] = {0, 0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
          const std::uint8_t* s = &rgba[(static_cast<std::size_t>(yy) * w + xx) * 4];
          for (int k = 0; k < 4; ++k) sum[k] += s[k];
          ++n;
        }
      }
      const unsigned alpha = sum[3] / n;
      const std::uint8_t slate[3] = {0x33, 0x3D, 0x49};  // R, G, B
      std::uint8_t* o = &out[(static_cast<std::size_t>(y) * kSide + x) * 4];
      for (int k = 0; k < 3; ++k) o[k] = SrgbToLinear((sum[k] / n * alpha + slate[k] * (255 - alpha)) / 255);
      o[3] = 255;
    }
  }
  return out;
}

struct Wanted {
  std::uint64_t id;
  std::string name;
};

void Fetch(std::vector<Wanted> wanted) {
  State& s = S();
  const std::string base = nevr_friends_platform::ServiceBase();
  const std::string token = nevr_friends_platform::Token();
  std::size_t matched = 0, loaded = 0;
  if (base.empty() || token.empty()) {
    Log(Level::Info, "[NEVR.MENU] friends panel avatars skipped reason=%s", token.empty() ? "no_token" : "no_http_uri");
    s.working = false;
    return;
  }
  const nevr_friends_platform::HttpReply list = nevr_friends_platform::Http("GET", base + "/v2/friend?limit=100", token, "");
  if (!list.ok || list.status != 200) {
    Log(Level::Warning, "[NEVR.MENU] friends panel avatars: friend list request failed status=%ld", list.status);
    s.working = false;
    return;
  }
  std::map<std::string, std::string> urlByName;  // lower-case display name and username -> avatar_url
  try {
    const nlohmann::json doc = nlohmann::json::parse(list.body);
    for (const auto& f : doc.value("friends", nlohmann::json::array())) {
      const auto& u = f.value("user", nlohmann::json::object());
      const std::string url = u.value("avatar_url", "");
      if (url.empty()) continue;
      for (const char* key : {"display_name", "username"}) {
        const std::string n = Lower(u.value(key, ""));
        if (!n.empty()) urlByName.emplace(n, url);
      }
    }
  } catch (const nlohmann::json::exception&) {
    Log(Level::Warning, "[NEVR.MENU] friends panel avatars: friend list unreadable bytes=%zu", list.body.size());
    s.working = false;
    return;
  }
  for (const Wanted& w : wanted) {
    const auto it = urlByName.find(Lower(w.name));
    if (it == urlByName.end()) continue;
    ++matched;
    const nevr_friends_platform::HttpReply image = nevr_friends_platform::Http("GET", AsPng128(it->second), "", "");
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    if (!image.ok || image.status != 200 || !DecodePng(image.body, &width, &height, &rgba)) {
      Log(Level::Info, "[NEVR.MENU] friends panel avatar not loaded status=%ld bytes=%zu", image.status, image.body.size());
      continue;
    }
    auto picture = std::make_shared<const std::vector<std::uint8_t>>(ToPicture(width, height, rgba));
    std::lock_guard<std::mutex> lock(s.mutex);
    s.byFriend[w.id] = std::move(picture);
    ++loaded;
  }
  Log(Level::Info, "[NEVR.MENU] friends panel avatars friends_with_avatar=%zu rows=%zu matched=%zu loaded=%zu",
      urlByName.size() / 2, wanted.size(), matched, loaded);
  s.working = false;
}

}  // namespace

void ShowProfile(std::uint64_t rosterId) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.profileWanted = rosterId;
  s.profileTexture = 0;  // set it again on open
}

void OnFrame() {
  if (!nevr_friends_platform::PicturesReady()) return;
  State& s = S();
  // The profile pop-up shows the texture the friend's row shows: that texture already holds their picture and is
  // loaded. A friend with no picture shows the placeholder.
  {
    const nevr_friends_platform::Picture picture = nevr_friends_platform::FindPicture(kPopupActor, kProfileElement);
    std::uint64_t texture = ProfileTexture(0);
    bool apply = false;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (picture.generation != s.profileGeneration) {
        s.profileGeneration = picture.generation;
        s.profileTexture = 0;
      }
      for (std::uint32_t row = 0; row < kRows; ++row) {
        if (s.profileWanted != 0 && s.shownFriend[row] == s.profileWanted && s.rowTexture[row] != 0) {
          texture = s.rowTexture[row];
        }
      }
      if (picture.behaviour != nullptr && s.profileWanted != 0 && texture != s.profileTexture) {
        s.profileTexture = texture;
        apply = true;
      }
    }
    if (apply) {
      Log(Level::Info, "[NEVR.MENU] friend profile avatar applied texture=0x%016llx picture=%s",
          static_cast<unsigned long long>(texture), texture == ProfileTexture(0) ? "placeholder" : "row");
      nevr_friends_platform::ApplyTexture(picture.behaviour, texture);
    }
  }
  const nevr_social_roster::Roster& roster = nevr_social_roster::Global();
  const std::uint32_t count = std::min(roster.Count(), kRows);
  std::vector<Wanted> wanted;
  std::string key;
  for (std::uint32_t row = 0; row < count; ++row) {
    std::uint64_t id = 0;
    if (!roster.IdAt(row, &id)) continue;
    wanted.push_back({id, roster.NameAt(row)});
    key += std::to_string(id) + ':' + roster.NameAt(row) + ';';
  }
  const std::uint64_t now = nevr_friends_platform::NowMs();
  if (!wanted.empty() && !s.working && (key != s.lastRosterKey || now - s.lastFetchMs > 10 * kRefetchMs) &&
      now - s.lastFetchMs > (s.lastFetchMs == 0 ? 0 : kRefetchMs / 6)) {
    s.lastRosterKey = key;
    s.lastFetchMs = now;
    s.working = true;
    std::thread(Fetch, std::move(wanted)).detach();
    return;
  }
  // Apply pictures: a row shows the picture of the friend now in that row, the placeholder (its slot a) while that
  // friend has none and when the row is empty (a friend removed moves the rows below up).
  for (std::uint32_t row = 0; row < kRows; ++row) {
    const nevr_friends_platform::Picture picture = nevr_friends_platform::FindPicture(kPanelActor, AvatarElement(row));
    if (picture.behaviour == nullptr) continue;
    std::uint64_t id = 0;
    const bool used = row < count && roster.IdAt(row, &id);
    std::uint64_t texture = 0;
    Pixels pixels;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (picture.generation != s.generation[row]) {  // a new canvas instance starts on its stock texture again
        s.generation[row] = picture.generation;
        s.shownFriend[row] = 0;
        s.rowTexture[row] = 0;
        s.rowSet[row] = false;
      }
      const auto it = used ? s.byFriend.find(id) : s.byFriend.end();
      const bool have = it != s.byFriend.end();
      const bool showPicture = have && (s.shownFriend[row] != id || s.rowTexture[row] == 0);
      const bool showPlaceholder = !have && (!s.rowSet[row] || s.shownFriend[row] != id || s.rowTexture[row] != 0);
      if (!showPicture && !showPlaceholder) continue;
      s.shownFriend[row] = used ? id : 0;
      s.rowSet[row] = true;
      if (showPicture) {
        const std::uint32_t slot = s.nextSlot[row];
        s.nextSlot[row] = slot == 1 ? 2 : 1;
        pixels = it->second;
        texture = SlotTexture(row, slot);
        s.rowTexture[row] = texture;
      } else {
        texture = SlotTexture(row, 0);
        s.rowTexture[row] = 0;
      }
    }
    Log(Level::Info, "[NEVR.MENU] friends panel avatar applied row=%u texture=0x%016llx picture=%s", row,
        static_cast<unsigned long long>(texture), pixels ? "friend" : "placeholder");
    if (pixels) nevr_friends_platform::ServePicture(texture, *pixels, "friends panel avatar " + std::to_string(row));
    nevr_friends_platform::ApplyTexture(picture.behaviour, texture);
  }
}

}  // namespace nevr_menu_friends_avatars
