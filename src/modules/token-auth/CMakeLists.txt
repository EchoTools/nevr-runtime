# token_auth — Device code authentication, JWT refresh. Linked statically
# into BugSplat64.dll (2026-08-02: folded from separate DLL per owner
# decision — separate deployable DLLs are a failure mode when they must
# always ship together).

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(token_auth STATIC
    src/token_auth.cpp
    src/token_auth.h
    src/device_poll_response.cpp
    src/device_poll_response.h
)

target_compile_definitions(token_auth PRIVATE NEVR_MODULE_EXPORTS)

target_include_directories(token_auth PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/runtime
    ${CMAKE_SOURCE_DIR}/plugins/common/include
)

target_link_libraries(token_auth PRIVATE nevr_core CURL::libcurl nlohmann_json::nlohmann_json)

# The production target deliberately has no NEVR_TEST_HOOKS.  Build a separate
# copy only for test_token_auth so its in-memory DeviceAuth state can be driven
# without cache, refresh-thread, or HTTP side effects.
if(BUILD_TESTING)
    add_library(token_auth_test_hooks STATIC
        src/token_auth.cpp
        src/device_poll_response.cpp
    )

    target_compile_definitions(token_auth_test_hooks PRIVATE
        NEVR_MODULE_EXPORTS
        NEVR_TEST_HOOKS
    )

    target_include_directories(token_auth_test_hooks PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/runtime
        ${CMAKE_SOURCE_DIR}/plugins/common/include
    )

    target_link_libraries(token_auth_test_hooks PRIVATE
        nevr_core CURL::libcurl nlohmann_json::nlohmann_json
    )
endif()
