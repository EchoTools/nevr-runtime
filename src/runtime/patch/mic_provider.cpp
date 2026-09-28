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
#include "core/mic_dsp.h"
#include "core/mic_capture_drain.h"
#include "core/mic_lifecycle.h"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>

#include <atomic>

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
// The pure ring/resample logic lives in core/mic_dsp.{h,cpp} (no windows.h),
// unit-tested directly in tests/test_mic_dsp.cpp. This file only adapts real
// WASAPI packets into it.
MicRingBuffer g_ring(kRingCapacitySamples);
bool g_ringOverflowLogged = false;

// --- WASAPI state ------------------------------------------------------
// The lifecycle controller serializes public create/start/stop/destroy calls,
// releases this state only on its recorded owner thread, and retains it until
// a worker join is confirmed. The capture worker owns packet reads while live;
// only the ring buffer is shared with the game thread.
IMMDeviceEnumerator* g_enumerator = nullptr;
IMMDevice* g_device = nullptr;
IAudioClient* g_audioClient = nullptr;
IAudioCaptureClient* g_captureClient = nullptr;
WAVEFORMATEX* g_mixFormat = nullptr;
HANDLE g_captureEvent = nullptr;
HANDLE g_captureThread = nullptr;
HANDLE g_workerStartupEvent = nullptr;
std::atomic<bool> g_running{false};
std::atomic<bool> g_workerSetupSucceeded{false};
bool g_ownerMustUninitializeCom = false;
MicCapturePacketAdapter g_captureAdapter;
MicCaptureLifecycle g_lifecycle;

void ReleaseWasapi() {
  if (g_captureClient) { g_captureClient->Release(); g_captureClient = nullptr; }
  if (g_audioClient) { g_audioClient->Release(); g_audioClient = nullptr; }
  if (g_device) { g_device->Release(); g_device = nullptr; }
  if (g_enumerator) { g_enumerator->Release(); g_enumerator = nullptr; }
  if (g_mixFormat) { CoTaskMemFree(g_mixFormat); g_mixFormat = nullptr; }
  if (g_captureEvent) { CloseHandle(g_captureEvent); g_captureEvent = nullptr; }
  if (g_workerStartupEvent) { CloseHandle(g_workerStartupEvent); g_workerStartupEvent = nullptr; }
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT's Data1 (0x00000003...) — compared
// directly rather than pulling in ksmedia.h for one well-known GUID.
constexpr unsigned long kSubtypeIeeeFloatData1 = 0x00000003ul;

// Converts a single WASAPI packet. The adapter retains only decoded sample
// state, never WASAPI's packet pointer, so ReleaseBuffer can run exactly once
// before this capture iteration proceeds.
void ConvertAndPush(const BYTE* data, UINT32 frameCount, DWORD flags, const WAVEFORMATEX* fmt) {
  if (fmt == nullptr || frameCount == 0) return;
  bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    if (fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
      Log(EchoVR::LogLevel::Error, "[NEVR.MIC] extensible mix format is truncated (cbSize=%u)",
          static_cast<unsigned>(fmt->cbSize));
      return;
    }
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    isFloat = (ext->SubFormat.Data1 == kSubtypeIeeeFloatData1);
  }
  const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
  const size_t bytes = silent ? 0 : static_cast<size_t>(frameCount) * fmt->nBlockAlign;
  const MicCapturePacketResult result = g_captureAdapter.Process(
      data, frameCount, bytes, fmt->nChannels, fmt->nBlockAlign,
      fmt->nSamplesPerSec, kTargetSampleRate, isFloat, fmt->wBitsPerSample,
      silent, g_ring);
  if (result.status != MicCapturePacketStatus::Complete) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.MIC] capture packet rejected (status=%u consumed=%u/%u frames)",
        static_cast<unsigned>(result.status), result.consumedFrames, frameCount);
  }
  if (result.ringOverflow && !g_ringOverflowLogged) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MIC] ring buffer full — game is not draining MicRead; oldest audio is being dropped");
    g_ringOverflowLogged = true;
  }
}

bool CaptureCancelled(void*) { return !g_running.load(std::memory_order_acquire); }

bool CaptureNextPacket(void*, uint32_t* frames) {
  UINT32 packetFrames = 0;
  const HRESULT hr = g_captureClient->GetNextPacketSize(&packetFrames);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] GetNextPacketSize failed: 0x%lx", static_cast<unsigned long>(hr));
    return false;
  }
  *frames = packetFrames;
  return true;
}

