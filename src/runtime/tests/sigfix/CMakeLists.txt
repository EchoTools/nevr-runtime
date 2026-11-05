# The DLLs tools/gen_plugin_signature_fixtures.py signs and damages (ADR 0010, #741), built here and not by the script
# (ADR 0041). Each is a bare PE with no C runtime: `-nostdlib`, entry point DllMain, one console-subsystem DLL per
# target, written to <bin>/sigfix/<name>.dll. The script is handed that directory (--dll-dir) and copies, signs and
# patches what it finds; it runs no compiler.
#
# The target `sigfix_dlls` builds all of them. Add a fixture with sigfix_dll(); a name the script expects and this
# file does not build fails the script by name.

set(SIGFIX_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sigfix")

# sigfix_dll(<name> SOURCES <files...> [DEFINES <defs...>] [LINK <targets or libs...>])
function(sigfix_dll name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEFINES;LINK" ${ARGN})
  set(target sigfix_${name})
  add_library(${target} SHARED ${ARG_SOURCES})
  # The fixtures were -O1 builds; the directory-wide -O3 comes first on the command line, so this one wins.
  target_compile_options(${target} PRIVATE -O1)
  if(ARG_DEFINES)
    target_compile_definitions(${target} PRIVATE ${ARG_DEFINES})
  endif()
  # -mconsole after the tree-wide -mwindows: the subsystem word of these DLLs is the linker's default for a DLL.
  target_link_options(${target} PRIVATE -nostdlib -mconsole -Wl,--entry=DllMain)
  if(ARG_LINK)
    target_link_libraries(${target} PRIVATE ${ARG_LINK})
  endif()
  set_target_properties(${target} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${name}"
    RUNTIME_OUTPUT_DIRECTORY "${SIGFIX_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_property(GLOBAL APPEND PROPERTY NEVR_SIGFIX_TARGETS ${target})
endfunction()

# Two plugin-shaped files with distinct bodies, so two files never share a digest.
sigfix_dll(plain SOURCES marker.c DEFINES "SIGFIX_MARKER=\"plugin-one\"")
sigfix_dll(plain2 SOURCES marker.c DEFINES "SIGFIX_MARKER=\"plugin-two\"")

# The dependency trees: dep exports Dep; importer imports it, mid exports Mid over it, toplevel imports Mid,
# forwarder forwards Fwd to dep.Dep, delayer names dep.dll in a hand-made delay-load descriptor, broken is patched by
# the script into a file whose import directory points outside the image.
sigfix_dll(dep SOURCES dep.c)
sigfix_dll(importer SOURCES importer.c LINK sigfix_dep)
sigfix_dll(mid SOURCES mid.c LINK sigfix_dep)
sigfix_dll(toplevel SOURCES toplevel.c LINK sigfix_mid)
sigfix_dll(forwarder SOURCES stub.c forwarder.def)
sigfix_dll(delayer SOURCES delayer.c)
sigfix_dll(broken SOURCES stub.c)
sigfix_dll(dllmain_loader SOURCES dllmain_loader.c)
sigfix_dll(sysimporter SOURCES sysimporter.c LINK hid)
sigfix_dll(decoy_hid SOURCES decoy_hid.c)

# The 64-file cap: capdep_001 to capdep_065 each export one function; cap64 imports the first 64, cap65 all 65.
set(SIGFIX_CAP_COUNT 65)
set(SIGFIX_CAP_LIBS "")
foreach(index RANGE 1 ${SIGFIX_CAP_COUNT})
  # Three digits, zero padded: capdep_001 .. capdep_065.
  string(LENGTH "000${index}" padded_length)
  math(EXPR padded_start "${padded_length} - 3")
  string(SUBSTRING "000${index}" ${padded_start} 3 padded)
  sigfix_dll(capdep_${padded} SOURCES capdep.c DEFINES CAP_INDEX=${index})
  list(APPEND SIGFIX_CAP_LIBS sigfix_capdep_${padded})
endforeach()
list(SUBLIST SIGFIX_CAP_LIBS 0 64 SIGFIX_CAP_FIRST_64)
sigfix_dll(cap64 SOURCES cap_importer.c DEFINES CAP_USED=64 LINK ${SIGFIX_CAP_FIRST_64})
sigfix_dll(cap65 SOURCES cap_importer.c DEFINES CAP_USED=65 LINK ${SIGFIX_CAP_LIBS})

get_property(sigfix_targets GLOBAL PROPERTY NEVR_SIGFIX_TARGETS)
add_custom_target(sigfix_dlls DEPENDS ${sigfix_targets})
