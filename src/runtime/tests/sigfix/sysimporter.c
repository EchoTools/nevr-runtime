/* A plugin whose one import is a System32 library (hid.dll, which the test process does not load). */
__declspec(dllimport) void __stdcall HidD_GetHidGuid(void *);
void(__stdcall *volatile nevr_keep_import)(void *) = HidD_GetHidGuid;

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return nevr_keep_import != 0;
}
