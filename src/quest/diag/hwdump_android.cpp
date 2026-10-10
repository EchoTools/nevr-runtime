#include "quest/diag/hwdump_android.h"

#include "quest/diag/hwdump_field.h"

#include <dlfcn.h>
#include <jni.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

namespace nevr_quest::hwdump {
namespace {

using nlohmann::json;

std::string DlError(const char* what) {
  const char* e = ::dlerror();
  return std::string(what) + ": " + (e != nullptr ? e : "no dlerror text");
}

// ---- JNI helpers ------------------------------------------------------------------------------------------
//
// Rules (JNI spec): a JNIEnv is used only on its own thread; after every call that can raise, the pending
// exception is checked and cleared before any other JNI call; every section runs in its own local frame.

JNIEnv* Env(const JniSession& s) { return static_cast<JNIEnv*>(s.env()); }

// Clears the pending exception and returns "<Throwable.toString()>".
std::string TakeException(JNIEnv* env) {
  jthrowable t = env->ExceptionOccurred();
  env->ExceptionClear();
  if (t == nullptr) return "java exception (no throwable)";
  std::string text = "java exception";
  jclass cls = env->GetObjectClass(t);
  jmethodID to_string = cls != nullptr ? env->GetMethodID(cls, "toString", "()Ljava/lang/String;") : nullptr;
  if (to_string != nullptr && !env->ExceptionCheck()) {
    auto s = static_cast<jstring>(env->CallObjectMethod(t, to_string));
    if (!env->ExceptionCheck() && s != nullptr) {
      const char* utf = env->GetStringUTFChars(s, nullptr);
      if (utf != nullptr) {
        text = utf;
        env->ReleaseStringUTFChars(s, utf);
      }
    }
  }
  env->ExceptionClear();
  return text;
}

std::string Str(JNIEnv* env, jobject s) {
  if (s == nullptr) return std::string();
  const char* utf = env->GetStringUTFChars(static_cast<jstring>(s), nullptr);
  if (utf == nullptr) {
    env->ExceptionClear();
    return std::string();
  }
  std::string out = utf;
  env->ReleaseStringUTFChars(static_cast<jstring>(s), utf);
  return out;
}

// A JNI read: `body` fills `value` and returns true, or returns false (with `error` set, or with a Java
// exception pending, which becomes the error). Runs in its own local frame.
json JniField(JNIEnv* env, const std::string& source, const std::function<bool(json*, std::string*)>& body) {
  if (env == nullptr) return Fail(source, "no JNIEnv: the dump thread is not attached to the VM");
  if (env->PushLocalFrame(256) != 0) return Fail(source, "PushLocalFrame: " + TakeException(env));
  json value;
  std::string error;
  bool ok = false;
  try {
    ok = body(&value, &error);
  } catch (const std::exception& e) {  // nlohmann conversions; never a Java exception
    ok = false;
    error = std::string("c++ exception: ") + e.what();
  }
  if (env->ExceptionCheck()) {
    error = TakeException(env);
    ok = false;
  }
  env->PopLocalFrame(nullptr);
  if (ok) return Ok(source, value);
  return Fail(source, error.empty() ? "failed without a java exception" : error);
}

// Small typed call helpers. Each returns with any Java exception left pending for JniField to report.
jclass Cls(JNIEnv* env, const char* name) { return env->FindClass(name); }

jobject CallObj(JNIEnv* env, jobject obj, const char* name, const char* sig, ...) {
  jclass c = env->GetObjectClass(obj);
  jmethodID m = env->GetMethodID(c, name, sig);
  if (m == nullptr) return nullptr;
  va_list args;
  va_start(args, sig);
  jobject r = env->CallObjectMethodV(obj, m, args);
  va_end(args);
  return r;
}

jint CallInt(JNIEnv* env, jobject obj, const char* name, const char* sig, ...) {
  jclass c = env->GetObjectClass(obj);
  jmethodID m = env->GetMethodID(c, name, sig);
  if (m == nullptr) return 0;
  va_list args;
  va_start(args, sig);
  jint r = env->CallIntMethodV(obj, m, args);
  va_end(args);
  return r;
}

jlong CallLong(JNIEnv* env, jobject obj, const char* name, const char* sig, ...) {
  jclass c = env->GetObjectClass(obj);
  jmethodID m = env->GetMethodID(c, name, sig);
  if (m == nullptr) return 0;
  va_list args;
  va_start(args, sig);
  jlong r = env->CallLongMethodV(obj, m, args);
  va_end(args);
  return r;
}

jboolean CallBool(JNIEnv* env, jobject obj, const char* name, const char* sig) {
  jclass c = env->GetObjectClass(obj);
  jmethodID m = env->GetMethodID(c, name, sig);
  if (m == nullptr) return JNI_FALSE;
  return env->CallBooleanMethod(obj, m);
}

jfloat CallFloat(JNIEnv* env, jobject obj, const char* name, const char* sig) {
  jclass c = env->GetObjectClass(obj);
  jmethodID m = env->GetMethodID(c, name, sig);
  if (m == nullptr) return 0;
  return env->CallFloatMethod(obj, m);
}

jobject Service(JNIEnv* env, jobject context, const char* name) {
  jstring n = env->NewStringUTF(name);
  if (n == nullptr) return nullptr;
  return CallObj(env, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", n);
}

json IntArray(JNIEnv* env, jobject arr) {
  json out = json::array();
  if (arr == nullptr) return out;
  const jsize n = env->GetArrayLength(static_cast<jintArray>(arr));
  std::vector<jint> v(static_cast<std::size_t>(n));
  env->GetIntArrayRegion(static_cast<jintArray>(arr), 0, n, v.data());
  for (jint x : v) out.push_back(x);
  return out;
}

json StringArray(JNIEnv* env, jobject arr) {
  json out = json::array();
  if (arr == nullptr) return out;
  const jsize n = env->GetArrayLength(static_cast<jobjectArray>(arr));
  for (jsize i = 0; i < n; ++i) {
    jobject s = env->GetObjectArrayElement(static_cast<jobjectArray>(arr), i);
    out.push_back(Str(env, s));
    env->DeleteLocalRef(s);
  }
  return out;
}

// String.valueOf(obj), or Arrays.toString for an object array.
std::string ValueText(JNIEnv* env, jobject obj) {
  if (obj == nullptr) return "null";
  jclass object_array = Cls(env, "[Ljava/lang/Object;");
  if (object_array != nullptr && env->IsInstanceOf(obj, object_array)) {
    jclass arrays = Cls(env, "java/util/Arrays");
    jmethodID m = env->GetStaticMethodID(arrays, "toString", "([Ljava/lang/Object;)Ljava/lang/String;");
    return Str(env, env->CallStaticObjectMethod(arrays, m, obj));
  }
  jclass string_cls = Cls(env, "java/lang/String");
  jmethodID value_of = env->GetStaticMethodID(string_cls, "valueOf", "(Ljava/lang/Object;)Ljava/lang/String;");
  return Str(env, env->CallStaticObjectMethod(string_cls, value_of, obj));
}

// Every public static field of `class_name`, as text, by reflection (Class.getFields).
json StaticFields(JNIEnv* env, const char* class_name) {
  return JniField(env, std::string("reflection ") + class_name + ".getFields", [&](json* value, std::string* err) {
    jclass cls = Cls(env, class_name);
    if (cls == nullptr) return false;
    auto fields = static_cast<jobjectArray>(CallObj(env, cls, "getFields", "()[Ljava/lang/reflect/Field;"));
    if (fields == nullptr) {
      *err = "getFields returned null";
      return false;
    }
    json out = json::object();
    const jsize n = env->GetArrayLength(fields);
    for (jsize i = 0; i < n; ++i) {
      jobject f = env->GetObjectArrayElement(fields, i);
      const std::string name = Str(env, CallObj(env, f, "getName", "()Ljava/lang/String;"));
      jobject v = CallObj(env, f, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", static_cast<jobject>(nullptr));
      if (env->ExceptionCheck()) {
        out[name] = Fail("Field.get", TakeException(env));
      } else {
        out[name] = ValueText(env, v);
      }
      env->DeleteLocalRef(f);
      if (env->ExceptionCheck()) return false;
    }
    *value = out;
    return true;
  });
}

// ---- NDK via dlsym ----------------------------------------------------------------------------------------

template <typename T>
T Sym(void* lib, const char* name) {
  return reinterpret_cast<T>(::dlsym(lib, name));
}

}  // namespace

// ---- JniSession -------------------------------------------------------------------------------------------

JniSession::JniSession(const GameCapture& capture) {
  describe_ = json::object();
  vm_ = capture.vm;
  activity_ = capture.activity;
  std::string vm_source = "libr15 ovrJava.Vm (captured by the vrapi hooks)";
  if (vm_ == nullptr) {
    // Fallback: the VM without an activity. JNI_GetCreatedJavaVMs is exported by libart through
    // libnativehelper on newer releases; below API 31 it may not be visible to the app namespace.
    using GetVMs = jint (*)(JavaVM**, jsize, jsize*);
    auto get = reinterpret_cast<GetVMs>(::dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (get == nullptr) {
      void* nh = ::dlopen("libnativehelper.so", RTLD_NOW);
      if (nh != nullptr) get = reinterpret_cast<GetVMs>(::dlsym(nh, "JNI_GetCreatedJavaVMs"));
    }
    JavaVM* found = nullptr;
    jsize count = 0;
    if (get != nullptr && get(&found, 1, &count) == JNI_OK && count > 0) vm_ = found;
    vm_source = "JNI_GetCreatedJavaVMs (no ovrJava captured)";
  }
  if (vm_ == nullptr) {
    describe_["vm"] = Fail("JavaVM", "no VM: no ovrJava was captured and JNI_GetCreatedJavaVMs is unavailable");
    describe_["context"] = Fail("Context", "no VM");
    return;
  }
  auto* vm = static_cast<JavaVM*>(vm_);
  JNIEnv* env = nullptr;
  const jint got = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (got == JNI_EDETACHED) {
    JavaVMAttachArgs args{JNI_VERSION_1_6, "nevr-hwdump", nullptr};
    if (vm->AttachCurrentThread(&env, &args) != JNI_OK) {
      describe_["vm"] = Fail(vm_source, "AttachCurrentThread failed");
      describe_["context"] = Fail("Context", "not attached");
      env = nullptr;
      return;
    }
    attached_here_ = true;
  } else if (got != JNI_OK) {
    describe_["vm"] = Fail(vm_source, "GetEnv returned " + std::to_string(got));
    describe_["context"] = Fail("Context", "no JNIEnv");
    return;
  }
  env_ = env;
  describe_["vm"] = Ok(vm_source, "attached");

  // The context: the Application of the game's NativeActivity. A global reference of our own, so the dump
  // does not depend on the activity reference staying valid while it runs.
  if (activity_ != nullptr) {
    auto act = static_cast<jobject>(activity_);
    const jobjectRefType kind = env->GetObjectRefType(act);
    if (kind != JNIGlobalRefType) {
      describe_["context"] =
          Fail("ovrJava.ActivityObject", "the activity is not a JNI global reference (kind " + std::to_string(static_cast<int>(kind)) + ")");
      return;
    }
    jobject app = CallObj(env, act, "getApplicationContext", "()Landroid/content/Context;");
    if (env->ExceptionCheck() || app == nullptr) {
      describe_["context"] = Fail("Activity.getApplicationContext",
                                  env->ExceptionCheck() ? TakeException(env) : std::string("returned null"));
      return;
    }
    context_ = env->NewGlobalRef(app);
    env->DeleteLocalRef(app);
    describe_["context"] = Ok("Activity.getApplicationContext", "application");
    return;
  }
  // No activity: ActivityThread.currentApplication() (hidden API; may be refused).
  jclass at = env->FindClass("android/app/ActivityThread");
  jmethodID m = at != nullptr ? env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;") : nullptr;
  jobject app = m != nullptr ? env->CallStaticObjectMethod(at, m) : nullptr;
  if (env->ExceptionCheck() || app == nullptr) {
    describe_["context"] = Fail("ActivityThread.currentApplication",
                                env->ExceptionCheck() ? TakeException(env) : std::string("returned null"));
    return;
  }
  context_ = env->NewGlobalRef(app);
  env->DeleteLocalRef(app);
  describe_["context"] = Ok("ActivityThread.currentApplication", "application");
}

JniSession::~JniSession() {
  if (env_ != nullptr && context_ != nullptr) static_cast<JNIEnv*>(env_)->DeleteGlobalRef(static_cast<jobject>(context_));
  if (attached_here_) static_cast<JavaVM*>(vm_)->DetachCurrentThread();
}

// ---- jni.* ------------------------------------------------------------------------------------------------

json ComposeJni(const JniSession& jni, std::vector<std::string>* storage_paths, std::string* package) {
  json out = json::object();
  out["vm"] = jni.describe().value("vm", Fail("JavaVM", "not described"));
  out["context"] = jni.describe().value("context", Fail("Context", "not described"));
  JNIEnv* env = Env(jni);
  auto ctx = static_cast<jobject>(jni.context());
  const auto need_ctx = [&](const std::string& source) -> json {
    return Fail(source, jni.attached() ? "no application context" : "not attached to the VM");
  };

  out["build"] = jni.attached() ? StaticFields(env, "android/os/Build") : Fail("android.os.Build", "not attached");
  out["build_version"] =
      jni.attached() ? StaticFields(env, "android/os/Build$VERSION") : Fail("android.os.Build$VERSION", "not attached");
  out["build_getSerial"] = JniField(env, "android.os.Build.getSerial()", [&](json* v, std::string*) {
    jclass b = Cls(env, "android/os/Build");
    jmethodID m = env->GetStaticMethodID(b, "getSerial", "()Ljava/lang/String;");
    if (m == nullptr) return false;
    jobject s = env->CallStaticObjectMethod(b, m);
    if (env->ExceptionCheck()) return false;
    *v = Str(env, s);
    return true;
  });
  out["runtime"] = JniField(env, "java.lang.Runtime + System.getProperty", [&](json* v, std::string*) {
    jclass rc = Cls(env, "java/lang/Runtime");
    jobject rt = env->CallStaticObjectMethod(rc, env->GetStaticMethodID(rc, "getRuntime", "()Ljava/lang/Runtime;"));
    if (env->ExceptionCheck()) return false;
    json r = json::object();
    r["availableProcessors"] = CallInt(env, rt, "availableProcessors", "()I");
    r["maxMemory"] = CallLong(env, rt, "maxMemory", "()J");
    r["totalMemory"] = CallLong(env, rt, "totalMemory", "()J");
    jclass sys = Cls(env, "java/lang/System");
    jmethodID gp = env->GetStaticMethodID(sys, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    for (const char* k : {"os.arch", "os.version", "java.vm.name", "java.vm.version", "http.agent"}) {
      r[std::string("property ") + k] = Str(env, env->CallStaticObjectMethod(sys, gp, env->NewStringUTF(k)));
    }
    *v = r;
    return !env->ExceptionCheck();
  });

  if (ctx == nullptr) {
    for (const char* k : {"package", "android_id", "activity_manager", "displays", "audio_devices", "audio_properties",
                          "permissions", "storage_dirs", "battery_intent", "battery_manager", "power",
                          "network_interfaces", "locale"}) {
      out[k] = need_ctx(std::string("Context (") + k + ")");
    }
    return out;
  }

  out["package"] = JniField(env, "Context.getPackageName + PackageManager.getPackageInfo", [&](json* v, std::string*) {
    jobject name = CallObj(env, ctx, "getPackageName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) return false;
    *package = Str(env, name);
    jobject pm = CallObj(env, ctx, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jobject info = CallObj(env, pm, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", name, 0);
    if (env->ExceptionCheck()) return false;
    jclass ic = env->GetObjectClass(info);
    json p = json::object();
    p["packageName"] = *package;
    p["versionName"] = Str(env, env->GetObjectField(info, env->GetFieldID(ic, "versionName", "Ljava/lang/String;")));
    p["versionCode"] = env->GetIntField(info, env->GetFieldID(ic, "versionCode", "I"));
    p["firstInstallTime"] = env->GetLongField(info, env->GetFieldID(ic, "firstInstallTime", "J"));
    p["lastUpdateTime"] = env->GetLongField(info, env->GetFieldID(ic, "lastUpdateTime", "J"));
    *v = p;
    return !env->ExceptionCheck();
  });

  out["android_id"] = JniField(env, "Settings.Secure.getString(ANDROID_ID)", [&](json* v, std::string*) {
    jobject resolver = CallObj(env, ctx, "getContentResolver", "()Landroid/content/ContentResolver;");
    jclass sec = Cls(env, "android/provider/Settings$Secure");
    jmethodID m = env->GetStaticMethodID(sec, "getString",
                                         "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    if (m == nullptr) return false;
    jobject id = env->CallStaticObjectMethod(sec, m, resolver, env->NewStringUTF("android_id"));
    if (env->ExceptionCheck()) return false;
    *v = Str(env, id);
    return true;
  });

  out["activity_manager"] = JniField(env, "ActivityManager.getMemoryInfo/getMemoryClass", [&](json* v, std::string* err) {
    jobject am = Service(env, ctx, "activity");
    if (am == nullptr) {
      if (!env->ExceptionCheck()) *err = "getSystemService(activity) returned null";
      return false;
    }
    jclass mic = Cls(env, "android/app/ActivityManager$MemoryInfo");
    jobject mi = env->NewObject(mic, env->GetMethodID(mic, "<init>", "()V"));
    if (env->ExceptionCheck()) return false;
    jclass amc = env->GetObjectClass(am);
    env->CallVoidMethod(am, env->GetMethodID(amc, "getMemoryInfo", "(Landroid/app/ActivityManager$MemoryInfo;)V"), mi);
    if (env->ExceptionCheck()) return false;
    json m = json::object();
    m["availMem"] = env->GetLongField(mi, env->GetFieldID(mic, "availMem", "J"));
    m["totalMem"] = env->GetLongField(mi, env->GetFieldID(mic, "totalMem", "J"));
    m["threshold"] = env->GetLongField(mi, env->GetFieldID(mic, "threshold", "J"));
    m["lowMemory"] = env->GetBooleanField(mi, env->GetFieldID(mic, "lowMemory", "Z")) == JNI_TRUE;
    m["memoryClass"] = CallInt(env, am, "getMemoryClass", "()I");
    m["largeMemoryClass"] = CallInt(env, am, "getLargeMemoryClass", "()I");
    m["isLowRamDevice"] = CallBool(env, am, "isLowRamDevice", "()Z") == JNI_TRUE;
    *v = m;
    return !env->ExceptionCheck();
  });

  out["displays"] = JniField(env, "DisplayManager.getDisplays", [&](json* v, std::string* err) {
    jobject dm = Service(env, ctx, "display");
    if (dm == nullptr) {
      if (!env->ExceptionCheck()) *err = "getSystemService(display) returned null";
      return false;
    }
    auto displays = static_cast<jobjectArray>(CallObj(env, dm, "getDisplays", "()[Landroid/view/Display;"));
    if (env->ExceptionCheck() || displays == nullptr) return false;
    jclass dmc = Cls(env, "android/util/DisplayMetrics");
    json list = json::array();
    for (jsize i = 0; i < env->GetArrayLength(displays); ++i) {
      jobject d = env->GetObjectArrayElement(displays, i);
      json e = json::object();
      e["displayId"] = CallInt(env, d, "getDisplayId", "()I");
      e["name"] = Str(env, CallObj(env, d, "getName", "()Ljava/lang/String;"));
      e["refreshRate"] = CallFloat(env, d, "getRefreshRate", "()F");
      e["state"] = CallInt(env, d, "getState", "()I");
      jobject mode = CallObj(env, d, "getMode", "()Landroid/view/Display$Mode;");
      if (mode != nullptr) e["currentModeId"] = CallInt(env, mode, "getModeId", "()I");
      auto modes = static_cast<jobjectArray>(CallObj(env, d, "getSupportedModes", "()[Landroid/view/Display$Mode;"));
      json ml = json::array();
      for (jsize j = 0; modes != nullptr && j < env->GetArrayLength(modes); ++j) {
        jobject md = env->GetObjectArrayElement(modes, j);
        ml.push_back({{"modeId", CallInt(env, md, "getModeId", "()I")},
                      {"physicalWidth", CallInt(env, md, "getPhysicalWidth", "()I")},
                      {"physicalHeight", CallInt(env, md, "getPhysicalHeight", "()I")},
                      {"refreshRate", CallFloat(env, md, "getRefreshRate", "()F")}});
        env->DeleteLocalRef(md);
      }
      e["supportedModes"] = ml;
      jobject metrics = env->NewObject(dmc, env->GetMethodID(dmc, "<init>", "()V"));
      jclass dc = env->GetObjectClass(d);
      env->CallVoidMethod(d, env->GetMethodID(dc, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V"), metrics);
      if (env->ExceptionCheck()) return false;
      e["realMetrics"] = {{"widthPixels", env->GetIntField(metrics, env->GetFieldID(dmc, "widthPixels", "I"))},
                          {"heightPixels", env->GetIntField(metrics, env->GetFieldID(dmc, "heightPixels", "I"))},
                          {"densityDpi", env->GetIntField(metrics, env->GetFieldID(dmc, "densityDpi", "I"))},
                          {"xdpi", env->GetFloatField(metrics, env->GetFieldID(dmc, "xdpi", "F"))},
                          {"ydpi", env->GetFloatField(metrics, env->GetFieldID(dmc, "ydpi", "F"))}};
      list.push_back(e);
      env->DeleteLocalRef(d);
      if (env->ExceptionCheck()) return false;
    }
    *v = list;
    return true;
  });

  out["audio_devices"] = JniField(env, "AudioManager.getDevices(GET_DEVICES_ALL)", [&](json* v, std::string* err) {
    jobject am = Service(env, ctx, "audio");
    if (am == nullptr) {
      if (!env->ExceptionCheck()) *err = "getSystemService(audio) returned null";
      return false;
    }
    auto devs = static_cast<jobjectArray>(CallObj(env, am, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;", 3));
    if (env->ExceptionCheck() || devs == nullptr) return false;
    json list = json::array();
    for (jsize i = 0; i < env->GetArrayLength(devs); ++i) {
      jobject d = env->GetObjectArrayElement(devs, i);
      json e = json::object();
      e["id"] = CallInt(env, d, "getId", "()I");
      e["type"] = CallInt(env, d, "getType", "()I");
      jobject pn = CallObj(env, d, "getProductName", "()Ljava/lang/CharSequence;");
      e["productName"] = pn != nullptr ? Str(env, CallObj(env, pn, "toString", "()Ljava/lang/String;")) : "";
      e["isSource"] = CallBool(env, d, "isSource", "()Z") == JNI_TRUE;
      e["isSink"] = CallBool(env, d, "isSink", "()Z") == JNI_TRUE;
      e["sampleRates"] = IntArray(env, CallObj(env, d, "getSampleRates", "()[I"));
      e["channelCounts"] = IntArray(env, CallObj(env, d, "getChannelCounts", "()[I"));
      e["encodings"] = IntArray(env, CallObj(env, d, "getEncodings", "()[I"));
      e["address"] = Str(env, CallObj(env, d, "getAddress", "()Ljava/lang/String;"));
      list.push_back(e);
      env->DeleteLocalRef(d);
      if (env->ExceptionCheck()) return false;
    }
    *v = list;
    return true;
  });

  out["audio_properties"] = JniField(env, "AudioManager.getProperty", [&](json* v, std::string* err) {
    jobject am = Service(env, ctx, "audio");
    if (am == nullptr) {
      if (!env->ExceptionCheck()) *err = "getSystemService(audio) returned null";
      return false;
    }
    json p = json::object();
    for (const char* k : {"android.media.property.OUTPUT_SAMPLE_RATE", "android.media.property.OUTPUT_FRAMES_PER_BUFFER"}) {
      p[k] = Str(env, CallObj(env, am, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", env->NewStringUTF(k)));
    }
    *v = p;
    return !env->ExceptionCheck();
  });

  out["permissions"] = JniField(env, "PackageManager.getPackageInfo(GET_PERMISSIONS)", [&](json* v, std::string*) {
    jobject name = CallObj(env, ctx, "getPackageName", "()Ljava/lang/String;");
    jobject pm = CallObj(env, ctx, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jobject info = CallObj(env, pm, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", name,
                           4096 /* GET_PERMISSIONS */);
    if (env->ExceptionCheck()) return false;
    jclass ic = env->GetObjectClass(info);
    const json names = StringArray(env, env->GetObjectField(info, env->GetFieldID(ic, "requestedPermissions", "[Ljava/lang/String;")));
    const json flags = IntArray(env, env->GetObjectField(info, env->GetFieldID(ic, "requestedPermissionsFlags", "[I")));
    json list = json::array();
    for (std::size_t i = 0; i < names.size(); ++i) {
      const int f = i < flags.size() ? flags[i].get<int>() : 0;
      list.push_back({{"name", names[i]}, {"flags", f}, {"granted", (f & 2) != 0 /* REQUESTED_PERMISSION_GRANTED */}});
    }
    *v = list;
    return !env->ExceptionCheck();
  });

  out["storage_dirs"] = JniField(env, "Context.get*Dir", [&](json* v, std::string*) {
    json d = json::object();
    const auto path_of = [&](jobject file) { return file != nullptr ? Str(env, CallObj(env, file, "getAbsolutePath", "()Ljava/lang/String;")) : std::string(); };
    d["filesDir"] = path_of(CallObj(env, ctx, "getFilesDir", "()Ljava/io/File;"));
    d["cacheDir"] = path_of(CallObj(env, ctx, "getCacheDir", "()Ljava/io/File;"));
    d["noBackupFilesDir"] = path_of(CallObj(env, ctx, "getNoBackupFilesDir", "()Ljava/io/File;"));
    d["dataDir"] = path_of(CallObj(env, ctx, "getDataDir", "()Ljava/io/File;"));
    d["obbDir"] = path_of(CallObj(env, ctx, "getObbDir", "()Ljava/io/File;"));
    d["externalFilesDir"] = path_of(CallObj(env, ctx, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", static_cast<jobject>(nullptr)));
    d["externalCacheDir"] = path_of(CallObj(env, ctx, "getExternalCacheDir", "()Ljava/io/File;"));
    if (env->ExceptionCheck()) return false;
    for (const auto& item : d.items()) {
      if (!item.value().get<std::string>().empty()) storage_paths->push_back(item.value().get<std::string>());
    }
    *v = d;
    return true;
  });

  out["battery_intent"] = JniField(env, "registerReceiver(null, ACTION_BATTERY_CHANGED)", [&](json* v, std::string* err) {
    jclass ifc = Cls(env, "android/content/IntentFilter");
    jobject filter = env->NewObject(ifc, env->GetMethodID(ifc, "<init>", "(Ljava/lang/String;)V"),
                                    env->NewStringUTF("android.intent.action.BATTERY_CHANGED"));
    if (env->ExceptionCheck()) return false;
    jobject intent = CallObj(env, ctx, "registerReceiver",
                             "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",
                             static_cast<jobject>(nullptr), filter);
    if (env->ExceptionCheck()) return false;
    if (intent == nullptr) {
      *err = "no sticky ACTION_BATTERY_CHANGED intent";
      return false;
    }
    json b = json::object();
    for (const char* k : {"level", "scale", "temperature", "voltage", "status", "health", "plugged"}) {
      b[k] = CallInt(env, intent, "getIntExtra", "(Ljava/lang/String;I)I", env->NewStringUTF(k), -1);
    }
    b["technology"] = Str(env, CallObj(env, intent, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;",
                                       env->NewStringUTF("technology")));
    *v = b;
    return !env->ExceptionCheck();
  });

  out["battery_manager"] = JniField(env, "BatteryManager.getIntProperty/getLongProperty", [&](json* v, std::string* err) {
    jobject bm = Service(env, ctx, "batterymanager");
    if (bm == nullptr) {
      if (!env->ExceptionCheck()) *err = "getSystemService(batterymanager) returned null";
      return false;
    }
    json b = json::object();
    b["CHARGE_COUNTER"] = CallInt(env, bm, "getIntProperty", "(I)I", 1);
    b["CURRENT_NOW"] = CallInt(env, bm, "getIntProperty", "(I)I", 2);
    b["CURRENT_AVERAGE"] = CallInt(env, bm, "getIntProperty", "(I)I", 3);
    b["CAPACITY"] = CallInt(env, bm, "getIntProperty", "(I)I", 4);
    b["ENERGY_COUNTER"] = CallLong(env, bm, "getLongProperty", "(I)J", 5);
    *v = b;
    return !env->ExceptionCheck();
  });

  out["power"] = JniField(env, "PowerManager.getCurrentThermalStatus/isPowerSaveMode", [&](json* v, std::string* err) {
    jobject pm = Service(env, ctx, "power");
    if (pm == nullptr) {
      if (!env->ExceptionCheck()) *err = "getSystemService(power) returned null";
      return false;
    }
    json p = json::object();
    p["currentThermalStatus"] = CallInt(env, pm, "getCurrentThermalStatus", "()I");
    p["isPowerSaveMode"] = CallBool(env, pm, "isPowerSaveMode", "()Z") == JNI_TRUE;
    p["isInteractive"] = CallBool(env, pm, "isInteractive", "()Z") == JNI_TRUE;
    *v = p;
    return !env->ExceptionCheck();
  });

  out["network_interfaces"] = JniField(env, "java.net.NetworkInterface.getNetworkInterfaces", [&](json* v, std::string*) {
    jclass nic = Cls(env, "java/net/NetworkInterface");
    jobject e = env->CallStaticObjectMethod(
        nic, env->GetStaticMethodID(nic, "getNetworkInterfaces", "()Ljava/util/Enumeration;"));
    if (env->ExceptionCheck()) return false;
    json list = json::array();
    while (e != nullptr && CallBool(env, e, "hasMoreElements", "()Z") == JNI_TRUE) {
      jobject ni = CallObj(env, e, "nextElement", "()Ljava/lang/Object;");
      if (env->ExceptionCheck()) return false;
      json n = json::object();
      n["name"] = Str(env, CallObj(env, ni, "getName", "()Ljava/lang/String;"));
      n["index"] = CallInt(env, ni, "getIndex", "()I");
      n["mtu"] = CallInt(env, ni, "getMTU", "()I");
      n["up"] = CallBool(env, ni, "isUp", "()Z") == JNI_TRUE;
      n["loopback"] = CallBool(env, ni, "isLoopback", "()Z") == JNI_TRUE;
      jobject hw = CallObj(env, ni, "getHardwareAddress", "()[B");
      if (env->ExceptionCheck()) {
        n["hardwareAddress"] = Fail("NetworkInterface.getHardwareAddress", TakeException(env));
      } else if (hw == nullptr) {
        n["hardwareAddress"] = Fail("NetworkInterface.getHardwareAddress", "null (no address, or hidden by the OS)");
      } else {
        const jsize len = env->GetArrayLength(static_cast<jbyteArray>(hw));
        std::vector<jbyte> b(static_cast<std::size_t>(len));
        env->GetByteArrayRegion(static_cast<jbyteArray>(hw), 0, len, b.data());
        std::string mac;
        for (jsize i = 0; i < len; ++i) {
          char part[4];
          std::snprintf(part, sizeof part, i == 0 ? "%02x" : ":%02x", static_cast<unsigned char>(b[static_cast<std::size_t>(i)]));
          mac += part;
        }
        n["hardwareAddress"] = Ok("NetworkInterface.getHardwareAddress", mac);
      }
      list.push_back(n);
      env->DeleteLocalRef(ni);
    }
    *v = list;
    return !env->ExceptionCheck();
  });

  out["locale"] = JniField(env, "Locale.getDefault + TimeZone.getDefault", [&](json* v, std::string*) {
    jclass lc = Cls(env, "java/util/Locale");
    jobject loc = env->CallStaticObjectMethod(lc, env->GetStaticMethodID(lc, "getDefault", "()Ljava/util/Locale;"));
    jclass tzc = Cls(env, "java/util/TimeZone");
    jobject tz = env->CallStaticObjectMethod(tzc, env->GetStaticMethodID(tzc, "getDefault", "()Ljava/util/TimeZone;"));
    if (env->ExceptionCheck()) return false;
    *v = {{"languageTag", Str(env, CallObj(env, loc, "toLanguageTag", "()Ljava/lang/String;"))},
          {"timeZone", Str(env, CallObj(env, tz, "getID", "()Ljava/lang/String;"))}};
    return !env->ExceptionCheck();
  });
  return out;
}

// ---- ndk.* ------------------------------------------------------------------------------------------------

json ComposeNdk(const std::string& package) {
  json out = json::object();
  ::dlerror();
  void* lib = ::dlopen("libandroid.so", RTLD_NOW);
  if (lib == nullptr) {
    const std::string e = DlError("dlopen libandroid.so");
    out["sensors"] = Fail("ASensorManager", e);
    out["thermal_status"] = Fail("AThermal_getCurrentThermalStatus", e);
    out["thermal_headroom"] = Fail("AThermal_getThermalHeadroom", e);
    return out;
  }

  using GetForPackage = void* (*)(const char*);
  using GetList = int (*)(void*, const void* const**);
  using CStr = const char* (*)(const void*);
  using Int = int (*)(const void*);
  using Float = float (*)(const void*);
  using Bool = bool (*)(const void*);
  auto get_mgr = Sym<GetForPackage>(lib, "ASensorManager_getInstanceForPackage");
  auto get_list = Sym<GetList>(lib, "ASensorManager_getSensorList");
  if (get_mgr == nullptr || get_list == nullptr) {
    out["sensors"] = Fail("ASensorManager_getInstanceForPackage/getSensorList", DlError("dlsym"));
  } else {
    void* mgr = get_mgr(package.empty() ? "com.readyatdawn.r15" : package.c_str());
    const void* const* list = nullptr;
    const int n = mgr != nullptr ? get_list(mgr, &list) : -1;
    if (mgr == nullptr || n < 0) {
      out["sensors"] = Fail("ASensorManager_getSensorList", mgr == nullptr ? "no sensor manager" : "returned " + std::to_string(n));
    } else {
      const auto s_name = Sym<CStr>(lib, "ASensor_getName");
      const auto s_vendor = Sym<CStr>(lib, "ASensor_getVendor");
      const auto s_type = Sym<Int>(lib, "ASensor_getType");
      const auto s_stype = Sym<CStr>(lib, "ASensor_getStringType");
      const auto s_res = Sym<Float>(lib, "ASensor_getResolution");
      const auto s_delay = Sym<Int>(lib, "ASensor_getMinDelay");
      const auto s_fifo_max = Sym<Int>(lib, "ASensor_getFifoMaxEventCount");
      const auto s_fifo_res = Sym<Int>(lib, "ASensor_getFifoReservedEventCount");
      const auto s_mode = Sym<Int>(lib, "ASensor_getReportingMode");
      const auto s_wake = Sym<Bool>(lib, "ASensor_isWakeUpSensor");
      const auto s_handle = Sym<Int>(lib, "ASensor_getHandle");  // API 29
      json sensors = json::array();
      for (int i = 0; i < n; ++i) {
        const void* s = list[i];
        json e = json::object();
        if (s_name != nullptr) e["name"] = s_name(s);
        if (s_vendor != nullptr) e["vendor"] = s_vendor(s);
        if (s_type != nullptr) e["type"] = s_type(s);
        if (s_stype != nullptr) e["stringType"] = s_stype(s) != nullptr ? s_stype(s) : "";
        if (s_res != nullptr) e["resolution"] = s_res(s);
        if (s_delay != nullptr) e["minDelayUs"] = s_delay(s);
        if (s_fifo_max != nullptr) e["fifoMaxEventCount"] = s_fifo_max(s);
        if (s_fifo_res != nullptr) e["fifoReservedEventCount"] = s_fifo_res(s);
        if (s_mode != nullptr) e["reportingMode"] = s_mode(s);
        if (s_wake != nullptr) e["wakeUp"] = s_wake(s);
        e["handle"] = s_handle != nullptr ? json(s_handle(s)) : Fail("ASensor_getHandle", "not exported (API < 29)");
        sensors.push_back(e);
      }
      out["sensors"] = Ok("ASensorManager_getSensorList", sensors);
    }
  }

  using Acquire = void* (*)();
  using Status = int (*)(void*);
  using Headroom = float (*)(void*, int);
  using Release = void (*)(void*);
  auto acquire = Sym<Acquire>(lib, "AThermal_acquireManager");
  auto status = Sym<Status>(lib, "AThermal_getCurrentThermalStatus");
  auto headroom = Sym<Headroom>(lib, "AThermal_getThermalHeadroom");
  auto release = Sym<Release>(lib, "AThermal_releaseManager");
  void* thermal = acquire != nullptr ? acquire() : nullptr;
  if (thermal == nullptr) {
    const std::string e = acquire == nullptr ? std::string("AThermal_acquireManager not exported (API < 30)")
                                             : std::string("AThermal_acquireManager returned null");
    out["thermal_status"] = Fail("AThermal_getCurrentThermalStatus", e);
    out["thermal_headroom"] = Fail("AThermal_getThermalHeadroom", e);
  } else {
    out["thermal_status"] = status != nullptr ? Ok("AThermal_getCurrentThermalStatus", status(thermal))
                                              : Fail("AThermal_getCurrentThermalStatus", "not exported");
    if (headroom != nullptr) {
      const float h = headroom(thermal, 0);
      out["thermal_headroom"] = !std::isnan(h) ? Ok("AThermal_getThermalHeadroom(0)", h)
                                       : Fail("AThermal_getThermalHeadroom(0)", "NaN: headroom unsupported or called too often");
    } else {
      out["thermal_headroom"] = Fail("AThermal_getThermalHeadroom", "not exported (API < 31)");
    }
    if (release != nullptr) release(thermal);
  }
  return out;
}

// ---- gpu.vulkan.* -----------------------------------------------------------------------------------------

#include "quest/diag/hwdump_vk_fields.inc"

namespace {

std::string VkError(const char* call, VkResult r) { return std::string(call) + " returned VkResult " + std::to_string(r); }

std::string Uuid(const std::uint8_t* u) {
  std::string s;
  for (std::uint32_t i = 0; i < VK_UUID_SIZE; ++i) {
    char part[3];
    std::snprintf(part, sizeof part, "%02x", u[i]);
    s += part;
  }
  return s;
}

json Limits(const VkPhysicalDeviceLimits& l) {
  json out = json::object();
#define NEVR_X(name) out[#name] = l.name;
#define NEVR_XA(name, n)                          \
  {                                               \
    json a = json::array();                       \
    for (int i = 0; i < (n); ++i) a.push_back(l.name[i]); \
    out[#name] = a;                               \
  }
  NEVR_HWDUMP_VK_LIMITS(NEVR_X, NEVR_XA)
#undef NEVR_X
#undef NEVR_XA
  return out;
}

json Features(const VkPhysicalDeviceFeatures& f) {
  json out = json::object();
#define NEVR_X(name) out[#name] = f.name == VK_TRUE;
  NEVR_HWDUMP_VK_FEATURES(NEVR_X)
#undef NEVR_X
  return out;
}

}  // namespace

json ComposeVulkan() {
  json out = json::object();
  ::dlerror();
  void* lib = ::dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) {
    const std::string e = DlError("dlopen libvulkan.so");
    for (const char* k : {"instance_version", "instance_extensions", "instance_layers", "devices"}) out[k] = Fail("libvulkan", e);
    return out;
  }
  auto gipa = Sym<PFN_vkGetInstanceProcAddr>(lib, "vkGetInstanceProcAddr");
  if (gipa == nullptr) {
    const std::string e = DlError("dlsym vkGetInstanceProcAddr");
    for (const char* k : {"instance_version", "instance_extensions", "instance_layers", "devices"}) out[k] = Fail("libvulkan", e);
    return out;
  }
  auto enum_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
  std::uint32_t api = VK_API_VERSION_1_0;
  if (enum_version != nullptr && enum_version(&api) == VK_SUCCESS) {
    out["instance_version"] = Ok("vkEnumerateInstanceVersion",
                                 std::to_string(VK_VERSION_MAJOR(api)) + "." + std::to_string(VK_VERSION_MINOR(api)) +
                                     "." + std::to_string(VK_VERSION_PATCH(api)));
  } else {
    out["instance_version"] = Fail("vkEnumerateInstanceVersion", "absent (a Vulkan 1.0 loader)");
  }
  auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
  auto enum_layers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(gipa(nullptr, "vkEnumerateInstanceLayerProperties"));
  std::uint32_t n = 0;
  if (enum_ext != nullptr && enum_ext(nullptr, &n, nullptr) == VK_SUCCESS) {
    std::vector<VkExtensionProperties> v(n);
    enum_ext(nullptr, &n, v.data());
    json list = json::array();
    for (std::uint32_t i = 0; i < n; ++i) list.push_back({{"name", v[i].extensionName}, {"specVersion", v[i].specVersion}});
    out["instance_extensions"] = Ok("vkEnumerateInstanceExtensionProperties", list);
  } else {
    out["instance_extensions"] = Fail("vkEnumerateInstanceExtensionProperties", "unavailable or failed");
  }
  n = 0;
  if (enum_layers != nullptr && enum_layers(&n, nullptr) == VK_SUCCESS) {
    std::vector<VkLayerProperties> v(n);
    enum_layers(&n, v.data());
    json list = json::array();
    for (std::uint32_t i = 0; i < n; ++i) list.push_back({{"name", v[i].layerName}, {"description", v[i].description}});
    out["instance_layers"] = Ok("vkEnumerateInstanceLayerProperties", list);
  } else {
    out["instance_layers"] = Fail("vkEnumerateInstanceLayerProperties", "unavailable or failed");
  }

  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  if (create == nullptr) {
    out["devices"] = Fail("vkCreateInstance", "not resolvable");
    return out;
  }
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "nevr-hwdump";
  app.apiVersion = api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult cr = create(&ci, nullptr, &instance);
  if (cr != VK_SUCCESS) {
    out["devices"] = Fail("vkCreateInstance", VkError("vkCreateInstance", cr));
    return out;
  }
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
  auto enum_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  auto feats = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance, "vkGetPhysicalDeviceFeatures"));
  auto mem = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
  auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  auto dev_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
  std::uint32_t count = 0;
  VkResult er = enum_devices != nullptr ? enum_devices(instance, &count, nullptr) : VK_ERROR_INITIALIZATION_FAILED;
  if (er != VK_SUCCESS || props == nullptr || feats == nullptr || mem == nullptr || queues == nullptr || dev_ext == nullptr) {
    out["devices"] = Fail("vkEnumeratePhysicalDevices", VkError("vkEnumeratePhysicalDevices", er));
  } else {
    std::vector<VkPhysicalDevice> devices(count);
    enum_devices(instance, &count, devices.data());
    json list = json::array();
    for (VkPhysicalDevice d : devices) {
      VkPhysicalDeviceProperties p{};
      props(d, &p);
      VkPhysicalDeviceFeatures f{};
      feats(d, &f);
      json e = json::object();
      e["deviceName"] = p.deviceName;
      e["apiVersion"] = std::to_string(VK_VERSION_MAJOR(p.apiVersion)) + "." + std::to_string(VK_VERSION_MINOR(p.apiVersion)) + "." +
                        std::to_string(VK_VERSION_PATCH(p.apiVersion));
      e["driverVersion"] = p.driverVersion;
      e["vendorID"] = p.vendorID;
      e["deviceID"] = p.deviceID;
      e["deviceType"] = static_cast<int>(p.deviceType);
      e["pipelineCacheUUID"] = Uuid(p.pipelineCacheUUID);
      e["limits"] = Limits(p.limits);
      e["sparseProperties"] = {{"residencyStandard2DBlockShape", p.sparseProperties.residencyStandard2DBlockShape == VK_TRUE},
                               {"residencyNonResidentStrict", p.sparseProperties.residencyNonResidentStrict == VK_TRUE}};
      e["features"] = Features(f);
      VkPhysicalDeviceMemoryProperties m{};
      mem(d, &m);
      json heaps = json::array();
      for (std::uint32_t i = 0; i < m.memoryHeapCount; ++i) heaps.push_back({{"size", m.memoryHeaps[i].size}, {"flags", m.memoryHeaps[i].flags}});
      json types = json::array();
      for (std::uint32_t i = 0; i < m.memoryTypeCount; ++i) types.push_back({{"heapIndex", m.memoryTypes[i].heapIndex}, {"propertyFlags", m.memoryTypes[i].propertyFlags}});
      e["memory"] = {{"heaps", heaps}, {"types", types}};
      std::uint32_t qn = 0;
      queues(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> q(qn);
      queues(d, &qn, q.data());
      json ql = json::array();
      for (const auto& fam : q) ql.push_back({{"queueFlags", fam.queueFlags}, {"queueCount", fam.queueCount}, {"timestampValidBits", fam.timestampValidBits}});
      e["queueFamilies"] = ql;
      std::uint32_t xn = 0;
      if (dev_ext(d, nullptr, &xn, nullptr) == VK_SUCCESS) {
        std::vector<VkExtensionProperties> xs(xn);
        dev_ext(d, nullptr, &xn, xs.data());
        json xl = json::array();
        for (const auto& x : xs) xl.push_back({{"name", x.extensionName}, {"specVersion", x.specVersion}});
        e["extensions"] = Ok("vkEnumerateDeviceExtensionProperties", xl);
      } else {
        e["extensions"] = Fail("vkEnumerateDeviceExtensionProperties", "failed");
      }
      list.push_back(e);
    }
    out["devices"] = Ok("vkGetPhysicalDeviceProperties/Features/MemoryProperties/QueueFamilyProperties", list);
  }
  if (destroy != nullptr) destroy(instance, nullptr);
  return out;
}

// ---- vrapi.* ----------------------------------------------------------------------------------------------

json ComposeVrapi(const JniSession& jni, const GameCapture& capture) {
  json out = json::object();
  const char* keys[] = {"version_string", "system_property_int", "system_property_float", "system_property_float_array",
                        "system_status_int"};
  std::string blocked;
  if (!capture.initialize_seen) {
    blocked = "vrapi_Initialize was not observed (hook not installed or not yet called)";
  } else if (capture.initialize_status != 0) {
    blocked = "vrapi_Initialize returned " + std::to_string(capture.initialize_status);
  } else if (!jni.attached() || jni.activity() == nullptr) {
    blocked = "no attached JNIEnv or no activity for this thread's ovrJava";
  }
  ::dlerror();
  void* lib = ::dlopen("libvrapi.so", RTLD_NOW | RTLD_NOLOAD);
  if (lib == nullptr && blocked.empty()) blocked = DlError("dlopen libvrapi.so (RTLD_NOLOAD)");

  using VersionString = const char* (*)();
  auto version = lib != nullptr ? Sym<VersionString>(lib, "vrapi_GetVersionString") : nullptr;
  out["version_string"] = version != nullptr ? Ok("vrapi_GetVersionString", std::string(version()))
                                             : Fail("vrapi_GetVersionString", lib == nullptr ? blocked : DlError("dlsym"));
  if (!blocked.empty()) {
    for (std::size_t i = 1; i < sizeof keys / sizeof keys[0]; ++i) out[keys[i]] = Fail(keys[i], blocked);
    return out;
  }
  // This thread's own ovrJava: the game's VM and activity, with the JNIEnv of this thread.
  const OvrJava java{jni.vm(), jni.env(), jni.activity()};
  using GetInt = int (*)(const OvrJava*, int);
  using GetFloat = float (*)(const OvrJava*, int);
  using GetFloatArray = int (*)(const OvrJava*, int, float*, int);
  auto get_int = Sym<GetInt>(lib, "vrapi_GetSystemPropertyInt");
  auto get_float = Sym<GetFloat>(lib, "vrapi_GetSystemPropertyFloat");
  auto get_array = Sym<GetFloatArray>(lib, "vrapi_GetSystemPropertyFloatArray");
  auto get_status = Sym<GetInt>(lib, "vrapi_GetSystemStatusInt");
  // Only the IDs libr15 itself reads through each getter (call sites in docs/adr/0006-quest-hardware-dump.md):
  // an ID the game never uses could assert inside libvrapi.
  json ints = json::object();
  for (int id : {0, 5, 6, 7, 8, 0xf, 0x40}) {
    const std::string src = "vrapi_GetSystemPropertyInt(" + std::to_string(id) + ")";
    ints[std::to_string(id)] = get_int != nullptr ? Ok(src, get_int(&java, id)) : Fail(src, "not exported");
  }
  out["system_property_int"] = ints;
  out["system_property_float"] = {
      {"4", get_float != nullptr ? Ok("vrapi_GetSystemPropertyFloat(4)", get_float(&java, 4))
                                 : Fail("vrapi_GetSystemPropertyFloat(4)", "not exported")}};
  if (get_int != nullptr && get_array != nullptr) {
    const int count = get_int(&java, 0x40);
    if (count >= 1 && count <= 31) {  // the bound libr15 applies before the same call (0x1a71d14)
      float rates[31] = {};
      const int got = get_array(&java, 0x41, rates, count);
      json r = json::array();
      for (int i = 0; i < got && i < count; ++i) r.push_back(rates[i]);
      out["system_property_float_array"] = {{"0x41", Ok("vrapi_GetSystemPropertyFloatArray(0x41)", r)}};
    } else {
      out["system_property_float_array"] = {
          {"0x41", Fail("vrapi_GetSystemPropertyFloatArray(0x41)", "count (0x40) out of 1..31: " + std::to_string(count))}};
    }
  } else {
    out["system_property_float_array"] = {{"0x41", Fail("vrapi_GetSystemPropertyFloatArray", "not exported")}};
  }
  out["system_status_int"] = {
      {"1", get_status != nullptr ? Ok("vrapi_GetSystemStatusInt(1)", get_status(&java, 1))
                                  : Fail("vrapi_GetSystemStatusInt(1)", "not exported")}};
  return out;
}

// ---- gpu.gl.* (stage "gl") --------------------------------------------------------------------------------

json ComposeGl() {
  json out = json::object();
  const auto fail_all = [&](const std::string& e) {
    for (const char* k : {"egl", "strings", "extensions", "limits"}) out[k] = Fail("EGL/GLES", e);
    return out;
  };
  ::dlerror();
  void* egl = ::dlopen("libEGL.so", RTLD_NOW);
  void* gles = ::dlopen("libGLESv3.so", RTLD_NOW);
  if (egl == nullptr || gles == nullptr) return fail_all(DlError("dlopen libEGL.so/libGLESv3.so"));
  auto get_display = Sym<EGLDisplay (*)(EGLNativeDisplayType)>(egl, "eglGetDisplay");
  auto initialize = Sym<EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*)>(egl, "eglInitialize");
  auto query = Sym<const char* (*)(EGLDisplay, EGLint)>(egl, "eglQueryString");
  auto choose = Sym<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>(egl, "eglChooseConfig");
  auto pbuffer = Sym<EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*)>(egl, "eglCreatePbufferSurface");
  auto context = Sym<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>(egl, "eglCreateContext");
  auto make_current = Sym<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(egl, "eglMakeCurrent");
  auto destroy_surface = Sym<EGLBoolean (*)(EGLDisplay, EGLSurface)>(egl, "eglDestroySurface");
  auto destroy_context = Sym<EGLBoolean (*)(EGLDisplay, EGLContext)>(egl, "eglDestroyContext");
  auto release_thread = Sym<EGLBoolean (*)()>(egl, "eglReleaseThread");
  auto get_error = Sym<EGLint (*)()>(egl, "eglGetError");
  auto gl_string = Sym<const GLubyte* (*)(GLenum)>(gles, "glGetString");
  auto gl_stringi = Sym<const GLubyte* (*)(GLenum, GLuint)>(gles, "glGetStringi");
  auto gl_integer = Sym<void (*)(GLenum, GLint*)>(gles, "glGetIntegerv");
  if (get_display == nullptr || initialize == nullptr || query == nullptr || choose == nullptr || pbuffer == nullptr ||
      context == nullptr || make_current == nullptr || destroy_surface == nullptr || destroy_context == nullptr ||
      release_thread == nullptr || get_error == nullptr || gl_string == nullptr || gl_stringi == nullptr ||
      gl_integer == nullptr) {
    return fail_all("an EGL/GLES entry point is not exported");
  }
  const auto egl_error = [&](const char* call) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%04x", static_cast<unsigned>(get_error()));
    return std::string(call) + " failed, eglGetError " + hex;
  };
  EGLDisplay dpy = get_display(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || initialize(dpy, &major, &minor) != EGL_TRUE) return fail_all(egl_error("eglInitialize"));
  // eglTerminate is never called: the display may be shared with anything else in the process.
  json e = json::object();
  e["version"] = std::to_string(major) + "." + std::to_string(minor);
  const std::pair<const char*, EGLint> egl_strings[] = {
      {"vendor", EGL_VENDOR}, {"version_string", EGL_VERSION}, {"client_apis", EGL_CLIENT_APIS}, {"extensions", EGL_EXTENSIONS}};
  for (const auto& [name, id] : egl_strings) {
    const char* s = query(dpy, id);
    e[name] = s != nullptr ? s : "";
  }
  out["egl"] = Ok("eglInitialize + eglQueryString", e);

  const EGLint config_attrs[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
  EGLConfig config = nullptr;
  EGLint num = 0;
  if (choose(dpy, config_attrs, &config, 1, &num) != EGL_TRUE || num < 1) {
    const std::string err = egl_error("eglChooseConfig");
    for (const char* k : {"strings", "extensions", "limits"}) out[k] = Fail("GLES", err);
    return out;
  }
  const EGLint surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLSurface surface = pbuffer(dpy, config, surface_attrs);
  const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext ctx = surface != EGL_NO_SURFACE ? context(dpy, config, EGL_NO_CONTEXT, context_attrs) : EGL_NO_CONTEXT;
  if (surface == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || make_current(dpy, surface, surface, ctx) != EGL_TRUE) {
    const std::string err = egl_error("eglCreatePbufferSurface/eglCreateContext/eglMakeCurrent");
    for (const char* k : {"strings", "extensions", "limits"}) out[k] = Fail("GLES", err);
  } else {
    const auto text = [&](GLenum id) {
      const GLubyte* s = gl_string(id);
      return s != nullptr ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    out["strings"] = Ok("glGetString", {{"GL_VENDOR", text(GL_VENDOR)}, {"GL_RENDERER", text(GL_RENDERER)},
                                        {"GL_VERSION", text(GL_VERSION)},
                                        {"GL_SHADING_LANGUAGE_VERSION", text(GL_SHADING_LANGUAGE_VERSION)}});
    GLint n = 0;
    gl_integer(GL_NUM_EXTENSIONS, &n);
    json exts = json::array();
    for (GLint i = 0; i < n; ++i) {
      const GLubyte* s = gl_stringi(GL_EXTENSIONS, static_cast<GLuint>(i));
      if (s != nullptr) exts.push_back(reinterpret_cast<const char*>(s));
    }
    out["extensions"] = Ok("glGetStringi(GL_EXTENSIONS)", exts);
    json limits = json::object();
    const std::pair<const char*, GLenum> gl_limits[] = {
        {"GL_MAX_TEXTURE_SIZE", GL_MAX_TEXTURE_SIZE},
        {"GL_MAX_RENDERBUFFER_SIZE", GL_MAX_RENDERBUFFER_SIZE},
        {"GL_MAX_SAMPLES", GL_MAX_SAMPLES},
        {"GL_MAX_TEXTURE_IMAGE_UNITS", GL_MAX_TEXTURE_IMAGE_UNITS},
        {"GL_MAX_VERTEX_ATTRIBS", GL_MAX_VERTEX_ATTRIBS},
        {"GL_MAX_UNIFORM_BUFFER_BINDINGS", GL_MAX_UNIFORM_BUFFER_BINDINGS},
        {"GL_MAX_ARRAY_TEXTURE_LAYERS", GL_MAX_ARRAY_TEXTURE_LAYERS},
        {"GL_MAX_COLOR_ATTACHMENTS", GL_MAX_COLOR_ATTACHMENTS},
        {"GL_MAX_DRAW_BUFFERS", GL_MAX_DRAW_BUFFERS}};
    for (const auto& [name, id] : gl_limits) {
      GLint v = 0;
      gl_integer(id, &v);
      limits[name] = v;
    }
    GLint dims[2] = {0, 0};
    gl_integer(GL_MAX_VIEWPORT_DIMS, dims);
    limits["GL_MAX_VIEWPORT_DIMS"] = {dims[0], dims[1]};
    out["limits"] = Ok("glGetIntegerv", limits);
  }
  // Teardown: unbind, destroy what was made, release this thread's EGL state.
  make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surface != EGL_NO_SURFACE) destroy_surface(dpy, surface);
  if (ctx != EGL_NO_CONTEXT) destroy_context(dpy, ctx);
  release_thread();
  return out;
}

// ---- openxr.* (stage "openxr") ----------------------------------------------------------------------------

json ComposeOpenXr() {
  json out = json::object();
  ::dlerror();
  void* lib = ::dlopen("libopenxr_loader.so", RTLD_NOW);
  if (lib == nullptr) {
    const std::string e = DlError("dlopen libopenxr_loader.so");
    out["loader"] = Fail("dlopen libopenxr_loader.so", e);
    for (const char* k : {"instance_extensions", "api_layers", "system"}) out[k] = Fail("OpenXR", "loader unavailable: " + e);
    return out;
  }
  const bool gipa = ::dlsym(lib, "xrGetInstanceProcAddr") != nullptr;
  out["loader"] = Ok("dlopen libopenxr_loader.so", {{"xrGetInstanceProcAddr", gipa}});
  // The OpenXR headers are not part of the NDK and are not vendored here, so the structures these calls take
  // are not declared in this build.
  for (const char* k : {"instance_extensions", "api_layers", "system"}) {
    out[k] = Fail("OpenXR", "loader present; querying it needs the OpenXR headers, which this build does not vendor");
  }
  return out;
}

}  // namespace nevr_quest::hwdump
