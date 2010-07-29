
#if _OS_linux == 1
#  define _BSD_SOURCE 1
#  define _ATFILE_SOURCE 1
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* some systems like Windows don't think in file descriptors */
typedef int fd_t;
#define FD_CLOSED (int)(-1)

/* maximum number of files we can have open at one time */
#define MAX_FILES	8

#ifndef PATH_SEP
#define PATH_SEP "/"
#endif

#ifndef PATH_SEP_CHAR
#define PATH_SEP_CHAR '/'
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef MAX_PATH
#define MAX_PATH 512
#endif

#if _V_ncursesw == 1 || _V_ncurses == 1
#  include <ncurses.h>
#endif

#if _OS_linux == 1
#  include <locale.h>
#endif

#include "unicode.h"
#include "errors.h"
#include "cwd.h"
#include "vid.h"

