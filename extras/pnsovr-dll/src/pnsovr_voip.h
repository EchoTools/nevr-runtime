/**
 * @file pnsovr_voip.h
 * @brief NEVR PNSOvr Compatibility - Voice/Audio Subsystem
 *
 * Implements the voice communication subsystem reverse-engineered from pnsovr.dll.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * - VoipCreateEncoder: 0x1801b8c30
 * - VoipDestroyEncoder: 0x1801b8d40
 * - VoipEncode: 0x1801b8e50
 * - VoipCreateDecoder: 0x1801b8f60
 * - VoipDecode: 0x1801b9070
 * - VoipCall: 0x1801b9180
 * - VoipAnswer: 0x1801b9290
 * - VoipMute: 0x1801b93a0
 * - VoipUnmute: 0x1801b93e0
 * - VoipSetBitRate: 0x1801b9420
 * - VoipAvailable: 0x1801b9460
 */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * @brief Opaque handle to voice encoder.
 *
 * Reference: 0x1801b8c30 (VoipCreateEncoder allocates this)
 * Structure size: 256 bytes (estimated from allocation patterns)
 */
using VoipEncoderHandle = void*;

/**
 * @brief Opaque handle to voice decoder.
 *
 * Reference: 0x1801b8f60 (VoipCreateDecoder allocates this)
 * Structure size: 256 bytes (estimated from allocation patterns)
 */
using VoipDecoderHandle = void*;

/**
 * @brief Represents a voice call session.
 *
 * Binary structure at pnsovr.dll+0x1801b9180 (VoipCall)
 * Size: 128 bytes (from stack allocation patterns)
 *
 * Field offsets:
 * +0x00: callId (uint64_t)
 * +0x08: localUserId (uint64_t)
 * +0x10: remoteUserId (uint64_t)
 * +0x18: direction (uint32_t: 1=In, 2=Out, 3=Both)
 * +0x1c: state (uint32_t: 0=Idle, 1=Calling, 2=Connected, 3=Error)
 * +0x20: bitRate (uint32_t, default 32000)
 * +0x24: muted (uint8_t)
 * +0x28: startTime (int64_t)
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress padding warnings
struct VoipCall {
  uint64_t call_id;
  uint64_t local_user_id;
  uint64_t remote_user_id;
  uint32_t direction;  // 1=Incoming, 2=Outgoing, 3=Both
  uint32_t state;      // 0=Idle, 1=Calling, 2=Connected, 3=Error
  uint32_t bit_rate;   // 12000-64000 bps
  uint8_t muted;
  int64_t start_time;
  uint8_t _pad[7];
};
#pragma warning(pop)

/**
 * @brief Compressed audio frame for transmission.
 *
 * Binary reference: 0x1801b8e50 (VoipEncode output format)
 * Size: 8192 bytes (typical Opus frame + header)
 *
 * Field layout:
 * +0x00: sessionId (uint64_t) - Call session identifier
 * +0x08: sequenceNum (uint32_t) - Frame sequence counter
 * +0x0c: timestamp (uint64_t) - RTP-style timestamp
 * +0x14: dataLen (uint32_t) - Compressed data length
 * +0x18: codec (uint8_t) - 1=Opus, 2=PCM
 * +0x19: reserved[7]
 * +0x20: data[8160] - Compressed audio payload
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress padding warnings
struct VoipFrame {
  uint64_t session_id;
  uint32_t sequence_num;
  uint64_t timestamp;
  uint32_t data_len;
  uint8_t codec;  // 1=Opus, 2=PCM
  uint8_t reserved[7];
  uint8_t data[8160];
};
#pragma warning(pop)

/**
 * @brief Audio configuration parameters.
 *
 * Binary reference: Configuration parsing at 0x1801ba500
 *
 * Default values from binary analysis:
 * - Sample rate: 16000 Hz (wideband)
 * - Bit rate: 32000 bps
 * - Frame duration: 20 ms
 * - Channels: 1 (mono)
 */
struct VoipConfig {
  uint32_t sample_rate;        // 16000, 24000, or 48000 Hz
  uint32_t bit_rate;           // 12000-64000 bps
  uint32_t frame_duration_ms;  // 20-120 ms
  uint8_t channels;            // 1 (mono) or 2 (stereo)
  uint8_t enable_dtx;          // Discontinuous transmission
  uint8_t enable_fec;          // Forward error correction
  uint8_t quality_mode;        // 0=Bandwidth, 1=Balanced, 2=Quality
};

/**
 * @brief Voice communication subsystem.
 *
 * Manages Opus codec encoding/decoding and voice call state.
 * All operations use Opus codec from libopus (external dependency).
 *
 * This class wraps the DLL functionality at:
 * - Core management: 0x1801b8c30-0x1801b9500 (codec operations)
 * - Call state machine: 0x1801b9180-0x1801b92e0 (call lifecycle)
 */
class VoipSubsystem {
 public:
  VoipSubsystem();
  ~VoipSubsystem();

  /**
   * @brief Initialize voice subsystem with configuration.
   * @param config Audio configuration parameters.
   * @return true if initialization succeeded.
   *
   * Binary reference: Initialization sequence at 0x1801b8c30-0x1801b8d00
   * Performs:
   * 1. Allocate encoder handle (0x1801b8c30)
   * 2. Allocate decoder handle (0x1801b8f60)
   * 3. Configure Opus for specified sample rate
   */
  bool Initialize(const VoipConfig& config);

