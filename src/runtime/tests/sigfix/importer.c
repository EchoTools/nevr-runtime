/* Imports Dep from dep.dll. */
__declspec(dllimport) int Dep(void);

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return Dep();
}
