/**
 * @file pnsovr_voip.cpp
 * @brief NEVR PNSOvr Compatibility - Voice/Audio Implementation
 *
 * Implements voice call management and Opus codec operations.
 * Uses external libopus library for actual codec work.
 *
 * Binary reference: pnsovr.dll v34.4 (Echo VR)
 */

#define _CRT_SECURE_NO_WARNINGS  // Disable deprecation warnings

#include "pnsovr_voip.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

// libopus includes (external dependency)
// When PNSOVR_VOIP_STUB_MODE is defined, stub implementations are used
// instead of actual Opus linking (for Wine cross-compilation)
#ifdef PNSOVR_VOIP_STUB_MODE
// Define stub types to allow compilation without linking opus.lib
typedef void OpusEncoder;
typedef void OpusDecoder;
typedef int16_t opus_int16;
typedef int32_t opus_int32;
#define OPUS_OK 0
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE(x) 4002, (x)
#define OPUS_SET_DTX(x) 4016, (x)

// Stub function implementations for Wine builds
inline OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error) {
  if (error) *error = OPUS_OK;
  return nullptr;  // Return null to indicate stub mode
}

inline void opus_encoder_destroy(OpusEncoder* st) {
  // No-op for stub
}

inline int opus_encoder_ctl(OpusEncoder* st, int request, ...) { return OPUS_OK; }

inline int opus_encode(OpusEncoder* st, const opus_int16* pcm, int frame_size, unsigned char* data,
                       opus_int32 max_data_bytes) {
  return 0;  // Return 0 bytes encoded (empty frame)
}

inline OpusDecoder* opus_decoder_create(int Fs, int channels, int* error) {
  if (error) *error = OPUS_OK;
  return nullptr;  // Return null to indicate stub mode
}

inline void opus_decoder_destroy(OpusDecoder* st) {
  // No-op for stub
}

inline int opus_decode(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_int16* pcm, int frame_size,
                       int decode_fec) {
  // Zero out output buffer to provide silent audio
  if (pcm && frame_size > 0) {
    std::memset(pcm, 0, frame_size * sizeof(opus_int16));
  }
  return frame_size;  // Return expected frame size
}

// Type definitions for stub
typedef int16_t opus_int16;
typedef int32_t opus_int32;

#else
#include <opus.h>
#endif

/**
 * @brief Internal implementation for VoipSubsystem.
 *
 * Reference: pnsovr.dll structure layout at 0x1801b8c30+
 */
#pragma warning(push)
#pragma warning(disable : 4820 4625 5026 4626 5027)  // Suppress struct and copy constructor warnings
struct VoipSubsystem::Impl {
  // Configuration
  VoipConfig config;
  bool initialized;

  // Codec state machines
  OpusEncoder* encoder;
  OpusDecoder* decoder;
  int opus_error;

  // Call tracking
  // Reference: Call state storage at 0x1801b9180+
  std::map<uint64_t, VoipCall> active_calls;
  uint64_t next_call_id;

  // Thread safety
  mutable std::mutex calls_mutex;

  // Frame sequence tracking
  std::map<uint64_t, uint32_t> frame_sequence;

  Impl() : config{}, initialized(false), encoder(nullptr), decoder(nullptr), opus_error(OPUS_OK), next_call_id(1) {}
};
#pragma warning(pop)

VoipSubsystem::VoipSubsystem() : impl_(new Impl()) {}

