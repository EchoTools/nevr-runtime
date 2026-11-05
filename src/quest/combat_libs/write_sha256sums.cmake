# Writes <NEVR_OUT_DIR>/SHA256SUMS: "<sha256>  <name>" for each library named in NEVR_NAMES_FILE, sorted by name.
# Run by the combat_libs_sha256sums target (CMakeLists.txt) as `cmake -P`.
file(STRINGS "${NEVR_NAMES_FILE}" nevr_names)
list(SORT nevr_names)
set(nevr_sums "")
foreach(nevr_name IN LISTS nevr_names)
  if(NOT EXISTS "${NEVR_OUT_DIR}/${nevr_name}")
    message(FATAL_ERROR "build-quest-combat-libs: ${nevr_name} was not built: ${NEVR_OUT_DIR}/${nevr_name} is missing")
  endif()
  file(SHA256 "${NEVR_OUT_DIR}/${nevr_name}" nevr_sum)
  string(APPEND nevr_sums "${nevr_sum}  ${nevr_name}\n")
endforeach()
file(WRITE "${NEVR_OUT_DIR}/SHA256SUMS" "${nevr_sums}")
