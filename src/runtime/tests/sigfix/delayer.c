/* A delay-load descriptor (data directory 13) laid down by hand: this linker has no delay-load support. The
 * descriptor is the attribute word (1: RVAs, not addresses), the DLL name RVA and six zero words; a zero descriptor
 * ends the table. The linker does not point the data directory at it: tools/gen_plugin_signature_fixtures.py does,
 * in its copy of the built file. */
__asm__(".section .didat,\"dr\"\n"
        "nevr_delay_desc:\n"
        ".long 1\n"
        ".rva nevr_delay_name\n"
        ".fill 24,1,0\n"
        ".fill 32,1,0\n"
        ".section .rdata,\"dr\"\n"
        "nevr_delay_name:\n"
        ".asciz \"dep.dll\"\n"
        ".text\n");

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return 1;
}
