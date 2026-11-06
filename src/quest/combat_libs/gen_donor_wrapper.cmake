# Writes the assembly file that is assembled in place of a libsrc <hash16>.donor.s: it includes the donor and declares every symbol
# the donor's code loads through the GOT global and hidden. Run by the donor wrapper command (CMakeLists.txt) as
#   cmake -DNEVR_DONOR=<donor.s> -DNEVR_WRAPPER=<out.s> -P gen_donor_wrapper.cmake
#
# Why: the NDK r21b assembler writes a `:got:` load of a label that is local to its file against the section symbol plus the
# label's offset (`.bss + 0x48`), and r21b's GNU ld makes one GOT entry per symbol and drops the offset, so every such load in a
# section reads the same slot, and a donor function that loads the address of a static it owns calls or writes through the
# last pointer bound into that slot. A global symbol is relocated against itself, which gets its own slot. `.hidden` keeps the
# symbol out of the library's dynamic symbol table (the link makes it local), so nothing outside the library can override it, and a
# direct `adrp`/`add` to it stays legal in a shared object.
if(NOT NEVR_DONOR OR NOT NEVR_WRAPPER)
  message(FATAL_ERROR "gen_donor_wrapper: pass -DNEVR_DONOR=<donor.s> and -DNEVR_WRAPPER=<out.s>")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/donor_got_operands.cmake")
nevr_donor_got_operands("${NEVR_DONOR}" nevr_operands)

set(nevr_text "\t.include\t\"${NEVR_DONOR}\"\n")
foreach(nevr_symbol IN LISTS nevr_operands)
  string(APPEND nevr_text "\t.globl\t${nevr_symbol}\n\t.hidden\t${nevr_symbol}\n")
endforeach()
file(WRITE "${NEVR_WRAPPER}" "${nevr_text}")
list(LENGTH nevr_operands nevr_count)
message(STATUS "gen_donor_wrapper: ${NEVR_WRAPPER}: ${nevr_count} :got: symbols made global and hidden")
