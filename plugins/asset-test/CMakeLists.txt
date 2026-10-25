# plugins/asset-test/CMakeLists.txt
# Test plugin for plugin-carried assets (#432): carries a clone of a shipped level inside its own image,
# registers it at init and loads it through the engine's resource manager.
#
# The payload is the game's own bytes, so it is generated from a local install and never committed:
#   python3 -I plugins/asset-test/tools/make_payload.py --data <game>/_data/<id>/rad15/win10 \
#       --manifest-id 48037dc70b0ecab2 --source mpl_tutorial_master --name nevr_test_level \
#       --out <dir>/asset_test_payload.h
# then configure with -DNEVR_ASSET_TEST_PAYLOAD_DIR=<dir>. Without it this target is not built.

if(NOT DEFINED NEVR_ASSET_TEST_PAYLOAD_DIR OR NOT EXISTS "${NEVR_ASSET_TEST_PAYLOAD_DIR}/asset_test_payload.h")
    message(STATUS "asset-test plugin: not built (set NEVR_ASSET_TEST_PAYLOAD_DIR to a directory holding asset_test_payload.h)")
    return()
endif()

add_library(nevr_asset_test SHARED
    src/plugin.cpp
)

target_include_directories(nevr_asset_test PRIVATE "${NEVR_ASSET_TEST_PAYLOAD_DIR}")
target_compile_definitions(nevr_asset_test PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(nevr_asset_test PRIVATE nevr_plugin_common)

if(WIN32 OR MINGW)
    target_compile_definitions(nevr_asset_test PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(nevr_asset_test PROPERTIES
    OUTPUT_NAME "nevr_asset_test"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)
