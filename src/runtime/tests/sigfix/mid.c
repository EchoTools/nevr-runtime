/* Exports Mid, which calls Dep from dep.dll. */
__declspec(dllimport) int Dep(void);
__declspec(dllexport) int Mid(void)
{
    return Dep();
}

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return 1;
}