VoipSubsystem::~VoipSubsystem() {
  if (impl_) {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

bool VoipSubsystem::Initialize(const VoipConfig& config) {
  if (impl_->initialized) {
    return true;
  }

#pragma warning(push)
#pragma warning(disable : 4365)  // Suppress signed/unsigned conversion warnings

  // Validate configuration
  // Reference: Validation at 0x1801ba500
  if (config.sample_rate != 16000 && config.sample_rate != 24000 && config.sample_rate != 48000) {
    return false;
  }
  if (config.bit_rate < 12000 || config.bit_rate > 64000) {
    return false;
  }
  if (config.channels != 1 && config.channels != 2) {
    return false;
  }

  impl_->config = config;

  // Create Opus encoder
  // Reference: VoipCreateEncoder at 0x1801b8c30
  impl_->encoder =
      opus_encoder_create((int)config.sample_rate, (int)config.channels, OPUS_APPLICATION_VOIP, &impl_->opus_error);
  if (!impl_->encoder || impl_->opus_error != OPUS_OK) {
    return false;
  }

  // Set encoder bitrate
  opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE((int)config.bit_rate));

  // Enable DTX if configured
  // Reference: DTX flag at call structure +0x26
  if (config.enable_dtx) {
    opus_encoder_ctl(impl_->encoder, OPUS_SET_DTX(1));
  }

  // Create Opus decoder
  // Reference: VoipCreateDecoder at 0x1801b8f60
  impl_->decoder = opus_decoder_create((int)config.sample_rate, (int)config.channels, &impl_->opus_error);
  if (!impl_->decoder || impl_->opus_error != OPUS_OK) {
    if (impl_->encoder) {
      opus_encoder_destroy(impl_->encoder);
      impl_->encoder = nullptr;
    }
    return false;
  }

  impl_->initialized = true;

#pragma warning(pop)

  return true;
}

void VoipSubsystem::Shutdown() {
  if (!impl_->initialized) {
    return;
  }

  // Reference: VoipDestroyEncoder at 0x1801b8d40
  if (impl_->encoder) {
    opus_encoder_destroy(impl_->encoder);
    impl_->encoder = nullptr;
  }

  // Reference: VoipDestroyDecoder at 0x1801b9070
  if (impl_->decoder) {
    opus_decoder_destroy(impl_->decoder);
    impl_->decoder = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->calls_mutex);
    impl_->active_calls.clear();
    impl_->frame_sequence.clear();
  }

  impl_->initialized = false;
}

uint64_t VoipSubsystem::CreateCall(uint64_t remote_user_id, uint32_t direction, uint32_t timeout_ms) {
  if (!impl_->initialized) {
    return 0;
  }

  (void)timeout_ms;  // Suppress unused parameter warning

  // Reference: Call state machine at 0x1801b9180
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  VoipCall call;
  call.call_id = impl_->next_call_id++;
  call.local_user_id = 0;  // Set by caller
  call.remote_user_id = remote_user_id;
  call.direction = direction;
  call.state = 1;  // Calling
  call.bit_rate = impl_->config.bit_rate;
  call.muted = 0;
  call.start_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();

  impl_->active_calls[call.call_id] = call;
  impl_->frame_sequence[call.call_id] = 0;

  return call.call_id;
}

bool VoipSubsystem::AnswerCall(uint64_t call_id) {
  // Reference: VoipAnswer state machine at 0x1801b9290
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  auto it = impl_->active_calls.find(call_id);
  if (it == impl_->active_calls.end()) {
    return false;
  }

  // Transition: Calling → Connected
  if (it->second.state == 1) {  // Calling
    it->second.state = 2;       // Connected
    return true;
  }

  return false;
}

void VoipSubsystem::HangupCall(uint64_t call_id) {
  // Reference: Cleanup at 0x1801b92e0
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  auto it = impl_->active_calls.find(call_id);
  if (it != impl_->active_calls.end()) {
    it->second.state = 0;  // Idle
    impl_->active_calls.erase(it);
    impl_->frame_sequence.erase(call_id);
  }
}

uint32_t VoipSubsystem::GetCallState(uint64_t call_id) const {
  // Reference: State field at call structure +0x1c
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  auto it = impl_->active_calls.find(call_id);
  if (it == impl_->active_calls.end()) {
    return 0;  // Idle/not found
  }

  return it->second.state;
}

void VoipSubsystem::MuteCall(uint64_t call_id) {
  // Reference: VoipMute at 0x1801b93a0
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  auto it = impl_->active_calls.find(call_id);
  if (it != impl_->active_calls.end()) {
    it->second.muted = 1;
  }
}

void VoipSubsystem::UnmuteCall(uint64_t call_id) {
  // Reference: VoipUnmute at 0x1801b93e0
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  auto it = impl_->active_calls.find(call_id);
  if (it != impl_->active_calls.end()) {
    it->second.muted = 0;
  }
}

