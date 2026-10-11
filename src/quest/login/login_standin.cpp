#include "quest/login/login_standin.h"

#include <stdlib.h>  // arc4random_buf (bionic; glibc >= 2.36)

#include <atomic>
#include <cstring>

// Built -fno-exceptions (src/quest/CMakeLists.txt): the predicates run inside game callbacks.
// All storage is constant-initialized, so nothing lands in .init_array.

namespace nevr_quest_login::StandIn {

namespace {

constexpr std::size_t kTokenHex = 40;
constexpr std::size_t kNonceHex = 32;
constexpr char kNamePrefix[] = "player-";
constexpr std::size_t kNameHex = 8;

// 0 = empty, 1 = being written, 2 = published (acquire to read the buffers).
std::atomic<int> g_state{0};
std::uint64_t g_org = 0;
char g_org_text[21] = {};  // "%llu" of g_org; u64 max is 20 digits + NUL
char g_token[kTokenHex + 1] = {};
char g_nonce[kNonceHex + 1] = {};
char g_name[sizeof(kNamePrefix) - 1 + kNameHex + 1] = {};

bool Published() { return g_state.load(std::memory_order_acquire) == 2; }

void Hex(const unsigned char* bytes, std::size_t count, char* out) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0xf];
  }
  out[2 * count] = '\0';
}

#if defined(NEVR_QUEST_TESTING)
void Copy(char* dst, std::size_t capacity, const char* src) {
  std::size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < capacity && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}
#endif

// True when NUL-terminated `value` equals the stand-in `standin` (a NUL-terminated array of
// `standin_size` bytes). Reads `value` only up to the first mismatch, so never past its own NUL.
bool Equal(const char* value, const char* standin, std::size_t standin_size) {
  if (value == nullptr || standin[0] == '\0') return false;
  for (std::size_t i = 0; i < standin_size; ++i) {
    if (value[i] != standin[i]) return false;
    if (standin[i] == '\0') return true;
  }
  return false;
}

}  // namespace

void Generate() noexcept {
  int expected = 0;
  // Only the first caller writes. A concurrent caller returns at once (no spin); until the writer
  // publishes, every accessor answers 0/"" and every predicate false, which is the safe answer.
  if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
  std::uint64_t org = 0;
  while (org == 0 || org == ~std::uint64_t{0}) arc4random_buf(&org, sizeof(org));
  unsigned char raw[kTokenHex / 2];
  arc4random_buf(raw, sizeof(raw));
  Hex(raw, kTokenHex / 2, g_token);
  arc4random_buf(raw, kNonceHex / 2);
  Hex(raw, kNonceHex / 2, g_nonce);
  arc4random_buf(raw, kNameHex / 2);
  std::memcpy(g_name, kNamePrefix, sizeof(kNamePrefix) - 1);
  Hex(raw, kNameHex / 2, g_name + sizeof(kNamePrefix) - 1);
  g_org = org;
  // decimal "%llu" without snprintf (keep this allocation-free and format-locale-free)
  char tmp[21];
  std::size_t n = 0;
  std::uint64_t v = org;
  do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0);
  for (std::size_t i = 0; i < n; ++i) g_org_text[i] = tmp[n - 1 - i];
  g_org_text[n] = '\0';
  g_state.store(2, std::memory_order_release);
}

std::uint64_t OrgId() noexcept { return Published() ? g_org : 0; }
const char* OrgIdText() noexcept { return Published() ? g_org_text : ""; }
const char* AccessToken() noexcept { return Published() ? g_token : ""; }
const char* Nonce() noexcept { return Published() ? g_nonce : ""; }
const char* OculusId() noexcept { return Published() ? g_name : ""; }

bool IsOrgId(std::uint64_t value) noexcept { return Published() && value != 0 && value == g_org; }
bool IsOrgIdText(const char* value) noexcept { return Published() && Equal(value, g_org_text, sizeof(g_org_text)); }
bool IsAccessToken(const char* value) noexcept { return Published() && Equal(value, g_token, sizeof(g_token)); }
bool IsNonce(const char* value) noexcept { return Published() && Equal(value, g_nonce, sizeof(g_nonce)); }
bool IsOculusId(const char* value, std::size_t capacity) noexcept {
  if (!Published() || value == nullptr || g_name[0] == '\0') return false;
  // The game's buffer may be full with no terminator: compare at most `capacity` bytes of it.
  for (std::size_t i = 0; i < sizeof(g_name); ++i) {
    if (i >= capacity) return false;
    if (value[i] != g_name[i]) return false;
    if (g_name[i] == '\0') return true;
  }
  return false;
}

#if defined(NEVR_QUEST_TESTING)
void SetForTest(std::uint64_t org, const char* token, const char* nonce, const char* oculus_id) noexcept {
  g_state.store(1, std::memory_order_release);
  g_org = org;
  char tmp[21];
  std::size_t n = 0;
  std::uint64_t v = org;
  do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0);
  for (std::size_t i = 0; i < n; ++i) g_org_text[i] = tmp[n - 1 - i];
  g_org_text[n] = '\0';
  Copy(g_token, sizeof(g_token), token);
  Copy(g_nonce, sizeof(g_nonce), nonce);
  Copy(g_name, sizeof(g_name), oculus_id);
  g_state.store(2, std::memory_order_release);
}

void ResetForTest() noexcept { g_state.store(0, std::memory_order_release); }
#endif

}  // namespace nevr_quest_login::StandIn
