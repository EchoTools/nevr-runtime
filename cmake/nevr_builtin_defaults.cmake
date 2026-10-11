# nevr_builtin_defaults.cmake — build-time defaults embedded into the runtime.
#
# The values are the public client defaults: <root>/config/public-defaults.env, which is git-ignored.
# CI writes it from the repository's Actions variables (.github/actions/write-public-defaults); a
# developer copies config/public-defaults.env.example and fills the four values.
# (the service endpoints and the public keys every client embeds). The build reads that file and
# nothing else: not the environment, not .env. A clean checkout therefore builds a client that logs
# in with no config file, and no build depends on what a developer's machine holds.
#
# They are written to a header in the BUILD tree only (generated/nevr_builtin_defaults.h), never a
# -D on the compiler line, so they do not appear in compile_commands.json or build logs.
# At run time config.yaml overrides what is embedded here, and the environment variables
# NEVR_API_KEY / NEVR_SOCKET_KEY override both for the two keys (#76); the build never reads them.
#
# The file must define exactly the four keys below, non-empty: a missing, empty, unknown or
# repeated key fails the configure.

set(_NEVR_DEFAULT_KEYS NEVR_SOCKET_URI NEVR_HTTP_URI NEVR_PUBLIC_API_KEY NEVR_PUBLIC_SOCKET_KEY)

# A value is embedded into a C++ string literal and later read back through the config
# layer, so anything that could be interpolated or split is refused at configure time.
function(_nevr_check_value key value)
  if(value MATCHES "\\$\\{" OR value MATCHES "[ \t\r\n;\"\\\\]")
    message(FATAL_ERROR
      "${key}: value contains a character that is not allowed in an embedded default "
      "(whitespace, ';', '\"', '\\' or '${'). Use a plain URL/token.")
  endif()
endfunction()

# Generate ${CMAKE_BINARY_DIR}/generated/nevr_builtin_defaults.h from <root>/config/public-defaults.env.
function(nevr_generate_builtin_defaults root)
  set(defaults_file "${root}/config/public-defaults.env")
  if(NOT EXISTS "${defaults_file}")
    message(FATAL_ERROR
      "missing ${defaults_file}: the public client defaults the build embeds. Copy "
      "${root}/config/public-defaults.env.example to it and fill the four values (CI writes it from the "
      "repository's Actions variables).")
  endif()
  # Re-configure when the file changes.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${defaults_file}")
  file(STRINGS "${defaults_file}" lines)
  foreach(key IN LISTS _NEVR_DEFAULT_KEYS)
    set(seen_${key} FALSE)
  endforeach()
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#" OR line MATCHES "^[ \t]*$")
      continue()
    endif()
    if(NOT line MATCHES "^([A-Z][A-Z0-9_]*)=(.*)$")
      message(FATAL_ERROR "${defaults_file}: a line is not KEY=VALUE, a comment or blank.")
    endif()
    set(key "${CMAKE_MATCH_1}")
    set(value "${CMAKE_MATCH_2}")
    if(NOT key IN_LIST _NEVR_DEFAULT_KEYS)
      message(FATAL_ERROR "${defaults_file}: unknown key ${key}.")
    endif()
    if(seen_${key})
      message(FATAL_ERROR "${defaults_file}: ${key} is defined twice.")
    endif()
    if(value STREQUAL "")
      message(FATAL_ERROR "${defaults_file}: ${key} is empty.")
    endif()
    _nevr_check_value("${key}" "${value}")
    set(seen_${key} TRUE)
    set(value_${key} "${value}")
  endforeach()
  foreach(key IN LISTS _NEVR_DEFAULT_KEYS)
    if(NOT seen_${key})
      message(FATAL_ERROR "${defaults_file}: ${key} is not defined.")
    endif()
    # Length only: the keys are public by design, but build logs are not where they belong.
    string(LENGTH "${value_${key}}" value_len)
    message(STATUS "nevr builtin default ${key}: embedded (${value_len} chars)")
  endforeach()
  # The client is a token-auth build (no identity in config.yaml), and token auth works only through
  # the /nevr ingress: the production /ws front replaces the client's Bearer token with the server
  # key, so the login arrives unauthenticated (#52, #65).
  if(NOT value_NEVR_SOCKET_URI MATCHES "^wss://[^/?#]+/nevr([?].*)?$")
    message(FATAL_ERROR
      "${defaults_file}: NEVR_SOCKET_URI is not a wss://<host>/nevr URL. "
      "Token-auth clients must use the /nevr ingress; /ws drops the client's Bearer token (#52).")
  endif()
  set(NEVR_DEFAULT_SOCKET_URI "${value_NEVR_SOCKET_URI}")
  set(NEVR_DEFAULT_HTTP_URI "${value_NEVR_HTTP_URI}")
  set(NEVR_DEFAULT_PUBLIC_API_KEY "${value_NEVR_PUBLIC_API_KEY}")
  set(NEVR_DEFAULT_PUBLIC_SOCKET_KEY "${value_NEVR_PUBLIC_SOCKET_KEY}")
  configure_file("${root}/cmake/nevr_builtin_defaults.h.in"
                 "${CMAKE_BINARY_DIR}/generated/nevr_builtin_defaults.h" @ONLY
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
endfunction()
