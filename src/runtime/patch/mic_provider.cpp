/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * mic_provider — WASAPI capture behind pnsrad.dll's stubbed Mic* exports
 *
 * GH nevr-runtime#15: voice is one-way under the NEVR runtime. Root cause
 * and full trace: docs/design/2026-09-21-mic-provider-voip-fix.md.
 * pnsrad.dll's MicAvailable/MicCreate/MicDetected/MicRead all `return 0`
 * (identical-code-folded onto one address); MicDestroy/MicStart/MicStop
 * fold onto a second no-op address. VoipEncode/VoipDecode (also in
 * pnsrad.dll) are real, working Opus — only capture was ever missing.
 *
 * The game's consumer (CaptureAndEncodeLocalVoice, echovr.exe 0x140d7bd90)
 * calls MicAvailable() for a count, MicRead() for up to that many samples,
 * then slices whatever it got into VoipPacketSize()-sized (960-sample,
 * 20ms) chunks for VoipEncode, carrying any remainder to the next tick.
 * So this file only has to report samples-available and samples-actually-
 * copied honestly — it does not need to deliver fixed-size reads.
 *
 * Not reached by hooking pnsrad's own code: see mic_provider.h and the
 * design doc for why (identical-code-folding means one hooked address
 * cannot distinguish which export name the game meant to call). Instead
 * CSysDLL_GetSymbolHook (lifecycle/initialize.cpp) resolves these seven
 * names, by pointer, directly to the functions in this file.
 * ====================================================================== */

#include "runtime/patch/mic_provider.h"

#include "core/logging.h"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>

#include <atomic>
#include <cstring>
#include <mutex>

// The precompiled header already pulled in <mmdeviceapi.h>/<audioclient.h>
// (via core/pch.h -> windows.h) without INITGUID, so their DEFINE_GUID
// invocations only produced `extern` declarations, never a definition — a
// #define INITGUID here would arrive too late (include guards). mingw-w64's
// libuuid.a does not carry these WASAPI-era GUIDs either (measured: -luuid is
// already on the link line and the reference is still unresolved). Defining
// them directly, with the same extern "C" linkage the headers declare, is the
// standard portable fix. Values are Microsoft's own published constants
// (mmdeviceapi.h / audioclient.h), not invented.
extern "C" const CLSID CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
extern "C" const IID IID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
extern "C" const IID IID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
extern "C" const IID IID_IAudioCaptureClient = {
    0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

namespace {

// Matches pnsrad.dll's own (real, unmodified) MicSampleRate/MicBufferSize —
// nothing downstream needs to change to accommodate these numbers.
constexpr uint32_t kTargetSampleRate = 48000;
constexpr uint32_t kRingCapacitySamples = 24000;  // ~500ms at 48kHz mono

// --- Ring buffer -----------------------------------------------------------
// One writer (the capture thread), one reader (MicRead, called from the
// game's own thread). A mutex is enough: MicRead runs a handful of times per
// game tick, nowhere near a hot path that would need lock-free structures.
std::mutex g_ringMutex;
int16_t g_ring[kRingCapacitySamples];
uint32_t g_ringHead = 0;   // next write position
uint32_t g_ringCount = 0;  // samples currently buffered
bool g_ringOverflowLogged = false;

void RingReset() {
  std::lock_guard<std::mutex> lock(g_ringMutex);
  g_ringHead = 0;
  g_ringCount = 0;
  g_ringOverflowLogged = false;
}

void RingPush(const int16_t* samples, uint32_t count) {
  std::lock_guard<std::mutex> lock(g_ringMutex);
  for (uint32_t i = 0; i < count; i++) {
    g_ring[g_ringHead] = samples[i];
    g_ringHead = (g_ringHead + 1) % kRingCapacitySamples;
    if (g_ringCount < kRingCapacitySamples) {
      g_ringCount++;
    } else if (!g_ringOverflowLogged) {
      // The game isn't draining fast enough (or never started). Log once,
      // not every overflowing sample — this runs on the capture thread
      // every ~10ms.
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MIC] ring buffer full — game is not draining MicRead; oldest "
          "audio is being dropped");
      g_ringOverflowLogged = true;
    }
  }
}

