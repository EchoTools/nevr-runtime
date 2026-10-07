// #35: the operator-facing cause text for registration rejection and token acquisition failures.

#include "runtime/server/failure_detail.h"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(FailureDetail, RejectionNamesSizeAndQuotesTextPayload) {
  const std::string payload = "region not allowed";
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection(payload.data(), payload.size()),
            "payload_bytes=18 payload_preview=\"region not allowed\"");
}

TEST(FailureDetail, RejectionMasksBinaryQuotesAndBackslashes) {
  const unsigned char payload[] = {0x01, 'a', '"', '\\', 0xFF, 'b'};
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection(payload, sizeof(payload)),
            "payload_bytes=6 payload_preview=\".a...b\"");
}

TEST(FailureDetail, RejectionTruncatesLongPayload) {
  const std::string payload(FailureDetail::kPayloadPreviewBytes + 10, 'x');
  const std::string out = FailureDetail::DescribeRegistrationRejection(payload.data(), payload.size());
  EXPECT_NE(out.find("payload_bytes=74 "), std::string::npos);
  EXPECT_NE(out.find(std::string(FailureDetail::kPayloadPreviewBytes, 'x') + "\" (truncated)"), std::string::npos);
  EXPECT_EQ(out.find(std::string(FailureDetail::kPayloadPreviewBytes + 1, 'x')), std::string::npos);
}

TEST(FailureDetail, RejectionHandlesNullAndEmpty) {
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection(nullptr, 5), "payload_bytes=5 payload_preview=\"\"");
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection("x", 0), "payload_bytes=0 payload_preview=\"\"");
}

TEST(FailureDetail, WithCauseAppendsOrLeavesBase) {
  EXPECT_EQ(FailureDetail::WithCause("auth failed", "http status 401"), "auth failed: http status 401");
  EXPECT_EQ(FailureDetail::WithCause("auth failed", ""), "auth failed");
}

}  // namespace
