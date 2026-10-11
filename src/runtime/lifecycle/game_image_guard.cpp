#include "runtime/lifecycle/game_image_guard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr DWORD kSupportedTimestamp = 0x6452dff6;
constexpr LONG kMaximumNtHeaderOffset = 1024 * 1024;
constexpr WORD kMaximumSectionCount = 96;
constexpr SIZE_T kMaximumOptionalHeaderSize = 4096;

bool IsReadableProtection(DWORD protection) noexcept {
  if ((protection & PAGE_GUARD) != 0 || (protection & 0xff) == PAGE_NOACCESS) return false;
  switch (protection & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

bool CheckedAdd(uintptr_t left, uintptr_t right, uintptr_t* result) noexcept {
  if (right > std::numeric_limits<uintptr_t>::max() - left) return false;
  *result = left + right;
  return true;
}

bool HasReadableSpan(const void* module, uintptr_t address, SIZE_T length) noexcept {
  if (module == nullptr || length == 0) return false;
  const uintptr_t module_address = reinterpret_cast<uintptr_t>(module);
  uintptr_t end = 0;
  if (!CheckedAdd(address, static_cast<uintptr_t>(length), &end)) return false;
  if (address < module_address) return false;

  uintptr_t cursor = address;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region)) {
      return false;
    }
    if (region.State != MEM_COMMIT ||
        reinterpret_cast<uintptr_t>(region.AllocationBase) != module_address ||
        !IsReadableProtection(region.Protect)) {
      return false;
    }

    const uintptr_t region_base = reinterpret_cast<uintptr_t>(region.BaseAddress);
    uintptr_t region_end = 0;
    if (!CheckedAdd(region_base, static_cast<uintptr_t>(region.RegionSize), &region_end) ||
        region_end <= cursor) {
      return false;
    }
    cursor = region_end < end ? region_end : end;
  }
  return true;
}

bool ReadExact(const void* module, uintptr_t address, void* destination, SIZE_T length) noexcept {
  if (!HasReadableSpan(module, address, length)) return false;
  SIZE_T bytes_read = 0;
  const BOOL read = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                      destination, length, &bytes_read);
  return read != FALSE && bytes_read == length;
}

template <SIZE_T Size>
bool ReadAtOffset(const void* module, uintptr_t offset, std::array<BYTE, Size>* bytes) noexcept {
  uintptr_t address = 0;
  if (!CheckedAdd(reinterpret_cast<uintptr_t>(module), offset, &address)) return false;
  return ReadExact(module, address, bytes->data(), bytes->size());
}

}  // namespace

bool nevr_game_image_guard::IsSupportedGameModule(const void* module) noexcept {
  if (module == nullptr) return false;

  std::array<BYTE, sizeof(IMAGE_DOS_HEADER)> dos_bytes{};
  if (!ReadAtOffset(module, 0, &dos_bytes)) return false;
  IMAGE_DOS_HEADER dos{};
  memcpy(&dos, dos_bytes.data(), sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      dos.e_lfanew > kMaximumNtHeaderOffset) {
    return false;
  }

  constexpr SIZE_T signature_size = sizeof(DWORD);
  constexpr SIZE_T file_header_size = sizeof(IMAGE_FILE_HEADER);
  constexpr SIZE_T nt_fixed_size = signature_size + file_header_size;
  std::array<BYTE, nt_fixed_size> nt_fixed_bytes{};
  if (!ReadAtOffset(module, static_cast<uintptr_t>(dos.e_lfanew), &nt_fixed_bytes)) return false;
  DWORD signature = 0;
  IMAGE_FILE_HEADER file_header{};
  memcpy(&signature, nt_fixed_bytes.data(), sizeof(signature));
  memcpy(&file_header, nt_fixed_bytes.data() + signature_size, sizeof(file_header));
  if (signature != IMAGE_NT_SIGNATURE || file_header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      file_header.NumberOfSections == 0 || file_header.NumberOfSections > kMaximumSectionCount ||
      file_header.TimeDateStamp != kSupportedTimestamp) {
    return false;
  }

  const SIZE_T optional_size = file_header.SizeOfOptionalHeader;
  constexpr SIZE_T minimum_optional_size = offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
  if (optional_size < minimum_optional_size || optional_size > kMaximumOptionalHeaderSize) return false;

  std::array<BYTE, kMaximumOptionalHeaderSize> optional_bytes{};
  uintptr_t optional_offset = 0;
  if (!CheckedAdd(static_cast<uintptr_t>(dos.e_lfanew), static_cast<uintptr_t>(nt_fixed_size),
                  &optional_offset)) {
    return false;
  }
  uintptr_t optional_address = 0;
  if (!CheckedAdd(reinterpret_cast<uintptr_t>(module), optional_offset, &optional_address) ||
      !ReadExact(module, optional_address, optional_bytes.data(), optional_size)) {
    return false;
  }

  IMAGE_OPTIONAL_HEADER64 optional_header{};
  memcpy(&optional_header, optional_bytes.data(), minimum_optional_size);
  if (optional_header.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

  uintptr_t header_end_offset = 0;
  if (!CheckedAdd(optional_offset, static_cast<uintptr_t>(optional_size), &header_end_offset)) return false;
  uintptr_t ignored_address = 0;
  if (!CheckedAdd(reinterpret_cast<uintptr_t>(module), header_end_offset, &ignored_address)) return false;

  // PE length fields are checked for internal consistency only; memory safety comes
  // from VirtualQuery spans and exact ReadProcessMemory copies above.
  if (optional_header.SizeOfHeaders < header_end_offset ||
      optional_header.SizeOfImage < optional_header.SizeOfHeaders) {
    return false;
  }
  return true;
}

void nevr_game_image_guard::RequireSupportedGameModule(HMODULE module) noexcept {
  if (IsSupportedGameModule(module)) return;
  TerminateProcess(GetCurrentProcess(), kUnsupportedImageExitCode);
  RaiseFailFastException(nullptr, nullptr, 0);
#if defined(_MSC_VER)
  __assume(0);
#else
  __builtin_unreachable();
#endif
}

void nevr_game_image_guard::RunWithSupportedGameModule(HMODULE module,
                                                void (*initialize)(HMODULE)) noexcept {
  RequireSupportedGameModule(module);
  if (initialize != nullptr) initialize(module);
}
