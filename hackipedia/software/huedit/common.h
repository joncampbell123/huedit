
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "errors.h"

#if _V_ncursesw == 1 || _V_ncurses == 1
#  include <ncurses.h>
#endif

#if _OS_linux == 1
#  include <locale.h>
#endif

