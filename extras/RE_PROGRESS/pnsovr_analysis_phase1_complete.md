# PNSOverr.dll Binary Analysis - Phase 1 Complete

## Overview
A comprehensive reverse engineering of `pnsovr.dll` from Echo VR v34.4, a platform native services plugin for the Oculus Platform SDK.

**File**: pnsovr.dll (v34.4.631547.1)
**Size**: ~3.3 MB
**Architecture**: x64 Windows DLL

---

## Discovered Subsystems

### 1. **Voice and Audio (Voip Subsystem)**
The core feature of this plugin.

**Functions**:
- `VoipCreateEncoder()` / `VoipDestroyEncoder()` - Opus codec encoder
- `VoipCreateDecoder()` / `VoipDestroyDecoder()` - Opus codec decoder
- `VoipEncode()` / `VoipDecode()` - Frame processing
- `VoipAnswer()` / `VoipCall()` - Call lifecycle
- `VoipMute()` / `VoipUnmute()` - Audio control
- `VoipSetBitRate()` - Quality tuning
- `VoipAvailable()` - Capability detection

**Configuration**:
- `VoipBufferSize`, `VoipPacketSize`, `VoipSampleRate` - audio parameters
- `VoipPushToTalkKey` - PTT keybinding

---

### 2. **Microphone Subsystem**
Directly integrates with Windows audio hardware.

**Functions**:
- `MicCreate()` / `MicDestroy()` - lifecycle
- `MicStart()` / `MicStop()` - capture control
- `MicRead()` - PCM buffer retrieval
- `MicDetected()` - device detection
- `MicAvailable()` - capability query

**Configuration**:
- `MicBufferSize`, `MicCaptureSize`, `MicSampleRate`

---

### 3. **User Management & Social**
Manages Oculus user accounts and social features.

**Core Functions**:
- `ovr_User_GetID()` / `ovr_User_GetOculusID()`
- `ovr_User_GetPresence()` - Rich presence information
- `ovr_User_GetInviteToken()` - Cross-game invites
- `ovr_UserArray_*` - Batch operations

**Implementation Details**:
- Uses Oculus Platform SDK extensively
- Supports user discovery and profile access

---

### 4. **Rooms & Multiplayer**
Implements virtual room-based networking.

**Key Functions**:
- `ovr_Room_CreateAndJoinPrivate2()` - Room creation
- `ovr_Room_Get()` / `ovr_Room_GetID()`
- `ovr_Room_GetUsers()` - Participant listing
- `ovr_Room_GetDataStore()` - Shared game state
- `ovr_Room_GetJoinPolicy()` - Access controls
- `ovr_RoomOptions_*` - Configuration

**Data Structures**:
- User arrays with pagination (`HasNextPage`)
- Configurable join policies
- Per-room data stores for game state

---

### 5. **Rich Presence**
Communicates player activity to Oculus system.

**Functions**:
- `ovr_RichPresence_Set()` / `ovr_RichPresence_Clear()`
- `ovr_RichPresenceOptions_*` - Builder pattern configuration

**Configured Fields**:
- `ApiName` - API identifier
- `CurrentCapacity` / `MaxCapacity` - Slot tracking
- `StartTime` / `EndTime` - Session timing
- `InstanceId` - Game instance identifier
- `IsJoinable` - Join state
- `DeeplinkMessageOverride` - Custom invite text
- `ExtraContext` - Additional game data

---

### 6. **In-App Purchases (IAP)**
Monetization system integration.

**Functions**:
- `ovr_IAP_GetProductsBySKU()` - Product catalog
- `ovr_IAP_LaunchCheckoutFlow()` - Payment UI
- `ovr_IAP_GetViewerPurchases()` - Transaction history
- `ovr_IAP_GetViewerPurchasesDurableCache()` - Offline access
- `ovr_IAP_GetNextProductArrayPage()` - Pagination

**Data Access**:
- Product properties (description, price, SKU)
- Purchase verification
- Array paging support

---

### 7. **Destinations & Deep Linking**
Controls launch points and social invites.

**Functions**:
- `ovr_Destination_GetApiName()` / `ovr_Destination_GetDisplayName()`
- `ovr_DestinationArray_*` - Destination management

**Message Types**:
- Launch details extraction
- Deeplink message customization

---

### 8. **Notifications & Invitations**
Social notification handling.

**Functions**:
- `ovr_Notification_GetRoomInvites()` - Invite retrieval
- `ovr_Notification_MarkAsRead()` - Read state tracking
- `ovr_RoomInviteNotification_*` - Invite metadata

---

### 9. **Entitlements & Verification**
License and permission checking.

**Functions**:
- `CheckEntitlement()` - Plugin internal
- `ovr_Entitlement_GetIsViewerEntitled()` - Oculus SDK

---

### 10. **Data Storage & Caching**
Persistent and transient data management.

**Functions**:
- `ovr_DataStore_GetValue()` - Key-value access
- `ovr_RoomOptions_SetOrdering()` - Data ordering

---

### 11. **Error Handling & Messaging**
Comprehensive error reporting infrastructure.

**Functions**:
- `ovr_Error_GetMessage()` - Human-readable errors
- `ovr_Error_GetCode()` - Error codes
- `ovr_Error_GetHttpCode()` - HTTP status codes
- `ovrLaunchType_ToString()` - Enum string conversion
- `ovrPlatformInitializeResult_ToString()` - Init status strings

