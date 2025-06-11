REM This script configures the project using CMake in a specified build directory.

cmake --preset=default -S . -B ./build -G "Visual Studio 17 2022"