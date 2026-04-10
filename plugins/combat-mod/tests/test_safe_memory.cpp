/*
 * test_safe_memory.cpp — Unit tests for VEH-based crash-safe memory access.
 *
 * Tests SafeMemcmp, SafeMemcpy, SafeReadU64, SafeReadU16 against valid
 * pointers, NULL, and inaccessible (PAGE_NOACCESS) memory.
 *
 * These tests only run on Windows (or Wine) since they use VirtualAlloc
 * and VEH, which are Windows-only APIs.
 */

#include <gtest/gtest.h>
#include "safe_memory.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

class SafeMemoryTest : public ::testing::Test {
protected:
    void* guard_page = nullptr;
    static constexpr size_t kPageSize = 4096;

    void SetUp() override {
        /* Allocate a page with PAGE_NOACCESS — any read/write causes AV */
        guard_page = VirtualAlloc(NULL, kPageSize,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
        ASSERT_NE(guard_page, nullptr)
            << "Failed to allocate guard page";
    }

    void TearDown() override {
        if (guard_page) {
            VirtualFree(guard_page, 0, MEM_RELEASE);
            guard_page = nullptr;
        }
    }
};

/* ── SafeMemcmp ──────────────────────────────────────────────────── */

TEST_F(SafeMemoryTest, SafeMemcmp_ValidPtr_Match) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t expected[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_TRUE(nevr::SafeMemcmp(data, expected, sizeof(data)));
}

TEST_F(SafeMemoryTest, SafeMemcmp_ValidPtr_Mismatch) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t expected[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(nevr::SafeMemcmp(data, expected, sizeof(data)));
}

TEST_F(SafeMemoryTest, SafeMemcmp_NullPtr) {
    uint8_t expected[] = {0x00};
    EXPECT_FALSE(nevr::SafeMemcmp(nullptr, expected, 1));
}

TEST_F(SafeMemoryTest, SafeMemcmp_GuardPage) {
    uint8_t expected[] = {0x00};
    EXPECT_FALSE(nevr::SafeMemcmp(guard_page, expected, 1));
}

/* ── SafeMemcpy ──────────────────────────────────────────────────── */

TEST_F(SafeMemoryTest, SafeMemcpy_ValidPtr) {
    uint8_t src[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t dst[4] = {};
    EXPECT_TRUE(nevr::SafeMemcpy(dst, src, sizeof(src)));
    EXPECT_EQ(memcmp(dst, src, sizeof(src)), 0);
}

TEST_F(SafeMemoryTest, SafeMemcpy_NullSrc) {
    uint8_t dst[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t orig[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(nevr::SafeMemcpy(dst, nullptr, 4));
    /* dst should be unchanged on failure */
    EXPECT_EQ(memcmp(dst, orig, sizeof(dst)), 0);
}

TEST_F(SafeMemoryTest, SafeMemcpy_GuardPageSrc) {
    uint8_t dst[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(nevr::SafeMemcpy(dst, guard_page, 4));
}

/* ── SafeReadU64 ─────────────────────────────────────────────────── */

TEST_F(SafeMemoryTest, SafeReadU64_ValidPtr) {
    uint64_t val = 0xDEADBEEFCAFEBABEULL;
    uint64_t out = 0;
    EXPECT_TRUE(nevr::SafeReadU64(reinterpret_cast<uintptr_t>(&val), &out));
    EXPECT_EQ(out, val);
}

TEST_F(SafeMemoryTest, SafeReadU64_NullPtr) {
    uint64_t out = 0x1234;
    EXPECT_FALSE(nevr::SafeReadU64(0, &out));
    EXPECT_EQ(out, 0x1234u) << "out should be unmodified on failure";
}

TEST_F(SafeMemoryTest, SafeReadU64_GuardPage) {
    uint64_t out = 0;
    EXPECT_FALSE(nevr::SafeReadU64(
        reinterpret_cast<uintptr_t>(guard_page), &out));
}

/* ── SafeReadU16 ─────────────────────────────────────────────────── */

TEST_F(SafeMemoryTest, SafeReadU16_ValidPtr) {
    uint16_t val = 0xBEEF;
    uint16_t out = 0;
    EXPECT_TRUE(nevr::SafeReadU16(reinterpret_cast<uintptr_t>(&val), &out));
    EXPECT_EQ(out, val);
}

TEST_F(SafeMemoryTest, SafeReadU16_NullPtr) {
    uint16_t out = 0x1234;
    EXPECT_FALSE(nevr::SafeReadU16(0, &out));
    EXPECT_EQ(out, 0x1234u);
}

/* ── Multiple sequential calls (VEH install/remove cycle) ────────── */

TEST_F(SafeMemoryTest, MultipleCallsInSequence) {
    /* Verify the VEH guard installs and removes cleanly across calls */
    uint8_t data[] = {0xAA, 0xBB};
    uint8_t expected[] = {0xAA, 0xBB};

    EXPECT_FALSE(nevr::SafeMemcmp(nullptr, expected, 1));
    EXPECT_TRUE(nevr::SafeMemcmp(data, expected, 2));
    EXPECT_FALSE(nevr::SafeMemcmp(guard_page, expected, 1));
    EXPECT_TRUE(nevr::SafeMemcmp(data, expected, 2));
}
