#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

using SoundEventCallback = void (*)(int, void*);

extern std::uint64_t DAT_03864210;
extern std::uintptr_t DAT_038cec70;
extern std::uint64_t DAT_038cec78;
extern std::uint64_t DAT_038cee60;
extern std::uint64_t DAT_038cecc0;

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
void BeginAudioCommandScope(void* scope, std::uint32_t command_kind,
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
void InitializeCallbackInfo(std::byte* storage, std::uint64_t source);
std::uint64_t CallbackInfoHasNoFlags(const std::byte* storage, int flags);
void ReleaseCallbackInfo(std::byte* storage);
void ReleaseCallbackSource(std::uint64_t source);

std::uint32_t WwiseSeekCommandSize();
std::uint32_t WwisePostCommandSize();
void AddAudioCommandReference(std::byte* field, void* object);
void InitializeCallbackArgument(std::byte* field, const void* argument);
std::uint32_t NextWwiseCommandId(std::uint64_t* state);
void AcquireWwiseCommandReference(std::byte* field);
std::uint32_t CurrentWwiseThreadToken();
int SubmitWwisePostCommand(std::uint64_t context, std::byte* command,
                           SoundEventCallback callback, void* cookie,
                           std::uint32_t flags, std::uint32_t thread_token);
int BeginWwiseSymbolLookup(std::byte* scope, std::uintptr_t registry);
void* LookupWwiseSymbol(std::uintptr_t table, std::uint32_t event);
void AcquireWwiseSymbolReference(void* symbol);
void EndWwiseSymbolLookup(std::byte* scope);
void* AllocateAudioCommandStorage(std::uint64_t context,
                                  std::uint32_t command_kind,
                                  std::uint32_t command_size);
void SubmitAudioCommandStorage(std::uint64_t context);

}  // namespace NRadEngine::AudioPost
