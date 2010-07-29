
#include "common.h"

/* current working directory */
char cwd[MAX_PATH+1];	/* string path */
#if _OS_linux == 1
DIR *cwd_dir = NULL;	/* in Linux we can opendir() and openat() */
fd_t cwd_fd = -1;
#endif

void CloseCwd() {
#if _OS_linux == 1
	if (cwd_dir != NULL) {
		closedir(cwd_dir);
		cwd_dir = NULL;
		cwd_fd = -1;
	}
	else {
		Warning(_HERE_ "CloseCwd() without cwd_dir");
	}
#endif
}

void OpenCwd() {
	if (getcwd(cwd,sizeof(cwd)-1) == NULL)
		Fatal(_HERE_ "getcwd: I'm somewhere where I don't know where I am");
#if _OS_linux == 1
	if ((cwd_dir = opendir(".")) == NULL)
		Fatal(_HERE_ "cannot open current directory");
	if ((cwd_fd = dirfd(cwd_dir)) < 0)
		Fatal(_HERE_ "cannot get current directory handle");
#endif
	Debug(_HERE_ "getcwd: I am in %s",cwd);
}

