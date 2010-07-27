/* errors.c
 *
 * for posix systems with stdout/stderr
 */

#include "common.h"

const char str_Debug[]   = "Debug";
const char str_Warning[] = "Warning";
const char str_Error[]   = "Error";

static void vblah_(const char *what,const char *file,const char *func,int line,const char *msg,va_list va) {
	fprintf(stderr,"%s [%s(%d): %s]: ",what,file,line,func);
	vfprintf(stderr,msg,va);
	fprintf(stderr,"\n");
}

static void vblah_errno(const char *what,const char *file,const char *func,int line,const char *msg,va_list va) {
	fprintf(stderr,"%s [%s(%d): %s]: ",what,file,line,func);
	vfprintf(stderr,msg,va);
	fprintf(stderr,"\n    errno(%d): %s",(int)errno,strerror(errno));
	fprintf(stderr,"\n");
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
#endif
	va_list va; va_start(va,msg);
	vblah_(str_Error,file,func,line,msg,va); va_end(va);
	exit(255);
}

void Fatal_Errno(const char *file,const char *func,int line,const char *msg,...) {
#if _V_ncursesw == 1 || _V_ncurses == 1
	endwin();
#endif
	va_list va; va_start(va,msg);
	vblah_errno(str_Error,file,func,line,msg,va); va_end(va);
	exit(255);
}

