# Build a minimal google_breakpad Linux/Android *client* static library for
# arm64-v8a — enough to install an ExceptionHandler and write real minidumps.
# Source list mirrors extern/breakpad/android/google_breakpad/Android.mk.

enable_language(ASM)

# breakpad.cmake is include()d from src/quest/sentinel/CMakeLists.txt, so
# CMAKE_CURRENT_SOURCE_DIR is src/quest/sentinel — repo root is three up.
set(_BP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/breakpad")
get_filename_component(_BP_ROOT "${_BP_ROOT}" ABSOLUTE)
set(_BP "${_BP_ROOT}/src")
set(_LSS_HDR "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/lss/linux_syscall_support.h")
get_filename_component(_LSS_HDR "${_LSS_HDR}" ABSOLUTE)

if(NOT EXISTS "${_BP}/client/linux/handler/exception_handler.cc")
  message(FATAL_ERROR "breakpad submodule missing — run: git submodule update --init extern/breakpad")
endif()
if(NOT EXISTS "${_LSS_HDR}")
  message(FATAL_ERROR "lss submodule missing — run: git submodule update --init extern/lss")
endif()

# Bridge extern/lss into breakpad's expected third_party/lss/ include layout,
# in the build tree (so neither submodule working tree is mutated).
set(_LSS_SHIM "${CMAKE_BINARY_DIR}/lss_shim")
file(MAKE_DIRECTORY "${_LSS_SHIM}/third_party/lss")
if(NOT EXISTS "${_LSS_SHIM}/third_party/lss/linux_syscall_support.h")
  file(CREATE_LINK "${_LSS_HDR}" "${_LSS_SHIM}/third_party/lss/linux_syscall_support.h" SYMBOLIC)
endif()

add_library(breakpad_client STATIC
  ${_BP}/client/linux/crash_generation/crash_generation_client.cc
  ${_BP}/client/linux/dump_writer_common/thread_info.cc
  ${_BP}/client/linux/dump_writer_common/ucontext_reader.cc
  ${_BP}/client/linux/handler/exception_handler.cc
  ${_BP}/client/linux/handler/minidump_descriptor.cc
  ${_BP}/client/linux/log/log.cc
  ${_BP}/client/linux/microdump_writer/microdump_writer.cc
  ${_BP}/client/linux/minidump_writer/linux_dumper.cc
  ${_BP}/client/linux/minidump_writer/linux_ptrace_dumper.cc
  ${_BP}/client/linux/minidump_writer/minidump_writer.cc
  ${_BP}/client/linux/minidump_writer/pe_file.cc
  ${_BP}/client/minidump_file_writer.cc
  ${_BP}/common/convert_UTF.cc
  ${_BP}/common/md5.cc
  ${_BP}/common/string_conversion.cc
  ${_BP}/common/linux/breakpad_getcontext.S
  ${_BP}/common/linux/elfutils.cc
  ${_BP}/common/linux/file_id.cc
  ${_BP}/common/linux/guid_creator.cc
  ${_BP}/common/linux/linux_libc_support.cc
  ${_BP}/common/linux/memory_mapped_file.cc
  ${_BP}/common/linux/safe_readlink.cc
)

# breakpad's Android shim headers must be searched BEFORE the NDK's, and the
# lss shim must resolve "third_party/lss/linux_syscall_support.h".
# ${_BP} and the lss shim are PUBLIC: consumers that include the breakpad
# handler headers (which transitively need lss) inherit them. The android/
# shim headers are only needed by breakpad's own .cc, so stay PRIVATE.
target_include_directories(breakpad_client BEFORE
  PUBLIC  ${_BP} ${_LSS_SHIM}
  PRIVATE ${_BP}/common/android/include)

target_compile_options(breakpad_client PRIVATE
  -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
  -funwind-tables)
set_target_properties(breakpad_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
