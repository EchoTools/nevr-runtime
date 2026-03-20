# Phase 2: Feature Specifications (nevr-server implementation)

Based on pnsovr.dll reverse engineering, specifications for Platform Native Services reimplementation.

---

## Feature Specification Format

Each feature includes:
- **Description**: What the feature does
- **API Surface**: Public functions/interfaces
- **Data Structures**: Required types
- **Dependencies**: Other features needed
- **Security**: Auth/validation requirements
- **Testing Strategy**: How to verify

---

## 1. Voice Communication (Priority: CRITICAL)

### 1.1 Voip Stream Management

**Feature**: Manage bidirectional voice channels between users.

**API**:
```
VoipCreateEncoder() → EncoderHandle
VoipDestroyEncoder(handle)
VoipEncode(pcmData, len) → CompressedFrame
VoipCreateDecoder() → DecoderHandle
VoipDecode(compressedFrame) → PCMData
VoipAnswer(callId, direction)
VoipCall(targetUserId, timeout) → callId
VoipMute(callId)
VoipUnmute(callId)
VoipSetBitRate(callId, rate)
VoipGetState(callId) → State
```

**Data Structures**:
```
CallState {
  callId: uint64
  localUserId: uint64
  remoteUserId: uint64
  direction: In|Out|Both
  state: Idle|Calling|Connected|Held|Error
  bitRate: uint32
  muted: bool
  startTime: timestamp
}

VoipFrame {
  sessionId: uint64
  sequenceNum: uint32
  timestamp: uint64
  data: CompressedAudioBuffer
  codec: Opus|PCM
}
```

**Dependencies**: Network I/O, User Management

**Security**:
- User must be authenticated
- Call requires mutual agreement
- Audio encrypted in transit (TLS)

**Testing Strategy**:
- Unit: Encoder/decoder round-trip (verify PCM reconstructed)
- Integration: Two connected users, voice quality metrics
- End-to-end: Echo test with latency measurement

---

### 1.2 Microphone Input Handling

**Feature**: Capture audio from local microphone, manage device selection.

**API**:
```
MicCreate() → MicHandle
MicDestroy(handle)
MicStart()
MicStop()
MicRead(buffer, len) → bytesRead
MicDetected() → bool
MicAvailable() → bool
MicSetDevice(deviceId)
MicGetDeviceList() → [Device]
```

**Data Structures**:
```
MicDevice {
  id: uint32
  name: string
  sampleRate: uint32
  channels: uint8
  bitDepth: uint8
}

MicConfig {
  sampleRate: 16000|24000|48000
  bufferSize: 512|1024|2048
  captureSize: 20ms
}
```

**Dependencies**: Windows audio APIs (WASAPI), Audio buffer management

**Security**:
- User permission required for microphone access
- Audio frames include source identification

**Testing Strategy**:
- Device enumeration on various hardware
- Capture duration and format validation
- Buffer management under load

---

### 1.3 Audio Configuration

**Feature**: Manage codec settings, bitrate, quality tuning.

**Configuration Parameters**:
```
VoipBufferSize: 4096-65536 bytes
VoipPacketSize: 20-120 ms frames
VoipSampleRate: 16000, 24000, 48000 Hz
VoipPushToTalkKey: Optional keybind
VoipBitRateDefault: 12000-64000 bps
VoipQualityMode: Bandwidth|Balanced|Quality
```

**Testing**: Configuration persistence, parameter validation

---

## 2. Microphone Device Management (Priority: HIGH)

### 2.1 Device Detection & Enumeration

**Feature**: Discover local audio devices via Windows audio APIs.

**API**:
```
GetAudioDevices() → [Device]
GetDefaultAudioDevice() → Device
SetAudioDevice(deviceId) → Success
GetAudioDeviceProperties(deviceId) → Properties
```

**Capabilities Detection**:
- Microphone available (driver loaded, permissions granted)
- Supported sample rates
- Recommended buffer sizes

**Testing**: Run on multiple Windows versions with various audio hardware

---

## 3. User Management (Priority: HIGH)

### 3.1 User Identification

**Feature**: Track Oculus user accounts, identity validation.

