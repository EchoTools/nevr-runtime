// LibOVRPlatform64_1.dll — stub implementation
// Exports the minimum Oculus Platform SDK surface required for pnsovr.dll to
// load without the real SDK.  Every function logs its name and returns a sane
// default (0, nullptr, false, empty string).
//
// Two import vectors in pnsovr.dll:
//   1. GetProcAddress — 7 init functions (ovr_ResolveSDKFunctions)
//   2. IAT — everything else (voip, mic, users, rooms, IAP, etc.)
//
// Goal: pnsovr.dll loads.  We fix individual stubs as they get exercised.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define STUB_LOG(fmt, ...) fprintf(stderr, "[NEVR.OVR-STUB] " fmt "\n", ##__VA_ARGS__)

// Opaque handle types — all just pointers the real SDK would allocate.
using ovrRequest          = uint64_t;
using ovrID               = uint64_t;
using ovrMessageHandle    = void*;
using ovrPlatformHandle   = void*;

// Monotonic request ID counter.  The real SDK returns unique non-zero handles
// for async operations; callers check (handle == 0) to detect submission failure.
static ovrRequest g_nextRequestId = 0x4E455652; // "NEVR" as a seed

// ============================================================================
// Platform initialization  (GetProcAddress-resolved in pnsovr.dll)
// ============================================================================

EXPORT ovrRequest ovr_PlatformInitializeWindows(uint64_t appId) {
    STUB_LOG("%s(appId=%llu)", __func__, (unsigned long long)appId);
    return 0; // 0 = success (pnsovr checks: if (result != 0) return false)
}

EXPORT ovrRequest ovr_PlatformInitializeWindowsAsynchronous(uint64_t appId) {
    STUB_LOG("%s(appId=%llu)", __func__, (unsigned long long)appId);
    // Async init returns a request handle (non-zero = submitted, 0 = failure).
    // The actual result arrives via ovr_PopMessage as ovrMessage_PlatformInitializeWindowsAsynchronous.
    return g_nextRequestId++;
}

EXPORT ovrRequest ovr_PlatformInitializeWithAccessToken(uint64_t appId, const char* token) {
    STUB_LOG("%s(appId=%llu, token=%s)", __func__, (unsigned long long)appId, token ? token : "(null)");
    return 0; // Sync: 0 = ovrPlatformInitialize_Success
}

EXPORT ovrRequest ovr_PlatformInitializeWithAccessTokenAndOptions(uint64_t appId, const char* token, void* opts) {
    STUB_LOG("%s(appId=%llu, token=%s)", __func__, (unsigned long long)appId, token ? token : "(null)");
    return 0; // Sync: 0 = ovrPlatformInitialize_Success
}

EXPORT ovrMessageHandle ovr_PopMessage() {
    // Called in a tight loop — do NOT log every call.
    return nullptr;
}

EXPORT ovrRequest ovr_PlatformInitializeStandaloneAccessToken(uint64_t appId, const char* token, const char* orgId) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(appId=%llu) -> req %llu", __func__, (unsigned long long)appId, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Platform_InitializeStandaloneOculus(uint64_t appId, const char* orgId) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(appId=%llu) -> req %llu", __func__, (unsigned long long)appId, (unsigned long long)id);
    return id;
}

EXPORT int ovr_IsPlatformInitialized() {
    STUB_LOG("%s called", __func__);
    return 1; // pretend we're initialized
}

// ============================================================================
// Message handling
// ============================================================================

EXPORT void ovr_FreeMessage(ovrMessageHandle msg) {
    // no-op — we never allocate real messages
}

EXPORT int32_t ovr_Message_GetType(ovrMessageHandle msg) {
    return 0; // ovrMessage_Unknown
}

EXPORT ovrRequest ovr_Message_GetRequestID(ovrMessageHandle msg) {
    return 0;
}

EXPORT int ovr_Message_IsError(ovrMessageHandle msg) {
    return 0; // not an error
}

EXPORT const char* ovr_Message_GetString(ovrMessageHandle msg) {
    return "";
}

