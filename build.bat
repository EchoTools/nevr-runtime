REM This script builds the project using CMake in a specified build directory.


cmake --build ./build --config Debug

REM If you want to build in Release mode, you can uncomment the next line
cmake --build ./build --config Release