uint32_t RingAvailable() {
  std::lock_guard<std::mutex> lock(g_ringMutex);
  return g_ringCount;
}

uint32_t RingPop(int16_t* out, uint32_t maxCount) {
  std::lock_guard<std::mutex> lock(g_ringMutex);
  uint32_t n = (maxCount < g_ringCount) ? maxCount : g_ringCount;
  uint32_t tail = (g_ringHead + kRingCapacitySamples - g_ringCount) % kRingCapacitySamples;
  for (uint32_t i = 0; i < n; i++) {
    out[i] = g_ring[(tail + i) % kRingCapacitySamples];
  }
  g_ringCount -= n;
  return n;
}

// --- WASAPI state ------------------------------------------------------
// Concurrency note: these are touched only by MicCreate/MicStart/MicStop/
// MicDestroy, which the game calls sequentially from what is, by every
// observed call site, one thread (the same thread that owns the provider).
// No lock here — only the ring buffer above is genuinely cross-thread
// (capture thread writes, game thread reads via MicRead).
IMMDeviceEnumerator* g_enumerator = nullptr;
IMMDevice* g_device = nullptr;
IAudioClient* g_audioClient = nullptr;
IAudioCaptureClient* g_captureClient = nullptr;
WAVEFORMATEX* g_mixFormat = nullptr;
HANDLE g_captureEvent = nullptr;
HANDLE g_captureThread = nullptr;
std::atomic<bool> g_running{false};
std::atomic<bool> g_created{false};
bool g_weInitializedCom = false;  // only we CoUninitialize if true
double g_resamplePhase = 0.0;     // carried across capture packets

void ReleaseWasapi() {
  if (g_captureClient) { g_captureClient->Release(); g_captureClient = nullptr; }
  if (g_audioClient) { g_audioClient->Release(); g_audioClient = nullptr; }
  if (g_device) { g_device->Release(); g_device = nullptr; }
  if (g_enumerator) { g_enumerator->Release(); g_enumerator = nullptr; }
  if (g_mixFormat) { CoTaskMemFree(g_mixFormat); g_mixFormat = nullptr; }
  if (g_captureEvent) { CloseHandle(g_captureEvent); g_captureEvent = nullptr; }
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT's Data1 (0x00000003...) — compared
// directly rather than pulling in ksmedia.h for one well-known GUID.
constexpr unsigned long kSubtypeIeeeFloatData1 = 0x00000003ul;

// Converts one WASAPI capture packet (native rate/channels/format) to mono
// int16 at kTargetSampleRate and appends it to the ring buffer. WASAPI's own
// contract is that shared-mode mix format is always PCM16 or IEEE float
// (optionally wrapped in WAVEFORMATEXTENSIBLE) — both are handled; anything
// else is treated as silence rather than misinterpreted as audio.
void ConvertAndPush(const BYTE* data, UINT32 frameCount, const WAVEFORMATEX* fmt) {
  if (frameCount == 0) return;

  uint16_t channels = fmt->nChannels;
  uint32_t srcRate = fmt->nSamplesPerSec;
  uint16_t bitsPerSample = fmt->wBitsPerSample;
  bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    isFloat = (ext->SubFormat.Data1 == kSubtypeIeeeFloatData1);
  }
  if (channels == 0) return;

  // WASAPI shared-mode packets are typically a few hundred to a few
  // thousand frames; bounding here avoids a heap allocation on the capture
  // hot path. Frames beyond this are dropped (never observed in practice on
  // a 10ms buffer at any common device rate).
  constexpr uint32_t kMaxFrames = 8192;
  if (frameCount > kMaxFrames) frameCount = kMaxFrames;

  float monoFloat[kMaxFrames];
  for (UINT32 f = 0; f < frameCount; f++) {
    float sum = 0.0f;
    for (uint16_t c = 0; c < channels; c++) {
      size_t byteOffset = (static_cast<size_t>(f) * channels + c);
      if (isFloat) {
        const float* sample = reinterpret_cast<const float*>(data + byteOffset * sizeof(float));
        sum += *sample;
      } else if (bitsPerSample == 16) {
        const int16_t* sample = reinterpret_cast<const int16_t*>(data + byteOffset * sizeof(int16_t));
        sum += static_cast<float>(*sample) / 32768.0f;
      }
      // Other bit depths (24/32-bit int) are not expected in shared-mode
      // WASAPI mix format and are treated as silence rather than guessed.
    }
    monoFloat[f] = sum / static_cast<float>(channels);
  }

  // Linear-interpolation resample to kTargetSampleRate, carrying fractional
  // phase across packets so boundaries don't click. Sufficient for voice —
  // Opus at 24kbps is the actual fidelity ceiling downstream — not intended
  // as a general-purpose resampler.
  constexpr uint32_t kMaxOut = kMaxFrames * 2 + 4;  // covers up to ~2x upsampling
  int16_t out[kMaxOut];
  uint32_t outCount = 0;
  double ratio = static_cast<double>(srcRate) / static_cast<double>(kTargetSampleRate);
  double pos = g_resamplePhase;
  while (pos < static_cast<double>(frameCount) - 1.0 && outCount < kMaxOut) {
    uint32_t i0 = static_cast<uint32_t>(pos);
    double frac = pos - static_cast<double>(i0);
    float s0 = monoFloat[i0];
    float s1 = monoFloat[i0 + 1];
    float s = static_cast<float>(s0 + (s1 - s0) * frac);
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    out[outCount++] = static_cast<int16_t>(s * 32767.0f);
    pos += ratio;
  }
  g_resamplePhase = pos - static_cast<double>(frameCount);
  if (g_resamplePhase < 0.0) g_resamplePhase = 0.0;

  RingPush(out, outCount);
}

