/**
 * pnsovr.dll - OVR/Meta VOIP & Microphone API Headers
 *
 * Generated from Ghidra reverse engineering analysis
 * DLL Base: 0x180000000 (Windows x64)
 *
 * Use these definitions to create hooks with Detours/MinHook
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

// ============================================================================
// VOIP Audio Codec & Processing
// ============================================================================

/**
 * VOIP Encoder - Compresses PCM audio to opus/similar codec for transmission
 * Initialized via VoipCreateEncoder()
 * Used by VoipEncode() to compress microphone audio
 */
typedef void *OVR_VoipEncoder;

/**
 * VOIP Decoder - Decompresses opus packets to PCM for playback
 * One decoder per remote peer (identified by source_id)
 * Pool managed in DAT_180346858
 */
typedef void *OVR_VoipDecoder;

/**
 * Microphone device handle
 * Cached in DAT_180346840 after creation
 */
typedef void *OVR_Microphone;

// ============================================================================
// Microphone Subsystem
// ============================================================================

namespace Microphone {

/**
 * MicCreate @ 0x97390
 *
 * Initialize microphone device and cache handle in DAT_180346840
 * Idempotent - safe to call multiple times
 *
 * @return 0 on success, error code on failure
 */
extern "C" int __fastcall MicCreate();

/**
 * MicDestroy @ 0x97400
 *
 * Clean up and release microphone resources
 * Nullifies DAT_180346840
 */
extern "C" void __fastcall MicDestroy();

/**
 * MicDetected @ 0x97430
 *
 * Check if OVR platform is initialized
 *
 * @return 1 if platform ready, 0 otherwise
 */
extern "C" uint8_t __fastcall MicDetected();

/**
 * MicAvailable @ 0x97370
 *
 * Get microphone capture frame size
 *
 * @return 0x960 (2400 bytes = 50ms @ 48kHz 16-bit mono)
 */
extern "C" size_t __fastcall MicAvailable();

/**
 * MicBufferSize @ 0x97380
 *
 * Get total microphone buffer size
 *
 * @return 24000 bytes (500ms @ 48kHz 16-bit)
 */
extern "C" size_t __fastcall MicBufferSize();

/**
 * MicSampleRate @ 0x97470
 *
 * Get microphone sample rate
 *
 * @return 48000 Hz
 */
extern "C" size_t __fastcall MicSampleRate();

/**
 * MicStart @ 0x97480
 *
 * Activate microphone hardware capture
 * Must call MicCreate() first
 */
extern "C" void __fastcall MicStart();

/**
 * MicStop @ 0x97490
 *
 * Deactivate microphone hardware capture
 * Does not destroy microphone, just pauses
 */
extern "C" void __fastcall MicStop();

/**
 * MicRead @ 0x97450
 *
 * Read PCM audio samples from microphone capture buffer
 *
 * CRITICAL FUNCTION - Can be hooked to:
 * - Inject synthetic audio
 * - Silence microphone
 * - Capture microphone data for analysis
 *
 * @param buffer [out] Destination for PCM samples
 * @param size Number of bytes to read (typically MicAvailable())
 */
extern "C" void __fastcall MicRead(void *buffer, size_t size);

}  // namespace Microphone

// ============================================================================
// VOIP Audio Processing
// ============================================================================

