#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  // pipeline for the parent
  int p1[2];
  // pipeline for the child
  int p2[2];
  pipe(p1);
  pipe(p2);
  char buf1[512] = "FUCKYOUAPEX";
  char buf2[512];
  int pid1 = getpid();
  int pid2 = fork();
  if (pid2 == 0) {
    close(p1[1]);
    close(p2[0]);
    if (read(p1[0], buf2, sizeof(buf2)) != sizeof(buf2)) {
      printf("error reading");
      exit(1);
    }
    close(p1[0]);
    int pid3 = getpid();
    printf("%d: received ping from pid %d\n", pid3, pid1);
    if (write(p2[1], buf2, sizeof(buf2)) != sizeof(buf2)) {
      printf("error writing");
      exit(1);
    }
    close(p2[1]);
  } else if (pid2 > 0) {
    close(p1[0]);
    close(p2[1]);
    if (write(p1[1], buf1, sizeof(buf1)) != sizeof(buf1)) {
      printf("errer writing");
      exit(1);
    }
    close(p1[1]);
    if (read(p2[0], buf1, sizeof(buf2)) != sizeof(buf2)) {
      printf("error reading");
      exit(1);
    }
    close(p2[0]);
    printf("%d: received pong from pid %d\n", pid1, pid2);
  } else {
    exit(1);
  }
  exit(0);
}
