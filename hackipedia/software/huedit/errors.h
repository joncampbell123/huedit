
#define _HERE_ __FILE__,__PRETTY_FUNCTION__,__LINE__,

void Debug(const char *file,const char *func,int line,const char *msg,...);
void Debug_Errno(const char *file,const char *func,int line,const char *msg,...);
void Warning(const char *file,const char *func,int line,const char *msg,...);
void Warning_Errno(const char *file,const char *func,int line,const char *msg,...);
void Error(const char *file,const char *func,int line,const char *msg,...);
void Error_Errno(const char *file,const char *func,int line,const char *msg,...);
void Fatal(const char *file,const char *func,int line,const char *msg,...);
void Fatal_Errno(const char *file,const char *func,int line,const char *msg,...);
void InitErrSystem();

extern const char str_Debug[];
extern const char str_Warning[];
extern const char str_Error[];