bool CaptureAcquirePacket(void*, const void** data, uint32_t* frames, uint32_t* flags) {
  BYTE* packet = nullptr;
  UINT32 packetFrames = 0;
  DWORD packetFlags = 0;
  const HRESULT hr = g_captureClient->GetBuffer(&packet, &packetFrames, &packetFlags, nullptr, nullptr);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] GetBuffer failed: 0x%lx", static_cast<unsigned long>(hr));
    return false;
  }
  *data = packet;
  *frames = packetFrames;
  *flags = packetFlags;
  return true;
}

void CaptureProcessPacket(void*, const void* data, uint32_t frames, uint32_t flags) {
  if (frames > 0) ConvertAndPush(static_cast<const BYTE*>(data), frames, flags, g_mixFormat);
}

bool CaptureReleasePacket(void*, uint32_t frames) {
  const HRESULT hr = g_captureClient->ReleaseBuffer(frames);
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] ReleaseBuffer failed: 0x%lx",
        static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}

const MicCaptureDrainOperations kCaptureDrainOperations = {
    nullptr, CaptureCancelled, CaptureNextPacket, CaptureAcquirePacket,
    CaptureProcessPacket, CaptureReleasePacket};

DWORD WINAPI CaptureThreadProc(LPVOID) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    g_running.store(false, std::memory_order_release);
    g_workerSetupSucceeded.store(false, std::memory_order_release);
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker COM initialization failed: 0x%lx",
        static_cast<unsigned long>(hr));
    if (g_workerStartupEvent != nullptr && !SetEvent(g_workerStartupEvent)) {
      Log(EchoVR::LogLevel::Error, "[NEVR.MIC] signaling worker setup failure failed: %lu",
          static_cast<unsigned long>(GetLastError()));
    }
    return 1;
  }
  const bool comMustUninitialize = MicComInitializationRequiresUninitialize(static_cast<int32_t>(hr));
  g_workerSetupSucceeded.store(true, std::memory_order_release);
  if (g_workerStartupEvent == nullptr || !SetEvent(g_workerStartupEvent)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] signaling worker readiness failed: %lu",
        static_cast<unsigned long>(GetLastError()));
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.MIC] capture thread started");

  while (g_running.load(std::memory_order_relaxed)) {
    DWORD wait = WaitForSingleObject(g_captureEvent, 200);
    if (wait == WAIT_TIMEOUT) continue;
    if (wait != WAIT_OBJECT_0) {
      Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture event wait failed: %lu (error %lu)",
          static_cast<unsigned long>(wait), static_cast<unsigned long>(GetLastError()));
      g_running.store(false, std::memory_order_release);
      break;
    }

    const MicCaptureDrainStatus status = DrainMicCapturePackets(kCaptureDrainOperations);
    if (status != MicCaptureDrainStatus::Complete && status != MicCaptureDrainStatus::Cancelled) {
      if (status == MicCaptureDrainStatus::BufferProcessFailed) {
        Log(EchoVR::LogLevel::Error, "[NEVR.MIC] processing capture packet raised std::exception");
      }
      g_running.store(false, std::memory_order_release);
    }
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.MIC] capture thread exiting");
  if (comMustUninitialize) CoUninitialize();
  return 0;
}

}  // namespace

