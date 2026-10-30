#include "safe_read.h"

#include <sys/uio.h>
#include <unistd.h>

namespace q15t {

bool SafeRead(std::uintptr_t addr, void* out, std::size_t len) {
  if (len == 0) return true;
  if (addr < 0x10000 || addr > 0x0000ffffffffffffull - len) return false;
  iovec local{out, len};
  iovec remote{reinterpret_cast<void*>(addr), len};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(len);
}

}  // namespace q15t
