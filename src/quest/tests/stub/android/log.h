// Host stand-in for <android/log.h>: the host test supplies the two functions and records
// every line the sentinel would have sent to logcat.
#pragma once

enum {
  ANDROID_LOG_INFO = 4,
  ANDROID_LOG_WARN = 5,
  ANDROID_LOG_ERROR = 6,
  ANDROID_LOG_FATAL = 7,
};

extern "C" {
int __android_log_write(int prio, const char* tag, const char* text);
int __android_log_print(int prio, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
}