namespace {

bool CreateWasapiResources(void*) {
  const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (initResult == RPC_E_CHANGED_MODE) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.MIC] MicCreate rejected an existing non-MTA COM apartment; cross-apartment capture is unverified");
    return false;
  }
  g_ownerMustUninitializeCom = MicComInitializationRequiresUninitialize(static_cast<int32_t>(initResult));
  if (FAILED(initResult)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CoInitializeEx failed: 0x%lx",
        static_cast<unsigned long>(initResult));
    return false;
  }
  const auto fail = [](const char* operation, HRESULT failure, EchoVR::LogLevel level) {
    Log(level, "[NEVR.MIC] %s failed: 0x%lx", operation, static_cast<unsigned long>(failure));
    ReleaseWasapi();
    if (g_ownerMustUninitializeCom) {
      CoUninitialize();
      g_ownerMustUninitializeCom = false;
    }
    return false;
  };

  HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                reinterpret_cast<void**>(&g_enumerator));
  if (FAILED(hr)) return fail("CoCreateInstance(MMDeviceEnumerator)", hr, EchoVR::LogLevel::Error);
  hr = g_enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &g_device);
  if (FAILED(hr)) return fail("GetDefaultAudioEndpoint", hr, EchoVR::LogLevel::Warning);
  hr = g_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&g_audioClient));
  if (FAILED(hr)) return fail("IAudioClient activation", hr, EchoVR::LogLevel::Error);
  hr = g_audioClient->GetMixFormat(&g_mixFormat);
  if (FAILED(hr)) return fail("GetMixFormat", hr, EchoVR::LogLevel::Error);

  constexpr REFERENCE_TIME kBufferDuration = 10 * 10000;
  hr = g_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 kBufferDuration, 0, g_mixFormat, nullptr);
  if (FAILED(hr)) return fail("IAudioClient::Initialize", hr, EchoVR::LogLevel::Error);
  g_captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (g_captureEvent == nullptr) {
    const DWORD error = GetLastError();
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CreateEvent(capture) failed: %lu",
        static_cast<unsigned long>(error));
    ReleaseWasapi();
    if (g_ownerMustUninitializeCom) {
      CoUninitialize();
      g_ownerMustUninitializeCom = false;
    }
    return false;
  }
  hr = g_audioClient->SetEventHandle(g_captureEvent);
  if (FAILED(hr)) return fail("SetEventHandle", hr, EchoVR::LogLevel::Error);
  g_workerStartupEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_workerStartupEvent == nullptr) {
    const DWORD error = GetLastError();
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CreateEvent(worker startup) failed: %lu",
        static_cast<unsigned long>(error));
    ReleaseWasapi();
    if (g_ownerMustUninitializeCom) {
      CoUninitialize();
      g_ownerMustUninitializeCom = false;
    }
    return false;
  }
  hr = g_audioClient->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(&g_captureClient));
  if (FAILED(hr)) return fail("GetService(IAudioCaptureClient)", hr, EchoVR::LogLevel::Error);

  Log(EchoVR::LogLevel::Info,
      "[NEVR.MIC] capture device ready: %u Hz, %u ch, %u-bit, tag=%u -> resampling to %u Hz mono int16",
      static_cast<unsigned>(g_mixFormat->nSamplesPerSec), static_cast<unsigned>(g_mixFormat->nChannels),
      static_cast<unsigned>(g_mixFormat->wBitsPerSample), static_cast<unsigned>(g_mixFormat->wFormatTag),
      static_cast<unsigned>(kTargetSampleRate));
  return true;
}

bool StartAudio(void*) {
  if (g_audioClient == nullptr) return false;
  const HRESULT hr = g_audioClient->Start();
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] IAudioClient::Start failed: 0x%lx",
        static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}

MicWorkerCreateResult CreateCaptureWorker(void*) {
  if (g_workerStartupEvent == nullptr || !ResetEvent(g_workerStartupEvent)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] ResetEvent(worker startup) failed: %lu",
        static_cast<unsigned long>(GetLastError()));
    g_running.store(false, std::memory_order_release);
    return MicWorkerCreateResult::FailedWithoutWorker;
  }
  g_workerSetupSucceeded.store(false, std::memory_order_release);
  g_running.store(true, std::memory_order_release);
  g_captureThread = CreateThread(nullptr, 0, CaptureThreadProc, nullptr, 0, nullptr);
  if (g_captureThread == nullptr) {
    const DWORD error = GetLastError();
    g_running.store(false, std::memory_order_release);
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] CreateThread failed: %lu", static_cast<unsigned long>(error));
    return MicWorkerCreateResult::FailedWithoutWorker;
  }

  const DWORD wait = WaitForSingleObject(g_workerStartupEvent, 2000);
  if (wait == WAIT_OBJECT_0 && g_workerSetupSucceeded.load(std::memory_order_acquire)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MIC] capture started");
    return MicWorkerCreateResult::Started;
  }
  if (wait == WAIT_TIMEOUT) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker setup timed out; cancelling and joining worker");
  } else if (wait == WAIT_FAILED) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker readiness wait failed: %lu",
        static_cast<unsigned long>(GetLastError()));
  } else {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker reported setup failure");
  }
  return MicWorkerCreateResult::FailedWithWorker;
}

bool RequestCaptureStop(void*) {
  g_running.store(false, std::memory_order_release);
  if (g_captureEvent == nullptr || !SetEvent(g_captureEvent)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture cancellation wake failed: %lu",
        static_cast<unsigned long>(GetLastError()));
    return false;
  }
  return true;
}

MicWorkerWaitResult WaitCaptureWorker(void*, uint32_t timeoutMs) {
  if (g_captureThread == nullptr) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker handle is missing during join");
    return MicWorkerWaitResult::Failed;
  }
  const DWORD result = WaitForSingleObject(g_captureThread, timeoutMs);
  if (result == WAIT_OBJECT_0) return MicWorkerWaitResult::Signaled;
  if (result == WAIT_TIMEOUT) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker join timed out after %u ms", timeoutMs);
    return MicWorkerWaitResult::Timeout;
  }
  Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture worker join failed: wait=%lu error=%lu",
      static_cast<unsigned long>(result), static_cast<unsigned long>(GetLastError()));
  return MicWorkerWaitResult::Failed;
}

