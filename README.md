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

## Environment requirements

Ensure that the cmake operation is running from `x64 Native Tools Command Prompt for VS 2022`