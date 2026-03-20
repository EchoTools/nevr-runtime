# Draft: PNSOvr Proxy DLL - Full Validation Plan

## User Requirements (Updated)

**Approach**: Two-phase with Ghidra validation
1. **Phase 1**: Test with hardcoded IDs (4 friends) - verify baseline works
2. **Phase 2**: Ghidra validation of complete implementation against binary
3. **Phase 3**: Quick Enhancement (Option A) - Debug logging + Nakama integration

## Reference Materials

**Source of Truth**: `~/src/evr-reconstruction/docs/kb/pnsovr/`
- `pnsovr_complete_analysis.md` - Complete architectural analysis (5,852 functions, 7,109 strings)
- `pnsovr_quick_reference.md` - Hook candidates and function addresses
- `pnsovr_re_summary.md` - Reverse engineering summary
- `pnsovr_hook_candidates.md` - Critical functions for hooking

**Binary Analysis Details**:
- **Base Address**: 0x180000000
- **Size**: 3.7MB PE32+ x86-64
- **Ghidra Project**: EchoVR_6323983201049540 (Port 8193)
- **Analysis Status**: 100% enumeration, 15% decompilation

## Phase 1: Setup & Baseline Test

### Objectives
- Set up Ghidra MCP server (ghydra) in this project
- Build pnsovr.dll from enhance/pnsnevr branch
- Test with hardcoded 4 friend IDs
- Verify DLL loads and game functions normally

### Tasks

1. **Copy Ghidra MCP Configuration**
   - Source: `~/src/evr-reconstruction/.opencode/opencode.json`
   - Copy ghydra MCP server config to `./.opencode/config.json`
   - Verify GhydraMCP bridge script path: `/home/andrew/src/GhydraMCP/bridge_mcp_hydra.py`

2. **Checkout enhance/pnsnevr Branch**
   - `git checkout enhance/pnsnevr`
   - OR: Merge/cherry-pick pnsovr-dll to current branch

3. **Build pnsovr.dll**
   - Uncomment lines 163,167 in CMakeLists.txt (pnsovr-dll, socialplugin)
   - Build with MinGW: `make configure && make build`
   - Verify output: `build/mingw-release/bin/pnsovr.dll` (expected ~166KB)

4. **Deploy to Game Directory**
   - Copy to: `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/`
   - Rename original pnsovr.dll to pnsovr.dll.backup
   - Deploy new proxy DLL as pnsovr.dll

5. **Test Baseline**
   - Launch game with loader.exe (DLL injection)
   - Verify game starts without crashes
   - Check if friend requests occur (no logging yet - just stability test)
   - Monitor for any error messages

### Success Criteria
- ✅ DLL builds successfully (~166KB)
- ✅ Game launches without crashes
- ✅ Game reaches main menu
- ✅ No obvious errors in console/logs
- ✅ Game functions normally (can navigate menus)

## Phase 2: Ghidra Validation

### Objectives
- Verify every exported function in pnsovr.dll matches the original binary
- Validate friend list function implementation against Ghidra decompilation
- Check VoIP/audio subsystem implementation
- Verify subsystem architecture matches analysis

### Setup

1. **Configure Ghidra MCP in .opencode/config.json**
   ```json
   {
     "mcp": {
       "ghydra": {
         "type": "local",
         "command": [
           "python3",
           "/home/andrew/src/GhydraMCP/bridge_mcp_hydra.py"
         ],
         "enabled": true,
         "environment": {
           "GHIDRA_HYDRA_HOST": "localhost"
         }
       }
     }
   }
   ```

2. **Verify Ghidra Project Access**
   - Ghidra instance on port 8193
   - Project: EchoVR_6323983201049540
   - Binary: pnsovr.dll (original from game)
   - Confirm MCP can connect

### Validation Tasks

#### 1. Export Table Validation
**Reference**: `pnsovr_complete_analysis.md` - Part VIII
- Verify 3 main exports:
  - `RadPluginInit` @ 0x1800974a0
  - `RadPluginMain` @ 0x1800974b0
  - `RadPluginShutdown` @ 0x180097a20
- Verify 69 subsystem function exports
- Check function signatures match

#### 2. Friend List Function Validation
**Reference**: `pnsovr_complete_analysis.md` - Part II, Section 3

**Key Functions to Verify**:
```
ovr_User_GetLoggedInUserFriends() - Get friend list
ovr_User_Get()                    - Fetch user profile
ovr_User_GetLoggedInUser()        - Get current user
```

**Validation Steps**:
1. Use Ghidra MCP to decompile `ovr_User_GetLoggedInUserFriends`
2. Trace call chain to understand data structures
3. Verify CustomSocialManager::GetFriends() implements same logic
4. Check if 4 hardcoded IDs are valid format (64-bit Oculus IDs)

