#pragma once

#include "common/base64.h"
#include "common/pch.h"

/// <summary>
/// Custom config.json path provided via command-line argument. If empty, the default path is used.
/// </summary>
extern CHAR customConfigJsonPath[MAX_PATH];

VOID Initialize();
