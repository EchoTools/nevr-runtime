/* capdep_<CAP_INDEX>.dll exports one function, F<CAP_INDEX>, returning CAP_INDEX. */
#define CAP_CAT2(a, b) a##b
#define CAP_CAT(a, b) CAP_CAT2(a, b)

__declspec(dllexport) int CAP_CAT(F, CAP_INDEX)(void)
{
    return CAP_INDEX;
}

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return 1;
}
