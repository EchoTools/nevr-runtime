// The reference libr15's eye-size route, pure half (#1737 C4): the RefreshRTSize body classifier, the reference build's start
// index parse, the index-to-level map and the float truncation. No game, no hook, no device.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "quest/sentinel/refresh_eye_size.h"

namespace tq = nevr_quest::texture_quality;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

void CheckEq(long long got, long long want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s: got %lld want %lld\n", what, got, want);
  }
}

// A copy of the stock body, which the cases alter.
struct Body {
  std::uint32_t words[tq::kRefreshBodyWords];
  Body() {
    std::memcpy(words, tq::kStockBody, sizeof(words));
  }
};

// The reference body: both sites call the cave.
Body ReferenceBody() {
  Body b;
  b.words[21] = tq::BlWordTo(tq::kRefreshVaddr + 4 * 21, tq::kReferenceCaveVaddr);
  b.words[25] = tq::BlWordTo(tq::kRefreshVaddr + 4 * 25, tq::kReferenceCaveVaddr);
  return b;
}

void ClassifierCases() {
  Body stock;
  tq::Classification c = tq::ClassifyRefreshBody(stock.words, tq::kRefreshVaddr, 0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kStock), "stock body is stock");
  CheckEq(c.badIndex, -1, "stock body names no bad word");

  Body ref = ReferenceBody();
  c = tq::ClassifyRefreshBody(ref.words, tq::kRefreshVaddr, tq::kReferenceCaveWord0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kReference), "reference body is reference");

  // The site words encode the cave target the reference build uses (0x1a84890 from the site at 0x1a709f4 is word 0x94004fa7).
  CheckEq(tq::BlWordTo(0x1a709f4, 0x1a84890), 0x94004fa7LL, "bl word from site 21 to the cave");
  CheckEq(static_cast<long long>(tq::BlTarget(0x94004fa7, 0x1a709f4)), 0x1a84890LL, "bl target decodes back");

  c = tq::ClassifyRefreshBody(ref.words, tq::kRefreshVaddr, 0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kUnknown), "reference body with a wrong cave word is unknown");
  CheckEq(c.badIndex, tq::kCaveBadIndex, "wrong cave word names the cave index");

  Body altered;
  altered.words[5] ^= 1;
  c = tq::ClassifyRefreshBody(altered.words, tq::kRefreshVaddr, 0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kUnknown), "an altered body word is unknown");
  CheckEq(c.badIndex, 5, "an altered body word names its index");

  Body mixed;
  mixed.words[25] = tq::BlWordTo(tq::kRefreshVaddr + 4 * 25, tq::kReferenceCaveVaddr);
  c = tq::ClassifyRefreshBody(mixed.words, tq::kRefreshVaddr, tq::kReferenceCaveWord0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kUnknown), "one stock site and one cave site is unknown");
  CheckEq(c.badIndex, 25, "mixed sites name the second site");

  Body wrongTarget;
  wrongTarget.words[21] = tq::BlWordTo(tq::kRefreshVaddr + 4 * 21, 0x1a84894);
  wrongTarget.words[25] = tq::BlWordTo(tq::kRefreshVaddr + 4 * 25, tq::kReferenceCaveVaddr);
  c = tq::ClassifyRefreshBody(wrongTarget.words, tq::kRefreshVaddr, tq::kReferenceCaveWord0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kUnknown), "a bl to another target is unknown");
  CheckEq(c.badIndex, 21, "a bl to another target names its site");
  CheckEq(c.expected, 0x94004fa7LL, "the expected word is the bl to the cave");
  CheckEq(c.actual, 0x94004fa8LL, "the actual word is the bl to the other target");

  Body nonBl;
  nonBl.words[21] = 0xd503201f;  // nop
  c = tq::ClassifyRefreshBody(nonBl.words, tq::kRefreshVaddr, 0);
  CheckEq(static_cast<int>(c.input), static_cast<int>(tq::EyeInput::kUnknown), "a non-bl site is unknown");
  CheckEq(c.badIndex, 21, "a non-bl site names its index");
}

