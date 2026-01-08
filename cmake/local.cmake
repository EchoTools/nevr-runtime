include("${CMAKE_SOURCE_DIR}/cmake/copy_artifacts_to_game_directory.cmake")
# Option to enable/disable copying to game directory
option(COPY_TO_GAME_DIR "Copy build artifacts to game directory" ON)
# Default path for EchoVR binaries - can be overridden
if(NOT DEFINED ENV{ECHOVR_BIN_PATH} AND NOT DEFINED ECHOVR_BIN_PATH)
    # Check if ready-at-dawn-echo-arena exists in repo root first
    if(EXISTS "${CMAKE_SOURCE_DIR}/ready-at-dawn-echo-arena/bin/win10")
        set(ECHOVR_BIN_PATH "${CMAKE_SOURCE_DIR}/ready-at-dawn-echo-arena/bin/win10")
        message(STATUS "Using local EchoVR installation in repo root")
    else()
        set(ECHOVR_BIN_PATH
            #"C:/Users/User/source/repos/EchoRelay9/_local/newnakama/echovr-newnakama/bin/win10"
            "/mnt/winos/OculusLibrary/Software/ready-at-dawn-echo-arena/bin/win10"
        )
    endif()
elseif(DEFINED ENV{ECHOVR_BIN_PATH} AND NOT DEFINED ECHOVR_BIN_PATH)
    set(ECHOVR_BIN_PATH "$ENV{ECHOVR_BIN_PATH}")
endif()

# Function to copy build artifacts to the game directory
copy_artifacts_to_game_directory()
