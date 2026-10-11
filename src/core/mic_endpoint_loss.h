/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

// The WASAPI results that mean the capture endpoint's IAudioClient is dead for good and the default
// endpoint has to be acquired again. Free of windows.h so the classification is unit-tested; the provider
// static_asserts these values against audioclient.h (mic_provider.cpp).
//
// AUDCLNT_E_DEVICE_INVALIDATED: "The audio endpoint device has been unplugged, or the audio hardware or
// associated hardware resources have been reconfigured, disabled, removed, or otherwise made unavailable
// for use." (IAudioCaptureClient::GetBuffer, Core Audio APIs).
// AUDCLNT_E_RESOURCES_INVALIDATED: "The stream's resources have been invalidated. This error may be thrown
// for the following reasons: the stream is suspended; an Exclusive or Offload stream is disconnected; a
// packaged application that has an exclusive mode or offload stream is quiesced; a 'protected output'
// stream is closed." (same page).
inline constexpr int32_t kMicDeviceInvalidated = static_cast<int32_t>(0x88890004u);
inline constexpr int32_t kMicResourcesInvalidated = static_cast<int32_t>(0x88890026u);

inline constexpr bool MicHresultMeansEndpointLost(int32_t hr) {
  return hr == kMicDeviceInvalidated || hr == kMicResourcesInvalidated;
}