**API**:
```
GetCurrentUser() → User
GetUser(userId) → User
GetUserPresence(userId) → PresenceData
GetUserList(filter) → [User]
ValidateUser(token) → (userId, expiryTime)
```

**Data Structures**:
```
User {
  userId: uint64
  oculusId: string
  displayName: string
  imageUrl: string
  verified: bool
  privacyLevel: Public|Friends|Private
}

PresenceData {
  userId: uint64
  activity: InLobby|InGame|AFK
  updatedAt: timestamp
  customData: string  // Game-specific
}
```

**Dependencies**: Token validation, Oculus SDK integration

**Testing**: Token refresh, multi-user scenarios

---

### 3.2 Invite Tokens & Social Linking

**Feature**: Generate cross-game invites, user discovery.

**API**:
```
GetInviteToken(targetUserId) → Token
DecodeInviteToken(token) → (userId, expiryTime)
InviteToRoom(userIds[], roomId) → Success
GetInviteHistory(limit) → [Invite]
```

**Security**: Tokens expire, scope-limited (single game/instance)

---

## 4. Room Management (Priority: HIGH)

### 4.1 Room Lifecycle

**Feature**: Create, join, manage virtual multiplayer rooms.

**API**:
```
CreateRoom(options) → roomId
CreatePrivateRoom(inviteList, options) → roomId
JoinRoom(roomId, token) → Success
LeaveRoom(roomId)
GetRoom(roomId) → Room
DestroyRoom(roomId)  // Owner only
```

**Data Structures**:
```
Room {
  id: uint64
  owner: UserId
  name: string
  maxCapacity: uint32
  currentCapacity: uint32
  joinPolicy: Public|FriendsOnly|InviteOnly|Private
  dataStore: Map<string, string>
  users: [User]
  createdAt: timestamp
  expiresAt: timestamp
}

RoomOptions {
  name: string
  capacity: 1-100
  joinPolicy: see above
  orderByUpdateTime: bool
  orderByMemberCount: bool
}
```

**Behaviors**:
- Automatic expiry if owner leaves
- Capacity limits enforcement
- User list pagination (HasNextPage)

**Testing**:
- Create/join/leave sequences
- Capacity enforcement
- Join policy validation
- User list pagination

---

### 4.2 Room Data Store

**Feature**: Persist game state in shared room store.

**API**:
```
SetRoomData(roomId, key, value)
GetRoomData(roomId, key) → value
DeleteRoomData(roomId, key)
GetRoomDataStore(roomId) → Map<string, string>
```

**Use Cases**:
- Game configuration (difficulty, map)
- Score tracking
- Custom game state

**Testing**: Consistency across all room members

---

## 5. Rich Presence (Priority: MEDIUM)

### 5.1 Presence Broadcasting

**Feature**: Inform Oculus platform and friends about current activity.

**API**:
```
SetRichPresence(config) → Success
ClearPresence()
GetPresence() → PresenceInfo
```

**Data Structures**:
```
RichPresenceConfig {
  apiName: string              // Platform identifier
  currentCapacity: uint32      // Current players
  maxCapacity: uint32          // Max players
  startTime: timestamp
  endTime: timestamp
  instanceId: string           // Game instance UUID
  isJoinable: bool            // Friends can join
  deeplinkMessageOverride: string  // Custom invite text
  extraContext: string        // JSON game data
}

PresenceInfo {
  userId: uint64
  activity: string
  capacity: string
  joinable: bool
  startedAt: timestamp
  joinUrl: string  // Deeplink for social features
}
```

**Features**:
- Activity display in Oculus home
- Friends can see you're playing
- One-click join via deeplinks
- Supports VR social features

**Testing**: Presence visible on Oculus app, join from external source

---

## 6. In-App Purchases (Priority: MEDIUM)

### 6.1 Product Catalog

**Feature**: Query available in-game purchases.

**API**:
```
GetProductsBySKU(skuList) → [Product]
GetProducts(filter) → [Product]
GetProductPage(offset, limit) → ProductPage
```

**Data Structures**:
```
Product {
  sku: string
  name: string
  description: string
  price: string  // USD, formatted
  priceInCents: uint64
  imagePath: string
  released: bool
}

ProductPage {
  items: [Product]
  offset: uint32
  limit: uint32
  hasNextPage: bool
  totalCount: uint32
}
```

