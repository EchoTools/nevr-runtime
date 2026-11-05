#pragma once
// Takes every pthread key the process has left, so the next pthread_key_create fails (a module's install must then refuse and change
// nothing); the destructor gives them back. Used by the q15t host tests of the modules that keep per-thread state in a ThreadWord.
#include <pthread.h>

#include <vector>

class KeyHog {
 public:
  KeyHog() {
    for (;;) {
      pthread_key_t key;
      if (pthread_key_create(&key, nullptr) != 0) break;
      keys_.push_back(key);
    }
  }
  ~KeyHog() {
    for (pthread_key_t key : keys_) pthread_key_delete(key);
  }
  KeyHog(const KeyHog&) = delete;
  KeyHog& operator=(const KeyHog&) = delete;

 private:
  std::vector<pthread_key_t> keys_;
};