void CloseCaptureWorker(void*) {
  if (g_captureThread != nullptr) {
    CloseHandle(g_captureThread);
    g_captureThread = nullptr;
  }
}

bool StopAudio(void*) {
  if (g_audioClient == nullptr) return true;
  const HRESULT hr = g_audioClient->Stop();
  if (FAILED(hr)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] IAudioClient::Stop failed: 0x%lx",
        static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}

void ReleaseWasapiResources(void*) {
  ReleaseWasapi();
  if (g_ownerMustUninitializeCom) {
    CoUninitialize();
    g_ownerMustUninitializeCom = false;
  }
}

void ResetCaptureStream(void*) {
  g_captureAdapter.Reset();
  g_ring.Reset();
  g_ringOverflowLogged = false;
}

const MicLifecycleOperations kMicLifecycleOperations = {
    nullptr, CreateWasapiResources, StartAudio, CreateCaptureWorker, RequestCaptureStop,
    WaitCaptureWorker, CloseCaptureWorker, StopAudio, ReleaseWasapiResources, ResetCaptureStream};

}  // namespace

uint64_t MicProvider::MicAvailable() {
  return static_cast<uint64_t>(g_ring.Available());
}

uint64_t MicProvider::MicCreate() {
  const DWORD ownerThread = GetCurrentThreadId();
  if (g_lifecycle.IsCreated() && ownerThread != g_lifecycle.OwnerThread()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] MicCreate rejected on non-owner thread");
    return 1;
  }
  if (!g_lifecycle.Create(ownerThread, kMicLifecycleOperations)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] capture provider creation failed or requires Destroy retry");
    return 1;
  }
  return 0;
}

uint64_t MicProvider::MicDetected() {
  if (g_lifecycle.IsCreated()) return 1;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] presence probe COM initialization failed: 0x%lx",
        static_cast<unsigned long>(hr));
    return 0;
  }
  const bool mustUninitialize = MicComInitializationRequiresUninitialize(static_cast<int32_t>(hr));

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
  if (mustUninitialize) CoUninitialize();
  return detected;
}

uint64_t MicProvider::MicRead(void* buffer, uint64_t sampleCount) {
  if (!buffer || sampleCount == 0) return 0;
  const uint32_t count = sampleCount > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sampleCount);
  return g_ring.Pop(static_cast<int16_t*>(buffer), count);
}

void MicProvider::MicStart() {
  const DWORD callerThread = GetCurrentThreadId();
  if (g_lifecycle.IsCreated() && callerThread != g_lifecycle.OwnerThread()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] MicStart rejected on non-owner thread");
    return;
  }
  if (!g_lifecycle.Start(callerThread, kMicLifecycleOperations)) {
    const MicLifecycleState state = g_lifecycle.State();
    if (state == MicLifecycleState::FaultedNoWorker) {
      Log(EchoVR::LogLevel::Error, "[NEVR.MIC] start refused while audio stop recovery is pending; call Destroy");
    } else if (state == MicLifecycleState::FaultedWorker || state == MicLifecycleState::Stopping) {
      Log(EchoVR::LogLevel::Error, "[NEVR.MIC] start refused while previous capture worker is retained; retry Stop/Destroy");
    } else if (g_lifecycle.IsCreated() && callerThread != g_lifecycle.OwnerThread()) {
      Log(EchoVR::LogLevel::Error, "[NEVR.MIC] MicStart rejected on non-owner thread");
    }
  }
}

void MicProvider::MicStop() {
  const DWORD callerThread = GetCurrentThreadId();
  if (g_lifecycle.IsCreated() && callerThread != g_lifecycle.OwnerThread()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] MicStop rejected on non-owner thread; resources retained");
    return;
  }
  if (!g_lifecycle.Stop(callerThread, kMicLifecycleOperations, 2000)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] stop incomplete (state=%u); resources retained for retry",
        static_cast<unsigned>(g_lifecycle.State()));
    return;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.MIC] capture stopped");
}

void MicProvider::MicDestroy() {
  const DWORD callerThread = GetCurrentThreadId();
  if (g_lifecycle.IsCreated() && callerThread != g_lifecycle.OwnerThread()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] MicDestroy rejected on non-owner thread; resources retained");
    return;
  }
  if (!g_lifecycle.Destroy(callerThread, kMicLifecycleOperations, 2000)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MIC] destroy incomplete (state=%u); resources retained for retry",
        static_cast<unsigned>(g_lifecycle.State()));
    return;
  }
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
