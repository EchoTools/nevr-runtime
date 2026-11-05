# Shared by gen_donor_wrapper.cmake (build time) and check_donor_got.cmake (post-link): reads a donor object's assembly (a libsrc
# <hash16>.donor.s) and lists the symbols its code takes the address of through the GOT.
#
# nevr_donor_got_operands(<donor.s> <out-var>) sets <out-var> to the sorted, de-duplicated symbols S named by an
# `adrp xN, :got:S` or `ldr xN, [xN, :got_lo12:S]` line. It stops with FATAL_ERROR when
#   - a line that names :got: or :got_lo12: has an operand it does not recognise (anything but a plain symbol name),
#   - S is not defined as a label in the file (`S:` at the start of a line): the wrapper's .globl would then declare an undefined
#     symbol, and the donor's loads would resolve against whatever the link finds, and
#   - S is already declared .globl, .global or .weak in the file: the wrapper hides every symbol it declares, and a symbol the donor
#     exports must stay exported.
#
# nevr_donor_refuse(<message>) is how every refusal here and in check_donor_got.cmake stops. When NEVR_LIB names a library (the
# post-link check), it deletes that file first: a library the check rejected must not stay in the build tree, where a later
# link-only change would let the build treat it as built and checked.
#
# The specifier is matched in any case (`:GOT:` assembles to the same folded relocation as `:got:`), so an upper-case spelling
# reaches the strict parser and is refused there. A `.globl`, `.global` or `.weak` line must name exactly one symbol; a list
# (`.globl a, b`) is refused, because the wrapper's `.hidden` would hide a symbol the list exports.
#
# The donor files are CRLF text; CMake's file(STRINGS) reads a CRLF as one line end.

function(nevr_donor_refuse message)
  if(NEVR_LIB)
    file(REMOVE "${NEVR_LIB}")
  endif()
  message(FATAL_ERROR "${message}")
endfunction()

function(nevr_donor_got_operands donor out_var)
  file(STRINGS "${donor}" nevr_got_lines REGEX ":[Gg][Oo][Tt](_[Ll][Oo]12)?:")
  set(nevr_operands "")
  foreach(nevr_line IN LISTS nevr_got_lines)
    # The line is `adrp x0, :got:S` or `ldr x0, [x0, :got_lo12:S]`: the operand runs to the end, bar the closing bracket.
    if(NOT nevr_line MATCHES "^[ \t]*(adrp[ \t]+[a-z0-9]+,[ \t]*:got:|ldr[ \t]+[a-z0-9]+,[ \t]*\\[[a-z0-9]+,[ \t]*:got_lo12:)([A-Za-z0-9_.$]+)\\]?[ \t\r]*$")
      nevr_donor_refuse("donor GOT: ${donor}: a :got: operand this tool does not read: ${nevr_line}")
    endif()
    list(APPEND nevr_operands "${CMAKE_MATCH_2}")
  endforeach()
  list(REMOVE_DUPLICATES nevr_operands)
  list(SORT nevr_operands)

  file(STRINGS "${donor}" nevr_label_lines REGEX "^[A-Za-z_.$][A-Za-z0-9_.$]*:")
  set(nevr_labels "")
  foreach(nevr_line IN LISTS nevr_label_lines)
    string(REGEX MATCH "^[A-Za-z_.$][A-Za-z0-9_.$]*" nevr_label "${nevr_line}")
    list(APPEND nevr_labels "${nevr_label}")
  endforeach()
  file(STRINGS "${donor}" nevr_global_lines REGEX "^[ \t]*\\.(globl|global|weak)[ \t]")
  set(nevr_globals "")
  foreach(nevr_line IN LISTS nevr_global_lines)
    if(NOT nevr_line MATCHES "^[ \t]*\\.(globl|global|weak)[ \t]+([A-Za-z0-9_.$]+)[ \t\r]*(([#@]|//).*)?$")
      nevr_donor_refuse("donor GOT: ${donor}: a global or weak declaration that does not name exactly one symbol: ${nevr_line}")
    endif()
    set(nevr_global "${CMAKE_MATCH_2}")
    list(APPEND nevr_globals "${nevr_global}")
  endforeach()

  foreach(nevr_symbol IN LISTS nevr_operands)
    list(FIND nevr_labels "${nevr_symbol}" nevr_at)
    if(nevr_at EQUAL -1)
      nevr_donor_refuse("donor GOT: ${donor}: ${nevr_symbol} is a :got: operand but not a label defined in the file")
    endif()
    list(FIND nevr_globals "${nevr_symbol}" nevr_at)
    if(NOT nevr_at EQUAL -1)
      nevr_donor_refuse("donor GOT: ${donor}: ${nevr_symbol} is a :got: operand and is already declared global or weak in the file")
    endif()
  endforeach()
  set(${out_var} "${nevr_operands}" PARENT_SCOPE)
endfunction()
