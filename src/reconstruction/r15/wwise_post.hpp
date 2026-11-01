#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

using SoundEventCallback = void (*)(int, void*);

extern std::uint64_t DAT_03864210;
extern const std::int32_t DAT_03859620;
extern std::uintptr_t DAT_038cec70;
extern std::uint64_t DAT_038cec78;
extern std::int32_t DAT_038cee60;
extern std::uint64_t DAT_038cecc0;
extern std::byte DAT_038ced68;

int PostSoundEvent(void* self, std::uint32_t event, std::uint32_t game_object,
                   const std::uint32_t* flags, SoundEventCallback callback,
                   void* cookie, std::uint32_t seek_ms, std::int32_t* playing_id);
std::uint64_t PostWwiseEvent(std::uint32_t event, std::uint64_t game_object,
                             std::uint32_t flags, SoundEventCallback callback,
                             void* cookie, std::int32_t callback_kind,
                             std::uint64_t callback_user, std::uint32_t playing_id);
std::uint32_t QueueWwiseSeek(std::uint32_t event, std::uint64_t game_object,
                             std::uint32_t seek_ms, std::uint16_t flags,
                             std::uint32_t playing_id);
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
void OutputError(std::uint64_t error, int flags);

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
