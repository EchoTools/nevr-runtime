// Host test of the SHA-256 backends (src/quest/data/sha256.*): the scalar code and, on an aarch64 CPU that has the SHA-2 instructions,
// the hardware code give the same digest for every length and every split of the input, and the scalar code is what runs when the CPU
// does not have them. The hash of every cargo object is made once, while it downloads, so on a headset it is made by the instructions.
//
// Built for the host by `just test-quest-data`; the same source is built for aarch64 and run under qemu-user (REPORT-P3-cargo.md has
// the commands), where the hardware backend is present.

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quest/data/sha256.h"
#include "quest/tests/test_check.h"

namespace {

namespace nd = nevr_quest::data;

std::string Pattern(std::size_t n, unsigned seed) {
  std::string s(n, '\0');
  std::uint32_t x = seed * 2654435761u + 1;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = static_cast<char>(x >> 24);
  }
  return s;
}

std::string HexOf(nd::Sha256Backend backend, const std::string& data, std::size_t split = 0) {
  QCHECK(nd::Sha256SelectBackend(backend));
  nd::Sha256 h;
  if (split == 0 || split >= data.size()) {
    h.Update(data.data(), data.size());
  } else {
    h.Update(data.data(), split);
    h.Update(data.data() + split, data.size() - split);
  }
  return h.Hex();
}

void TheScalarBackendGivesTheKnownVectors() {
  QCHECK(nd::Sha256SelectBackend(nd::Sha256Backend::kScalar));
  QCHECK(std::string(nd::Sha256BackendName()) == "scalar");
  QCHECK(nd::Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  QCHECK(nd::Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  QCHECK(nd::Sha256Hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  QCHECK(nd::Sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The CPU's own word about its instructions, read here and not through the code under test: on an aarch64 Linux CPU that lists SHA2
// among its hardware capabilities (HWCAP_SHA2, bit 6) the hardware backend must be present and must be the default.
void TheHardwareBackendIsPresentExactlyWhenTheCpuHasTheInstructions() {
#if defined(__aarch64__) && defined(__linux__)
  const bool cpu = (getauxval(AT_HWCAP) & (1ul << 6)) != 0;
  std::printf("sha256_backend_test: AT_HWCAP says SHA2: %s\n", cpu ? "yes" : "no");
  QCHECK(nd::Sha256HardwareAvailable() == cpu);
#else
  QCHECK(!nd::Sha256HardwareAvailable());  // the hardware code is aarch64's
#endif
}

void TheBackendInUseIsNamedAndTheDefaultIsTheHardwareWhenThereIsAny() {
  nd::Sha256SelectBackend(nd::Sha256Backend::kAuto);
  const std::string name = nd::Sha256BackendName();
  if (nd::Sha256HardwareAvailable()) {
    QCHECK(name == "arm64-sha2");
  } else {
    QCHECK(name == "scalar");
  }
  std::printf("sha256_backend_test: default backend %s (hardware available: %s)\n", name.c_str(), nd::Sha256HardwareAvailable() ? "yes" : "no");
}

void AnUnavailableBackendCannotBeSelected() {
  if (!nd::Sha256HardwareAvailable()) {
    QCHECK(!nd::Sha256SelectBackend(nd::Sha256Backend::kArm64Sha2));
    QCHECK(std::string(nd::Sha256BackendName()) == "scalar" || std::string(nd::Sha256BackendName()) == "arm64-sha2");
    nd::Sha256SelectBackend(nd::Sha256Backend::kAuto);
    QCHECK(std::string(nd::Sha256BackendName()) == "scalar");  // a refused selection leaves the fallback in force
  }
}

void EveryBackendAgreesWithTheScalarOneForEveryLengthAndSplit() {
  if (!nd::Sha256HardwareAvailable()) {
    std::printf("sha256_backend_test: no hardware backend on this CPU; the agreement check runs on aarch64\n");
    return;
  }
  for (std::size_t n = 0; n <= 300; ++n) {
    const std::string data = Pattern(n, static_cast<unsigned>(n));
    const std::string want = HexOf(nd::Sha256Backend::kScalar, data);
    QCHECK(HexOf(nd::Sha256Backend::kArm64Sha2, data) == want);
    for (const std::size_t split : {std::size_t{1}, std::size_t{7}, std::size_t{55}, std::size_t{56}, std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{128}}) {
      QCHECK(HexOf(nd::Sha256Backend::kArm64Sha2, data, split) == want);
    }
  }
  const std::string big = Pattern(3 * 1024 * 1024 + 13, 99);
  QCHECK(HexOf(nd::Sha256Backend::kArm64Sha2, big) == HexOf(nd::Sha256Backend::kScalar, big));
  // The vectors, by the instructions.
  QCHECK(nd::Sha256SelectBackend(nd::Sha256Backend::kArm64Sha2));
  QCHECK(nd::Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string million(1000000, 'a');
  QCHECK(nd::Sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  nd::Sha256SelectBackend(nd::Sha256Backend::kAuto);
}

void AStateHashedByOneBackendContinuesOnTheOther() {
  if (!nd::Sha256HardwareAvailable()) return;
  const std::string data = Pattern(1000, 5);
  nd::Sha256SelectBackend(nd::Sha256Backend::kScalar);
  nd::Sha256 h;
  h.Update(data.data(), 500);
  nd::Sha256SelectBackend(nd::Sha256Backend::kArm64Sha2);  // switched mid-stream: the state is the same words either way
  h.Update(data.data() + 500, 500);
  nd::Sha256SelectBackend(nd::Sha256Backend::kScalar);
  QCHECK(h.Hex() == nd::Sha256Hex(data.data(), data.size()));
  nd::Sha256SelectBackend(nd::Sha256Backend::kAuto);
}

}  // namespace

int main() {
  TheScalarBackendGivesTheKnownVectors();
  TheHardwareBackendIsPresentExactlyWhenTheCpuHasTheInstructions();
  TheBackendInUseIsNamedAndTheDefaultIsTheHardwareWhenThereIsAny();
  AnUnavailableBackendCannotBeSelected();
  EveryBackendAgreesWithTheScalarOneForEveryLengthAndSplit();
  AStateHashedByOneBackendContinuesOnTheOther();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "sha256_backend_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("sha256_backend_test: all checks passed\n");
  return 0;
}
