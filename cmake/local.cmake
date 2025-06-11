# Option to enable/disable copying to game directory

# Default path for EchoVR binaries - can be overridden
set(ECHOVR_BIN_PATH
    "C:/Program Files/Oculus/Software/Software/ready-at-dawn-echo-arena/bin/win10"
)
set(COPY_TO_GAME_DIR
    ON
    CACHE BOOL "Copy build artifacts to game directory")
# Include the copy_to_game_directory.cmake script to copy the built files to the
# game directory after building
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_to_game_directory.cmake")