#### 3. VoIP/Audio Subsystem Validation
**Reference**: `pnsovr_quick_reference.md` - Critical Audio Path

**Critical Functions** (Priority order):
| Priority | Address | Function | Validation Check |
|----------|---------|----------|------------------|
| ⭐⭐⭐⭐⭐ | 0x984e0 | VoipEncode | Verify hook signature, codec integration |
| ⭐⭐⭐⭐⭐ | 0x98370 | VoipDecode | Check decoder implementation |
| ⭐⭐⭐⭐ | 0x97450 | MicRead | Verify PCM buffer reading |
| ⭐⭐⭐⭐ | 0x98300 | VoipCreateEncoder | Opus encoder init |
| ⭐⭐⭐ | 0x98100 | VoipCreateDecoder | Decoder pool management |

**Global Variables to Verify**:
```
DAT_180346840 = Microphone handle
DAT_180346848 = Encoder input buffer ptr
DAT_180346850 = Encoder handle
DAT_180346858 = Decoder pool base array
```

**Validation Process**:
1. For each critical function, use Ghidra MCP to:
   - Get function decompilation
   - Extract parameter types and calling convention
   - Identify referenced global variables
   - Map call chains (what it calls, what calls it)
2. Compare with pnsovr-dll implementation:
   - Check function signatures match
   - Verify global variable usage
   - Validate codec integration (libopus)
   - Ensure stub implementations are correct

#### 4. Room/Party Management Validation
**Reference**: `pnsovr_complete_analysis.md` - Part II, Section 3

**Functions to Verify**:
```
ovr_Room_CreateAndJoinPrivate2()
ovr_Room_Join2()
ovr_Room_Get()
ovr_Room_Leave()
ovr_Room_InviteUser()
ovr_Room_KickUser()
```

**Check**:
- Correct OVR SDK function calls
- Proper room ID handling
- User list management

#### 5. Entitlement System Validation
**Reference**: `pnsovr_complete_analysis.md` - Part II, Section 4

**Function**: `CheckEntitlement()` @ 0x180096f70

**Validation**:
1. Decompile CheckEntitlement in Ghidra
2. Understand license verification flow
3. Verify pnsovr-dll handles entitlement checks
4. Decide: Skip check? Bypass? Mock success?

#### 6. Subsystem Architecture Verification

**Reference**: All 6 subsystems from `pnsovr_complete_analysis.md` - Part II

**Verify Each Subsystem**:
1. **VoipSubsystem** (17 functions) - Audio encoding/decoding with Opus
2. **UserSubsystem** (10 functions) - User registration, auth, presence
3. **RoomSubsystem** (11 functions) - Room creation, user management
4. **PresenceSubsystem** (6 functions) - Activity broadcasting
5. **IAPSubsystem** (9 functions) - In-app purchases
6. **NotificationSubsystem** (8 functions) - Social notifications

**For Each**:
- Count exported functions (should match)
- Verify key functions exist and have correct signatures
- Check integration with OVR SDK (ovr_* calls)
- Validate data structures

### Validation Workflow

**Use Ghidra MCP Tools**:
```
ghydra_analyze_function(address) - Get decompilation
ghydra_get_function_signature(address) - Verify signature
ghydra_get_xrefs(address) - Find callers/callees
ghydra_get_strings_at(address) - Check error messages
ghydra_get_global_var(address) - Verify global usage
```

**Document Findings**:
- Create: `.sisyphus/validation/pnsovr-ghidra-validation.md`
- For each discrepancy:
  - Note address, function name
  - Describe mismatch
  - Propose fix
  - Classify severity: CRITICAL / HIGH / MEDIUM / LOW

### Success Criteria
- ✅ All 3 main exports verified (RadPluginInit/Main/Shutdown)
- ✅ All 69 subsystem functions accounted for
- ✅ Friend list function logic matches Ghidra decompilation
- ✅ VoIP critical path (5 functions) verified
- ✅ Global variable usage confirmed
- ✅ No CRITICAL discrepancies found
- ✅ HIGH/MEDIUM issues documented with fixes

## Phase 3: Quick Enhancement (Option A)

### Objectives
- Add debug logging to friend list requests
- Integrate Nakama for pulling friends from backend
- Maintain fallback to hardcoded IDs

### Tasks

#### 1. Add Debug Logging

**File**: `pnsovr-dll/src/CustomSocialManager.cpp`

