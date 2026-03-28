add_library(audio_intercom SHARED
    src/audio_intercom.cpp
    src/plugin_main.cpp
)

target_include_directories(audio_intercom PRIVATE src)
target_compile_definitions(audio_intercom PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(audio_intercom PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(audio_intercom PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_link_libraries(audio_intercom PRIVATE ws2_32)
    target_compile_definitions(audio_intercom PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(audio_intercom PROPERTIES
    OUTPUT_NAME "audio_intercom"
    PREFIX ""
)

if(BUILD_TESTING AND GTest_FOUND)
    add_executable(test_audio_intercom
        src/audio_intercom.cpp
        tests/test_voip_format.cpp
    )
    target_include_directories(test_audio_intercom PRIVATE src)
    target_link_libraries(test_audio_intercom PRIVATE nevr_plugin_common GTest::gtest GTest::gtest_main)
    if(WIN32 OR MINGW)
        target_link_libraries(test_audio_intercom PRIVATE ws2_32)
        target_compile_definitions(test_audio_intercom PRIVATE
            _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
        )
    endif()
    gtest_discover_tests(test_audio_intercom)
endif()
