#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static double now_ms(void) {
  LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void run(const char* label, const char* node, const char* svc, int flags) {
  struct addrinfo hints; struct addrinfo* res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = flags; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP;
  double t0 = now_ms();
  int rc = getaddrinfo(node, svc, &hints, &res);
  double t1 = now_ms();
  printf("%-34s rc=%d wsaerr=%d elapsed=%.1f ms", label, rc, rc ? WSAGetLastError() : 0, t1 - t0);
  if (rc == 0) {
    for (struct addrinfo* p = res; p; p = p->ai_next) {
      if (p->ai_family == AF_INET) {
        char buf[64]; struct sockaddr_in* a = (struct sockaddr_in*)p->ai_addr;
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
        printf(" -> %s", buf); break;
      }
    }
    freeaddrinfo(res);
  }
  printf("\n"); fflush(stdout);
}

int main(int argc, char** argv) {
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
  int iters = argc > 1 ? atoi(argv[1]) : 3;
  char name[256] = {0}; gethostname(name, sizeof(name)); printf("gethostname=%s\n", name);
  for (int i = 0; i < iters; i++) {
    printf("--- iteration %d ---\n", i + 1);
    run("game call: node=\"\" flags=0", "", "6792", 0);
    run("node=NULL flags=0", NULL, "6792", 0);
    run("node=NULL flags=AI_PASSIVE", NULL, "6792", AI_PASSIVE);
    run("node=hostname flags=0", name, "6792", 0);
  }
  WSACleanup(); return 0;
}
