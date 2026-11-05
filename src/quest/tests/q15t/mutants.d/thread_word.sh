# thread_word mutants: each removes or breaks one rule of src/thread_word.h; tests/thread_word_test.cpp must fail on it.
runm thread_word tw_shared_value      src/thread_word.h 's/^namespace q15t {$/namespace q15t {\ninline std::uintptr_t g_shared_mutant = 0;/;s/return reinterpret_cast<std::uintptr_t>(pthread_getspecific(key_));/return g_shared_mutant;/;s/pthread_setspecific(key_, reinterpret_cast<void\*>(value));/g_shared_mutant = value;/'
runm thread_word tw_set_dropped       src/thread_word.h 's/pthread_setspecific(key_, reinterpret_cast<void\*>(value));/static_cast<void>(value);/'
runm thread_word tw_get_zero          src/thread_word.h 's/return reinterpret_cast<std::uintptr_t>(pthread_getspecific(key_));/return 0;/'
runm thread_word tw_get_unready       src/thread_word.h 's/if (!ready_.load(std::memory_order_acquire)) return 0;//'
runm thread_word tw_set_unready       src/thread_word.h 's/if (!ready_.load(std::memory_order_acquire)) return;//'
runm thread_word tw_create_twice      src/thread_word.h 's/bool ok = ready_.load(std::memory_order_relaxed);/bool ok = false;/'
runm thread_word tw_create_failure_hidden src/thread_word.h 's/ok = pthread_key_create(&key_, nullptr) == 0;/ok = pthread_key_create(\&key_, nullptr) == 0 || true;/'
runm thread_word tw_never_ready       src/thread_word.h 's/ready_.store(ok, std::memory_order_release);/static_cast<void>(ok);/'
