/**
 * @file telemetry_agent.cpp
 * @brief Main telemetry agent implementation
 *
 * This file implements the public DLL interface and coordinates the
 * data source, frame processor, and telemetry client components.
 */

#include "telemetry_agent.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "data_source.h"
#include "frame_processor.h"
#include "http_poller.h"
#include "memory_poller.h"
#include "telemetry_client.h"

// Include generated protobuf headers
#include "rtapi/telemetry_v1.pb.h"

namespace {

/**
 * @class TelemetryAgentImpl
 * @brief Internal implementation of the telemetry agent
 */
class TelemetryAgentImpl {
 public:
  TelemetryAgentImpl() : m_initialized(false), m_sessionActive(false), m_pollingFrequencyHz(10), m_framesPolled(0) {}

  ~TelemetryAgentImpl() { Shutdown(); }

  bool Initialize(const TelemetryAgentConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
      return true;
    }

    m_config = config;
    m_pollingFrequencyHz = config.pollingFrequencyHz > 0 ? config.pollingFrequencyHz : 10;
    m_initialized = true;

    return true;
  }

  bool IsInitialized() const { return m_initialized; }

  bool StartSession() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized || m_sessionActive) {
      return false;
    }

    // Create data source based on configuration
    if (m_config.dataSource == TELEMETRY_SOURCE_MEMORY && m_config.gameBaseAddress != 0) {
      m_dataSource = std::make_unique<TelemetryAgent::MemoryPoller>(m_config.gameBaseAddress);
    } else {
      std::string host = m_config.gameServerHost ? m_config.gameServerHost : "127.0.0.1";
      uint16_t port = m_config.gameServerPort > 0 ? m_config.gameServerPort : 6721;
      m_dataSource = std::make_unique<TelemetryAgent::HttpPoller>(host, port);
    }

    if (!m_dataSource->Start()) {
      m_dataSource.reset();
      return false;
    }

    // Create frame processor
    m_frameProcessor = std::make_unique<TelemetryAgent::FrameProcessor>();

    // Create telemetry client
    TelemetryAgent::TelemetryClientConfig clientConfig;
    clientConfig.baseUrl = m_config.telemetryApiUrl ? m_config.telemetryApiUrl : "http://localhost:8081";
    clientConfig.userId = m_config.userId ? m_config.userId : "";
    clientConfig.nodeId = m_config.nodeId ? m_config.nodeId : "game-server";

    m_telemetryClient = std::make_unique<TelemetryAgent::TelemetryClient>(clientConfig);

    if (!m_telemetryClient->Start()) {
      m_dataSource->Stop();
      m_dataSource.reset();
      m_frameProcessor.reset();
      m_telemetryClient.reset();
      return false;
    }

    // Start polling thread
    m_sessionActive = true;
    m_framesPolled = 0;
    m_pollingThread = std::thread(&TelemetryAgentImpl::PollingLoop, this);

    return true;
  }

  bool StopSession() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);

      if (!m_sessionActive) {
        return false;
      }

      m_sessionActive = false;
    }

    // Wait for polling thread to finish
    if (m_pollingThread.joinable()) {
      m_pollingThread.join();
    }

    // Clean up components
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_telemetryClient) {
      m_telemetryClient->Stop();
      m_telemetryClient.reset();
    }

    if (m_dataSource) {
      m_dataSource->Stop();
      m_dataSource.reset();
    }

    if (m_frameProcessor) {
      m_frameProcessor->Reset();
      m_frameProcessor.reset();
    }

    return true;
  }

  bool IsSessionActive() const { return m_sessionActive; }

  bool Shutdown() {
    StopSession();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized = false;

    return true;
  }

  std::string GetSessionUUID() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_frameProcessor) {
      return m_frameProcessor->GetSessionUUID();
    }
    return "";
  }

  void GetStats(uint64_t& framesPolled, uint64_t& framesSent, uint64_t& framesFailed, uint32_t& queueDepth) const {
    framesPolled = m_framesPolled;

    if (m_telemetryClient) {
      framesSent = m_telemetryClient->GetSentCount();
      framesFailed = m_telemetryClient->GetFailedCount();
      queueDepth = static_cast<uint32_t>(m_telemetryClient->GetQueueDepth());
    } else {
      framesSent = 0;
      framesFailed = 0;
      queueDepth = 0;
    }
  }

  bool SetApiUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_sessionActive) {
      return false;  // Can't change during active session
    }

    // Store for next session
    // Note: m_config.telemetryApiUrl is const char*, so we need to manage this properly
    m_apiUrlStorage = url;
    m_config.telemetryApiUrl = m_apiUrlStorage.c_str();

    return true;
  }

  void SetPollingFrequency(uint32_t frequencyHz) { m_pollingFrequencyHz = frequencyHz > 0 ? frequencyHz : 10; }

 private:
  void PollingLoop() {
    auto interval = std::chrono::microseconds(1000000 / m_pollingFrequencyHz);

    while (m_sessionActive) {
      auto startTime = std::chrono::steady_clock::now();

      // Poll for frame data
      TelemetryAgent::FrameData rawData;
      bool gotData = false;

      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_dataSource && m_dataSource->IsActive()) {
          gotData = m_dataSource->GetFrameData(rawData);
        }
      }

      if (gotData) {
        ++m_framesPolled;

        // Process into telemetry frame
        telemetry::LobbySessionStateFrame frame;
        bool processed = false;

        {
          std::lock_guard<std::mutex> lock(m_mutex);
          if (m_frameProcessor) {
            processed = m_frameProcessor->ProcessFrame(rawData, frame);
          }
        }

        // Submit to telemetry client
        if (processed && m_telemetryClient) {
          m_telemetryClient->SubmitFrame(frame);
        }
      } else {
        // No data - session might have ended
        // Give a short delay before trying again
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      // Calculate time to sleep for consistent polling rate
      auto elapsed = std::chrono::steady_clock::now() - startTime;
      if (elapsed < interval) {
        std::this_thread::sleep_for(interval - elapsed);
      }
    }
  }

 private:
  mutable std::mutex m_mutex;

  std::atomic<bool> m_initialized;
  std::atomic<bool> m_sessionActive;
  std::atomic<uint32_t> m_pollingFrequencyHz;
  std::atomic<uint64_t> m_framesPolled;

  TelemetryAgentConfig m_config;
  std::string m_apiUrlStorage;  // Storage for dynamic API URL

  std::unique_ptr<TelemetryAgent::IDataSource> m_dataSource;
  std::unique_ptr<TelemetryAgent::FrameProcessor> m_frameProcessor;
  std::unique_ptr<TelemetryAgent::TelemetryClient> m_telemetryClient;

  std::thread m_pollingThread;
};

