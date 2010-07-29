
#include "common.h"

int screen_width = 80,screen_height = 25;

#if _V_ncursesw == 1 || _V_ncurses == 1
WINDOW *ncurses_window = NULL;
#endif

void InitVid() {
#if _V_ncursesw == 1 || _V_ncurses == 1
	/* make sure STDOUT is a TTY */
	if (!isatty(1) || !isatty(0))
		Fatal(_HERE_ "ncurses: STDIN or STDOUT redirected away from terminal");

	if ((ncurses_window=initscr()) == NULL)
		Fatal(_HERE_ "ncurses: initscr() failed");

	screen_width = COLS;
	screen_height = LINES;
	Debug(_HERE_ "ncurses terminal is %d x %d\n",screen_width,screen_height);
#endif
}

void FreeVid() {
#if _V_ncursesw == 1 || _V_ncurses == 1
	endwin();
	delwin(ncurses_window);
#endif
}

