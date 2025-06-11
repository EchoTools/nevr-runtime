# My Project

## Overview

This project consists of two main components: a game patch library and a game server. The game patch library provides functionalities to modify or enhance game behavior, while the game server handles the multiplayer aspects of the game.

## Directory Structure

```sh
native-game-server
├── gamepatch
│   └── CMakeLists.txt      # Build configuration for the game patch library
├── gameserver
│   └── CMakeLists.txt      # Build configuration for the game server
├── CMakeLists.txt          # Top-level CMake configuration
└── README.md               # Project documentation
```

## Building the Project

### Prerequisites

- Cmake 4.0 or higher [Download](https://cmake.org/download/)
- Visual Studio 2022 or higher [Download](https://visualstudio.microsoft.com/vs/)

### VS Code

Open the command pallet (CTRL+P) and CMake build, when asked to select a kit, select `Visual Studio Community 2022 Release - x86_amd64`.

### Command Line

To build the project, follow these steps:

Create a build directory:

```sh
   mkdir build
   cd build
```

Run CMake to configure the project:

   ```sh
   cmake ..
   ```

1. Build the project:

   ```sh
   cmake --build .
   ```

## Usage

After building the project, you can use the game patch library and game server as per your requirements. Refer to the individual `README.md` files in the `gamepatch` and `gameserver` directories for more specific usage instructions.

## Dependencies

This project may have dependencies that need to be installed separately. Please refer to the respective `CMakeLists.txt` files for details on any external libraries used.

## Local CMake Configuration

Add any local customizations to `cmake/local.cmake` file. This file will be included automatically if it exists, allowing you to override or extend the default CMake configuration.

By default it includes commands to copy the `dbgcore.dll` and `pnsradgameserver.dll` to the specified game server directory. You can modify this file to add additional configurations or custom commands.

## User Presets

The `CMakeUserPresets.json` file in the root directory to define custom CMake presets. This allows you to easily switch between different build configurations without modifying the main `CMakeLists.txt`.
Here's an example of a `CMakeUserPresets.json` file:

```json
{
  "version": 10,
  "configurePresets": [
    {
      "name": "local-ninja-msvc-debug",
      "displayName": "Local Ninja - MSVC Debug (User specific paths)",
      "description": "User-specific preset for Ninja with MSVC compiler (Debug), defining explicit tool paths.",
      "environment": {
        "VCPKG_ROOT": "C:/opt/vcpkg",
        "WINDOWS_KITS_10_PATH": "C:/Program Files (x86)/Windows Kits/10",
        "WINDOWS_SDK_VERSION": "10.0.26100.0",
        "VSTUDIO_PATH": "C:/Program Files/Microsoft Visual Studio/2022/Community",
        "MSVC_VERSION": "14.38.33130",
        "PATH": "${env:VSTUDIO_PATH}/VC/Tools/MSVC/${env:MSVC_VERSION}/bin/Hostx64/x64;${env:WINDOWS_KITS_10_PATH}/bin/${env:WINDOWS_SDK_VERSION}/x64;${env:VSTUDIO_PATH}/Common7/IDE;${env:VSTUDIO_PATH}/Common7/Tools;C:/Windows/System32;C:/Windows/System32/WindowsPowerShell/v1.0",
        "INCLUDE": "${env:VSTUDIO_PATH}/VC/Tools/MSVC/${env:MSVC_VERSION}/include;${env:WINDOWS_KITS_10_PATH}/Include/${env:WINDOWS_SDK_VERSION}/ucrt;${env:WINDOWS_KITS_10_PATH}/Include/${env:WINDOWS_SDK_VERSION}/shared;${env:WINDOWS_KITS_10_PATH}/Include/${env:WINDOWS_SDK_VERSION}/um",
        "LIB": "${env:VSTUDIO_PATH}/VC/Tools/MSVC/${env:MSVC_VERSION}/lib/x64;${env:WINDOWS_KITS_10_PATH}/Lib/${env:WINDOWS_SDK_VERSION}/ucrt/x64;${env:WINDOWS_KITS_10_PATH}/Lib/${env:WINDOWS_SDK_VERSION}/um/x64"
      },
      "cacheVariables": {
        "CMAKE_CXX_COMPILER": {
          "type": "FILEPATH",
          "value": "${env:VSTUDIO_PATH}/VC/Tools/MSVC/${env:MSVC_VERSION}/bin/Hostx64/x64/cl.exe"
        },
        "CMAKE_C_COMPILER": {
          "type": "FILEPATH",
          "value": "${env:VSTUDIO_PATH}/VC/Tools/MSVC/${env:MSVC_VERSION}/bin/Hostx64/x64/cl.exe"
        }
      }
    }
  ]
}
```