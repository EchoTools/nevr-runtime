// HTTP session API port scan (issue #1511). Drives the real ScanPorts and
// DescribeAttempts from runtime/patch/http_port_scan.h with a fake bind wrapper.

#include "runtime/patch/http_port_scan.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using nevr_http_port_scan::Attempt;
using nevr_http_port_scan::ScanPorts;

// A bind wrapper that succeeds on every port not in `taken`, failing the others
// with WSAEADDRINUSE the way the game's wrapper returns 0.
auto FakeBind(std::set<uint16_t> taken) {
    return [taken](uint16_t port) {
        Attempt attempt;
        attempt.port = port;
        if (taken.count(port) != 0) {
            attempt.result = 0;
            attempt.error = nevr_http_port_scan::kErrAddressInUse;
        } else {
            attempt.result = 1;
            attempt.error = 0;
        }
        return attempt;
    };
}

TEST(HttpPortScan, FirstPortFreeIsOneAttempt) {
    auto scan = ScanPorts(6721, FakeBind({}));
    EXPECT_TRUE(scan.bound());
    EXPECT_EQ(scan.port(), 6721);
    EXPECT_EQ(scan.attempts.size(), 1U);
}

TEST(HttpPortScan, InUseMovesToNextFreePort) {
    // Two clients on one host: the second finds 6721 held and takes 6722.
    auto scan = ScanPorts(6721, FakeBind({6721}));
    EXPECT_TRUE(scan.bound());
    EXPECT_EQ(scan.port(), 6722);
    ASSERT_EQ(scan.attempts.size(), 2U);
    EXPECT_EQ(scan.attempts[0].error, nevr_http_port_scan::kErrAddressInUse);
    EXPECT_EQ(scan.attempts[1].result, 1U);
}

TEST(HttpPortScan, GivesUpAfterFallbackLimit) {
    std::set<uint16_t> taken;
    for (uint16_t p = 6721; p <= 6721 + nevr_http_port_scan::kFallbackPorts; ++p) taken.insert(p);
    auto scan = ScanPorts(6721, FakeBind(taken));
    EXPECT_FALSE(scan.bound());
    EXPECT_EQ(scan.attempts.size(), static_cast<size_t>(nevr_http_port_scan::kFallbackPorts) + 1U);
    EXPECT_EQ(scan.attempts.back().port, 6721 + nevr_http_port_scan::kFallbackPorts);
}

TEST(HttpPortScan, StopsAtPort65535) {
    auto scan = ScanPorts(65534, FakeBind({65534, 65535}));
    EXPECT_FALSE(scan.bound());
    ASSERT_EQ(scan.attempts.size(), 2U);
    EXPECT_EQ(scan.attempts.back().port, 65535);
}

TEST(HttpPortScan, FailureMessageNamesEveryPortAndError) {
    auto all = ScanPorts(6721, FakeBind([] {
        std::set<uint16_t> s;
        for (uint16_t p = 6721; p <= 6721 + nevr_http_port_scan::kFallbackPorts; ++p) s.insert(p);
        return s;
    }()));
    const std::string text = nevr_http_port_scan::DescribeAttempts(all.attempts);
    EXPECT_NE(text.find("6721 err=10048 (address already in use)"), std::string::npos) << text;
    EXPECT_NE(text.find("6729 err=10048"), std::string::npos) << text;
}

TEST(HttpPortScan, ErrorMeaningNamesTheCommonCases) {
    EXPECT_EQ(nevr_http_port_scan::ErrorMeaning(10048), "address already in use");
    EXPECT_EQ(nevr_http_port_scan::ErrorMeaning(10013), "access denied");
    EXPECT_EQ(nevr_http_port_scan::ErrorMeaning(0), "no error reported");
}

}  // namespace
