// Quest libr15.so VA 0x10824ac; ReVault binary 576.
// ReVault raw parent 100207837, reconstruction node 100600573; grade H.
#include <cstdint>

namespace NRadEngine {

class CSysString {
public:
    static unsigned long Compare(const char* left, const char* right,
                                 unsigned int case_sensitive,
                                 unsigned long long max_length);
};

// @0x10824ac — NRadEngine::CSysString::Compare
/* @confidence: H — both byte-comparison paths, ASCII folding, bound checks, and signed ordering match all 39 instructions. */
unsigned long CSysString::Compare(const char* left, const char* right,
                                  unsigned int case_sensitive,
                                  unsigned long long max_length) {
    if (case_sensitive == 0) {
        while (max_length != 0) {
            const auto left_byte = static_cast<unsigned char>(*left);
            const auto right_byte = static_cast<unsigned char>(*right);
            if ((left_byte | right_byte) == 0) {
                return 0;
            }

            auto folded_left = left_byte;
            const auto left_offset =
                static_cast<unsigned char>(left_byte - 0x41U);
            if (left_offset > 0x19U) {
                folded_left = left_byte;
            } else {
                folded_left = static_cast<unsigned char>(left_byte + 0x20U);
            }

            auto folded_right = right_byte;
            const auto right_offset =
                static_cast<unsigned char>(right_byte - 0x41U);
            if (right_offset > 0x19U) {
                folded_right = right_byte;
            } else {
                folded_right =
                    static_cast<unsigned char>(right_byte + 0x20U);
            }

            const int signed_left =
                folded_left < 0x80U
                    ? static_cast<int>(folded_left)
                    : static_cast<int>(folded_left) - 0x100;
            const int signed_right =
                folded_right < 0x80U
                    ? static_cast<int>(folded_right)
                    : static_cast<int>(folded_right) - 0x100;
            if (signed_right < signed_left) {
                return 1UL;
            }
            if (signed_left < signed_right) {
                return static_cast<unsigned long>(-1);
            }

            ++left;
            ++right;
            --max_length;
        }
        return 0;
    }

    while (max_length != 0) {
        const auto left_byte = static_cast<unsigned char>(*left);
        const auto right_byte = static_cast<unsigned char>(*right);
        if ((left_byte | right_byte) == 0) {
            return 0;
        }

        const int signed_left =
            left_byte < 0x80U ? static_cast<int>(left_byte)
                              : static_cast<int>(left_byte) - 0x100;
        const int signed_right =
            right_byte < 0x80U ? static_cast<int>(right_byte)
                              : static_cast<int>(right_byte) - 0x100;
        if (signed_right < signed_left) {
            return 1UL;
        }
        if (signed_left < signed_right) {
            return static_cast<unsigned long>(-1);
        }

        ++left;
        ++right;
        --max_length;
    }
    return 0;
}

}  // namespace NRadEngine
