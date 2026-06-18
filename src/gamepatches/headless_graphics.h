/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

/// Install DXGI/D3D11 headless stubs.
/// Must be called after DllLoadHook::Install() and before the game loads
/// dxgi.dll or d3d11.dll.  In headless mode (g_isHeadless), hooks
/// CreateDXGIFactory1 and D3D11CreateDevice to return stub COM objects
/// that satisfy the game's initialization without allocating a GPU.
void InstallHeadlessGraphicsHooks();