bool VoipSubsystem::IsCallMuted(uint64_t call_id) const {
  std::lock_guard<std::mutex> lock(impl_->calls_mutex);

  auto it = impl_->active_calls.find(call_id);
  if (it == impl_->active_calls.end()) {
    return false;
  }

  return it->second.muted != 0;
}

void VoipSubsystem::SetCallBitRate(uint64_t call_id, uint32_t bit_rate) {
  // Reference: VoipSetBitRate at 0x1801b9420
  // Valid range: 12000-64000 bps
  if (bit_rate < 12000 || bit_rate > 64000) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->calls_mutex);

    auto it = impl_->active_calls.find(call_id);
    if (it != impl_->active_calls.end()) {
      it->second.bit_rate = bit_rate;
    }
  }

  // Update encoder bitrate
  // Reference: Encoder configuration at 0x1801ba500+
  if (impl_->encoder) {
    opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(bit_rate));
  }
}

bool VoipSubsystem::IsVoipAvailable() const {
  // Reference: VoipAvailable at 0x1801b9460
  // Checks microphone availability via Windows audio APIs
  // For now, return true if initialized (real implementation would check WASAPI)
  return impl_->initialized;
}

bool VoipSubsystem::EncodeFrame(uint64_t call_id, const int16_t* pcm_data, uint32_t pcm_len, VoipFrame& frame) {
  // Reference: VoipEncode at 0x1801b8e50
  if (!impl_->encoder || !impl_->initialized) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->calls_mutex);

    auto it = impl_->active_calls.find(call_id);
    if (it == impl_->active_calls.end() || it->second.state != 2) {  // Connected
      return false;
    }

    // Setup frame metadata
    frame.session_id = call_id;
    frame.codec = 1;  // Opus
    frame.timestamp = static_cast<uint64_t>(it->second.start_time) + pcm_len;

    // Get or initialize sequence number
    // Reference: Sequence tracking at VoipFrame +0x08
    if (impl_->frame_sequence.find(call_id) == impl_->frame_sequence.end()) {
      impl_->frame_sequence[call_id] = 0;
    }
    frame.sequence_num = impl_->frame_sequence[call_id]++;
  }

  // Encode using Opus
  // Reference: Codec operation at 0x1801b8e50
#pragma warning(push)
#pragma warning(disable : 4365)  // Suppress signed/unsigned conversion warnings
  int encoded_len = opus_encode(impl_->encoder, (const opus_int16*)pcm_data, (int)pcm_len, frame.data,
                                (opus_int32)sizeof(frame.data));
#pragma warning(pop)

  if (encoded_len < 0) {
    return false;
  }

  frame.data_len = (uint32_t)encoded_len;
  return true;
}

bool VoipSubsystem::DecodeFrame(uint64_t call_id, const VoipFrame& frame, int16_t* pcm_data, uint32_t& pcm_len) {
  // Reference: VoipDecode at 0x1801b9070
  (void)call_id;  // Suppress unused parameter warning
  if (!impl_->decoder || !impl_->initialized) {
    return false;
  }

  // Validate frame
  // Reference: Frame validation at 0x1801b9070+
  if (frame.codec != 1 && frame.codec != 2) {  // 1=Opus, 2=PCM
    return false;
  }

  if (frame.codec == 2) {
    // PCM pass-through (no decoding needed)
    std::memcpy(pcm_data, frame.data, frame.data_len);
    pcm_len = frame.data_len / sizeof(int16_t);
    return true;
  }

  // Decode Opus frame
  // Reference: Codec operation at 0x1801b9070
  // Maximum output for typical frame: 48000 * 120ms / 1000 = 5760 samples
#pragma warning(push)
#pragma warning(disable : 4365)  // Suppress signed/unsigned conversion warnings
  int decoded_len =
      opus_decode(impl_->decoder, (const unsigned char*)frame.data, (opus_int32)frame.data_len, (opus_int16*)pcm_data,
                  5760,  // Max samples for 120ms frame at 48kHz
                  0);    // No FEC
#pragma warning(pop)

  if (decoded_len < 0) {
    return false;
  }

  pcm_len = (uint32_t)decoded_len;
  return true;
}
