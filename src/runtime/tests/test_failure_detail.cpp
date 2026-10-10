// #35: the operator-facing cause text for registration rejection and token acquisition failures.

#include "runtime/server/failure_detail.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// The server sends one BroadcasterRegistrationFailureCode byte (nakama
// server/evr/broadcaster_registration_failure.go Stream: StreamByte).
TEST(FailureDetail, RejectionDecodesTheServersOneByteCode) {
  const unsigned char restricted = 8;
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection(&restricted, 1),
            "payload_bytes=1 code=8 (Restricted)");
  const unsigned char accountMissing = 4;
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection(&accountMissing, 1),
            "payload_bytes=1 code=4 (AccountDoesNotExist)");
}

TEST(FailureDetail, RejectionNamesEveryServerCode) {
  const char* const expected[] = {"InvalidRequest", "Timeout",       "CryptographyError", "DatabaseError",
                                  "AccountDoesNotExist", "ConnectionFailed", "ConnectionLost", "ProviderError",
                                  "Restricted",     "Unknown",       "Failure",           "Success"};
  for (unsigned char code = 0; code < 12; ++code) {
    EXPECT_STREQ(FailureDetail::RegistrationFailureCodeName(code), expected[code]) << int{code};
  }
  EXPECT_STREQ(FailureDetail::RegistrationFailureCodeName(12), "UnrecognizedCode");
  const unsigned char beyond = 0xFF;
  EXPECT_EQ(FailureDetail::DescribeRegistrationRejection(&beyond, 1),
            "payload_bytes=1 code=255 (UnrecognizedCode)");
}

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

TEST(FailureDetail, AuthReasonsRedactTheUrl) {
  const std::string uri = "https://user:hunter2@nk.example:7350/base?token=sekrit#frag";
  for (const std::string& reason : {FailureDetail::PasswordAuthRequestFailed(uri, "Couldn't connect", 7),
                                    FailureDetail::PasswordAuthHttpStatus(uri, 503)}) {
    EXPECT_EQ(reason.find("hunter2"), std::string::npos) << reason;
    EXPECT_EQ(reason.find("sekrit"), std::string::npos) << reason;
    EXPECT_NE(reason.find("nk.example"), std::string::npos) << reason;
  }
  EXPECT_NE(FailureDetail::PasswordAuthRequestFailed(uri, "Couldn't connect", 7).find("(curl code 7)"),
            std::string::npos);
  EXPECT_NE(FailureDetail::PasswordAuthHttpStatus(uri, 503).find("answered HTTP 503"), std::string::npos);
}

TEST(FailureDetail, ExtractAuthTokenTakesAStringToken) {
  std::string reason;
  EXPECT_EQ(FailureDetail::ExtractAuthToken(R"({"token":"abc","refresh_token":"r"})", reason), "abc");
  EXPECT_TRUE(reason.empty());
}

TEST(FailureDetail, ExtractAuthTokenNeverThrowsOnOddShapes) {
  std::string reason;
  EXPECT_EQ(FailureDetail::ExtractAuthToken(R"({"token":5})", reason), "");
  EXPECT_EQ(reason, "password auth: the response token was not a string");
  EXPECT_EQ(FailureDetail::ExtractAuthToken(R"({"token":null})", reason), "");
  EXPECT_EQ(reason, "password auth: the response token was not a string");
  EXPECT_EQ(FailureDetail::ExtractAuthToken(R"(["token"])", reason), "");
  EXPECT_EQ(reason, "password auth: the response was not a JSON object");
  EXPECT_EQ(FailureDetail::ExtractAuthToken(R"({"token":""})", reason), "");
  EXPECT_EQ(reason, "password auth: the response carried no token");
  EXPECT_EQ(FailureDetail::ExtractAuthToken(R"({})", reason), "");
  EXPECT_EQ(reason, "password auth: the response carried no token");
  EXPECT_EQ(FailureDetail::ExtractAuthToken("<html>", reason), "");
  EXPECT_EQ(reason, "password auth: the response was not valid JSON");
}

}  // namespace
