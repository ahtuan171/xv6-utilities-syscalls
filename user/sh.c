// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/param.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS    10
#define CMDBUFSIZE 100

#define HISTORY_CAPACITY 10

// Only the parent shell inserts entries. Strings never alias the input buffer.
static struct {
  struct {
    uint64 id;
    char command[CMDBUFSIZE];
  } entries[HISTORY_CAPACITY];
  int first;
  int count;
  uint64 last_id;
} history;
static char history_draft[CMDBUFSIZE];

static const char *
history_command(int index)
{
  return history.entries[(history.first + index) % HISTORY_CAPACITY].command;
}

static int
history_add(const char *command)
{
  int slot;

  if (strlen(command) >= CMDBUFSIZE) {
    fprintf(2, "history: command too long\n");
    return -1;
  }
  if (history.last_id == ~(uint64)0) {
    fprintf(2, "history: command ID limit reached\n");
    return -1;
  }
  if (history.count == HISTORY_CAPACITY) {
    slot = history.first;
    history.first = (history.first + 1) % HISTORY_CAPACITY;
  } else {
    slot = (history.first + history.count++) % HISTORY_CAPACITY;
  }
  history.entries[slot].id = ++history.last_id;
  strcpy(history.entries[slot].command, command);
  return 0;
}

static void
history_print(void)
{
  for (int i = 0; i < history.count; i++) {
    int slot = (history.first + i) % HISTORY_CAPACITY;
    printf("%lu %s\n", history.entries[slot].id, history_command(i));
  }
}

// Normalize only the outer whitespace, using the parser's whitespace set.
static void
trim_command(char *buf)
{
  int start = 0, end = strlen(buf);

  while (start < end && strchr(" \t\r\n\v", buf[start]))
    start++;
  while (end > start && strchr(" \t\r\n\v", buf[end - 1]))
    end--;
  memmove(buf, buf + start, end - start);
  buf[end - start] = 0;
}

// Resolve a standalone reference before history_add can evict its source.
// Compound commands remain the parser's responsibility; expansion is not recursive.
static int
history_expand(char *buf)
{
  const char *command = 0;
  uint64 id = 0;

  if (buf[0] != '!')
    return 0;
  for (int i = 1; buf[i]; i++)
    if (strchr(" \t\r\n\v<|>&;()", buf[i]))
      return 0;

  if (strcmp(buf, "!!") == 0) {
    if (history.count)
      command = history_command(history.count - 1);
  } else {
    if (buf[1] == 0)
      goto invalid;
    for (int i = 1; buf[i]; i++) {
      int digit = buf[i] - '0';
      if (digit < 0 || digit > 9 || id > (~(uint64)0 - digit) / 10)
        goto invalid;
      id = id * 10 + digit;
    }
    if (id == 0)
      goto invalid;
    for (int i = 0; i < history.count; i++) {
      int slot = (history.first + i) % HISTORY_CAPACITY;
      if (history.entries[slot].id == id) {
        command = history_command(i);
        break;
      }
    }
  }
  if (command == 0) {
    fprintf(2, "history: event not found: %s\n", buf);
    return -1;
  }
  strcpy(buf, command);
  printf("%s\n", buf);
  return 0;

invalid:
  fprintf(2, "history: invalid reference: %s\n", buf);
  return -1;
}

// Save the prompt origin for replacing a line, including soft-wrapped rows
// still in the viewport. No terminal dimensions or additional syscall needed.
static void
editor_prompt(void)
{
  write(2, "\0337$ ", 4);
}

static void
editor_redraw(const char *buf, int len)
{
  write(1, "\0338\033[J", 5);
  write(2, "$ ", 2);
  write(1, buf, len);
}

enum escape_state {
  ESC_NONE,
  ESC_START,
  ESC_INTERMEDIATE,
  ESC_CSI,
  ESC_PARAMS,
  ESC_SS3,
  ESC_STRING,
  ESC_STRING_END
};
enum history_key { KEY_TEXT, KEY_IGNORE, KEY_UP, KEY_DOWN };

