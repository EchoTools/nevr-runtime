#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

using SoundEventCallback = void (*)(int, void*);

struct SoundKey {
    std::uint32_t playing_id;
    std::uint32_t padding;
    std::uint64_t sound;
};
static_assert(sizeof(SoundKey) == 0x10);
static_assert(offsetof(SoundKey, playing_id) == 0x00);
static_assert(offsetof(SoundKey, padding) == 0x04);
static_assert(offsetof(SoundKey, sound) == 0x08);

struct AudioParameterRequestSlot {
    std::uint64_t symbol;
    std::uint64_t object_id;
    std::uint32_t status;
    std::uint32_t pool_index;
    std::uint32_t request_kind;
    std::uint32_t output_value;
};
static_assert(sizeof(AudioParameterRequestSlot) == 0x20);
static_assert(offsetof(AudioParameterRequestSlot, pool_index) == 0x14);

struct AudioParameterRequestMapEntry {
    std::uint32_t request_slot;
    std::uint32_t value;
};
static_assert(sizeof(AudioParameterRequestMapEntry) == 0x08);

struct SoundSwitchKey {
    std::uint32_t group;
    std::uint32_t value;
};
static_assert(sizeof(SoundSwitchKey) == 0x08);

extern std::uint64_t DAT_03864210;
extern const std::int32_t DAT_03859620;
extern std::uintptr_t DAT_038cec70;
extern std::uint64_t DAT_038cec78;
extern std::int32_t DAT_038cee60;
extern std::uint64_t DAT_038cecc0;
extern std::byte DAT_038ced68;
extern std::uintptr_t g_audio_disabled_flag;
extern std::uintptr_t g_wwise_engine;
extern SoundEventCallback g_sound_event_callback;
extern std::uint32_t DAT_0386ee28;
extern AudioParameterRequestSlot DAT_0386eed0[32];
extern AudioParameterRequestMapEntry DAT_03852374[32];
extern std::byte DAT_0386eeb0;

int PostSoundEvent(void* self, std::uint32_t event, std::uint32_t game_object,
                   const std::uint32_t* flags, SoundEventCallback callback,
                   void* cookie, std::uint32_t seek_ms, std::int32_t* playing_id);
std::int32_t Play2DSound(std::uint64_t player, std::uint64_t event,
                         float volume);
std::int32_t Play3DSound(std::uint64_t player, std::uint64_t event,
                         std::uint32_t emitter, float volume);
std::uint64_t SetParameter(std::uint32_t symbol, float value,
                           std::uint32_t playing_id);
bool SoundPaused(const void* player, std::uint32_t expected_state,
                 std::uint32_t entry_key);
std::uint64_t SetSwitch(const void* player, std::uint32_t group,
                        std::uint32_t value, std::uint32_t emitter_id);
std::uint64_t SetState(void* player, std::uint32_t group,
                       std::uint32_t state);
std::uint32_t GetState(std::uint32_t group);
std::uint32_t GetSwitch(const void* player, std::uint32_t group,
                        std::uint32_t emitter_id);
std::uint64_t SetParameter2D(const void* player, std::uint32_t symbol,
                             float value);
std::uint64_t ResetParameter(std::uint32_t symbol, float value,
                             std::uint32_t object_id);
std::uint64_t RegisterParameterRequest(std::uint32_t symbol,
                                       std::uint32_t object_id);
std::uint32_t RetrieveParameter(std::uint64_t handle,
                                std::uint32_t* output_value);
void UnregisterParameterRequest(std::uint64_t handle);
std::uint64_t ResetParameter2D(const void* player, std::uint32_t symbol,
                               float value);
std::uint64_t QueueWwiseResetCommand(std::uint32_t symbol,
                                     std::uint64_t object_id,
                                     std::uint32_t scaled_value,
                                     std::uint32_t command_kind,
                                     std::uint32_t flags);
std::uint64_t QueueWwiseSwitchCommand(std::uint32_t group,
                                      std::uint32_t value,
                                      std::uint32_t emitter_id);
std::uint64_t BuildWwiseStateCommand(std::uint32_t group,
                                     std::uint32_t state,
                                     std::uint16_t flags_a,
                                     std::uint16_t flags_b);
std::uint64_t QueueWwiseStateCommand(std::uint32_t group,
                                     std::uint32_t state);
void InsertSoundSwitchKey(void* table, std::uint64_t index,
                          const SoundSwitchKey* key, std::uint64_t count);
void InsertSoundKey(void* table, std::uint64_t index, std::uint64_t count,
                    const SoundKey* value);
std::uint64_t PostWwiseEvent(std::uint32_t event, std::uint64_t game_object,
                             std::uint32_t flags, SoundEventCallback callback,
                             void* cookie, std::int32_t callback_kind,
                             std::uint64_t callback_user, std::uint32_t playing_id);
std::uint32_t QueueWwiseSeek(std::uint32_t event, std::uint64_t game_object,
                             std::uint32_t seek_ms, std::uint16_t flags,
                             std::uint32_t playing_id);
std::uint64_t QueueWwiseValueCommand(std::uint64_t symbol,
                                     std::uint64_t game_object, float value,
                                     std::uint32_t scaled_value,
                                     std::uint32_t command_kind,
                                     std::uint32_t flags);
std::uint64_t BuildWwiseValueCommand(std::uint32_t command_id,
                                     std::uint64_t game_object, float value,
                                     std::uint32_t reserved,
                                     std::uint32_t scaled_value,
                                     std::uint32_t command_kind,
                                     std::uint32_t flags);
