#include "VoIPManager.h"
#include "../Plugin/Globals.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>

/* @module: pnsrad.dll */

// Forward declarations for Opus sub-encoder CTL and helpers
// @0x1801dd930 — opus_subencoder_ctl: delegates CTL to SILK/CELT sub-encoders
extern void opus_subencoder_ctl(void* sub_encoder, int request, ...);
// @0x1801d5f80 — opus_compute_bitrate: computes effective bitrate from encoder state
extern int opus_compute_bitrate(void* encoder, uint32_t frame_size);
// @0x1801e5810 — opus_reset_internal: resets internal encoder state
extern void opus_reset_internal(void* ptr);
// @0x1801e1530 — opus_reconfigure: reconfigures encoder for sample rate
extern void opus_reconfigure(void* sub_encoder, uint32_t bandwidth, void* out);
// @0x1801e5cd0 — opus_compute_frame_param: computes a frame parameter from period
extern int opus_compute_frame_param(int period);
// @0x180211d10 — stack cookie check (__security_check_cookie)
extern void fcn_180211d10(void* cookie);

// Helper for encoder/decoder size queries (used in create functions)
extern void opus_encoder_get_size(uint32_t* out);
extern int opus_silk_encoder_get_size(int channels);
extern int opus_encoder_init(void* encoder, int samplerate, int channels, int application);
extern void opus_decoder_get_size(uint32_t* out);
extern int opus_silk_decoder_get_size(int channels);
extern int opus_decoder_init(void* decoder, int samplerate, int channels);

// @0x1801d4990 — opus_encoder_create
// Validates sample rate (8000/12000/16000/24000/48000), channel count (1 or 2),
// and application mode (0x800/0x801/0x803 — VOIP/AUDIO/RESTRICTED_LOWDELAY).
// Computes encoder state size via CELT/SILK sub-encoders, allocates via malloc,
// initializes via opus_encoder_init (opus_encoder_init @ 0x1801d5280). Returns pointer or null.
/* @addr: 0x1801d4990 (pnsrad.dll) */ /* @confidence: H */
void* opus_encoder_create(int samplerate, int channels, int application, int* error) {
    // Validate sample rate
    if (samplerate != 48000 && samplerate != 24000 &&
        samplerate != 16000 && samplerate != 12000 && samplerate != 8000) {
        if (error != nullptr) *error = -1;  // OPUS_BAD_ARG
        return nullptr;
    }

    // Validate channels (1 or 2)
    if (static_cast<uint32_t>(channels - 1) >= 2) {
        if (error != nullptr) *error = -1;
        return nullptr;
    }

    // Validate application (0x800=VOIP, 0x801=AUDIO, 0x803=RESTRICTED_LOWDELAY)
    if (((application - 0x800) & 0xFFFFFFFC) != 0 || application == 0x802) {
        if (error != nullptr) *error = -1;
        return nullptr;
    }

    // Compute encoder state size
    int state_size = 0;
    uint32_t celt_size_raw = 0;
    if (static_cast<uint32_t>(channels - 1) < 2) {
        opus_encoder_get_size(&celt_size_raw);
        celt_size_raw = (celt_size_raw + 7) & ~7u;
        int silk_size = opus_silk_encoder_get_size(channels);
        state_size = silk_size + celt_size_raw + 0x46E0;
    }

    // Allocate
    void* encoder = malloc(state_size);
    if (encoder == nullptr) {
        if (error != nullptr) *error = -7;  // OPUS_ALLOC_FAIL
        return nullptr;
    }

    // Initialize
    int init_result = opus_encoder_init(encoder, samplerate, channels, application);
    if (error != nullptr) *error = init_result;

    if (init_result != 0) {
        free(encoder);
        return nullptr;
    }

    return encoder;
}

