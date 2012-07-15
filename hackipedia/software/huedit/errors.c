/* errors.c
 *
 * error, warning, fatal, and debug messages are sent to this code.
 * warnings and errors are displayed on the GUI while debug messages are not.
 * if you want all information logged to a file, simply run the editor with STDERR redirected to file: huedit 2>/log.txt
 */

#include "common.h"

const char str_Debug[]   = "Debug";
const char str_Warning[] = "Warning";
const char str_Error[]   = "Error";
int print_to_stderr = 1;
static char _vblah_tmp[256];

/* TODO: where should these definitions be? */
void draw_single_box(WINDOW *sw,int px,int py,int w,int h);
void draw_single_box_with_fill(WINDOW *sw,int px,int py,int w,int h);
	
static void vblah_gui(const char *what,const char *file,const char *func,int line,const char *msg) {
	int c;
	int cx,cy;
	int lines = 3;
	int message_width = strlen(msg) + 2;
	const char *p,*ep;

	if (message_width > screen_width) {
		lines = ((message_width + screen_width - 3) / (screen_width - 2));
		if (lines == 0) lines++;
		message_width = screen_width;
	}

	cx = (screen_width - message_width) / 2;
	cy = (screen_height - (lines + 2)) / 2;

	WINDOW *sw = newwin(lines + 2,screen_width,cy,cx);
	if (sw == NULL) Fatal(_HERE_ "Cannot make window");

	PANEL *pan = new_panel(sw);
	if (pan == NULL) Fatal(_HERE_ "Cannot make panel");

	attrset(0);
	curs_set(0);
	wattrset(sw,A_BOLD);
	wcolor_set(sw,NCURSES_PAIR_ACTIVE_EDIT,NULL);
	draw_single_box_with_fill(sw,0,0,message_width,lines + 2);
	show_panel(pan);

	cy = 1;
	mvwaddstr(sw,cy,1,what);
	cy += 2;
	for (p=msg;*p;) {
		ep = p;
		while (*ep && ep < (p + message_width - 2)) ep++;
		mvwaddnstr(sw,cy,1,p,(int)(ep - p));
		p = ep;
		cy++;
	}

	wrefresh(sw);
	update_panels();

	do {
		c = getch();
	} while (!(c == 13 || c == 10));

	del_panel(pan);
	delwin(sw);
	update_panels();
	refresh();
}

static void vblah_(const char *what,const char *file,const char *func,int line,const char *msg,va_list va) {
	if (print_to_stderr) {
		fprintf(stderr,"%s [%s(%d): %s]: ",what,file,line,func);
		vfprintf(stderr,msg,va);
		fprintf(stderr,"\n");
	}
	else {
		if (what == str_Debug) return;
		vsnprintf(_vblah_tmp,sizeof(_vblah_tmp)-1,msg,va);
		vblah_gui(what,file,func,line,_vblah_tmp);
	}
}

static void vblah_errno(const char *what,const char *file,const char *func,int line,const char *msg,va_list va) {
	if (print_to_stderr) {
		fprintf(stderr,"%s [%s(%d): %s]: ",what,file,line,func);
		vfprintf(stderr,msg,va);
		fprintf(stderr,"\n    errno(%d): %s",(int)errno,strerror(errno));
		fprintf(stderr,"\n");
	}
	else {
		if (what == str_Debug) return;
		vsnprintf(_vblah_tmp,sizeof(_vblah_tmp)-1,msg,va);
		vblah_gui(what,file,func,line,_vblah_tmp);
	}
}

void Debug(const char *file,const char *func,int line,const char *msg,...) {
	va_list va; va_start(va,msg);
	vblah_(str_Debug,file,func,line,msg,va); va_end(va);
}

void Debug_Errno(const char *file,const char *func,int line,const char *msg,...) {
	va_list va; va_start(va,msg);
	vblah_errno(str_Debug,file,func,line,msg,va); va_end(va);
}

void Warning(const char *file,const char *func,int line,const char *msg,...) {
	va_list va; va_start(va,msg);
	vblah_(str_Warning,file,func,line,msg,va); va_end(va);
}

void Warning_Errno(const char *file,const char *func,int line,const char *msg,...) {
	va_list va; va_start(va,msg);
	vblah_errno(str_Warning,file,func,line,msg,va); va_end(va);
}

void Error(const char *file,const char *func,int line,const char *msg,...) {
	va_list va; va_start(va,msg);
	vblah_(str_Error,file,func,line,msg,va); va_end(va);
}

void Error_Errno(const char *file,const char *func,int line,const char *msg,...) {
	va_list va; va_start(va,msg);
	vblah_errno(str_Error,file,func,line,msg,va); va_end(va);
}

void Fatal(const char *file,const char *func,int line,const char *msg,...) {
#if _V_ncursesw == 1 || _V_ncurses == 1
	endwin();
	print_to_stderr = 1;
#endif
	va_list va; va_start(va,msg);
	vblah_(str_Error,file,func,line,msg,va); va_end(va);
	exit(255);
}

void Fatal_Errno(const char *file,const char *func,int line,const char *msg,...) {
#if _V_ncursesw == 1 || _V_ncurses == 1
	endwin();
	print_to_stderr = 1;
#endif
	va_list va; va_start(va,msg);
	vblah_errno(str_Error,file,func,line,msg,va); va_end(va);
	exit(255);
}

void InitErrSystem() {
	print_to_stderr = !isatty(2);
}

