/* Imports Mid from mid.dll, which imports Dep from dep.dll. */
__declspec(dllimport) int Mid(void);

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return Mid();
}