**Error Categories** (found in binary):
- Generic errors (`Error_(generic)`)
- Frame descriptor errors
- Version/compatibility errors
- Memory allocation failures
- Async operation errors
- Compression errors (Zstandard, gzip)
- Network errors (socket, DNS)
- JSON parsing errors
- Cryptography errors (OpenSSL)

---

### 12. **Crypto & Security**
Uses OpenSSL for TLS and certificate handling.

**Detected Components**:
- TLS/SSL networking
- Certificate validation (X.509)
- CMS (Cryptographic Message Syntax)
- PKCS#7 signed data
- Key derivation (PBKDF2, HKDF, Scrypt)
- Hash functions (MD5, SHA1, SHA256)
- AES encryption

**Configuration**: 
- Windows Crypto API (`CryptAcquireContextW`, `CryptGenRandom`)
- Certificate store access

---

### 13. **Network & Event Loop**
Libevent-based asynchronous networking.

**Detected Libraries**:
- Libevent 2.x (event-driven I/O)
- libuv features (threadpool, async operations)
- Socket API (WSARecv, WSASend)
- IPv4/IPv6 support (getaddrinfo)

**Features**:
- Buffered I/O (bufferevent)
- Connection pooling
- Signal handling
- I/O completion ports (Windows IOCP)

---

### 14. **JSON Processing**
Data interchange format support.

**Features**:
- JSON parsing and generation
- CJSON integration
- Error recovery
- Unicode escape handling

---

### 15. **Compression**
Data compression for bandwidth optimization.

**Algorithms Detected**:
- **Zstandard (Zstd)**: Modern high-ratio compression
- **gzip**: Standard HTTP compression
- **LZ4**: Fast compression variant

---

### 16. **Threading & Synchronization**
Multi-threaded architecture with proper synchronization.

**Primitives**:
- Critical sections
- Reader-writer locks (SRWLock)
- Condition variables
- Thread-local storage (TLS)
- Semaphores
- Events

**Threading Models**:
- Libevent worker threads
- Threadpool (via libuv)
- Fiber support (ConvertThreadToFiber)

---

### 17. **System Integration**
Deep Windows OS integration.

**Capabilities**:
- Console I/O and debugging
- Event logging
- Registry access
- File I/O (full path handling)
- Memory management (VirtualAlloc)
- Process introspection

---

## Data Flow Architecture

### Incoming Data Path
1. **Network Reception** (Libevent)
   - WSARecv → event loop
   
2. **TLS Decryption** (OpenSSL)
   - Certificate validation
   - Stream cipher processing
   
3. **Message Parsing** (JSON/CJSON)
   - Deserialize Oculus Platform SDK messages
   - Extract audio/metadata
   
4. **Processing**
   - Voice: Opus decoder → speaker output
   - Social: User/room updates → UI callbacks
   - Purchases: Product catalog updates

### Outgoing Data Path
1. **Data Preparation**
   - Voip: Opus encoder
   - Social: Rich presence state
   
2. **Serialization** (JSON)
   - Pack Oculus Platform SDK structures
   
3. **Compression** (Zstd/gzip)
   - Bandwidth optimization
   
4. **Encryption** (TLS)
   - Certificate-based security
   
5. **Transmission** (WSASend)
   - Windows socket API

---

## Critical Exports (Rad Tools Plugin Interface)

### Initialization
- `RadPluginInit()` - Main entry point
- `RadPluginInitMemoryStatics()` - Memory setup
- `RadPluginInitNonMemoryStatics()` - Resource setup
- `RadPluginSetAllocator()` - Memory hooks
- `RadPluginSetEnvironment()` - Configuration

### Runtime
- `RadPluginMain()` - Event loop handler
- `RadPluginShutdown()` - Cleanup

### Configuration
- `RadPluginSetEnvironmentMethods()`
- `RadPluginSetSystemInfo()`
- `RadPluginSetPresenceFactory()`
- `RadPluginSetFileTypes()`

---

## External Dependencies
- **Oculus Platform SDK** - Primary dependency
- **OpenSSL 1.1.x** - TLS/crypto
- **Libevent 2.x** - Event loop
- **CJSON** - JSON processing
- **Zstandard** - Compression
- **Windows APIs** - System integration

---

## Security Observations

### Strengths
- Uses OpenSSL for TLS encryption
- Certificate validation for authentication
- PKCS#7 signed data support
- Key derivation functions (HKDF, PBKDF2)

### Potential Weaknesses
- Direct Windows Crypto API usage (may limit algorithm options)
- File paths hardcoded (registry keys, event logs)
- Exception handling in C++ (potential DoS vectors)

---

## Performance Characteristics
- Asynchronous I/O throughout
- Connection pooling for network efficiency
- Compression enabled for bandwidth
- Multi-threaded processing
- Fiber support for context switching

---

## Estimated Function Count
- **Total named functions**: 40+
- **Oculus SDK wrappers**: 150+
- **Internal functions**: 100+
- **Total estimated**: 300+ functions

---

## Next Steps (Phase 2)
1. Create feature specifications
2. Design test cases
3. Implement in nevr-server
4. Add comprehensive documentation
