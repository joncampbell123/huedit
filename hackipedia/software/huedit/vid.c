
#include "common.h"

int curses_tty_fd = -1;
int curses_has_mouse = 1;
int screen_width = 80,screen_height = 25;
int curses_can_change_colors = 0;
int curses_with_color = 0;

#if _V_ncursesw == 1 || _V_ncurses == 1
WINDOW *ncurses_window = NULL;
#endif

void InitVid() {
#if _V_ncursesw == 1 || _V_ncurses == 1
	/* make sure STDOUT is a TTY */
	if (!isatty(1) || !isatty(0))
		Fatal(_HERE_ "ncurses: STDIN or STDOUT redirected away from terminal");

	/* before ncurses() can redirect STDOUT away from us, dup the TTY handle.
	 * some hacks and workarounds we do rely heavily on being able to bypass ncurses */
	if ((curses_tty_fd = dup(1)) < 0)
		Fatal(_HERE_ "ncurses: cannot dup tty handle");

	if ((ncurses_window=initscr()) == NULL)
		Fatal(_HERE_ "ncurses: initscr() failed");

	raw();
	noecho();
	keypad(ncurses_window,TRUE);

	screen_width = COLS;
	screen_height = LINES;
	Debug(_HERE_ "ncurses terminal is %d x %d",screen_width,screen_height);

	curses_with_color = has_colors();
	if (curses_with_color) {
		Debug(_HERE_ "ncurses has colors available");
		start_color();
	}

	curses_can_change_colors = can_change_color();
	if (curses_can_change_colors)
		Debug(_HERE_ "ncurses says the colors are changeable");

	/* we want xterm/PuTTY mouse input too */
	mousemask(ALL_MOUSE_EVENTS,NULL);
	mouseinterval(0);
	halfdelay(1); /* so that ESC by itself is possible */
#endif
}

void FreeVid() {
#if _V_ncursesw == 1 || _V_ncurses == 1
	close(curses_tty_fd); curses_tty_fd = -1;
	delwin(ncurses_window);
	endwin();
#endif
}

