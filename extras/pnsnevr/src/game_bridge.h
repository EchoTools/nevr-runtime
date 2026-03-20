/*
 * pnsnevr - Game Bridge
 *
 * Interfaces with NEVR's internal structures to register message handlers
 * and communicate with the game's networking system.
 *
 * Based on Ghidra analysis of echovr.exe build 6323983201049540
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declarations for game structures (from Ghidra analysis)
// These are opaque pointers - we only need to pass them around
struct CR15NetGame;
struct UdpBroadcaster;
struct TcpBroadcaster;

// Message handler callback type
// Based on FUN_14013c8e0, FUN_14013c9a0, etc. patterns
using MessageHandler = std::function<void(void* context, const void* data, size_t length)>;

// Message IDs (hashed symbol names)
// Format: _HashA_SMatSymData_NRadEngine(0x6d451003fb4b172e, hash_of_string)
struct MessageIds {
  static const uint64_t NSLobbySettingsResponse = 0;  // Computed at runtime
  static const uint64_t R15NetSaveLoadoutRequest = 0;
  static const uint64_t R15NetCurrentLoadoutRequest = 0;
  // ... etc
};

// Game offsets (from Ghidra analysis of PlatformModuleDecisionAndInitialize)
struct GameOffsets {
  // Broadcaster pointers
  static const size_t TcpBroadcaster = 0xa30 * sizeof(int);  // param_1[0xa30]
  static const size_t UdpBroadcaster = 0xa32 * sizeof(int);  // param_1[0xa32]

  // Mode flags
  static const size_t ModeFlags = 0xb68 * sizeof(int);  // param_1[0xb68]

  // Message handler tracking
  static const size_t TcpHandlerCount = 0xb65 * sizeof(int);  // param_1[0xb65]
  static const size_t UdpHandlerCount = 0xb54 * sizeof(int);  // param_1[0xb54]

  // Config pointer
  static const size_t ConfigPtr = 0x18c90 * sizeof(int);  // param_1[0x18c90]
};

class GameBridge {
 public:
  explicit GameBridge(void* game_state);
  ~GameBridge();

  // Initialize access to game structures
  bool Initialize();

  // Cleanup registered handlers
  void UnregisterHandlers();

  // Get device ID for authentication
  std::string GetDeviceId();

  // Get user's Oculus ID (if available)
  std::string GetOculusId();

  // Register a message handler with the game's UDP broadcaster
  // messageName: String name like "NakamaFriendsUpdate"
  // handler: Callback function
  // Returns: Handler ID for unregistration
  uint16_t RegisterUdpHandler(const std::string& messageName, MessageHandler handler);

  // Register a TCP peer connection handler
  uint16_t RegisterTcpHandler(uint64_t peerHash, MessageHandler handler);

  // Send a message through the game's broadcaster
  bool SendMessage(const std::string& messageName, const void* data, size_t length);

  // Get current mode flags
  uint64_t GetModeFlags();

  // Check if we're in server mode
  bool IsServerMode();

  // Check if we're in client mode
  bool IsClientMode();

  // Access to raw game state for advanced operations
  void* GetGameState() { return m_gameState; }
  UdpBroadcaster* GetUdpBroadcaster() { return m_udpBroadcaster; }
  TcpBroadcaster* GetTcpBroadcaster() { return m_tcpBroadcaster; }

 private:
  void* m_gameState;  // CR15NetGame instance
  UdpBroadcaster* m_udpBroadcaster;
  TcpBroadcaster* m_tcpBroadcaster;
  uint64_t* m_modeFlags;

  // Track registered handlers for cleanup
  std::vector<uint16_t> m_registeredTcpHandlers;
  std::vector<uint16_t> m_registeredUdpHandlers;

  // Function pointers for game functions (resolved at runtime)
  using RegisterHandlerFn = uint16_t (*)(void* broadcaster, uint64_t messageHash, int param3, void* context,
                                         uint64_t param5);
  using HashStringFn = uint64_t (*)(const char* str);
  using HashCombineFn = uint64_t (*)(uint64_t seed, uint64_t hash);

  RegisterHandlerFn m_registerUdpHandler;  // FUN_140f80ed0
  RegisterHandlerFn m_registerTcpHandler;  // FUN_140f81100
  HashStringFn m_hashString;               // _Hash_CMatSym_NRadEngine__SA_KPEBD_Z
  HashCombineFn m_hashCombine;             // _HashA_SMatSymData_NRadEngine__SA_K_K0_Z

  // Compute message hash from string name
  uint64_t ComputeMessageHash(const std::string& messageName);
};
