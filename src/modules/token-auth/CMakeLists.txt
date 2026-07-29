# token-auth module — Device code authentication, JWT refresh.

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(token_auth SHARED
    src/token_auth.cpp
)

target_compile_definitions(token_auth PRIVATE NEVR_MODULE_EXPORTS)

target_include_directories(token_auth PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/gamepatches
    ${CMAKE_SOURCE_DIR}/plugins/common/include
)

# Disable PCH — nlohmann_json headers conflict with Windows.h macros
# set_source_files_properties(src/token_auth.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_link_libraries(token_auth PRIVATE nevr_core CURL::libcurl nlohmann_json::nlohmann_json)

set_target_properties(token_auth PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules"
)
