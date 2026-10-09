// Host-test fixture: the library that defines the symbols the consumer imports.
extern "C" {
int fx_add(int a, int b) { return a + b; }
int fx_sub(int a, int b) { return a - b; }
}
