#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREATE 0x200
#define O_TRUNC  0x400
// Console only: one line of unechoed input, edited by the caller.
// Ends on newline/EOF or last close. Close before forking a command.
#define O_CONSOLE_EDIT 0x800
