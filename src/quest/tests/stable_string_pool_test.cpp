#include "runtime/lifecycle/stable_string_pool.h"
#include "quest/tests/stable_string_pool_vectors.h"

#include <cstddef>
#include <cstring>

int main() {
  nevr_runtime::lifecycle::StableStringPool pool;
  for (const auto& vector : nevr_quest_test::kStableStringVectors) {
    const auto first = pool.Intern(vector.value);
    if (first.status != nevr_runtime::lifecycle::InternStatus::kSuccess || first.pointer == nullptr ||
        std::strlen(first.pointer) + 1 != vector.byteCountWithTerminator) {
      return 1;
    }
    const auto repeated = pool.Intern(vector.value);
    if (repeated.status != nevr_runtime::lifecycle::InternStatus::kSuccess ||
        repeated.pointer != first.pointer) {
      return 2;
    }
  }
  return 0;
}
