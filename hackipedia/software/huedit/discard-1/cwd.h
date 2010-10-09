
extern char cwd[MAX_PATH+1];
#if _OS_linux == 1
extern DIR *cwd_dir;
extern fd_t cwd_fd;
#endif

void CloseCwd();
void OpenCwd();