DWORD WINAPI CaptureThreadProc(LPVOID) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool comInitializedHere = SUCCEEDED(hr) && hr != S_FALSE;

  Log(EchoVR::LogLevel::Debug, "[NEVR.MIC] capture thread started");

  while (g_running.load(std::memory_order_relaxed)) {
    DWORD wait = WaitForSingleObject(g_captureEvent, 200);
    if (wait != WAIT_OBJECT_0) continue;  // timeout: re-check g_running and loop

    UINT32 packetLength = 0;
    HRESULT hrNext = g_captureClient->GetNextPacketSize(&packetLength);
    while (SUCCEEDED(hrNext) && packetLength != 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      HRESULT hrBuf = g_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hrBuf)) break;

      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0) {
        ConvertAndPush(data, frames, g_mixFormat);
      }
      g_captureClient->ReleaseBuffer(frames);
      hrNext = g_captureClient->GetNextPacketSize(&packetLength);
    }
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.MIC] capture thread exiting");
  if (comInitializedHere) CoUninitialize();
  return 0;
}

}  // namespace

uint64_t MicProvider::MicAvailable() {
  return static_cast<uint64_t>(RingAvailable());
}

uint64_t MicProvider::MicCreate() {
  if (g_created.load()) return 0;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  g_weInitializedCom = SUCCEEDED(hr) && hr != S_FALSE;
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CoInitializeEx failed: 0x%lx", (unsigned long)hr);
    return 1;
  }

  hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                         reinterpret_cast<void**>(&g_enumerator));
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CoCreateInstance(MMDeviceEnumerator) failed: 0x%lx",
        (unsigned long)hr);
    return 1;
  }

  hr = g_enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &g_device);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MIC] no default capture device (0x%lx) — mic will report unavailable",
        (unsigned long)hr);
    ReleaseWasapi();
    return 1;
  }

  hr = g_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&g_audioClient));
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] IAudioClient activation failed: 0x%lx", (unsigned long)hr);
    ReleaseWasapi();
    return 1;
  }

  hr = g_audioClient->GetMixFormat(&g_mixFormat);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] GetMixFormat failed: 0x%lx", (unsigned long)hr);
    ReleaseWasapi();
    return 1;
  }

  // 10ms shared-mode buffer — small relative to our 500ms ring; event-driven,
  // not polled (CPP-MINGW-ADDENDUM: "No busy loops. Use waits on events").
  REFERENCE_TIME bufferDuration = 10 * 10000;  // 100ns units == 10ms
  hr = g_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  bufferDuration, 0, g_mixFormat, nullptr);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] IAudioClient::Initialize failed: 0x%lx", (unsigned long)hr);
    ReleaseWasapi();
    return 1;
  }

  g_captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_captureEvent) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CreateEvent failed: %lu", (unsigned long)GetLastError());
    ReleaseWasapi();
    return 1;
  }
  hr = g_audioClient->SetEventHandle(g_captureEvent);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] SetEventHandle failed: 0x%lx", (unsigned long)hr);
    ReleaseWasapi();
    return 1;
  }

  hr = g_audioClient->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(&g_captureClient));
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] GetService(IAudioCaptureClient) failed: 0x%lx",
        (unsigned long)hr);
    ReleaseWasapi();
    return 1;
  }

  Log(EchoVR::LogLevel::Info,
      "[NEVR.MIC] capture device ready: %u Hz, %u ch, %u-bit, tag=%u -> resampling to %u Hz mono int16",
      (unsigned)g_mixFormat->nSamplesPerSec, (unsigned)g_mixFormat->nChannels,
      (unsigned)g_mixFormat->wBitsPerSample, (unsigned)g_mixFormat->wFormatTag,
      (unsigned)kTargetSampleRate);

  g_created.store(true);
  return 0;
}

