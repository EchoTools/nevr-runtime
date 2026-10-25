// Host-test fixture: calls android_main through a PLT slot (JUMP_SLOT), as the native-activity glue does in libr15.
extern "C" {
void android_main(void* app);
void pefx_call_android_main(void* app) { android_main(app); }
}
