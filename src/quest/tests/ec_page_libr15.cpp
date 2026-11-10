// Stands for libr15: a library that has the sentinel (the ec_page shared object) as a DT_NEEDED, as libr15 has libovrplatformloader.so.
extern "C" __attribute__((visibility("default"))) int libr15_standin() { return 15; }
