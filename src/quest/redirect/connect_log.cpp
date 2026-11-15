#include "quest/redirect/connect_log.h"

#include <atomic>
#include <cstring>

#include "hook_log.h"
#include "quest/redirect/service_redirector.h"

namespace nevr_quest::redirect {
namespace {

constexpr std::size_t kWordBytes = sizeof(std::uint64_t);
constexpr std::size_t kHostWords = kConnectHostBytes / kWordBytes;
static_assert(kConnectHostBytes % kWordBytes == 0, "a host is stored as whole 64-bit words");

// The slot flags.
constexpr std::uint32_t kGameTruncated = 1;
constexpr std::uint32_t kUsedTruncated = 2;
constexpr std::uint32_t kGameNull = 4;
constexpr std::uint32_t kUsedNull = 8;
constexpr std::uint32_t kUrlChanged = 16;  // the hook handed the original another URL than the game passed

// "https://" and "http://" are skipped before the authority.
constexpr char kHttps[] = "https://";
constexpr char kHttp[] = "http://";
constexpr char kRefusedHost[] = "127.0.0.1:9";  // the authority of kRefusedUrl
constexpr char kServiceDomain[] = ".readyatdawn.com";
constexpr char kScheme[] = "https://";

// One record. Every field is an atomic so the reporter's read of a slot a game thread is rewriting is not a data race; `seq`
// says which record the slot holds (0: being written), and the reader checks it before and after the copy.
struct Slot {
  std::atomic<std::uint64_t> seq{0};
  std::atomic<std::uint64_t> game[kHostWords]{};
  std::atomic<std::uint64_t> used[kHostWords]{};
  std::atomic<std::int32_t> result{0};
  std::atomic<std::uint32_t> flags{0};
};

struct Ring {
  std::atomic<std::uint64_t> head{0};  // records noted so far; the next record is number head
  Slot slots[kConnectLogSlots];
  std::uint64_t cursor = 0;            // reporter only: the next record number to log
  std::uint64_t dropped = 0;           // reporter only: records lost to overwrite or a torn read
  std::uint64_t droppedLogged = 0;
};

Ring& GlobalRing() noexcept {
  static Ring ring;
  return ring;
}

bool EqualsLowerPrefix(const char* text, const char* lower) noexcept {
  for (std::size_t i = 0; lower[i] != '\0'; ++i) {
    const char c = text[i];
    if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != lower[i]) return false;
  }
  return true;
}

void StoreHost(std::atomic<std::uint64_t>* words, const ConnectHost& host) noexcept {
  char padded[kConnectHostBytes] = {};
  std::memcpy(padded, host.text, kConnectHostBytes);
  for (std::size_t i = 0; i < kHostWords; ++i) {
    std::uint64_t word = 0;
    std::memcpy(&word, padded + i * kWordBytes, kWordBytes);
    words[i].store(word, std::memory_order_release);
  }
}

void LoadHost(const std::atomic<std::uint64_t>* words, char* out) noexcept {
  for (std::size_t i = 0; i < kHostWords; ++i) {
    const std::uint64_t word = words[i].load(std::memory_order_acquire);
    std::memcpy(out + i * kWordBytes, &word, kWordBytes);
  }
  out[kConnectHostBytes - 1] = '\0';
}

bool EndsWith(const char* text, const char* suffix) noexcept {
  const std::size_t t = std::strlen(text);
  const std::size_t s = std::strlen(suffix);
  return t >= s && std::memcmp(text + t - s, suffix, s) == 0;
}

// The class of the game's host, by the same rules the redirect and the block use (they take a URL, so the host gets a scheme).
const char* ClassOf(const char* host) noexcept {
  char url[sizeof(kScheme) + kConnectHostBytes];
  std::memcpy(url, kScheme, sizeof(kScheme) - 1);
  std::strncpy(url + sizeof(kScheme) - 1, host, kConnectHostBytes);
  url[sizeof(url) - 1] = '\0';
  switch (ClassifyOutboundHost(url)) {
    case OutboundHost::kRadBugSplat: return "rad_bugsplat";
    case OutboundHost::kAmazonS3: return "amazon_s3";
    case OutboundHost::kOculusGraph: return "oculus_graph";
    case OutboundHost::kApiEnvironment: return "api_environment";
    case OutboundHost::kNone: break;
  }
  if (IsApiBaseUrl(url)) return "api_base";
  // Lower-case compare of the host name part (the port is not part of the domain).
  char name[kConnectHostBytes];
  std::strncpy(name, host, sizeof(name));
  name[sizeof(name) - 1] = '\0';
  char* colon = std::strchr(name, ':');
  if (colon != nullptr) *colon = '\0';
  for (char* c = name; *c != '\0'; ++c) {
    if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c - 'A' + 'a');
  }
  if (std::strcmp(name, "readyatdawn.com") == 0 || EndsWith(name, kServiceDomain)) return "readyatdawn_other";
  return "external";
}

}  // namespace