**Modifications**:
```cpp
// Add logging to GetFriends()
std::vector<ovrID> CustomSocialManager::GetFriends(ovrID user_id) {
    // NEW: Log entry
    LogDebug("[PNSOVR] GetFriends called for user_id: 0x%llx", user_id);
    
    // Check cache first
    auto it = users_.find(user_id);
    if (it != users_.end()) {
        LogDebug("[PNSOVR] Using cached friend list (%zu friends)", 
                 it->second.friends.size());
        
        // Log each friend
        for (auto friend_id : it->second.friends) {
            LogDebug("[PNSOVR]   Friend: 0x%llx", friend_id);
        }
        
        return it->second.friends;
    }
    
    // NEW: Log cache miss
    LogDebug("[PNSOVR] Cache miss - using placeholder data");
    
    auto placeholder = GetPlaceholderFriends(user_id);
    
    // NEW: Log placeholder friends
    LogDebug("[PNSOVR] Returning %zu placeholder friends", placeholder.size());
    for (auto friend_id : placeholder) {
        LogDebug("[PNSOVR]   Placeholder Friend: 0x%llx", friend_id);
    }
    
    return placeholder;
}
```

**Add Logging Infrastructure**:
- Create: `pnsovr-dll/src/logging.h` and `pnsovr-dll/src/logging.cpp`
- Log to: Console (stdout) + File (`pnsovr_debug.log`)
- Thread-safe logging with mutex
- Log levels: DEBUG, INFO, WARN, ERROR

#### 2. Nakama Integration

**New Files**:
- `pnsovr-dll/src/NakamaClient.h` - Nakama HTTP/WebSocket client
- `pnsovr-dll/src/NakamaClient.cpp` - Implementation

**Architecture**:
```cpp
class NakamaClient {
public:
    bool Initialize(const std::string& host, int port, const std::string& api_key);
    bool IsConnected() const;
    
    // Friend list RPC
    std::vector<ovrID> GetFriends(ovrID user_id);
    
    // Authentication
    bool Authenticate(const std::string& device_id);
    
private:
    std::string host_;
    int port_;
    std::string session_token_;
    bool connected_;
};
```

**Modify CustomSocialManager**:
```cpp
class CustomSocialManager {
    // ...existing code...
    
    // NEW: Nakama client
    std::unique_ptr<NakamaClient> nakama_client_;
    
    // NEW: Cache management
    struct FriendsCacheEntry {
        std::vector<ovrID> friends;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::unordered_map<ovrID, FriendsCacheEntry> friends_cache_;
    std::chrono::seconds cache_ttl_{300}; // 5 minutes
    
    // NEW: Fetch from Nakama
    std::vector<ovrID> FetchFriendsFromNakama(ovrID user_id);
};

// Implementation
std::vector<ovrID> CustomSocialManager::FetchFriendsFromNakama(ovrID user_id) {
    if (!nakama_client_ || !nakama_client_->IsConnected()) {
        LogWarn("[PNSOVR] Nakama not available - using placeholder");
        return GetPlaceholderFriends(user_id);
    }
    
    try {
        auto friends = nakama_client_->GetFriends(user_id);
        LogInfo("[PNSOVR] Fetched %zu friends from Nakama", friends.size());
        
        // Update cache
        FriendsCacheEntry entry;
        entry.friends = friends;
        entry.timestamp = std::chrono::steady_clock::now();
        friends_cache_[user_id] = entry;
        
        return friends;
    } catch (const std::exception& e) {
        LogError("[PNSOVR] Nakama error: %s", e.what());
        return GetPlaceholderFriends(user_id);
    }
}

std::vector<ovrID> CustomSocialManager::GetFriends(ovrID user_id) {
    LogDebug("[PNSOVR] GetFriends called for user_id: 0x%llx", user_id);
    
    // Check cache first
    auto cache_it = friends_cache_.find(user_id);
    if (cache_it != friends_cache_.end()) {
        auto age = std::chrono::steady_clock::now() - cache_it->second.timestamp;
        if (age < cache_ttl_) {
            LogDebug("[PNSOVR] Using cached data (age: %llds)", 
                     std::chrono::duration_cast<std::chrono::seconds>(age).count());
            return cache_it->second.friends;
        } else {
            LogDebug("[PNSOVR] Cache expired - fetching fresh data");
        }
    }
    
    // Fetch from Nakama (or fall back to placeholder)
    return FetchFriendsFromNakama(user_id);
}
```

#### 3. Configuration

**File**: `pnsovr-dll/pnsovr_config.ini`

```ini
[Nakama]
Enabled=true
Host=127.0.0.1
Port=7350
ServerKey=defaultkey
Timeout=5000

[Cache]
FriendListTTL=300

[Logging]
Level=DEBUG
ConsoleOutput=true
FileOutput=true
FilePath=pnsovr_debug.log
```

**Load in RadPluginInit**:
```cpp
// In pnsovr_plugin.cpp RadPluginInit()
LoadConfiguration("pnsovr_config.ini");
InitializeLogging();
InitializeNakama();
```