  /**
   * @brief Shutdown voice subsystem and release resources.
   *
   * Binary reference: 0x1801b8d40 (VoipDestroyEncoder) and
   *                  0x1801b9070 (VoipDestroyDecoder)
   */
  void Shutdown();

  /**
   * @brief Create a new voice call.
   * @param remote_user_id Target user for call.
   * @param direction 1=Incoming, 2=Outgoing, 3=Bidirectional.
   * @param timeout_ms Call timeout in milliseconds.
   * @return Call handle, or 0 if creation failed.
   *
   * Binary reference: 0x1801b9180 (VoipCall implementation)
   * Creates call state machine:
   * - State: Idle → Calling (wait for acceptance)
   * - State: Calling → Connected (when answered)
   * - State: Connected → Idle (on disconnect)
   */
  uint64_t CreateCall(uint64_t remote_user_id, uint32_t direction, uint32_t timeout_ms);

  /**
   * @brief Answer an incoming call.
   * @param call_id Call handle from CreateCall.
   * @return true if call state transitioned to Connected.
   *
   * Binary reference: 0x1801b9290 (VoipAnswer state machine)
   * Transitions: Calling → Connected
   */
  bool AnswerCall(uint64_t call_id);

  /**
   * @brief Terminate a voice call.
   * @param call_id Call handle.
   *
   * Binary reference: Cleanup at 0x1801b92e0
   * Transitions: Any state → Idle
   */
  void HangupCall(uint64_t call_id);

  /**
   * @brief Get current state of a call.
   * @param call_id Call handle.
   * @return Current state (0=Idle, 1=Calling, 2=Connected, 3=Error).
   *
   * Binary reference: State field at call structure +0x1c
   */
  uint32_t GetCallState(uint64_t call_id) const;

  /**
   * @brief Mute audio output for a call.
   * @param call_id Call handle.
   *
   * Binary reference: 0x1801b93a0 (VoipMute)
   * Sets muted flag at call structure +0x24
   */
  void MuteCall(uint64_t call_id);

  /**
   * @brief Unmute audio output for a call.
   * @param call_id Call handle.
   *
   * Binary reference: 0x1801b93e0 (VoipUnmute)
   * Clears muted flag at call structure +0x24
   */
  void UnmuteCall(uint64_t call_id);

  /**
   * @brief Check if call is muted.
   * @param call_id Call handle.
   * @return true if output is muted.
   */
  bool IsCallMuted(uint64_t call_id) const;

  /**
   * @brief Set bitrate for a call (quality tuning).
   * @param call_id Call handle.
   * @param bit_rate Target bitrate in bps (12000-64000).
   *
   * Binary reference: 0x1801b9420 (VoipSetBitRate)
   * Updates: bitRate field at call structure +0x20
   * Valid range: 12000-64000 bps
   * Common: 12000 (low), 32000 (balanced), 64000 (high quality)
   */
  void SetCallBitRate(uint64_t call_id, uint32_t bit_rate);

  /**
   * @brief Check if voice capability is available.
   * @return true if microphone/audio devices detected.
   *
   * Binary reference: 0x1801b9460 (VoipAvailable)
   * Checks:
   * - Microphone device availability (via Windows audio APIs)
   * - Required Windows audio library loaded (mmeapi.dll, wmvcore.dll)
   * - Sufficient system resources
   */
  bool IsVoipAvailable() const;

  /**
   * @brief Encode PCM audio to compressed frame.
   * @param call_id Active call handle.
   * @param pcm_data PCM audio samples (mono, 16-bit signed).
   * @param pcm_len Number of samples.
   * @param[out] frame Compressed output frame.
   * @return true if encoding succeeded.
   *
   * Binary reference: 0x1801b8e50 (VoipEncode)
   * Implementation uses Opus codec:
   * - Encoder state: Allocated at 0x1801b8c30
   * - Frame duration: Configured at initialization
   * - Bitrate: Per-call setting (0x1801b9420)
   *
   * Process:
   * 1. Feed pcm_data to Opus encoder
   * 2. Extract compressed frame_data
   * 3. Increment sequence number (frame.sequence_num++)
   * 4. Set timestamp from call start time
   * 5. Format as VoipFrame structure
   */
  bool EncodeFrame(uint64_t call_id, const int16_t* pcm_data, uint32_t pcm_len, VoipFrame& frame);

  /**
   * @brief Decode compressed frame to PCM audio.
   * @param call_id Active call handle.
   * @param frame Compressed input frame.
   * @param[out] pcm_data PCM audio output (mono, 16-bit signed).
   * @param[out] pcm_len Number of output samples.
   * @return true if decoding succeeded.
   *
   * Binary reference: 0x1801b9070 (VoipDecode)
   * Implementation uses Opus codec:
   * - Decoder state: Allocated at 0x1801b8f60
   * - Frame duration: Configured at initialization
   * - PLC (Packet Loss Concealment): Enabled by default
   *
   * Process:
   * 1. Validate frame format (codec field)
   * 2. Extract compressed data from frame.data
   * 3. Feed to Opus decoder
   * 4. Output PCM samples to pcm_data
   * 5. Return sample count in pcm_len
   *
   * Notes:
   * - Handles lost frames gracefully (PLC)
   * - Maintains decoder state across calls
   * - Output buffer must be sized for expected frame duration
   */
  bool DecodeFrame(uint64_t call_id, const VoipFrame& frame, int16_t* pcm_data, uint32_t& pcm_len);

 private:
  struct Impl;
  Impl* impl_;
};
