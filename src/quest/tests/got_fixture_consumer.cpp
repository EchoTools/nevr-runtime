// Host-test fixture: imports fx_add through a call (a PLT slot, JUMP_SLOT) and
// fx_sub through its address (a GOT slot, GLOB_DAT). Built three times with
// different link flags: BIND_NOW with RELRO (the Quest libraries' shape), BIND_NOW
// without RELRO (a writable GOT page), and lazy binding.
extern "C" {
int fx_add(int a, int b);
int fx_sub(int a, int b);

int fx_call_add(int a, int b) { return fx_add(a, b); }

int (*fx_sub_address())(int, int) { return &fx_sub; }

int fx_call_sub(int a, int b) { return fx_sub_address()(a, b); }
}
