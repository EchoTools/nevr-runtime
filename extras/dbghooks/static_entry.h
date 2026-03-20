#pragma once
#include <windows.h>

namespace dbghooks {
void InitializeStatic(HMODULE hModule);
void ShutdownStatic();
}  // namespace dbghooks
