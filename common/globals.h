#pragma once

#ifndef ECHOVR_GLOBALS_H
#define ECHOVR_GLOBALS_H

#include "echovrInternal.h"

#ifndef PROJECT_VERSION          // Set by CMake during the build process
#define PROJECT_VERSION "1.0.0"  // Fallback default version
#endif

#ifndef PROJECT_VERSION_MAJOR    // Set by CMake during the build process
#define PROJECT_VERSION_MAJOR 1  // Fallback default major version
#endif
#ifndef PROJECT_VERSION_MINOR    // Set by CMake during the build process
#define PROJECT_VERSION_MINOR 0  // Fallback default minor version
#endif
#ifndef PROJECT_VERSION_PATCH    // Set by CMake during the build process
#define PROJECT_VERSION_PATCH 0  // Fallback default patch version
#endif

#ifndef GIT_COMMIT_HASH            // Set by CMake during the build process
#define GIT_COMMIT_HASH "unknown"  // Fallback default commit hash
#endif

extern BOOL noConsole;
extern BOOL isHeadless;
extern UINT32 headlessTimeStep;

#endif  // ECHOVR_GLOBALS_H