// Each call consumes at most the byte already read by getcmd. No lookahead
// read, byte buffer, or assumption that an escape sequence arrives together.
static int
history_key(unsigned char c, int *state)
{
  if (c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\x7f' ||
      c == ('U' - '@')) {
    *state = ESC_NONE;
    return KEY_TEXT;
  }
  if (*state == ESC_STRING || *state == ESC_STRING_END) {
    if (c == '\a' || (*state == ESC_STRING_END && c == '\\'))
      *state = ESC_NONE;
    else
      *state = c == '\033' ? ESC_STRING_END : ESC_STRING;
    return KEY_IGNORE;
  }
  if (c == '\033') {
    *state = ESC_START;
    return KEY_IGNORE;
  }
  switch (*state) {
  case ESC_NONE:
    return KEY_TEXT;
  case ESC_START:
    if (c == '[')
      *state = ESC_CSI;
    else if (c == 'O')
      *state = ESC_SS3;
    else if (c == ']' || c == 'P' || c == '^' || c == '_')
      *state = ESC_STRING;
    else {
      *state = c >= 0x20 && c <= 0x2f ? ESC_INTERMEDIATE : ESC_NONE;
      // A standalone ESC must not swallow the next ordinary character.
      // Let getcmd process this byte once; keep recognized sequences ignored.
      if (*state == ESC_NONE && c >= 0x20 && c <= 0x7e)
        return KEY_TEXT;
    }
    return KEY_IGNORE;
  case ESC_INTERMEDIATE:
    if (c < 0x20 || c > 0x2f)
      *state = ESC_NONE;
    return KEY_IGNORE;
  default: // CSI, SS3, or a parameterized unsupported sequence
    if (c >= 0x40 && c <= 0x7e) {
      int arrow = *state != ESC_PARAMS;
      *state = ESC_NONE;
      if (arrow && c == 'A')
        return KEY_UP;
      if (arrow && c == 'B')
        return KEY_DOWN;
    } else {
      *state = c >= 0x20 && c <= 0x3f ? ESC_PARAMS : ESC_NONE;
    }
    return KEY_IGNORE;
  }
}

static int
history_navigate(int key, char *buf, int *len, int *position)
{
  const char *command;

  if (key == KEY_UP) {
    if (*position == 0)
      return 0;
    if (*position == history.count)
      strcpy(history_draft, buf);
    (*position)--;
  } else {
    if (*position == history.count)
      return 0;
    (*position)++;
  }
  command =
    *position == history.count ? history_draft : history_command(*position);
  strcpy(buf, command);
  *len = strlen(buf);
  editor_redraw(buf, *len);
  return 1;
}

// Check the type before reading: a device such as /console may block forever.
int
is_elf_program(const char *path)
{
  unsigned char magic[4];
  struct stat st;
  int fd = open(path, O_RDONLY);
  int n = 0;

  if (fd < 0)
    return 0;
  if (fstat(fd, &st) == 0 && st.type == T_FILE)
    n = read(fd, magic, sizeof(magic));
  close(fd);
  return n == sizeof(magic) && magic[0] == 0x7f && magic[1] == 'E' &&
         magic[2] == 'L' && magic[3] == 'F';
}

int
prefix_match(const char *name, const char *prefix)
{
  while (*prefix) {
    if (*name++ != *prefix++)
      return 0;
  }
  return 1;
}

// Scan once for a unique match, or again to print all matches. Keep the path
// used for lookups separate from the basename inserted into the command.
int
scan_completions(const char *dir, const char *prefix, int is_command,
                 int path_command, int list, char *first)
{
  struct dirent de;
  struct stat st;
  char name[DIRSIZ + 2], path[MAXPATH];
  int fd, count = 0;
  int dirlen = strlen(dir);
  int namelen, slash;

  fd = open(dir, O_RDONLY);
  if (fd < 0)
    return -1;
  if (fstat(fd, &st) < 0 || st.type != T_DIR) {
    close(fd);
    return -1;
  }

  slash = dirlen > 0 && dir[dirlen - 1] != '/';
  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (de.inum == 0)
      continue;
    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = 0;
    if (!prefix_match(name, prefix))
      continue;
    if (*prefix == 0 && (strcmp(name, ".") == 0 || strcmp(name, "..") == 0))
      continue;

    namelen = strlen(name);
    if (dirlen + slash + namelen + 1 > sizeof(path))
      continue;
    strcpy(path, dir);
    if (slash)
      path[dirlen] = '/';
    strcpy(path + dirlen + slash, name);
    if (stat(path, &st) < 0)
      continue;
    if (is_command && !(path_command && st.type == T_DIR) &&
        !is_elf_program(path))
      continue;

    // A trailing slash lets the next Tab continue into this directory.
    if (st.type == T_DIR)
      strcpy(name + namelen, "/");
    if (count == 0)
      strcpy(first, name);
    if (list)
      printf("%s ", name);
    count++;
  }
  close(fd);
  return count;
}

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

