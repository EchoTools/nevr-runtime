/* A stand-in for hid.dll: exports the same function and says it was loaded by setting NEVR_TEST_DECOY_LOADED. A test
 * puts it in the plugin's directory after the check; the loader searches that directory before System32 for a name
 * that is not already loaded. */
__declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char *, const char *);
__declspec(dllexport) void __stdcall HidD_GetHidGuid(void *guid)
{
}

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    if (reason == 1) {
        SetEnvironmentVariableA("NEVR_TEST_DECOY_LOADED", "1");
    }
    return 1;
}