uint64_t MicProvider::MicDetected() {
  // Cheap presence probe: does not require MicCreate to have run — the game
  // may check MicDetected before deciding whether to create a provider.
  if (g_created.load()) return 1;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool weInit = SUCCEEDED(hr) && hr != S_FALSE;

  uint64_t detected = 0;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator)))) {
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) {
      detected = 1;
      device->Release();
    }
    enumerator->Release();
  }
  if (weInit) CoUninitialize();
  return detected;
}

uint64_t MicProvider::MicRead(void* buffer, uint64_t sampleCount) {
  if (!buffer || sampleCount == 0) return 0;
  uint32_t n = (sampleCount > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(sampleCount);
  return RingPop(reinterpret_cast<int16_t*>(buffer), n);
}

void MicProvider::MicStart() {
  if (!g_created.load() || g_running.load()) return;
  HRESULT hr = g_audioClient->Start();
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] IAudioClient::Start failed: 0x%lx", (unsigned long)hr);
    return;
  }
  g_running.store(true);
  g_captureThread = CreateThread(nullptr, 0, CaptureThreadProc, nullptr, 0, nullptr);
  Log(EchoVR::LogLevel::Info, "[NEVR.MIC] capture started");
}

void MicProvider::MicStop() {
  if (!g_running.load()) return;
  g_running.store(false);
  if (g_captureThread) {
    WaitForSingleObject(g_captureThread, 2000);
    CloseHandle(g_captureThread);
    g_captureThread = nullptr;
  }
  if (g_audioClient) g_audioClient->Stop();
  Log(EchoVR::LogLevel::Info, "[NEVR.MIC] capture stopped");
}

void MicProvider::MicDestroy() {
  MicProvider::MicStop();
  ReleaseWasapi();
  RingReset();
  if (g_weInitializedCom) {
    CoUninitialize();
    g_weInitializedCom = false;
  }
  g_created.store(false);
  Log(EchoVR::LogLevel::Info, "[NEVR.MIC] destroyed");
}

#else  // !_WIN32

uint64_t MicProvider::MicAvailable() { return 0; }
uint64_t MicProvider::MicCreate() { return 1; }
uint64_t MicProvider::MicDetected() { return 0; }
uint64_t MicProvider::MicRead(void*, uint64_t) { return 0; }
void MicProvider::MicStart() {}
void MicProvider::MicStop() {}
void MicProvider::MicDestroy() {}

#endif  // _WIN32