namespace Voip {

/**
 * VoipCreateEncoder @ 0x98300
 *
 * Initialize audio encoder (opus-compatible)
 * Cached in DAT_180346850
 * Idempotent - safe to call multiple times
 *
 * SETUP FUNCTION - Called during VOIP session init
 *
 * @return 0 on success, error code on failure
 */
extern "C" int __fastcall VoipCreateEncoder();

/**
 * VoipCreateDecoder @ 0x98100
 *
 * Initialize decoder for receiving audio from a specific source
 * Each peer gets one decoder in the pool (DAT_180346858)
 *
 * Decoder pool structure (0x40 bytes per block):
 *   +0x00: source_id (8 bytes)
 *   +0x08: config1 (8 bytes)
 *   +0x10: decoder_handle (8 bytes) - OVR_VoipDecoder
 *   +0x18: output_buffer_ptr (8 bytes)
 *   +0x20-0x38: config2-4 + padding (24 bytes)
 *
 * @param source_id Identifier for audio source (peer ID)
 * @param config Pointer to configuration array
 * @return 0 on success, error code on failure
 */
extern "C" int __fastcall VoipCreateDecoder(void *source_id, void *config);

/**
 * VoipEncode @ 0x984e0
 *
 * Compress raw PCM audio to opus/compressed format
 * Used to encode microphone audio for transmission to peers
 *
 * CRITICAL FUNCTION - Main TX audio path
 * Hook this to intercept/replace outgoing audio
 *
 * Data flow: MicRead() → VoipEncode() → Network TX
 *
 * Global variables used:
 * - DAT_180346848: encoder input buffer
 * - DAT_180346850: encoder handle (OVR_VoipEncoder)
 *
 * @param context Encoder context/state
 * @param sample_count Number of PCM samples (-1 for special handling)
 * @param compressed_output [out] Buffer for compressed audio data
 * @param output_size Maximum size of compressed output buffer
 */
extern "C" void __fastcall VoipEncode(void *context, int64_t sample_count, void *compressed_output, size_t output_size);

/**
 * VoipDecode @ 0x98370
 *
 * Decompress opus/compressed audio to PCM
 * Used to decode received audio from peers for playback
 *
 * CRITICAL FUNCTION - Main RX audio path
 * Hook this to intercept/replace incoming audio
 *
 * Data flow: Network RX → VoipDecode() → PCM playback
 *
 * Decoder lookup: Searches DAT_180346858 pool for frame_data
 *
 * @param frame_data Compressed audio frame from network
 * @param output_buffer [out] Buffer for decompressed PCM samples
 * @param output_size Maximum size of output buffer
 */
extern "C" void __fastcall VoipDecode(void *frame_data, void *output_buffer, size_t output_size);

/**
 * VoipPacketSize @ 0x98570
 *
 * Get standard VOIP audio packet size
 *
 * @return 0x3C0 (960 bytes = 20ms frame @ 48kHz)
 */
extern "C" size_t __fastcall VoipPacketSize();

/**
 * VoipAvailable @ 0x980d0
 *
 * Get amount of PCM data available in decoder buffer
 *
 * @return Number of bytes available for playback
 */
extern "C" size_t __fastcall VoipAvailable();

/**
 * VoipBufferSize @ 0x980e0
 *
 * Get maximum decoder output buffer capacity
 * Used for pre-allocation during decoder setup
 *
 * @return Maximum bytes in decoder output buffer
 */
extern "C" size_t __fastcall VoipBufferSize();

/**
 * VoipCall @ 0x980f0
 *
 * Initiate outgoing VOIP call to remote peer
 *
 * CONTROL FUNCTION - Can hook to intercept call initiation
 */
extern "C" void __fastcall VoipCall();

/**
 * VoipAnswer @ 0x980c0
 *
 * Accept incoming VOIP call from remote peer
 *
 * CONTROL FUNCTION - Can hook to intercept call acceptance
 */
extern "C" void __fastcall VoipAnswer();

/**
 * VoipHangUp @ 0x98550
 *
 * Terminate active VOIP call
 */
extern "C" void __fastcall VoipHangUp();

/**
 * VoipMute @ 0x98560
 *
 * Mute local audio transmission
 */
extern "C" void __fastcall VoipMute();

/**
 * VoipUnmute @ 0x985b0
 *
 * Unmute local audio transmission
 */
extern "C" void __fastcall VoipUnmute();

/**
 * VoipPushToTalkKey @ 0x98580
 *
 * Handle push-to-talk key input
 *
 * @param key Key code
 */
extern "C" void __fastcall VoipPushToTalkKey(int key);

}  // namespace Voip

// ============================================================================
// Platform Verification
// ============================================================================

/**
 * CheckEntitlement @ 0x96f70
 *
 * Verify user's Meta/Oculus platform access rights
 * Logs "Checking OVR entitlement..." message
 */
extern "C" void __fastcall CheckEntitlement();

// ============================================================================
// Global Variable Storage (in pnsovr.dll data segment)
// ============================================================================

