# nevr_builtin_defaults.cmake — build-time defaults embedded into the runtime.
#
# The values come from the real environment, falling back to <root>/.env (git-ignored;
# see .env.example). They are written to a header in the BUILD tree only
# (generated/nevr_builtin_defaults.h): never the source tree, and never a -D on the
# compiler line, so they do not appear in compile_commands.json or build logs.
# config.yaml on the game side overrides anything embedded here.
#
# Unset or empty values embed nothing, so CI and `just verify` build without secrets.
# Configure with -DNEVR_REQUIRE_BUILTIN_DEFAULTS=ON to fail instead when any is missing
# (live-test and release builds: a configure without the env or .env must not silently
# produce a DLL that embeds nothing).

option(NEVR_REQUIRE_BUILTIN_DEFAULTS
       "Fail configure when any built-in default (NEVR_SOCKET_URI, NEVR_HTTP_URI, NEVR_HTTP_KEY, NEVR_SERVER_KEY) is empty" OFF)

# Parse <root>/.env (KEY=VALUE lines; # comments; optional single/double quotes) and set
# ENV{KEY} for every key that is unset OR empty in the real environment.
function(nevr_load_dotenv root)
  set(dotenv "${root}/.env")
  if(NOT EXISTS "${dotenv}")
    return()
  endif()
  # Re-configure when .env changes (an env change alone would not retrigger CMake). Only
  # registered when the file exists: a missing CMake input makes ninja re-run configure.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dotenv}")
  file(STRINGS "${dotenv}" lines)
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#" OR line MATCHES "^[ \t]*$")
      continue()
    endif()
    if(NOT line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
      continue()
    endif()
    set(key "${CMAKE_MATCH_1}")
    set(value "${CMAKE_MATCH_2}")
    string(REGEX REPLACE "[ \t\r]+$" "" value "${value}")
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${value}")
    string(REGEX REPLACE "^'(.*)'$" "\\1" value "${value}")
    if("$ENV{${key}}" STREQUAL "")
      set(ENV{${key}} "${value}")
    endif()
  endforeach()
endfunction()

# A value is embedded into a C++ string literal and later read back through the config
# layer, so anything that could be interpolated or split is refused at configure time.
function(_nevr_check_value env_name value)
  if(value MATCHES "\\$\\{" OR value MATCHES "[ \t\r\n;\"\\\\]")
    message(FATAL_ERROR
      "${env_name}: value contains a character that is not allowed in an embedded default "
      "(whitespace, ';', '\"', '\\' or '${'). Use a plain URL/token.")
  endif()
endfunction()

# Generate ${CMAKE_BINARY_DIR}/generated/nevr_builtin_defaults.h.
function(nevr_generate_builtin_defaults root)
  nevr_load_dotenv("${root}")
  set(missing "")
  foreach(pair
      "NEVR_SOCKET_URI;SOCKET_URI"
      "NEVR_HTTP_URI;HTTP_URI"
      "NEVR_HTTP_KEY;HTTP_KEY"
      "NEVR_SERVER_KEY;SERVER_KEY")
    list(GET pair 0 env_name)
    list(GET pair 1 var_name)
    set(raw "$ENV{${env_name}}")
    _nevr_check_value("${env_name}" "${raw}")
    set(NEVR_DEFAULT_${var_name} "${raw}")
    if(raw STREQUAL "")
      list(APPEND missing "${env_name}")
      message(STATUS "nevr builtin default ${env_name}: not set (nothing embedded)")
    else()
      string(LENGTH "${raw}" raw_len)
      # Length only: never echo a value (two of these are secrets).
      message(STATUS "nevr builtin default ${env_name}: embedded (${raw_len} chars)")
    endif()
  endforeach()
  if(NEVR_REQUIRE_BUILTIN_DEFAULTS AND missing)
    message(FATAL_ERROR
      "NEVR_REQUIRE_BUILTIN_DEFAULTS=ON but these are empty: ${missing}. "
      "Set them in the environment or in ${root}/.env (see .env.example).")
  endif()
  # A build that requires its defaults is a token-auth build (no identity in config.yaml), and
  # token auth works only through the /nevr ingress: the production /ws front replaces the
  # client's Bearer token with the server key, so the login arrives unauthenticated (#52, #65).
  if(NEVR_REQUIRE_BUILTIN_DEFAULTS AND NOT NEVR_DEFAULT_SOCKET_URI MATCHES "^wss://[^/?#]+/nevr([?].*)?$")
    message(FATAL_ERROR
      "NEVR_REQUIRE_BUILTIN_DEFAULTS=ON but NEVR_SOCKET_URI is not a wss://<host>/nevr URL. "
      "Token-auth builds must use the /nevr ingress; /ws drops the client's Bearer token (#52).")
  endif()
  configure_file("${root}/cmake/nevr_builtin_defaults.h.in"
                 "${CMAKE_BINARY_DIR}/generated/nevr_builtin_defaults.h" @ONLY
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
endfunction()
