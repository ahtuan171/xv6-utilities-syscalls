#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/param.h"
#include "user/user.h"

char *target; // tên file/thư mục cần tìm
char **cmd;   // lệnh sau -exec (trỏ vào argv), 0 nếu không có -exec
int ncmd;     // số phần tử của lệnh

// Trả về phần tên cuối cùng của path: "./a/b" -> "b"
char *
basename(char *path)
{
  char *p = path + strlen(path);
  while (p >= path && *p != '/') {
    p--;
  }
  return p + 1;
}

// Chạy: cmd... path   (fork + exec + wait)
void
run(char *path)
{
  char *argv[MAXARG];
  int pid;

  if (ncmd + 2 > MAXARG) {
    fprintf(2, "find: too many arguments\n");
    return;
  }
  for (int i = 0; i < ncmd; i++) {
    argv[i] = cmd[i];
  }
  argv[ncmd] = path;
  argv[ncmd + 1] = 0; // argv phải kết thúc bằng con trỏ NULL

  pid = fork();
  if (pid < 0) {
    fprintf(2, "find: fork failed\n");
    return;
  }
  if (pid == 0) {
    exec(argv[0], argv);
    // exec chỉ trả về khi lỗi
    fprintf(2, "find: exec %s failed\n", argv[0]);
    exit(1);
  }
  wait(0);
}

void
find(char *path)
{
  char buf[256], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  // Tên khớp (file hoặc thư mục đều tính) -> in hoặc chạy lệnh
  if (strcmp(basename(path), target) == 0) {
    if (cmd == 0) {
      printf("%s\n", path);
    } else {
      run(path);
    }
  }

  // Nếu là thư mục thì đi vào từng mục bên trong
  if (st.type == T_DIR) {
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
      fprintf(2, "find: path too long\n");
      close(fd);
      return;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0) { // ô trống
        continue;
      }
      memmove(p, de.name, DIRSIZ); // name không chắc có '\0' ở cuối
      p[DIRSIZ] = '\0';
      if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
        continue; // tránh đệ quy vô hạn
      }
      find(buf);
    }
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if (argc < 3) {
    fprintf(2, "Usage: find <dir> <name> [-exec cmd ...]\n");
    exit(1);
  }
  target = argv[2];
  if (argc > 3) {
    if (strcmp(argv[3], "-exec") != 0 || argc < 5) {
      fprintf(2, "Usage: find <dir> <name> [-exec cmd ...]\n");
      exit(1);
    }
    cmd = argv + 4;
    ncmd = argc - 4;
  }
  find(argv[1]);
  exit(0);
}
