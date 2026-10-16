#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define PGSIZE 4096
#define NPAGES 64
#define PREFIX "Here it is: "

int
main(int argc, char *argv[])
{
  int prefix_len = strlen(PREFIX);
  int total_size = NPAGES * PGSIZE;

  char *start = sbrk(0);

  if (sbrk(total_size) == (char *)-1) {
    exit(1);
  }

  char *end = start + total_size;

  for (char *p = start; p <= end - prefix_len; p++) {
    if (*p == 'H' && memcmp(p, PREFIX, prefix_len) == 0) {
      char *secret = p + prefix_len;
      printf("%s\n", secret);
      exit(0);
    }
  }

  exit(1);
}
