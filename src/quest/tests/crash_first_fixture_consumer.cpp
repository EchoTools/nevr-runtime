// Host-test fixture: calls libc's sigaction and signal through PLT slots (JUMP_SLOT), as libr15 does when the game
// installs its crash handler (#1605).
#include <signal.h>

extern "C" {
int cffx_sigaction(int signo, const struct sigaction* act, struct sigaction* old) { return sigaction(signo, act, old); }
void (*cffx_signal(int signo, void (*handler)(int)))(int) { return signal(signo, handler); }
}