EXPORT void* ovr_Message_GetError(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetUser(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetUserArray(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetUserAndRoomArray(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetUserProof(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetOrgScopedID(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetProductArray(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetPurchase(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetPurchaseArray(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetDestinationArray(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetRoom(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetRoomInviteNotification(ovrMessageHandle msg) {
    return nullptr;
}

EXPORT void* ovr_Message_GetRoomInviteNotificationArray(ovrMessageHandle msg) {
    return nullptr;
}

// ============================================================================
// Error handling
// ============================================================================

EXPORT const char* ovr_Error_GetMessage(void* error) {
    return "[NEVR.OVR-STUB] no error";
}

EXPORT int ovr_Error_GetCode(void* error) {
    return 0;
}

EXPORT int ovr_Error_GetHttpCode(void* error) {
    return 0;
}

// ============================================================================
// User functions
// ============================================================================

EXPORT ovrRequest ovr_User_GetLoggedInUser() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_User_GetAccessToken() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_User_GetUserProof() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_User_Get(ovrID userID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(userID=%llu) -> req %llu", __func__, (unsigned long long)userID, (unsigned long long)id);
    return id;
}

EXPORT ovrID ovr_User_GetID(void* user) {
    return 1000; // Fake user ID
}

EXPORT const char* ovr_User_GetOculusID(void* user) {
    return "StubUser";
}

EXPORT ovrRequest ovr_User_GetOrgScopedID(ovrID userID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(userID=%llu) -> req %llu", __func__, (unsigned long long)userID, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_User_GetLoggedInUserFriends() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_User_GetLoggedInUserRecentlyMetUsersAndRooms(int filter) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT void* ovr_User_GetPresence(void* user) {
    return nullptr;
}

EXPORT int ovr_User_GetPresenceStatus(void* user) {
    return 0;
}

EXPORT const char* ovr_User_GetPresenceDeeplinkMessage(void* user) {
    return "";
}

EXPORT const char* ovr_User_GetInviteToken(void* user) {
    return "";
}

EXPORT ovrRequest ovr_User_GetNextUserArrayPage(void* userArray) {
    STUB_LOG("%s called", __func__);
    return 0; // 0 = no more pages
}

EXPORT ovrRequest ovr_User_GetNextUserAndRoomArrayPage(void* userAndRoomArray) {
    STUB_LOG("%s called", __func__);
    return 0; // 0 = no more pages
}

EXPORT ovrID ovr_GetLoggedInUserID() {
    STUB_LOG("%s called", __func__);
    return 1000; // Fake user ID — non-zero so callers don't treat as "no user"
}

EXPORT void ovrID_FromString(ovrID* outId, const char* str) {
    STUB_LOG("%s(str=\"%s\")", __func__, str ? str : "(null)");
    if (outId) *outId = 0;
}

// ============================================================================
// User proof
// ============================================================================

EXPORT const char* ovr_UserProof_GetNonce(void* proof) {
    return "";
}

// ============================================================================
// Org-scoped ID
// ============================================================================

EXPORT ovrID ovr_OrgScopedID_GetID(void* orgScopedID) {
    return 0;
}

// ============================================================================
// User arrays
// ============================================================================

EXPORT uint64_t ovr_UserArray_GetSize(void* arr) {
    return 0;
}

EXPORT void* ovr_UserArray_GetElement(void* arr, uint64_t index) {
    return nullptr;
}

EXPORT int ovr_UserArray_HasNextPage(void* arr) {
    return 0;
}

// ============================================================================
// UserAndRoom arrays
// ============================================================================

EXPORT uint64_t ovr_UserAndRoomArray_GetSize(void* arr) {
    return 0;
}

EXPORT void* ovr_UserAndRoomArray_GetElement(void* arr, uint64_t index) {
    return nullptr;
}

EXPORT int ovr_UserAndRoomArray_HasNextPage(void* arr) {
    return 0;
}

EXPORT void* ovr_UserAndRoom_GetUser(void* userAndRoom) {
    return nullptr;
}

// ============================================================================
// Entitlement
// ============================================================================

EXPORT ovrRequest ovr_Entitlement_GetIsViewerEntitled() {
    STUB_LOG("%s called — returning fake success", __func__);
    return 1; // non-zero = request submitted
}

// ============================================================================
// IAP (in-app purchases)
// ============================================================================

EXPORT ovrRequest ovr_IAP_GetProductsBySKU(const char** skus, int count) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(count=%d) -> req %llu", __func__, count, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_IAP_GetViewerPurchases() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_IAP_GetViewerPurchasesDurableCache() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_IAP_LaunchCheckoutFlow(const char* sku) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(sku=\"%s\") -> req %llu", __func__, sku ? sku : "(null)", (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_IAP_GetNextProductArrayPage(void* arr) {
    return 0;
}

EXPORT ovrRequest ovr_IAP_GetNextPurchaseArrayPage(void* arr) {
    return 0;
}

// ============================================================================
// Product arrays
// ============================================================================

EXPORT uint64_t ovr_ProductArray_GetSize(void* arr) { return 0; }
EXPORT void* ovr_ProductArray_GetElement(void* arr, uint64_t index) { return nullptr; }
EXPORT int ovr_ProductArray_HasNextPage(void* arr) { return 0; }
EXPORT const char* ovr_Product_GetSKU(void* product) { return ""; }
EXPORT const char* ovr_Product_GetName(void* product) { return ""; }
EXPORT const char* ovr_Product_GetDescription(void* product) { return ""; }
EXPORT const char* ovr_Product_GetFormattedPrice(void* product) { return "$0.00"; }

// ============================================================================
// Purchase arrays
// ============================================================================

EXPORT uint64_t ovr_PurchaseArray_GetSize(void* arr) { return 0; }
EXPORT void* ovr_PurchaseArray_GetElement(void* arr, uint64_t index) { return nullptr; }
EXPORT int ovr_PurchaseArray_HasNextPage(void* arr) { return 0; }
EXPORT const char* ovr_Purchase_GetSKU(void* purchase) { return ""; }

// ============================================================================
// Rich Presence
// ============================================================================

EXPORT ovrRequest ovr_RichPresence_Set(void* options) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_RichPresence_Clear() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_RichPresence_GetDestinations() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_RichPresence_GetNextDestinationArrayPage(void* arr) {
    return 0;
}

EXPORT void* ovr_RichPresenceOptions_Create() {
    STUB_LOG("%s called", __func__);
    return nullptr;
}

EXPORT void ovr_RichPresenceOptions_Destroy(void* handle) {}
EXPORT void ovr_RichPresenceOptions_SetApiName(void* h, const char* v) {}
EXPORT void ovr_RichPresenceOptions_SetCurrentCapacity(void* h, unsigned int v) {}
EXPORT void ovr_RichPresenceOptions_SetMaxCapacity(void* h, unsigned int v) {}
EXPORT void ovr_RichPresenceOptions_SetStartTime(void* h, uint64_t v) {}
EXPORT void ovr_RichPresenceOptions_SetEndTime(void* h, uint64_t v) {}
EXPORT void ovr_RichPresenceOptions_SetIsJoinable(void* h, int v) {}
EXPORT void ovr_RichPresenceOptions_SetLobbySessionId(void* h, const char* v) {}
EXPORT void ovr_RichPresenceOptions_SetExtraContext(void* h, int v) {}
EXPORT void ovr_RichPresenceOptions_SetDeeplinkMessageOverride(void* h, const char* v) {}
EXPORT void ovr_RichPresenceOptions_SetInstanceId(void* h, const char* v) {}

// ============================================================================
// Destination arrays
// ============================================================================

EXPORT uint64_t ovr_DestinationArray_GetSize(void* arr) { return 0; }
EXPORT void* ovr_DestinationArray_GetElement(void* arr, uint64_t index) { return nullptr; }
EXPORT int ovr_DestinationArray_HasNextPage(void* arr) { return 0; }
EXPORT const char* ovr_Destination_GetApiName(void* dest) { return ""; }
EXPORT const char* ovr_Destination_GetDisplayName(void* dest) { return ""; }

// ============================================================================
// Rooms
// ============================================================================

EXPORT ovrRequest ovr_Room_CreateAndJoinPrivate2(int joinPolicy, unsigned int maxUsers, void* opts) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_Get(ovrID roomID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(roomID=%llu) -> req %llu", __func__, (unsigned long long)roomID, (unsigned long long)id);
    return id;
}

EXPORT ovrID ovr_Room_GetID(void* room) { return 0; }
EXPORT void* ovr_Room_GetOwner(void* room) { return nullptr; }
EXPORT void* ovr_Room_GetUsers(void* room) { return nullptr; }
EXPORT void* ovr_Room_GetDataStore(void* room) { return nullptr; }
EXPORT int ovr_Room_GetJoinPolicy(void* room) { return 0; }
EXPORT int ovr_Room_GetIsMembershipLocked(void* room) { return 0; }

EXPORT ovrRequest ovr_Room_GetInvitableUsers2(ovrID roomID, void* opts) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_Join2(ovrID roomID, void* opts) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_Leave(ovrID roomID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_KickUser(ovrID roomID, ovrID userID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_InviteUser(ovrID roomID, const char* inviteToken) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_UpdateDataStore(ovrID roomID, void* data, int count) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_UpdateOwner(ovrID roomID, ovrID newOwner) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_UpdateMembershipLockStatus(ovrID roomID, int membershipLockStatus) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_UpdatePrivateRoomJoinPolicy(ovrID roomID, int joinPolicy) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Room_LaunchInvitableUserFlow(ovrID roomID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT void* ovr_RoomOptions_Create() { return nullptr; }
EXPORT void ovr_RoomOptions_Destroy(void* opts) {}
EXPORT void ovr_RoomOptions_SetOrdering(void* opts, int ordering) {}

// ============================================================================
// Room invite notifications
// ============================================================================

EXPORT ovrID ovr_RoomInviteNotification_GetID(void* notif) { return 0; }
EXPORT ovrID ovr_RoomInviteNotification_GetRoomID(void* notif) { return 0; }
EXPORT uint64_t ovr_RoomInviteNotification_GetSentTime(void* notif) { return 0; }
EXPORT uint64_t ovr_RoomInviteNotificationArray_GetSize(void* arr) { return 0; }
EXPORT void* ovr_RoomInviteNotificationArray_GetElement(void* arr, uint64_t index) { return nullptr; }
EXPORT int ovr_RoomInviteNotificationArray_HasNextPage(void* arr) { return 0; }

EXPORT ovrRequest ovr_Notification_GetRoomInvites() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Notification_GetNextRoomInviteNotificationArrayPage(void* arr) {
    return 0;
}

EXPORT ovrRequest ovr_Notification_MarkAsRead(ovrID notifID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s(notifID=%llu) -> req %llu", __func__, (unsigned long long)notifID, (unsigned long long)id);
    return id;
}

// ============================================================================
// Party
// ============================================================================

EXPORT ovrRequest ovr_Party_GetCurrent() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Party_Create() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Party_Invite(ovrID userID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_Party_Leave(ovrID partyID) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

// ============================================================================
// Application / Lifecycle
// ============================================================================

EXPORT ovrRequest ovr_ApplicationLifecycle_GetLaunchDetails() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_ApplicationInvite_GetInvites() {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT ovrRequest ovr_ApplicationInvite_Send(void* opts) {
    ovrRequest id = g_nextRequestId++;
    STUB_LOG("%s called -> req %llu", __func__, (unsigned long long)id);
    return id;
}

EXPORT int ovr_LaunchDetails_GetLaunchType(void* details) { return 0; }
EXPORT const char* ovr_LaunchDetails_GetLaunchSource(void* details) { return ""; }
EXPORT const char* ovr_LaunchDetails_GetDeeplinkMessage(void* details) { return ""; }
EXPORT const char* ovr_LaunchDetails_GetDestinationApiName(void* details) { return ""; }
EXPORT ovrID ovr_LaunchDetails_GetRoomID(void* details) { return 0; }
EXPORT const char* ovr_DataStore_GetValue(void* store, const char* key) { return ""; }

// ============================================================================
// Networking (P2P)
// ============================================================================

EXPORT int ovr_Net_SendPacket(ovrID userID, uint64_t length, const void* bytes, int policy) {
    return 0; // success
}

EXPORT int ovr_Net_SendPacketToCurrentRoom(uint64_t length, const void* bytes, int policy) {
    return 0;
}

EXPORT void* ovr_Net_ReadPacket() {
    return nullptr; // no packet
}

EXPORT void ovr_Net_AcceptForCurrentRoom() {
    STUB_LOG("%s called", __func__);
}

EXPORT void ovr_Net_CloseForCurrentRoom() {
    STUB_LOG("%s called", __func__);
}

EXPORT const void* ovr_Packet_GetBytes(void* packet) { return nullptr; }
EXPORT uint64_t ovr_Packet_GetSize(void* packet) { return 0; }
EXPORT ovrID ovr_Packet_GetSenderID(void* packet) { return 0; }
EXPORT void ovr_Packet_Free(void* packet) {}

// ============================================================================
// VoIP
// ============================================================================

EXPORT void ovr_Voip_Start(ovrID userID) {
    STUB_LOG("%s(userID=%llu)", __func__, (unsigned long long)userID);
}

EXPORT void ovr_Voip_Stop(ovrID userID) {
    STUB_LOG("%s(userID=%llu)", __func__, (unsigned long long)userID);
}

EXPORT void ovr_Voip_Accept(ovrID userID) {
    STUB_LOG("%s(userID=%llu)", __func__, (unsigned long long)userID);
}

EXPORT uint64_t ovr_Voip_GetPCM(ovrID userID, int16_t* buffer, uint64_t bufferSize) {
    return 0;
}

EXPORT uint64_t ovr_Voip_GetPCMSize(ovrID userID) {
    return 0;
}

EXPORT uint64_t ovr_Voip_GetOutputBufferMaxSize() {
    return 0;
}

EXPORT void ovr_Voip_SetMicrophoneMuted(int muted) {
    STUB_LOG("%s(muted=%d)", __func__, muted);
}

EXPORT void ovr_Voip_SendPacket(ovrID userID, uint64_t length, const void* bytes) {
    // drop silently
}

EXPORT void* ovr_Voip_CreateEncoder() {
    STUB_LOG("%s called", __func__);
    return nullptr;
}

EXPORT void* ovr_Voip_CreateDecoder() {
    STUB_LOG("%s called", __func__);
    return nullptr;
}

EXPORT void ovr_Voip_DestroyEncoder(void* encoder) {}
EXPORT void ovr_Voip_DestroyDecoder(void* decoder) {}

// ============================================================================
// VoIP codec (low-level encoder/decoder)
// ============================================================================

EXPORT void* ovr_VoipEncoder_Create() {
    STUB_LOG("%s called", __func__);
    return nullptr;
}

EXPORT void ovr_VoipEncoder_AddPCM(void* encoder, const float* pcm, unsigned int numSamples) {}

EXPORT uint64_t ovr_VoipEncoder_GetCompressedData(void* encoder, void* buffer, uint64_t bufferSize) {
    return 0;
}

EXPORT void ovr_VoipDecoder_Decode(void* decoder, const void* data, uint64_t dataSize) {}

EXPORT uint64_t ovr_VoipDecoder_GetDecodedPCM(void* decoder, float* buffer, uint64_t bufferSize) {
    return 0;
}

// ============================================================================
// Microphone
// ============================================================================

EXPORT void* ovr_Microphone_Create() {
    STUB_LOG("%s called", __func__);
    return nullptr;
}

EXPORT void ovr_Microphone_Destroy(void* mic) {
    STUB_LOG("%s called", __func__);
}

EXPORT void ovr_Microphone_Start(void* mic) {
    STUB_LOG("%s called", __func__);
}

EXPORT void ovr_Microphone_Stop(void* mic) {
    STUB_LOG("%s called", __func__);
}

EXPORT uint64_t ovr_Microphone_GetPCM(void* mic, int16_t* buffer, uint64_t bufferSize) {
    return 0;
}

// ============================================================================
// Memory
// ============================================================================

EXPORT void* ovr_Malloc(uint64_t size) {
    return malloc((size_t)size);
}

EXPORT void ovr_Free(void* ptr) {
    free(ptr);
}

// ============================================================================
// DllMain
// ============================================================================

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        STUB_LOG("LibOVRPlatform64_1.dll STUB loaded (NEVR runtime)");
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
#endif
// Result code → human-readable string (matches real Oculus SDK enum values).
// The real SDK returns "ovrPlatformInitialize_Success" etc.  We include the
// numeric code in brackets so the game's error dialog becomes diagnostic.
EXPORT const char* ovrPlatformInitializeResult_ToString(int result) {
    // Thread-unsafe but fine — only called once during init error path.
    static char buf[128];
    switch (result) {
    case  0: return "Success";
    case -1: return "Uninitialized [-1]";
    case -2: return "PreLoaded [-2]";
    case -3: return "FileInvalid [-3]";
    case -4: return "SignatureInvalid [-4]";
    case -5: return "UnableToVerify [-5]";
    case -6: return "VersionMismatch [-6]";
    case -7: return "Unknown [-7]";
    case -8: return "InvalidCredentials [-8]";
    case -9: return "NotEntitled [-9]";
    default:
        snprintf(buf, sizeof(buf), "UnknownResult [%d / 0x%08X]", result, (unsigned)result);
        return buf;
    }
}