// @0x1801d7150 — opus_decoder_create
// Same validation pattern as encoder. Validates sample rate and channels,
// computes decoder state size (CELT + SILK decoders + 0x58 header),
// allocates, initializes via opus_decoder_init (opus_decoder_init @ 0x1801d7250).
/* @addr: 0x1801d7150 (pnsrad.dll) */ /* @confidence: H */
void* opus_decoder_create(int samplerate, int channels, int* error) {
    // Validate sample rate
    if (samplerate != 48000 && samplerate != 24000 &&
        samplerate != 16000 && samplerate != 12000 && samplerate != 8000) {
        if (error != nullptr) *error = -1;
        return nullptr;
    }

    // Validate channels (1 or 2)
    if (static_cast<uint32_t>(channels - 1) >= 2) {
        if (error != nullptr) *error = -1;
        return nullptr;
    }

    // Compute decoder state size
    int state_size = 0;
    uint32_t celt_size_raw = 0;
    if (static_cast<uint32_t>(channels - 1) < 2) {
        opus_decoder_get_size(&celt_size_raw);
        celt_size_raw = (celt_size_raw + 7) & ~7u;
        int silk_size = opus_silk_decoder_get_size(channels);
        state_size = silk_size + celt_size_raw + 0x58;
    }

    // Allocate
    void* decoder = malloc(state_size);
    if (decoder == nullptr) {
        if (error != nullptr) *error = -7;
        return nullptr;
    }

    // Initialize
    int init_result = opus_decoder_init(decoder, samplerate, channels);
    if (error != nullptr) *error = init_result;

    if (init_result != 0) {
        free(decoder);
        return nullptr;
    }

    return decoder;
}

// @0x1801d5270 — opus_destroy (encoder or decoder)
// Simply calls free() on the Opus state. Used for both encoder and decoder.
// 5 bytes in binary — just a tail-call to _free_base.
/* @addr: 0x1801d5270 (pnsrad.dll) */ /* @confidence: H */
void opus_destroy(void* state) {
    free(state);
}

