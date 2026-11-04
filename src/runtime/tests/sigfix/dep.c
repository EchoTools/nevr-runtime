/* The library the dependency-tree fixtures name: exports Dep. */
__declspec(dllexport) int Dep(void)
{
    return 7;
}

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return 1;
}
