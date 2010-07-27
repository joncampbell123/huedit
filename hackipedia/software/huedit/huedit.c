
#include "common.h"

#if _V_ncursesw == 1 || _V_ncurses == 1
WINDOW *ncurses_window = NULL;
#endif

int main(int argc,char **argv) {
#if _OS_linux == 1
	if (setlocale(LC_ALL,"") == NULL)
		Debug_Errno(_HERE_ "setlocale failed");
#endif

#if _V_ncursesw == 1 || _V_ncurses == 1
	if ((ncurses_window=initscr()) == NULL)
		Fatal(_HERE_ "ncurses: initscr() failed");
#endif

#if _V_ncursesw == 1 || _V_ncurses == 1
	endwin();
	delwin(ncurses_window);
#endif

	return 0;
}

