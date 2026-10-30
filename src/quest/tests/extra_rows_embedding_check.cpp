// Writes the bytes the generated extra-rows source embeds to stdout (the host recipe compares them with the file).
#include <cstddef>
#include <cstdio>

namespace nevr_libr15_extra_rows_data {
extern const unsigned char kBytes[];
extern const std::size_t kSize;
}  // namespace nevr_libr15_extra_rows_data

int main() {
  return std::fwrite(nevr_libr15_extra_rows_data::kBytes, 1, nevr_libr15_extra_rows_data::kSize, stdout) == nevr_libr15_extra_rows_data::kSize ? 0 : 1;
}
