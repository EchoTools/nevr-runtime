include("${CMAKE_SOURCE_DIR}/cmake/copy_artifacts_to_game_directory.cmake")
# Option to enable/disable copying to game directory
option(COPY_TO_GAME_DIR "Copy build artifacts to game directory" ON)
# Default path for EchoVR binaries - can be overridden
set(ECHOVR_BIN_PATH
    "C:/Program Files/Oculus/Software/Software/ready-at-dawn-echo-arena/bin/win10"
)

# Function to copy build artifacts to the game directory
copy_artifacts_to_game_directory()
