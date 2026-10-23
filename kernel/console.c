//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100       // erase the last output character
#define C(x)      ((x) - '@') // Control-x

//
// send one character to the uart, but don't use
// interrupts or sleep(). safe to be called from
// interrupts, e.g. by printk and to echo input
// characters.
//
void
consputc(int c)
{
  if (c == BACKSPACE) {
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b');
    uartputc_sync(' ');
    uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;

#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r; // next byte to read
  uint w; // end of committed input
  uint e; // end of input being edited

  uint edit_owner; // token of the open console editor, or zero
  uint edit_next;  // tokens distinguish reused file table entries
  int edit_raw;    // accept raw keys until the first newline/EOF
  uint edit_end;   // boundary separating the editor from canonical typeahead
} cons;

// Start one line editor on an otherwise idle console. Refuse pending canonical
// input: it has already been echoed and must retain normal line semantics.
uint
consoleeditopen(void)
{
  uint token = 0;

  acquire(&cons.lock);
  if (cons.edit_owner == 0 && cons.r == cons.e) {
    if (++cons.edit_next == 0)
      ++cons.edit_next;
    token = cons.edit_owner = cons.edit_next;
    cons.edit_raw = 1;
  }
  release(&cons.lock);
  return token;
}

// Last close (including exit/kill) cancels an unfinished editor. Tokens keep
// a delayed close from disabling a newer editor on a reused file table entry.
void
consoleeditclose(uint token)
{
  acquire(&cons.lock);
  if (token != 0 && cons.edit_owner == token) {
    if (cons.edit_raw)
      cons.e = cons.w = cons.r;
    else
      cons.r = cons.edit_end;
    cons.edit_owner = 0;
    cons.edit_raw = 0;
    wakeup(&cons.r);
  }
  release(&cons.lock);
}

//
// user write() system calls to the console go here.
// uses sleep() and UART interrupts.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;

  while (i < n) {
    int nn = sizeof(buf);
    if (nn > n - i)
      nn = n - i;
    if (either_copyin(buf, user_src, src + i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dst indicates whether dst is a user
// or kernel address.
//
static int
consolereadinput(int user_dst, uint64 dst, int n, uint token)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while (n > 0) {
    // wait until interrupt handler has put some
    // input into cons.buffer.
    for (;;) {
      // Ordinary readers cannot consume an editor's raw keys. The editor
      // cannot consume canonical typeahead beyond its own line boundary.
      if (token && (cons.edit_owner != token ||
                    (!cons.edit_raw && cons.r == cons.edit_end))) {
        release(&cons.lock);
        return target - n;
      }
      if (cons.r != cons.w &&
          (cons.edit_owner == 0 || cons.edit_owner == token))
        break;
      if (killed(myproc())) {
        release(&cons.lock);
        return -1;
      }
      sleep_prepare(&cons.r);
      release(&cons.lock);
      sleep();
      acquire(&cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if (c == C('D')) { // end-of-file
      if (n < target) {
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if (either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if (c == '\n') {
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

int
consoleread(int user_dst, uint64 dst, int n)
{
  return consolereadinput(user_dst, dst, n, 0);
}

int
consoleeditread(uint token, uint64 dst, int n)
{
  return consolereadinput(1, dst, n, token);
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  // Only an explicitly opened editor gets unechoed, byte-at-a-time input.
  // End the mode at the line boundary so typeahead for a child is canonical.
  if (cons.edit_owner && cons.edit_raw && c != C('P')) {
    if (c != 0 && cons.e - cons.r < INPUT_BUF_SIZE) {
      c = (c == '\r') ? '\n' : c;
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;
      cons.w = cons.e;
      if (c == '\n' || c == C('D')) {
        cons.edit_raw = 0;
        cons.edit_end = cons.e;
      }
      wakeup(&cons.r);
    }
    release(&cons.lock);
    return;
  }

  switch (c) {
  case C('P'): // Print process list.
    procdump();
    break;
  case C('U'): // Kill line.
    while (cons.e != cons.w &&
           cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != '\n') {
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if (cons.e != cons.w) {
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if (c != 0 && cons.e - cons.r < INPUT_BUF_SIZE) {
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if (c == '\n' || c == C('D') || cons.e - cons.r == INPUT_BUF_SIZE) {
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }

  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