void
complete_line(char *buf, int *len, int nbuf)
{
  int i = 0, start = *len;
  int command = 1, redir = 0, is_command = 0;
  int base, prefix_len, name_len, count;
  char dir[MAXPATH], prefix[CMDBUFSIZE], first[DIRSIZ + 2];

  // Follow only the token boundaries understood by the existing parser.
  // A redirection target does not consume the command position: >file ec.
  while (i < *len) {
    if (strchr(" \t\r\n\v", buf[i])) {
      i++;
    } else if (strchr("|;&()", buf[i])) {
      command = buf[i] != ')';
      redir = 0;
      i++;
    } else if (buf[i] == '<' || buf[i] == '>') {
      redir = 1;
      i++;
    } else {
      start = i;
      while (i < *len && !strchr(" \t\r\n\v<|>&;()", buf[i]))
        i++;
      if (i == *len) {
        is_command = command && !redir;
        break;
      }
      if (redir)
        redir = 0;
      else
        command = 0;
      start = *len;
    }
  }
  if (start == *len)
    return;

  base = start;
  for (i = start; i < *len; i++)
    if (buf[i] == '/')
      base = i + 1;
  if (base > start) {
    if (base - start >= sizeof(dir))
      return;
    memmove(dir, buf + start, base - start);
    dir[base - start] = 0;
  } else {
    strcpy(dir, is_command ? "/" : ".");
  }
  prefix_len = *len - base;
  if (prefix_len >= sizeof(prefix))
    return;
  memmove(prefix, buf + base, prefix_len);
  prefix[prefix_len] = 0;

  count = scan_completions(dir, prefix, is_command, base > start, 0, first);
  if (count <= 0)
    return;
  if (count == 1) {
    name_len = strlen(first);
    // Reserve space for both the eventual newline and the NUL terminator.
    if (*len + name_len - prefix_len + 2 > nbuf) {
      write(1, "\a", 1);
      return;
    }
    memmove(buf + *len, first + prefix_len, name_len - prefix_len);
    *len += name_len - prefix_len;
    buf[*len] = 0;
    write(1, first + prefix_len, name_len - prefix_len);
    return;
  }

  printf("\n");
  scan_completions(dir, prefix, is_command, base > start, 1, first);
  printf("\n");
  editor_prompt();
  write(1, buf, *len);
}

