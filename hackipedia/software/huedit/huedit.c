
#include "common.h"

int main(int argc,char **argv) {
#if _OS_linux == 1
	if (setlocale(LC_ALL,"ass") == NULL)
		Debug_Errno(_HERE_ "setlocale failed");
#endif

	return 0;
}

