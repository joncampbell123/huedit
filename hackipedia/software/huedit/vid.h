
extern int screen_width,screen_height;

#if _V_ncursesw == 1 || _V_ncurses == 1
extern WINDOW *ncurses_window;
#endif

void InitVid();
void FreeVid();