#### 4. CMakeLists.txt Updates

```cmake
# Add Nakama SDK (optional dependency)
find_package(Nakama QUIET)
if(Nakama_FOUND)
    target_link_libraries(PNSOvrDLL PRIVATE Nakama::NakamaClient)
    target_compile_definitions(PNSOvrDLL PRIVATE PNSOVR_WITH_NAKAMA=1)
else()
    # Implement HTTP client manually (libcurl or WinHTTP)
    target_link_libraries(PNSOvrDLL PRIVATE winhttp)
endif()

# Add new source files
set(PNSOVR_SOURCES
    # ...existing sources...
    "src/logging.cpp"
    "src/NakamaClient.cpp"
)
```

### Testing Strategy

1. **Logging Test**:
   - Build with debug logging enabled
   - Launch game
   - Check console output: `[PNSOVR] GetFriends called...`
   - Check `pnsovr_debug.log` exists and contains logs
   - Verify 4 placeholder friends logged

2. **Nakama Offline Test**:
   - Disable Nakama in config (Enabled=false)
   - Launch game
   - Verify fallback to placeholder friends
   - Check logs: "Nakama not available - using placeholder"

3. **Nakama Online Test**:
   - Setup Nakama server (or mock endpoint)
   - Enable Nakama in config
   - Launch game
   - Verify logs: "Fetched X friends from Nakama"
   - Verify real friend IDs returned

4. **Cache Test**:
   - First GetFriends() call → Nakama fetch
   - Second GetFriends() call (within 5 min) → Cache hit
   - Check logs: "Using cached data (age: Xs)"

5. **Error Handling Test**:
   - Break Nakama connection mid-game
   - Trigger GetFriends()
   - Verify logs: "Nakama error: ..."
   - Verify graceful fallback to placeholder

### Success Criteria
- ✅ Debug logs appear in console + file
- ✅ Placeholder friends logged correctly (4 IDs)
- ✅ Nakama client can connect and authenticate
- ✅ Friends fetched from Nakama successfully
- ✅ Cache reduces redundant Nakama calls
- ✅ Graceful fallback when Nakama unavailable
- ✅ No crashes or memory leaks
- ✅ Game functions normally with enhanced DLL

## Open Questions (To Clarify)

### Nakama Server
1. **Endpoint**: What's the Nakama server address? (e.g., `http://127.0.0.1:7350`)
2. **Setup**: Do you have Nakama running, or should I include setup instructions?
3. **RPC Function**: What's the friend list RPC function name? (default: `get_friends`)

### Authentication
4. **Auth Method**: How should we authenticate?
   - Device authentication (default)
   - Session token (from game)
   - API key (hardcoded)
5. **User ID Mapping**: How does Oculus ID map to Nakama user ID?

### Friend Data Format
6. **Response Format**: What does Nakama return?
   - JSON array of friend IDs?
   - JSON array of objects (with profile data)?
   - Custom protobuf?
7. **Example**: Can you provide sample Nakama response?

### Scope
8. **Just Friends**: Or also presence, rooms, etc.?
9. **Read-Only**: Or also add/remove friends?

## Deliverables

### Phase 1
- [ ] Ghidra MCP configured in `.opencode/config.json`
- [ ] pnsovr.dll built from enhance/pnsnevr branch
- [ ] DLL deployed to game directory
- [ ] Baseline test report: Game launches successfully with hardcoded friends

### Phase 2
- [ ] Validation document: `.sisyphus/validation/pnsovr-ghidra-validation.md`
- [ ] Export table verification (3 main + 69 subsystem functions)
- [ ] Friend list function validation (Ghidra decompilation match)
- [ ] VoIP critical path validation (5 functions)
- [ ] Issue tracker: All discrepancies documented with severity
- [ ] Fix recommendations for any CRITICAL/HIGH issues

### Phase 3
- [ ] Logging infrastructure (`logging.h`, `logging.cpp`)
- [ ] Debug logs for friend requests (console + file)
- [ ] Nakama client implementation (`NakamaClient.h`, `NakamaClient.cpp`)
- [ ] Cache management in CustomSocialManager
- [ ] Configuration file (`pnsovr_config.ini`)
- [ ] Updated CMakeLists.txt
- [ ] Test report: All 5 tests passed
- [ ] Final DLL with logging + Nakama ready for deployment

## Timeline Estimate

- **Phase 1 (Setup & Baseline)**: 2-3 hours
- **Phase 2 (Ghidra Validation)**: 4-6 hours (comprehensive)
- **Phase 3 (Quick Enhancement)**: 3-4 hours

**Total**: ~10-13 hours of work

---

**Next Step**: Confirm approach and answer open questions, then I'll generate the work plan!
