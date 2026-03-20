/*
 * pnsnevr - Game Bridge Implementation
 * 
 * Interfaces with NEVR's internal structures.
 * Based on Ghidra analysis of echovr.exe build 6323983201049540
 */

#include "game_bridge.h"
#include <windows.h>
#include <cstdio>

GameBridge::GameBridge(void* game_state)
    : m_gameState(game_state)
    , m_udpBroadcaster(nullptr)
    , m_tcpBroadcaster(nullptr)
    , m_modeFlags(nullptr)
    , m_registerUdpHandler(nullptr)
    , m_registerTcpHandler(nullptr)
    , m_hashString(nullptr)
    , m_hashCombine(nullptr)
{
}

GameBridge::~GameBridge()
{
    UnregisterHandlers();
}

bool GameBridge::Initialize()
{
    OutputDebugStringA("[pnsnevr] GameBridge::Initialize\n");
    
    if (!m_gameState) {
        OutputDebugStringA("[pnsnevr] ERROR: game_state is null\n");
        return false;
    }
    
    // Cast game_state to int* for offset calculations (matches Ghidra analysis)
    int* gameStateInt = static_cast<int*>(m_gameState);
    
    // Extract broadcaster pointers from game state
    // From Ghidra: uVar11 = *(undefined8 *)(param_1 + 0xa32); // UDP
    //              uVar11 = *(undefined8 *)(param_1 + 0xa30); // TCP
    m_udpBroadcaster = *reinterpret_cast<UdpBroadcaster**>(
        reinterpret_cast<char*>(m_gameState) + GameOffsets::UdpBroadcaster);
    m_tcpBroadcaster = *reinterpret_cast<TcpBroadcaster**>(
        reinterpret_cast<char*>(m_gameState) + GameOffsets::TcpBroadcaster);
    
    // Extract mode flags pointer
    // From Ghidra: bVar16 = (byte)**(ulonglong **)(param_1 + 0xb68);
    m_modeFlags = *reinterpret_cast<uint64_t**>(
        reinterpret_cast<char*>(m_gameState) + GameOffsets::ModeFlags);
    
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] GameBridge: UDP=%p TCP=%p Flags=%p\n",
             m_udpBroadcaster, m_tcpBroadcaster, m_modeFlags);
    OutputDebugStringA(logBuf);
    
    // Resolve function pointers from the game's module
    // These are found via pattern scanning or import table
    HMODULE gameModule = GetModuleHandleA(nullptr);  // Main executable
    
    /* 
     * In a production implementation, we would resolve these functions:
     * 
     * m_registerUdpHandler = (RegisterHandlerFn)((uintptr_t)gameModule + 0xf80ed0);
     * m_registerTcpHandler = (RegisterHandlerFn)((uintptr_t)gameModule + 0xf81100);
     * m_hashString = (HashStringFn)((uintptr_t)gameModule + 0x...);
     * m_hashCombine = (HashCombineFn)((uintptr_t)gameModule + 0x...);
     * 
     * For now, we'll use a mock implementation.
     */
    
    OutputDebugStringA("[pnsnevr] GameBridge initialized (MOCK mode)\n");
    return true;
}

void GameBridge::UnregisterHandlers()
{
    OutputDebugStringA("[pnsnevr] GameBridge::UnregisterHandlers\n");
    
    // TODO: Call game's unregister functions for each registered handler
    // For now, just clear our tracking lists
    m_registeredTcpHandlers.clear();
    m_registeredUdpHandlers.clear();
}

std::string GameBridge::GetDeviceId()
{
    // Get a unique device identifier
    // Options:
    // 1. Machine GUID from registry
    // 2. MAC address hash
    // 3. Oculus device ID if available
    
    // For prototype, use machine GUID
    char guid[256] = {0};
    DWORD guidSize = sizeof(guid);
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, 
                      "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr, 
                        (LPBYTE)guid, &guidSize);
        RegCloseKey(hKey);
    }
    
    if (guid[0] == 0) {
        // Fallback to a generated ID
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        snprintf(guid, sizeof(guid), "fallback-%llx", counter.QuadPart);
    }
    
    return std::string(guid);
}

std::string GameBridge::GetOculusId()
{
    // TODO: Extract Oculus user ID from game state if available
    // This would require more reverse engineering of the game's Oculus integration
    return "";
}

uint16_t GameBridge::RegisterUdpHandler(const std::string& messageName, MessageHandler handler)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] RegisterUdpHandler: %s\n", messageName.c_str());
    OutputDebugStringA(logBuf);
    
    /* 
     * Full implementation would:
     * 1. Compute message hash: hash = ComputeMessageHash(messageName);
     * 2. Create a trampoline function that calls our std::function
     * 3. Call the game's registration function:
     *    uint16_t id = m_registerUdpHandler(m_udpBroadcaster, hash, 1, context, 0);
     * 4. Track the ID for unregistration
     */
    
    // Mock: Return a fake handler ID
    uint16_t mockId = static_cast<uint16_t>(m_registeredUdpHandlers.size() + 1);
    m_registeredUdpHandlers.push_back(mockId);
    return mockId;
}

uint16_t GameBridge::RegisterTcpHandler(uint64_t peerHash, MessageHandler handler)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] RegisterTcpHandler: 0x%llx\n", peerHash);
    OutputDebugStringA(logBuf);
    
    // Similar to UDP, but for TCP peer connections
    uint16_t mockId = static_cast<uint16_t>(m_registeredTcpHandlers.size() + 1);
    m_registeredTcpHandlers.push_back(mockId);
    return mockId;
}

bool GameBridge::SendMessage(const std::string& messageName, const void* data, size_t length)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] SendMessage: %s (%zu bytes)\n", messageName.c_str(), length);
    OutputDebugStringA(logBuf);
    
    /*
     * Full implementation would:
     * 1. Compute message hash
     * 2. Call the game's broadcast function with the message
     * 
     * The exact broadcast function needs to be identified from Ghidra analysis.
     */
    
    return true;  // Mock: Always succeed
}

uint64_t GameBridge::GetModeFlags()
{
    if (m_modeFlags) {
        return *m_modeFlags;
    }
    return 0;
}

bool GameBridge::IsServerMode()
{
    // From Ghidra: bit 2 set indicates server mode
    uint64_t flags = GetModeFlags();
    return (flags >> 2) & 1;
}

bool GameBridge::IsClientMode()
{
    // From Ghidra: bit 2 clear AND (bit 1 clear OR bit 6 clear)
    uint64_t flags = GetModeFlags();
    bool bit1 = (flags >> 1) & 1;
    bool bit2 = (flags >> 2) & 1;
    bool bit6 = (flags >> 6) & 1;
    
    return !bit2 && (!bit1 || !bit6);
}

uint64_t GameBridge::ComputeMessageHash(const std::string& messageName)
{
    /*
     * From Ghidra analysis, the game uses a two-step hash:
     * 1. _Hash_CMatSym_NRadEngine__SA_KPEBD_Z(messageName) -> intermediate hash
     * 2. _HashA_SMatSymData_NRadEngine__SA_K_K0_Z(0x6d451003fb4b172e, intermediate) -> final hash
     * 
     * For prototype, we'll use a simple hash.
     * Production code should call the actual game functions or replicate the algorithm.
     */
    
    // Simple djb2 hash for prototype
    uint64_t hash = 5381;
    for (char c : messageName) {
        hash = ((hash << 5) + hash) + c;
    }
    
    // Combine with the game's seed (0x6d451003fb4b172e)
    hash ^= 0x6d451003fb4b172e;
    
    return hash;
}
