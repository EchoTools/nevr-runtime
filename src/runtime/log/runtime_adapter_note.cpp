#include "runtime/log/runtime_adapter_note.h"

#include <mutex>

namespace nevr_gpu_hung {

namespace {
std::mutex g_mutex;
std::string g_name;
std::string g_driver;
}  // namespace

void NoteRuntimeAdapter(const char* gpu_name, const char* gpu_driver) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (gpu_name != nullptr && gpu_name[0] != '\0') g_name = gpu_name;
  if (gpu_driver != nullptr && gpu_driver[0] != '\0') g_driver = gpu_driver;
}

void KnownRuntimeAdapter(std::string& gpu_name, std::string& gpu_driver) {
  std::lock_guard<std::mutex> lock(g_mutex);
  gpu_name = g_name;
  gpu_driver = g_driver;
}

}  // namespace nevr_gpu_hung