**Testing**: Catalog retrieval, pagination correctness

---

### 6.2 Purchase Processing

**Feature**: Initiate purchase flow, verify transactions.

**API**:
```
LaunchCheckoutFlow(skuList, options) → transactionId
GetViewerPurchases() → [Purchase]
GetPurchases(userId) → [Purchase]
GetDurablePurchases() → [Purchase]  // Offline cache
VerifyPurchase(receiptToken) → bool
```

**Data Structures**:
```
Purchase {
  transactionId: uint64
  sku: string
  purchaseTime: timestamp
  durable: bool
  receiptToken: string  // For server verification
}
```

**Security**:
- Verify receipts server-side
- Token-based validation
- Prevent duplicate purchases

**Testing**: Purchase workflow, receipt verification, offline mode

---

## 7. Notifications & Invitations (Priority: MEDIUM)

### 7.1 Room Invites

**Feature**: Send and receive room invitations.

**API**:
```
GetRoomInvites(limit) → [RoomInvite]
MarkInviteAsRead(inviteId)
AcceptInvite(inviteId) → roomId
DeclineInvite(inviteId)
SendInvite(userIds[], roomId, customMessage)
```

**Data Structures**:
```
RoomInvite {
  id: uint64
  fromUser: User
  roomId: uint64
  roomName: string
  message: string
  sentAt: timestamp
  expiresAt: timestamp
  read: bool
}
```

**Testing**: Invite lifecycle, expiry, read state

---

## 8. Destinations & Deep Linking (Priority: LOW)

### 8.1 Launch Points

**Feature**: Define joinable activities in the app.

**API**:
```
GetDestinations() → [Destination]
GetDestination(apiName) → Destination
LaunchFromDeeplink(url, params) → launchContext
```

**Data Structures**:
```
Destination {
  apiName: string      // "lobby", "game.matchmaking"
  displayName: string
  description: string
}

LaunchContext {
  destination: string
  parameters: Map<string, string>
}
```

---

## 9. Error Handling (Priority: CRITICAL)

### 9.1 Error Reporting

**Feature**: Standardized error reporting across all APIs.

**API**:
```
GetError() → Error
ClearError()
```

**Data Structures**:
```
Error {
  code: uint32
  message: string
  httpCode: uint32  // If network error
  details: string
  recoverable: bool
}
```

**Error Categories**:
- Generic (0-999)
- Network (1000-1999)
- Auth (2000-2999)
- Resource (3000-3999)
- Timeout (4000-4999)
- Codec (5000-5999)

**Testing**: All failure paths return proper errors

---

## 10. Entitlements (Priority: MEDIUM)

### 10.1 Permission Checking

**Feature**: Verify user is entitled to use plugin.

**API**:
```
CheckEntitlement() → bool
GetEntitlement() → Entitlement
```

**Data Structures**:
```
Entitlement {
  userId: uint64
  appId: uint64
  entitled: bool
  expiresAt: timestamp
}
```

**Testing**: Entitlement verification, expiry handling

---

## Implementation Priority Matrix

| Feature | Priority | Complexity | Blocking | Est. Hours |
|---------|----------|-----------|----------|-----------|
| Voice (Voip) | CRITICAL | High | All | 40 |
| Microphone | HIGH | Medium | Voice | 20 |
| User Mgmt | HIGH | Medium | All | 15 |
| Rooms | HIGH | High | Voice, Users | 30 |
| Rich Presence | MEDIUM | Low | Users | 8 |
| Invitations | MEDIUM | Low | Users, Rooms | 12 |
| Purchases | MEDIUM | Medium | Users | 15 |
| Entitlements | MEDIUM | Low | Users | 5 |
| Destinations | LOW | Low | Rooms | 5 |
| Error Handling | CRITICAL | Low | All | 5 |

**Total Estimated**: ~155 hours

---

## Next: Phase 3 - Acceptance Tests

Before implementation (Phase 4), create comprehensive test suites for each feature:
1. Unit tests for data structures
2. Integration tests for feature interactions  
3. End-to-end tests for complete workflows
4. Error scenario coverage
5. Performance benchmarks

Tests must pass BEFORE moving to Phase 4 implementation.
