#pragma once

/* @module: pnsrad.dll */
/* @purpose: VoIP integration — Opus codec wrappers */

#include <cstdint>

// VoIP functions are now declared in Plugin/RadPluginAPI.h
// This header is kept for backward compatibility with existing includes.

// Internal Opus wrapper functions (not exported)

/* @addr: 0x1801d4990 (pnsrad.dll) */ /* @confidence: H */
// opus_encoder_create wrapper
void* opus_encoder_create_wrapper(int samplerate, int channels, int application, int* error);

/* @addr: 0x1801d7150 (pnsrad.dll) */ /* @confidence: H */
// opus_decoder_create wrapper
void* opus_decoder_create_wrapper(int samplerate, int channels, int* error);

/* @addr: 0x1801d5270 (pnsrad.dll) */ /* @confidence: H */
// opus_destroy wrapper (shared encoder/decoder)
void opus_destroy_wrapper(void* state);

/* @addr: 0x1801d4ac0 (pnsrad.dll) */ /* @confidence: H */
// opus_encoder_ctl wrapper
int opus_encoder_ctl_wrapper(void* encoder, int request, ...);

// VoIP shutdown helper
void voip_shutdown();