void HostOfUrl(const char* url, ConnectHost* out) noexcept {
  std::memset(out->text, 0, kConnectHostBytes);
  out->truncated = false;
  if (url == nullptr) return;
  const char* p = url;
  if (EqualsLowerPrefix(p, kHttps)) {
    p += sizeof(kHttps) - 1;
  } else if (EqualsLowerPrefix(p, kHttp)) {
    p += sizeof(kHttp) - 1;
  }
  std::size_t end = 0;
  while (p[end] != '\0' && p[end] != '/' && p[end] != '?' && p[end] != '#') ++end;
  std::size_t start = 0;
  for (std::size_t i = 0; i < end; ++i) {
    if (p[i] == '@') start = i + 1;  // drop userinfo
  }
  std::size_t n = 0;
  for (std::size_t i = start; i < end; ++i) {
    if (n + 1 >= kConnectHostBytes) {
      out->truncated = true;
      break;
    }
    const char c = p[i];
    out->text[n++] = (c < ' ' || c > '~') ? '?' : c;
  }
}

void NoteConnect(const char* gameUrl, const char* usedUrl, int result) noexcept {
  Ring& ring = GlobalRing();
  const std::uint64_t number = ring.head.fetch_add(1, std::memory_order_relaxed);
  Slot& slot = ring.slots[number % kConnectLogSlots];
  // Being written: a reader that sees 0 leaves the slot alone. The invalidating store is ordered before the field stores by their
  // release order (StoreHost, result, flags): a release store makes every earlier store, this one included, visible first, which
  // aarch64 would not guarantee for relaxed field stores (no fence: ThreadSanitizer refuses one).
  slot.seq.store(0, std::memory_order_relaxed);
  ConnectHost game;
  HostOfUrl(gameUrl, &game);
  ConnectHost used;
  HostOfUrl(usedUrl, &used);
  StoreHost(slot.game, game);
  StoreHost(slot.used, used);
  slot.result.store(result, std::memory_order_release);
  slot.flags.store((game.truncated ? kGameTruncated : 0U) | (used.truncated ? kUsedTruncated : 0U) |
                       (gameUrl == nullptr ? kGameNull : 0U) | (usedUrl == nullptr ? kUsedNull : 0U) |
                       (gameUrl != usedUrl ? kUrlChanged : 0U),
                   std::memory_order_release);
  slot.seq.store(number + 1, std::memory_order_release);
}

void DrainConnectLog() noexcept {
  Ring& ring = GlobalRing();
  const std::uint64_t head = ring.head.load(std::memory_order_acquire);
  if (head - ring.cursor > kConnectLogSlots) {
    ring.dropped += head - ring.cursor - kConnectLogSlots;
    ring.cursor = head - kConnectLogSlots;
  }
  while (ring.cursor < head) {
    Slot& slot = ring.slots[ring.cursor % kConnectLogSlots];
    const std::uint64_t before = slot.seq.load(std::memory_order_acquire);
    if (before < ring.cursor + 1) break;  // still being written (0) or not yet published: the next pass takes it
    if (before > ring.cursor + 1) {       // a later record took the slot
      ++ring.dropped;
      ++ring.cursor;
      continue;
    }
    char game[kConnectHostBytes];
    char used[kConnectHostBytes];
    LoadHost(slot.game, game);
    LoadHost(slot.used, used);
    const std::int32_t result = slot.result.load(std::memory_order_acquire);
    const std::uint32_t flags = slot.flags.load(std::memory_order_acquire);  // acquire loads keep the re-read of seq below them
    if (slot.seq.load(std::memory_order_relaxed) != before) {  // rewritten while it was read
      ++ring.dropped;
      ++ring.cursor;
      continue;
    }
    // The redirect hands back an interned pointer and the block the refusal's own, so a changed pointer is a decision even when the
    // host text is the same; an unchanged pointer is the game's URL passed through.
    const char* decision = (flags & kGameNull) != 0 ? "null_url"
                           : (flags & kUrlChanged) == 0 ? "pass_through"
                           : std::strcmp(used, kRefusedHost) == 0 ? "refused"
                                                                  : "redirected";
    sentinel::LogFields(sentinel::LogLevel::kInfo, "http_connect",
                        {{"seq", static_cast<long long>(ring.cursor + 1)},
                         {"game_host", game},
                         {"class", ClassOf(game)},
                         {"decision", decision},
                         {"used_host", used},
                         {"result", result},
                         {"host_truncated", (flags & (kGameTruncated | kUsedTruncated)) != 0 ? 1 : 0}});
    ++ring.cursor;
  }
  if (ring.dropped != ring.droppedLogged) {
    ring.droppedLogged = ring.dropped;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "http_connect_dropped",
                        {{"total", static_cast<long long>(ring.dropped)},
                         {"consequence", "connections past the ring were not named in the log; the call counters still count them"}});
  }
}

void ResetConnectLogForTest() noexcept {
  Ring& ring = GlobalRing();
  ring.head.store(0, std::memory_order_relaxed);
  for (Slot& slot : ring.slots) slot.seq.store(0, std::memory_order_relaxed);
  ring.cursor = 0;
  ring.dropped = 0;
  ring.droppedLogged = 0;
}

}  // namespace nevr_quest::redirect
