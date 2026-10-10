#pragma once
// The Android-only sections of the hardware dump (#335): JNI (android.* APIs through the app's context), NDK
// (sensors, thermal), Vulkan, VrApi, and the last two stages, GLES and OpenXR. Every library is reached with
// dlopen/dlsym, so the sentinel gains no DT_NEEDED entry. Every function returns fields (hwdump_field.h) and
// none lets a C++ exception out.

#include "quest/diag/hwdump_handlers.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace nevr_quest::hwdump {

// One JNI attachment of the calling thread, released (DetachCurrentThread) on every path by the destructor.
// The context is the Application, taken from the game's NativeActivity (vm and activity captured from
// libr15's ovrJava) and held as a global reference; it is what every JNI section uses.
class JniSession {
 public:
  explicit JniSession(const GameCapture& capture);
  ~JniSession();
  JniSession(const JniSession&) = delete;
  JniSession& operator=(const JniSession&) = delete;

  bool attached() const { return env_ != nullptr; }
  bool has_context() const { return context_ != nullptr; }
  void* env() const { return env_; }        // JNIEnv* of this thread
  void* vm() const { return vm_; }          // JavaVM*
  void* activity() const { return activity_; }
  void* context() const { return context_; }  // jobject (global ref) of the Application
  const nlohmann::json& describe() const { return describe_; }  // jni.vm and jni.context fields

 private:
  void* vm_ = nullptr;
  void* env_ = nullptr;
  void* activity_ = nullptr;
  void* context_ = nullptr;
  bool attached_here_ = false;
  nlohmann::json describe_;
};

// jni.* : Build, Build.VERSION, ActivityManager, displays, audio, permissions, storage dirs, battery, thermal,
// network interfaces, locale, ids. `storage_paths` receives the app's directories (for statvfs).
nlohmann::json ComposeJni(const JniSession& jni, std::vector<std::string>* storage_paths, std::string* package);

// ndk.* : sensors (ASensorManager), thermal (AThermal).
nlohmann::json ComposeNdk(const std::string& package);

// gpu.vulkan.* : a separate VkInstance (no surface, no device), destroyed before returning.
nlohmann::json ComposeVulkan();

// vrapi.* : the IDs libr15 itself reads, through libvrapi with this thread's own ovrJava.
nlohmann::json ComposeVrapi(const JniSession& jni, const GameCapture& capture);

// gpu.gl.* (stage "gl"): an own GLES 3 context on a 1x1 pbuffer, current on this thread only, torn down after.
nlohmann::json ComposeGl();

// openxr.* (stage "openxr").
nlohmann::json ComposeOpenXr();

}  // namespace nevr_quest::hwdump
