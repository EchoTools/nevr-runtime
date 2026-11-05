# Post-link check of one combat library that links a donor object: every symbol the donor's code loads through the GOT has a GOT
# slot that holds that symbol's own address. Run by the library's POST_BUILD step (CMakeLists.txt) as
#   cmake -DNEVR_LLVM_BIN=<dir> -DNEVR_LIB=<lib.so> -DNEVR_DONOR=<donor.s> -P check_donor_got.cmake
#
# The symbols are those donor_got_operands.cmake reads from the donor's assembly. The library's own symbol table gives each one's
# address, and its R_AARCH64_RELATIVE relocations in .got give each slot's content (a GOT slot of a symbol the library defines is
# filled at load by a relative relocation whose addend is the symbol's address). A library whose GOT has folded the donor's
# loads into one slot per section has slots for the section starts only, so the other symbols have none. A symbol with no slot
# holding its address is a failure naming it; the build stops.
#
# This reads the addresses, not the instructions: that each load names its own slot is what the assembler's wrapper
# (gen_donor_wrapper.cmake) arranges, and a slot per symbol is what the link has to produce for it.
foreach(nevr_arg NEVR_LLVM_BIN NEVR_LIB NEVR_DONOR)
  if(NOT ${nevr_arg})
    message(FATAL_ERROR "check_donor_got: pass -D${nevr_arg}=<path>")
  endif()
endforeach()
get_filename_component(nevr_lib_name "${NEVR_LIB}" NAME)
foreach(nevr_file "${NEVR_LIB}" "${NEVR_DONOR}" "${NEVR_LLVM_BIN}/llvm-nm" "${NEVR_LLVM_BIN}/llvm-readelf")
  if(NOT EXISTS "${nevr_file}")
    message(FATAL_ERROR "check_donor_got: ${nevr_lib_name}: missing ${nevr_file}")
  endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/donor_got_operands.cmake")
nevr_donor_got_operands("${NEVR_DONOR}" nevr_operands)
list(LENGTH nevr_operands nevr_count)
if(nevr_count EQUAL 0)
  message(STATUS "check_donor_got: OK ${nevr_lib_name}: the donor loads no address through the GOT")
  return()
endif()

function(nevr_tool_output out_var)
  execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE nevr_out ERROR_VARIABLE nevr_err RESULT_VARIABLE nevr_rc)
  if(NOT nevr_rc EQUAL 0)
    message(FATAL_ERROR "check_donor_got: ${nevr_lib_name}: ${ARGN} exited ${nevr_rc}: ${nevr_err}")
  endif()
  set(${out_var} "${nevr_out}" PARENT_SCOPE)
endfunction()

# An address, as the tools print it, without its leading zeros and in lower case: the key a slot or a symbol is compared by.
function(nevr_address_key out_var address)
  string(TOLOWER "${address}" nevr_key)
  string(REGEX REPLACE "^0x" "" nevr_key "${nevr_key}")
  string(REGEX REPLACE "^0+" "" nevr_key "${nevr_key}")
  if(nevr_key STREQUAL "")
    set(nevr_key "0")
  endif()
  set(${out_var} "${nevr_key}" PARENT_SCOPE)
endfunction()

# .got: its address and size, from the section table (`[ 9] .got PROGBITS 0000000000024fb0 023fb0 000050 08 WA 0 0 8`).
nevr_tool_output(nevr_sections "${NEVR_LLVM_BIN}/llvm-readelf" -S -W "${NEVR_LIB}")
if(NOT nevr_sections MATCHES "\\]+[ \t]+\\.got[ \t]+PROGBITS[ \t]+([0-9a-f]+)[ \t]+[0-9a-f]+[ \t]+([0-9a-f]+)[ \t]")
  message(FATAL_ERROR "check_donor_got: ${nevr_lib_name}: no .got section, and the donor loads ${nevr_count} addresses through the GOT")
endif()
math(EXPR nevr_got_begin "0x${CMAKE_MATCH_1}")
math(EXPR nevr_got_end "0x${CMAKE_MATCH_1} + 0x${CMAKE_MATCH_2}")

# The content of every slot of .got that a relative relocation fills: `0000000000024ff0 0000000000000403 R_AARCH64_RELATIVE 24f30`.
nevr_tool_output(nevr_relocs "${NEVR_LLVM_BIN}/llvm-readelf" -r -W "${NEVR_LIB}")
string(REGEX MATCHALL "[0-9a-fA-F]+[ \t]+[0-9a-fA-F]+[ \t]+R_AARCH64_RELATIVE[ \t]+[0-9a-fA-F]+" nevr_relative "${nevr_relocs}")
set(nevr_slot_addresses "")
foreach(nevr_reloc IN LISTS nevr_relative)
  string(REGEX MATCH "^([0-9a-fA-F]+)[ \t]+[0-9a-fA-F]+[ \t]+R_AARCH64_RELATIVE[ \t]+([0-9a-fA-F]+)$" nevr_unused "${nevr_reloc}")
  math(EXPR nevr_slot "0x${CMAKE_MATCH_1}")
  if(nevr_slot GREATER_EQUAL nevr_got_begin AND nevr_slot LESS nevr_got_end)
    nevr_address_key(nevr_key "${CMAKE_MATCH_2}")
    set(nevr_slot_holds_${nevr_key} TRUE)
  endif()
endforeach()

# Each symbol's address, from the library's symbol table (the donor's labels are local symbols, which the link keeps).
nevr_tool_output(nevr_symbols "${NEVR_LLVM_BIN}/llvm-nm" "${NEVR_LIB}")
string(REGEX MATCHALL "[^\n]+" nevr_symbol_lines "${nevr_symbols}")
foreach(nevr_line IN LISTS nevr_symbol_lines)
  if(nevr_line MATCHES "^([0-9a-fA-F]+)[ \t]+[a-zA-Z][ \t]+([A-Za-z0-9_.$]+)$")
    nevr_address_key(nevr_key "${CMAKE_MATCH_1}")
    set(nevr_address_of_${CMAKE_MATCH_2} "${nevr_key}")
  endif()
endforeach()

set(nevr_wrong "")
set(nevr_wrong_count 0)
foreach(nevr_symbol IN LISTS nevr_operands)
  if(NOT DEFINED nevr_address_of_${nevr_symbol})
    list(APPEND nevr_wrong "${nevr_symbol} (not in the symbol table)")
    math(EXPR nevr_wrong_count "${nevr_wrong_count} + 1")
  elseif(NOT nevr_slot_holds_${nevr_address_of_${nevr_symbol}})
    list(APPEND nevr_wrong "${nevr_symbol} (at 0x${nevr_address_of_${nevr_symbol}}, in no GOT slot)")
    math(EXPR nevr_wrong_count "${nevr_wrong_count} + 1")
  endif()
endforeach()
if(nevr_wrong_count GREATER 0)
  list(LENGTH nevr_wrong nevr_wrong_total)
  list(SUBLIST nevr_wrong 0 3 nevr_first)
  string(REPLACE ";" ", " nevr_first "${nevr_first}")
  message(FATAL_ERROR "check_donor_got: ${nevr_lib_name}: ${nevr_wrong_count} of ${nevr_count} symbols the donor loads through the GOT have no GOT slot that holds their address, first: ${nevr_first}")
endif()
message(STATUS "check_donor_got: OK ${nevr_lib_name}: ${nevr_count} of ${nevr_count} symbols the donor loads through the GOT have a GOT slot holding their own address")