// @0x1801d4ac0 — opus_encoder_ctl
// Large switch on Opus CTL request codes. 1758 bytes in binary.
// Handles application, bitrate, bandwidth, complexity, DTX, VBR, and many more settings.
// Delegates to opus_subencoder_ctl @0x1801dd930 for SILK/CELT-specific settings.
/* @addr: 0x1801d4ac0 (pnsrad.dll) */ /* @confidence: H */
int opus_encoder_ctl(void* encoder, int request, ...) {
    // The encoder state has a self-referencing offset at *(int32_t*)encoder:
    // sub_encoder = *(int32_t*)encoder + (intptr_t)encoder
    int32_t sub_offset = *reinterpret_cast<int32_t*>(encoder);
    void* sub_encoder = reinterpret_cast<uint8_t*>(encoder) + sub_offset;

    // The variadic arg (if any) is treated as int64_t for value or pointer
    // We use va_list to extract it. In practice, all Opus CTLs take 0 or 1 arg.
    va_list ap;
    va_start(ap, request);
    int64_t arg_val = va_arg(ap, int64_t);
    void* arg_ptr = reinterpret_cast<void*>(arg_val);
    uint32_t val = static_cast<uint32_t>(arg_val);
    va_end(ap);

    uint8_t* enc = static_cast<uint8_t*>(encoder);

    if (request > 0x271F) {
        // High CTL range
        if (request == 0x2728) {
            // OPUS_SET_PHASE_INVERSION_DISABLED
            *reinterpret_cast<uint32_t*>(enc + 0xb0) = val;
            opus_subencoder_ctl(sub_encoder, 0x2728, arg_val, 0);
        } else if (request == 0x272a) {
            // OPUS_SET_IN_DTX (extended)
            *reinterpret_cast<int64_t*>(enc + 0x37b0) = arg_val;
            opus_subencoder_ctl(sub_encoder, 0x272a, arg_val, 0);
        } else if (request == 0x2afa) {
            // Frame duration (1000..1002 or -1000)
            if ((val - 1000 < 3) || (val == 0xFFFFFC18)) {
                *reinterpret_cast<uint32_t*>(enc + 0x88) = val;
            }
        } else if (request == 0x2b0a) {
            // Packet loss percentage (0..100)
            if (val + 1 < 0x66) {
                *reinterpret_cast<uint32_t*>(enc + 0x8c) = val;
            }
        } else if (request == 0x2b0b) {
            // GET packet loss percentage
            if (arg_ptr != nullptr) {
                *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x8c);
            }
        }
        return 0;
    }

    if (request == 0x271F) {
        // OPUS_GET_IN_DTX
        if (arg_ptr != nullptr) {
            opus_subencoder_ctl(sub_encoder, 0x271f, arg_val, 0);
        }
        return 0;
    }

    switch (request) {
    case 4000: {
        // OPUS_SET_APPLICATION
        if (((val - 0x800) & 0xFFFFFFFC) == 0 && val != 0x802) {
            if (*reinterpret_cast<int32_t*>(enc + 0x37ac) != 0 ||
                *reinterpret_cast<uint32_t*>(enc + 0x6c) == val) {
                *reinterpret_cast<uint32_t*>(enc + 0x6c) = val;
                *reinterpret_cast<uint32_t*>(enc + 0xc0) = val;
            }
        }
        break;
    }
    case 0xFA1: {
        // OPUS_GET_APPLICATION
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x6c);
        }
        break;
    }
    case 0xFA2: {
        // OPUS_SET_BITRATE
        if (val != 0xFFFFFC18 && val != 0xFFFFFFFF) {
            if (static_cast<int32_t>(val) < 1) break;
            if (static_cast<int32_t>(val) < 0x1F5) {
                *reinterpret_cast<uint32_t*>(enc + 0xa4) = 500;
                break;
            }
            uint32_t max_rate = *reinterpret_cast<int32_t*>(enc + 0x70) * 300000;
            if (static_cast<int32_t>(max_rate) < static_cast<int32_t>(val)) {
                val = max_rate;
            }
        }
        *reinterpret_cast<uint32_t*>(enc + 0xa4) = val;
        break;
    }
    case 0xFA3: {
        // OPUS_GET_BITRATE
        if (arg_ptr != nullptr) {
            int bitrate = opus_compute_bitrate(encoder,
                static_cast<uint32_t>(*reinterpret_cast<uint32_t*>(enc + 0x379c)));
            *reinterpret_cast<int*>(arg_ptr) = bitrate;
        }
        break;
    }
    case 0xFA4: {
        // OPUS_SET_MAX_BANDWIDTH
        if (val - 0x44d > 4) break;
        *reinterpret_cast<uint32_t*>(enc + 0x84) = val;
        goto set_bandwidth_common;
    }
    case 0xFA5: {
        // OPUS_GET_MAX_BANDWIDTH
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x84);
        }
        break;
    }
    case 0xFA6: {
        // OPUS_SET_VBR
        if (val < 2) {
            *reinterpret_cast<uint32_t*>(enc + 0x94) = val;
            *reinterpret_cast<uint32_t*>(enc + 0x3c) = 1 - val;
        }
        break;
    }
    case 0xFA7: {
        // OPUS_GET_VBR
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x94);
        }
        break;
    }
    case 0xFA8: {
        // OPUS_SET_BANDWIDTH
        if ((val - 0x44d <= 4) || val == 0xFFFFFC18) {
            *reinterpret_cast<uint32_t*>(enc + 0x80) = val;
        } else {
            break;
        }
    set_bandwidth_common:
        if (val == 0x44d) {
            *reinterpret_cast<uint32_t*>(enc + 0x14) = 8000;
        } else {
            uint32_t rate = 16000;
            if (val == 0x44e) {
                rate = 12000;
            }
            *reinterpret_cast<uint32_t*>(enc + 0x14) = rate;
        }
        break;
    }
    case 0xFA9: {
        // OPUS_GET_BANDWIDTH
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x37a0);
        }
        break;
    }
    case 0xFAA: {
        // OPUS_SET_COMPLEXITY
        if (val < 0x0b) {
            *reinterpret_cast<uint32_t*>(enc + 0x2c) = val;
            opus_subencoder_ctl(sub_encoder, 0xfaa, static_cast<int64_t>(val), 0);
        }
        break;
    }
    case 0xFAB: {
        // OPUS_GET_COMPLEXITY
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x2c);
        }
        break;
    }
    case 0xFAC: {
        // OPUS_SET_INBAND_FEC
        if (val < 2) {
            *reinterpret_cast<uint32_t*>(enc + 0x30) = val;
        }
        break;
    }
    case 0xFAD: {
        // OPUS_GET_INBAND_FEC
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x30);
        }
        break;
    }
    case 0xFAE: {
        // OPUS_SET_PACKET_LOSS_PERC
        if (val < 0x65) {
            *reinterpret_cast<uint32_t*>(enc + 0x28) = val;
            opus_subencoder_ctl(sub_encoder, 0xfae, static_cast<int64_t>(val), 0);
        }
        break;
    }
    case 0xFAF: {
        // OPUS_GET_PACKET_LOSS_PERC
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x28);
        }
        break;
    }
    case 0xFB0: {
        // OPUS_SET_DTX
        if (val < 2) {
            *reinterpret_cast<uint32_t*>(enc + 0xb8) = val;
        }
        break;
    }
    case 0xFB1: {
        // OPUS_GET_DTX
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0xb8);
        }
        break;
    }
    case 0xFB4: {
        // OPUS_SET_VBR_CONSTRAINT
        if (val < 2) {
            *reinterpret_cast<uint32_t*>(enc + 0x98) = val;
        }
        break;
    }
    case 0xFB5: {
        // OPUS_GET_VBR_CONSTRAINT
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x98);
        }
        break;
    }
    case 0xFB6: {
        // OPUS_SET_FORCE_CHANNELS
        if ((0 < static_cast<int32_t>(val) &&
             static_cast<int32_t>(val) <= *reinterpret_cast<int32_t*>(enc + 0x70)) ||
            val == 0xFFFFFC18) {
            *reinterpret_cast<uint32_t*>(enc + 0x78) = val;
        }
        break;
    }
    case 0xFB7: {
        // OPUS_GET_FORCE_CHANNELS
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x78);
        }
        break;
    }
    case 0xFB8: {
        // OPUS_SET_SIGNAL
        if (val == 0xFFFFFC18 || (val - 0xbb9 < 2)) {
            *reinterpret_cast<uint32_t*>(enc + 0x7c) = val;
        }
        break;
    }
    case 0xFB9: {
        // OPUS_GET_SIGNAL
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x7c);
        }
        break;
    }
    case 0xFBB: {
        // OPUS_GET_LOOKAHEAD
        if (arg_ptr != nullptr) {
            int32_t lookahead = *reinterpret_cast<int32_t*>(enc + 0x90) / 400;
            *reinterpret_cast<int32_t*>(arg_ptr) = lookahead;
            if (*reinterpret_cast<int32_t*>(enc + 0x6c) != 0x803) {
                *reinterpret_cast<int32_t*>(arg_ptr) =
                    *reinterpret_cast<int32_t*>(enc + 0x74) + lookahead;
            }
        }
        break;
    }
    case 0xFBC: {
        // OPUS_RESET_STATE
        int32_t sub_off = *reinterpret_cast<int32_t*>(enc + 4);
        opus_reset_internal(enc + 0xbc);

        // Zero range from +0x3770 to end of CELT state
        uint8_t* start = enc + 0x3770;
        size_t clear_size = static_cast<size_t>((enc - start) + 0x46E0);
        memset(start, 0, clear_size);

        opus_subencoder_ctl(sub_encoder, 0xfbc, static_cast<int64_t>(clear_size), 0);
        opus_reconfigure(enc + sub_off,
                      static_cast<uint32_t>(*reinterpret_cast<uint32_t*>(enc + 0xb4)), nullptr);

        *reinterpret_cast<uint32_t*>(enc + 0x3770) = *reinterpret_cast<uint32_t*>(enc + 0x70);
        *reinterpret_cast<uint16_t*>(enc + 0x3774) = 0x4000;
        *reinterpret_cast<uint32_t*>(enc + 0x377c) = 0x3f800000;  // 1.0f
        *reinterpret_cast<uint32_t*>(enc + 0x37ac) = 1;
        *reinterpret_cast<uint32_t*>(enc + 0x3790) = 0x3e9;
        *reinterpret_cast<uint32_t*>(enc + 0x37a0) = 0x451;
        int32_t frame_val = opus_compute_frame_param(0x3c);
        *reinterpret_cast<int32_t*>(enc + 0x3778) = frame_val << 8;
        break;
    }
    case 0xFBD: {
        // OPUS_GET_SAMPLE_RATE
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x90);
        }
        break;
    }
    case 0xFBF: {
        // OPUS_GET_FINAL_RANGE
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x46dc);
        }
        break;
    }
    case 0xFC4: {
        // OPUS_SET_LSB_DEPTH
        if (val - 8 < 0x11) {
            *reinterpret_cast<uint32_t*>(enc + 0xa8) = val;
        }
        break;
    }
    case 0xFC5: {
        // OPUS_GET_LSB_DEPTH
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0xa8);
        }
        break;
    }
    case 0xFC8: {
        // OPUS_SET_EXPERT_FRAME_DURATION
        if (val - 5000 < 10) {
            *reinterpret_cast<uint32_t*>(enc + 0x9c) = val;
        }
        break;
    }
    case 0xFC9: {
        // OPUS_GET_EXPERT_FRAME_DURATION
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x9c);
        }
        break;
    }
    case 0xFCA: {
        // OPUS_SET_PREDICTION_DISABLED
        if (val < 2) {
            *reinterpret_cast<uint32_t*>(enc + 0x4c) = val;
        }
        break;
    }
    case 0xFCB: {
        // OPUS_GET_PREDICTION_DISABLED
        if (arg_ptr != nullptr) {
            *reinterpret_cast<uint32_t*>(arg_ptr) = *reinterpret_cast<uint32_t*>(enc + 0x4c);
        }
        break;
    }
    case 0xFCE: {
        // Sub-encoder CTL (set) — delegate
        if (val < 2) {
            opus_subencoder_ctl(sub_encoder, 0xfce, static_cast<int64_t>(val), 0);
        }
        break;
    }
    case 0xFCF: {
        // Sub-encoder CTL (get) — delegate
        if (arg_ptr != nullptr) {
            opus_subencoder_ctl(sub_encoder, 0xfcf, arg_val, 0);
        }
        break;
    }
    case 0xFD1: {
        // OPUS_GET_IN_DTX (extended check)
        if (arg_ptr != nullptr) {
            if (*reinterpret_cast<int32_t*>(enc + 0x38) == 0 ||
                *reinterpret_cast<int32_t*>(enc + 0x3794) - 1000 > 1) {
                if (*reinterpret_cast<int32_t*>(enc + 0xb8) == 0) {
                    *reinterpret_cast<uint32_t*>(arg_ptr) = 0;
                } else {
                    *reinterpret_cast<uint32_t*>(arg_ptr) =
                        static_cast<uint32_t>(9 < *reinterpret_cast<int32_t*>(enc + 0x46d0));
                }
            } else {
                int32_t sub_off2 = *reinterpret_cast<int32_t*>(enc + 4);
                *reinterpret_cast<uint32_t*>(arg_ptr) = 1;
                int32_t num_channels = *reinterpret_cast<int32_t*>(enc + 0xc);
                for (int32_t ch = 0; ch < num_channels; ch++) {
                    int32_t* channel_ptr = reinterpret_cast<int32_t*>(
                        enc + sub_off2 + 0x17d8 + ch * 0x2768);
                    uint32_t current = *reinterpret_cast<uint32_t*>(arg_ptr);
                    if (current == 0 || *channel_ptr < 10) {
                        *reinterpret_cast<uint32_t*>(arg_ptr) = 0;
                    } else {
                        *reinterpret_cast<uint32_t*>(arg_ptr) = 1;
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }

    return 0;
}

/* SYNTHESIS — helper/wrapper code, not from binary */
void voip_shutdown() {
    // Destroy encoder
    if (g_voip_encoder != nullptr) {
        opus_destroy(g_voip_encoder);
        g_voip_encoder = nullptr;
    }
}