std::uint32_t PostWwiseEventCommand(std::uint32_t event, std::uint64_t game_object,
                                    std::uint32_t flags, SoundEventCallback callback,
                                    void* cookie, const void* callback_info,
                                    std::uint32_t playing_id);
void* FindWwiseEventSymbol(std::uintptr_t registry, std::uint32_t event);
void BeginAudioCommandScope(void* scope, std::uint16_t command_kind,
                            std::uint32_t command_size);
std::byte* GetAudioCommandScope(const void* scope);
void CommitAudioCommandScope(void* scope);

void* GetCurrentTaskGroup(std::uint64_t tls_key);
void ExecuteTask(void* task_group, int flags);
const char* SoundSymbolString(std::uint32_t event);
void InitializeSuppressErrorBreakSelect(std::byte* storage, int enabled);
void DestroySuppressErrorBreakSelect(std::byte* storage);
int CreateAndAddError(const char* file, std::uint32_t line, std::uint64_t id,
                      const char* format, ...);
std::uint64_t TranslateWwiseResult(const char* label, std::uint32_t result,
                                   const char* file, std::uint32_t line);
int OutputError(std::uint64_t error, int flags);
std::uint64_t SoundSymbolError(std::uint32_t symbol, const char* action,
                               const char* symbol_kind);
std::int32_t QueryWwiseState(std::uint32_t group, std::uint32_t* state);
void CLockReadWriteWriteLock(void* lock);
void CLockReadWriteWriteUnlock(void* lock);
void CLockReadWriteReadLock(void* lock);
void CLockReadWriteReadUnlock(void* lock);
void NWriteLogWriteLog(int severity, void* context, const char* format, ...);

std::uint64_t CreateCallbackSource(std::int32_t kind, std::uint64_t user);
bool CallbackInfoEqualsValue(const std::byte* storage, std::uintptr_t value);
void* CallbackInfoSource(const std::byte* storage);
void ReleaseCallbackSource(std::uint64_t source);

std::uint32_t WwiseSeekCommandSize();
std::uint32_t WwisePostCommandSize();
void* StoreAudioCommandReference(std::byte* field, void* object);
const void* InitializeCallbackArgument(std::byte* field, const void* argument);
std::int32_t NextWwiseCommandId(std::int32_t* state);
std::uint32_t LoadAudioCommandWordAtOffset10(const void* object);
void* LoadAudioCommandPointer(void* const* source);
void BeginWwisePlacementContext(void* context);
void* WwisePlacementStorage(std::size_t size, void* storage);
void InitializeWwiseCallbackMetadata(std::byte* destination,
                                    std::uint32_t callback_count,
                                    const std::byte* source);
std::byte* AllocateWwisePoolBlock(std::int32_t pool, std::size_t size);
void* ReallocateWwisePoolBlock(std::int32_t pool, void* allocation,
                               std::size_t size);
void FreeWwisePoolBlock(std::int32_t pool, void* allocation);
void InitializeAudioPostCommandNode(std::byte* node);
std::uint32_t GrowWwisePostQueue(std::byte* queue,
                                 std::uint32_t bucket_count);
bool EnsureWwisePostQueueCapacity(std::byte* queue);
bool InsertWwisePostNode(std::uint64_t context, std::byte* node);
bool ResizeWwisePostQueueBuckets(std::byte* table,
                                 std::uint32_t added_buckets);
int SubmitWwisePostCommand(std::uint64_t context, std::byte* command,
                           SoundEventCallback callback, void* cookie,
                           std::uint32_t flags, std::uint32_t thread_token);
std::uint32_t LockAudioMutex(void* mutex);
std::uint32_t UnlockAudioMutex(void* mutex);
void BeginWwiseSymbolLookup(std::byte* scope, std::uintptr_t mutex);
void* LookupWwiseSymbol(std::uintptr_t table, std::uint32_t event);
std::uint32_t WwiseSymbolBucketCount(const void* table);
std::uint32_t WwiseSymbolBucketCountThunk(const void* table);
std::uint32_t WwiseSymbolHashInput(std::uint32_t event);
void* FindWwiseSymbolBucketEntry(const void* table, std::uint32_t event,
                                 std::uint32_t bucket_index);
std::int32_t AcquireWwiseSymbolReference(void* symbol);
void EndWwiseSymbolLookup(std::byte* scope);
std::byte* AllocateAudioCommandStorage(std::uint64_t context,
                                       std::uint16_t command_kind,
                                       std::uint32_t command_size);
std::byte* AllocateAudioCommandRegion(std::uint64_t context,
                                      std::uint32_t* requested_size);
std::byte* AllocateAudioRingBlock(std::uint64_t context,
                                  std::uint32_t* requested_size);
void SleepAudioCommandWait(std::uint32_t milliseconds);
void AudioCommandMemoryBarrier();
void WaitForAudioCommandActivity(std::uint64_t context);
std::uint64_t ProcessAudioCommandQueue(std::uint64_t context,
                                       std::uint64_t mode,
                                       std::uint8_t* process_flags);
bool ReserveAudioRingCursor(std::uintptr_t* cursor, std::uintptr_t expected,
                            std::uintptr_t next);
std::int32_t DecrementAtomicReference(std::int32_t* reference_count);
void DecrementAudioCommandActivity(std::uint64_t context);
std::uint32_t AudioCommandQueueCapacity(const void* queue);
std::uint64_t AudioCommandQueueSize(const std::uint64_t* queue);
std::uint64_t AudioCommandQueueHead(const std::uint64_t* queue);
bool AudioCommandQueueHasFourBytes(const std::uint64_t* queue);
void AdvanceAudioCommandQueueHead(std::uint64_t* queue,
                                 std::int32_t byte_count);

}  // namespace NRadEngine::AudioPost
