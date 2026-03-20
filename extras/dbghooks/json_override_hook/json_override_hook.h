#pragma once

#include <cstdint>

// Function pointer for the original CJson_LoadFromPath
using CJson_LoadFromPath_t = int(__fastcall*)(void* this_ptr, const char* path, bool flag);

// Function pointer for the game's internal JSON parser
using CJson_Parse_t = int(__fastcall*)(void* this_ptr, char* json_buffer, uint64_t buffer_size);

// Public API for the hook
void InitializeJsonOverrideHook();
void ShutdownJsonOverrideHook();