namespace Globals {

// All offsets relative to DLL base 0x180000000
// Raw offset = declared_offset - 0x180000000

/**
 * Microphone device handle
 * Type: OVR_Microphone*
 * Address: 0x180346840 (offset: 0x346840)
 * Size: 8 bytes
 *
 * Set by: MicCreate()
 * Cleared by: MicDestroy()
 * Used by: MicRead, MicStart, MicStop
 */
extern OVR_Microphone *g_MicrophoneHandle;  // @ 0x346840

/**
 * Encoder input buffer pointer
 * Type: void*
 * Address: 0x180346848 (offset: 0x346848)
 * Size: 8 bytes
 *
 * Used by: VoipEncode() to feed PCM samples
 */
extern void **g_EncoderInputBuffer;  // @ 0x346848

/**
 * VOIP encoder handle
 * Type: OVR_VoipEncoder*
 * Address: 0x180346850 (offset: 0x346850)
 * Size: 8 bytes
 *
 * Set by: VoipCreateEncoder()
 * Used by: VoipEncode()
 */
extern OVR_VoipEncoder *g_VoipEncoder;  // @ 0x346850

/**
 * VOIP decoder pool base array
 * Type: struct DecoderBlock[]
 * Address: 0x180346858 (offset: 0x346858)
 * Size: 8 bytes (base pointer)
 * Block size: 0x40 bytes per decoder
 *
 * Layout per block:
 *   [0x00] source_id (8)
 *   [0x08] config1 (8)
 *   [0x10] decoder_handle (8) - OVR_VoipDecoder
 *   [0x18] output_buffer (8)
 *   [0x20] config234 (24)
 *
 * Set by: VoipCreateDecoder()
 * Used by: VoipDecode() to lookup decoders by source_id
 */
extern void **g_DecoderPool;  // @ 0x346858

}  // namespace Globals

// ============================================================================
// Hook Type Definitions (for Detours/MinHook)
// ============================================================================

namespace Hooks {

// Microphone hooks
typedef int(__fastcall *MicCreateFn)(void);
typedef void(__fastcall *MicDestroyFn)(void);
typedef void(__fastcall *MicReadFn)(void *, size_t);
typedef void(__fastcall *MicStartFn)(void);
typedef void(__fastcall *MicStopFn)(void);
typedef size_t(__fastcall *MicAvailableFn)(void);
typedef size_t(__fastcall *MicBufferSizeFn)(void);
typedef size_t(__fastcall *MicSampleRateFn)(void);
typedef uint8_t(__fastcall *MicDetectedFn)(void);

// VOIP hooks (critical audio path)
typedef int(__fastcall *VoipCreateEncoderFn)(void);
typedef int(__fastcall *VoipCreateDecoderFn)(void *, void *);
typedef void(__fastcall *VoipEncodeFn)(void *, int64_t, void *, size_t);
typedef void(__fastcall *VoipDecodeFn)(void *, void *, size_t);

// VOIP control hooks
typedef void(__fastcall *VoipCallFn)(void);
typedef void(__fastcall *VoipAnswerFn)(void);
typedef void(__fastcall *VoipHangUpFn)(void);
typedef void(__fastcall *VoipMuteFn)(void);
typedef void(__fastcall *VoipUnmuteFn)(void);

// Query hooks
typedef size_t(__fastcall *VoipPacketSizeFn)(void);
typedef size_t(__fastcall *VoipAvailableFn)(void);
typedef size_t(__fastcall *VoipBufferSizeFn)(void);

}  // namespace Hooks

// ============================================================================
// Example Hook Implementation
// ============================================================================

/*
Example: Hook VoipEncode to inject synthetic audio

    namespace Hooks {
        static Hooks::VoipEncodeFn g_pfnVoipEncodeReal = nullptr;

        void __fastcall VoipEncodeHook(void *ctx, int64_t count, void *out, size_t sz) {
            // Modify audio before encoding
            // Option 1: Fill output with silence
            memset(out, 0, sz);

            // Option 2: Fill with synthetic audio
            // generate_synthetic_audio(out, sz);

            // Option 3: Call original
            // g_pfnVoipEncodeReal(ctx, count, out, sz);
        }

        void Install() {
            // Get original function address from pnsovr.dll base
            void *pnsovrBase = GetModuleHandleA("pnsovr.dll");
            g_pfnVoipEncodeReal = (Hooks::VoipEncodeFn)
                ((uintptr_t)pnsovrBase + 0x984e0);  // offset within DLL

            // Detach any existing hooks
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());

            // Attach hook
            DetourAttach(&(PVOID&)g_pfnVoipEncodeReal, VoipEncodeHook);

            if (DetourTransactionCommit() == NO_ERROR) {
                // Hook installed successfully
            }
        }
    }
*/

#endif  // PNSOVR_H