void IndexCases() {
  bool saw = false;
  auto index = [&](const std::string& text) {
    return tq::ReferenceIndexFromSettings(text.data(), text.size(), &saw);
  };

  CheckEq(index("{\"graphics\":{\"msaa\":3}}"), 3, "msaa 3 reads 3");
  Check(saw, "msaa 3 sets sawKey");
  CheckEq(index("{\"msaa\": 9}"), 4, "msaa 9 clamps to 4");
  CheckEq(index("{\"msaa\":-1}"), 1, "msaa -1 reads the digit after the sign");
  CheckEq(index("{\"other\":1}"), 2, "no key gives the default 2");
  Check(!saw, "no key does not set sawKey");
  CheckEq(index("{\"msaa\":\"none\"}"), 2, "a key with no digit within 24 bytes gives the default");
  Check(!saw, "a key with no digit does not set sawKey");
  CheckEq(index("msaa" + std::string(23, 'x') + "3"), 3, "a digit at the 24th byte after the key counts");
  CheckEq(index("msaa" + std::string(24, 'x') + "3"), 2, "a digit at the 25th byte after the key does not count");
  CheckEq(index(std::string(15358, 'x') + "msaa3"), 2, "a key that starts past the read cap is not seen");
  CheckEq(index(std::string(15000, 'x') + "msaa3"), 3, "a key inside the read cap is seen");
  CheckEq(tq::ReferenceIndexFromSettings(nullptr, 0, &saw), 2, "a null text gives the default");
}

void LevelCases() {
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(0)), static_cast<int>(tq::Level::kLow), "index 0 is Low");
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(1)), static_cast<int>(tq::Level::kMedium), "index 1 is Medium");
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(2)), static_cast<int>(tq::Level::kHigh), "index 2 is High");
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(3)), static_cast<int>(tq::Level::kVeryHigh), "index 3 is Very High");
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(4)), static_cast<int>(tq::Level::kUltra), "index 4 is Ultra");
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(-3)), static_cast<int>(tq::Level::kLow), "a negative index clamps to Low");
  CheckEq(static_cast<int>(tq::LevelForReferenceIndex(9)), static_cast<int>(tq::Level::kUltra), "a large index clamps to Ultra");

  tq::EyeSize low3 = tq::SizeFor(tq::LevelForReferenceIndex(0), tq::Headset::kQuest3);
  CheckEq(low3.width, 1680, "index 0 on Quest 3 is 1680 wide");
  CheckEq(low3.height, 1760, "index 0 on Quest 3 is 1760 high");
  tq::EyeSize ultra3 = tq::SizeFor(tq::LevelForReferenceIndex(4), tq::Headset::kQuest3);
  CheckEq(ultra3.width, 4096, "index 4 on Quest 3 is 4096 wide");
  CheckEq(ultra3.height, 4506, "index 4 on Quest 3 is 4506 high");
}

void TruncateCases() {
  CheckEq(tq::TruncateToInt(1680.504f), 1680, "1680.504 truncates to 1680");
  CheckEq(tq::TruncateToInt(-1.9f), -1, "-1.9 truncates toward zero");
  CheckEq(tq::TruncateToInt(std::nanf("")), 0, "NaN gives 0");
  CheckEq(tq::TruncateToInt(3.0e9f), std::numeric_limits<std::int32_t>::max(), "a large value saturates high");
  CheckEq(tq::TruncateToInt(-3.0e9f), std::numeric_limits<std::int32_t>::min(), "a large negative value saturates low");
}

}  // namespace

int main() {
  ClassifierCases();
  IndexCases();
  LevelCases();
  TruncateCases();
  if (g_failures != 0) {
    std::fprintf(stderr, "refresh_eye_size_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("refresh_eye_size_test: %d checks OK\n", g_checks);
  return 0;
}