// Global agent instance
TelemetryAgentImpl g_agent;

}  // anonymous namespace

// ============================================================================
// Public C API Implementation
// ============================================================================

extern "C" {

TELEMETRY_API void TelemetryAgent_GetDefaultConfig(TelemetryAgentConfig* config) {
  if (config) {
    config->pollingFrequencyHz = 10;
    config->dataSource = TELEMETRY_SOURCE_HTTP;
    config->gameServerHost = "127.0.0.1";
    config->gameServerPort = 6721;
    config->telemetryApiUrl = "http://localhost:8081";
    config->userId = nullptr;
    config->nodeId = "game-server";
    config->gameBaseAddress = 0;
  }
}

TELEMETRY_API int TelemetryAgent_Initialize(const TelemetryAgentConfig* config) {
  TelemetryAgentConfig effectiveConfig;

  if (config) {
    effectiveConfig = *config;
  } else {
    TelemetryAgent_GetDefaultConfig(&effectiveConfig);
  }

  return g_agent.Initialize(effectiveConfig) ? 0 : 1;
}

TELEMETRY_API int TelemetryAgent_IsInitialized(void) { return g_agent.IsInitialized() ? 1 : 0; }

TELEMETRY_API int TelemetryAgent_StartSession(void) { return g_agent.StartSession() ? 0 : 1; }

TELEMETRY_API int TelemetryAgent_StopSession(void) { return g_agent.StopSession() ? 0 : 1; }

TELEMETRY_API int TelemetryAgent_IsSessionActive(void) { return g_agent.IsSessionActive() ? 1 : 0; }

TELEMETRY_API int TelemetryAgent_Shutdown(void) { return g_agent.Shutdown() ? 0 : 1; }

TELEMETRY_API int TelemetryAgent_GetSessionUUID(char* buffer, uint32_t bufferSize) {
  if (!buffer || bufferSize == 0) {
    return 0;
  }

  std::string uuid = g_agent.GetSessionUUID();

  if (uuid.empty()) {
    buffer[0] = '\0';
    return 0;
  }

  size_t copyLen = (uuid.size() < bufferSize - 1) ? uuid.size() : (bufferSize - 1);
  memcpy(buffer, uuid.c_str(), copyLen);
  buffer[copyLen] = '\0';

  return static_cast<int>(uuid.size());
}

TELEMETRY_API void TelemetryAgent_GetStats(uint64_t* framesPolled, uint64_t* framesSent, uint64_t* framesFailed,
                                           uint32_t* queueDepth) {
  uint64_t polled = 0, sent = 0, failed = 0;
  uint32_t queue = 0;

  g_agent.GetStats(polled, sent, failed, queue);

  if (framesPolled) *framesPolled = polled;
  if (framesSent) *framesSent = sent;
  if (framesFailed) *framesFailed = failed;
  if (queueDepth) *queueDepth = queue;
}

TELEMETRY_API int TelemetryAgent_SetApiUrl(const char* url) {
  if (!url) {
    return 1;
  }
  return g_agent.SetApiUrl(url) ? 0 : 1;
}

TELEMETRY_API int TelemetryAgent_SetPollingFrequency(uint32_t frequencyHz) {
  g_agent.SetPollingFrequency(frequencyHz);
  return 0;
}

}  // extern "C"

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      // Disable thread library calls for performance
      DisableThreadLibraryCalls(hinstDLL);
      break;

    case DLL_PROCESS_DETACH:
      // Clean up on unload
      if (lpvReserved == nullptr) {
        // Only clean up if the process isn't terminating
        g_agent.Shutdown();
      }
      break;
  }

  return TRUE;
}