int fork1(void); // Fork but panics on failure.
void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;
  char path[MAXPATH];

  if (cmd == 0)
    exit(1);

  switch (cmd->type) {
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd *)cmd;
    if (ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    // Preserve local executables and explicit paths; bare names also search /.
    if (strchr(ecmd->argv[0], '/') == 0 &&
        strlen(ecmd->argv[0]) + 2 <= sizeof(path)) {
      path[0] = '/';
      strcpy(path + 1, ecmd->argv[0]);
      exec(path, ecmd->argv);
    }
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    close(rcmd->fd);
    if (open(rcmd->file, rcmd->mode) < 0) {
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    if (fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    if (pipe(p) < 0)
      panic("pipe");
    if (fork1() == 0) {
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if (fork1() == 0) {
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    if (fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf)
{
  struct stat st, console;
  int len = 0, n, overflow = 0;
  int editfd = -1;
  int escape = ESC_NONE, position = history.count, recalled = 0;
  int is_console = fstat(0, &st) == 0 && st.type == T_DEVICE &&
                   stat("/console", &console) == 0 && st.dev == console.dev &&
                   st.ino == console.ino;
  char c;

  // A private open gives the editor a lifetime independent of stdin/stdout.
  // It is never inherited by a command: every return below closes it.
  if (is_console) {
    editfd = open("/console", O_RDONLY | O_CONSOLE_EDIT);
    editor_prompt();
  }
  memset(buf, 0, nbuf);
  history_draft[0] = 0;

  for (;;) {
    n = read(editfd >= 0 ? editfd : 0, &c, 1);
    if (n <= 0) {
      if (editfd >= 0)
        write(1, "\n", 1);
      break;
    }
    if (editfd >= 0) {
      int key = history_key((unsigned char)c, &escape);
      if (key == KEY_IGNORE)
        continue;
      if (key == KEY_UP || key == KEY_DOWN) {
        recalled |= history_navigate(key, buf, &len, &position);
        continue;
      }
      if (c == '\t') {
        complete_line(buf, &len, nbuf);
        continue;
      }
      if (c == '\b' || c == '\x7f' || c == ('U' - '@')) {
        int erase = c == ('U' - '@') ? len : (len > 0);
        while (erase-- > 0) {
          buf[--len] = 0;
          if (!recalled)
            write(1, "\b \b", 3);
        }
        if (recalled)
          editor_redraw(buf, len);
        continue;
      }
    }
    if (c == '\n' || c == '\r') {
      buf[len++] = '\n';
      if (editfd >= 0)
        write(1, "\n", 1);
      break;
    }
    if (len + 2 >= nbuf) {
      if (editfd >= 0)
        write(1, "\a", 1); // reject visibly, allow Backspace/Ctrl+U/Enter
      else
        overflow =
          1; // drain and reject the whole line, never execute fragments
      continue;
    }
    buf[len++] = c;
    buf[len] = 0;
    if (editfd >= 0)
      write(1, &c, 1);
  }
  if (editfd >= 0)
    close(editfd);
  buf[len] = 0;
  if (n < 0)
    return -1;
  if (overflow) {
    fprintf(2, "sh: command too long\n");
    strcpy(buf, "\n");
  }
  return buf[0] == 0 ? -1 : 0;
}

int
main(void)
{
  static char buf[CMDBUFSIZE];
  int fd;

  // Ensure that three file descriptors are open.
  while ((fd = open("console", O_RDWR)) >= 0) {
    if (fd >= 3) {
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while (getcmd(buf, sizeof(buf)) >= 0) {
    trim_command(buf);
    if (buf[0] == 0 || history_expand(buf) < 0)
      continue;
    if (history_add(buf) < 0)
      continue;
    if (strcmp(buf, "history") == 0) {
      history_print();
      continue;
    }
    char *cmd = buf;
    if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' ') {
      // Chdir must be called by the parent, not the child.
      if (chdir(cmd + 3) < 0)
        fprintf(2, "cannot cd %s\n", cmd + 3);
    } else {
      if (fork1() == 0)
        runcmd(parsecmd(cmd));
      wait(0);
    }
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if (pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd *
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd *)cmd;
}

struct cmd *
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd *)cmd;
}

struct cmd *
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd *)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  if (q)
    *q = s;
  ret = *s;
  switch (*s) {
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if (*s == '>') {
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if (eq)
    *eq = s;

  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if (s != es) {
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd *
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while (peek(ps, es, "&")) {
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if (peek(ps, es, ";")) {
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd *
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if (peek(ps, es, "|")) {
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd *
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while (peek(ps, es, "<>")) {
    tok = gettoken(ps, es, 0, 0);
    if (gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch (tok) {
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE | O_TRUNC, 1);
      break;
    case '+': // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd *
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if (!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if (!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd *
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if (peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd *)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while (!peek(ps, es, "|)&;")) {
    if ((tok = gettoken(ps, es, &q, &eq)) == 0)
      break;
    if (tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if (argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd *
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    return 0;

  switch (cmd->type) {
  case EXEC:
    ecmd = (struct execcmd *)cmd;
    for (i = 0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
