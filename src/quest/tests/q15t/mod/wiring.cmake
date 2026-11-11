# plugin.cpp's wiring: (1) against a fake image, the plugin installs no module and leaves levelname's and boosterpool's words alone (they are the
# runtime's, #2181), and returns 0; (2) the capability matrix. Module objects are compiled once, with the plugin's own flags.
if(NOT Q15T_ARM64)
  set(_wmods plugin_log script_redirect import_hook combat_map game_tick safe_read got_swap gamespace)
  set(_wsources "")
  foreach(m IN LISTS _wmods)
    list(APPEND _wsources "${Q15T_PLUGIN_SRC}/${m}.cpp")
  endforeach()
  set(_wopts -g -O0 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
  add_library(nevr_q15t_wiring_modules OBJECT ${_wsources})
  target_compile_options(nevr_q15t_wiring_modules PRIVATE ${_wopts})
  target_include_directories(nevr_q15t_wiring_modules PRIVATE "${Q15T_PLUGIN_SRC}" "${Q15T_SDK_INCLUDE}")
  add_dependencies(mod_wiring nevr_q15t_wiring_modules)
endif()

set(_wslots)
set(_wbuild "Q15T_BUILD_ID_FOR_TEST=\"${Q15T_BUILD_ID_FOR_TEST}\"")
set(_winclude "SHELL:-include ${Q15T_TESTS_DIR}/host_overrides.h")

# wiring_program(<program> <defs>...): wiring_test.cpp and plugin.cpp built with the defines, linked with the module objects
function(wiring_program program)
  q15t_host_test(${program} TEST wiring_test.cpp SRC plugin BASE_OPTS ${_wopts} OPTS "${_winclude}"
    OBJECTS $<TARGET_OBJECTS:nevr_q15t_wiring_modules> DEFS ${ARGN} LINK_OPTS -rdynamic LIBS dl pthread)
endfunction()

wiring_program(wiring_test ${_wslots} ${_wbuild})
# (1a) no module on: no module of the plugin registers a game tick, so InitEx installs none, and it still registers the script libraries once
wiring_program(wiring_no_tick WIRING_NO_TICK ${_wslots} ${_wbuild})
# capability matrix: <label> <expect ALTERS_GAMEPLAY> <expect NETWORK> <defines>...; nothing else on
function(wiring_caps label alters network)
  string(MAKE_C_IDENTIFIER "${label}" id)
  wiring_program(wiring_caps_${id} WIRING_CAPS_ONLY "WIRING_LABEL=\"${label}\"" EXPECT_ALTERS=${alters} EXPECT_NETWORK=${network} ${ARGN} ${_wslots})
endfunction()
wiring_caps(none 0 0)
