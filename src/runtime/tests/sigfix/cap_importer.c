/* cap<CAP_USED>.dll imports F1..F<CAP_USED>, one function from each of the first CAP_USED capdep libraries. The call
 * to a function past CAP_USED is a constant-false branch, so the linker records no import for it. */
#include "cap_list.h"

#define CAP_DECLARE(n) __declspec(dllimport) int F##n(void);
CAP_LIST(CAP_DECLARE)

#define CAP_TERM(n) +((n) <= CAP_USED ? F##n() : 0)

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    return 0 CAP_LIST(CAP_TERM);
}
