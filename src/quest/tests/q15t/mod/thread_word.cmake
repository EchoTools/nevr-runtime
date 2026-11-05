# ThreadWord, the per-thread value that replaces thread_local in the hook paths (header only, so no SRC): each thread sees its own value
q15t_host_test(thread_word_test TEST thread_word_test.cpp LIBS pthread)
