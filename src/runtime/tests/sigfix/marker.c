/* A plugin-shaped DLL with nothing but DllMain. SIGFIX_MARKER makes the body distinct per file so two fixtures never
 * share an Authenticode digest. Built by CMake (CMakeLists.txt here); tools/gen_plugin_signature_fixtures.py signs it. */
const char kMarker[] = SIGFIX_MARKER;

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return kMarker[0] != 0;
}
