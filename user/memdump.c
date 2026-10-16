#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

void memdump(char *fmt, char *data, int len);

int
main(int argc, char *argv[])
{
  if (argc == 1) {
    printf("Example 1:\n");
    int a[2] = {61810, 2026};
    memdump("ii", (char *)a, sizeof(a));

    printf("Example 2:\n");
    memdump("S", "a string", sizeof("a string"));

    printf("Example 3:\n");
    char *s = "another";
    memdump("s", (char *)&s, sizeof(s));

    struct sss {
      char *ptr;
      int num1;
      short num2;
      char byte;
      char bytes[8];
    } example;

    example.ptr = "hello";
    example.num1 = 1819438967;
    example.num2 = 100;
    example.byte = 'z';
    strcpy(example.bytes, "xyzzy");

    printf("Example 4:\n");
    memdump("pihcS", (char *)&example, sizeof(example));

    printf("Example 5:\n");
    memdump("sccccc", (char *)&example, sizeof(example));
  } else if (argc == 2) {
    // format in argv[1], up to 512 bytes of data from standard input.
    char data[512];
    int n = 0;
    memset(data, '\0', sizeof(data));
    while (n < sizeof(data)) {
      int nn = read(0, data + n, sizeof(data) - n);
      if (nn <= 0)
        break;
      n += nn;
    }
    memdump(argv[1], data, n);
  } else {
    printf("Usage: memdump [format]\n");
    exit(1);
  }
  exit(0);
}

void
memdump(char *fmt, char *data, int len)
{
  int off = 0; // số byte của data đã dùng (vị trí đọc tiếp theo)
  char f = 0;  // ký tự format đang xử lý (dùng khi báo lỗi)

  for (int k = 0; fmt[k] != '\0'; k++) {
    f = fmt[k];

    switch (f) {
    case 'i': { // 4 byte -> số nguyên 32-bit, in thập phân
      int v = 0;
      if (len - off < (int)sizeof(v)) {
        goto short_data;
      }
      memmove(&v, data + off, sizeof(v)); // copy byte vào biến
      off += sizeof(v);
      printf("%d\n", v);
      break;
    }
    case 'p': { // 8 byte -> số 64-bit, in hex
      uint64 v = 0;
      if (len - off < (int)sizeof(v)) {
        goto short_data;
      }
      memmove(&v, data + off, sizeof(v));
      off += sizeof(v);
      printf("%lx\n", v); // %lx vì %x của xv6 chỉ in 32-bit
      break;
    }
    case 'h': { // 2 byte -> số nguyên 16-bit, in thập phân
      short v = 0;
      if (len - off < (int)sizeof(v)) {
        goto short_data;
      }
      memmove(&v, data + off, sizeof(v));
      off += sizeof(v);
      printf("%d\n", v);
      break;
    }
    case 'c': { // 1 byte -> ký tự ASCII
      char v = 0;
      if (len - off < (int)sizeof(v)) {
        goto short_data;
      }
      v = data[off];
      off += sizeof(v);
      printf("%c\n", v);
      break;
    }
    case 's': { // 8 byte là con trỏ tới một chuỗi C; in chuỗi đó
      char *v = 0;
      if (len - off < (int)sizeof(v)) {
        goto short_data;
      }
      memmove(&v, data + off, sizeof(v));
      off += sizeof(v);
      printf("%s\n", v);
      break;
    }
    case 'S': { // phần còn lại của data là chuỗi C; dừng ở '\0' hoặc hết len
      int n = 0;
      while (off + n < len && data[off + n] != '\0') {
        n++;
      }
      write(1, data + off, n); // không dùng %s để khỏi đọc lố quá len
      write(1, "\n", 1);
      off = len;
      break;
    }
    default:
      break;
    }
  }
  return;

short_data:
  printf("memdump: not enough data for '%c'\n", f);
}
