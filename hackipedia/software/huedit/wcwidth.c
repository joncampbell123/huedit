
#if _OS_linux == 1

/* GNU iconv and friends provide a wcwidth() function */
#include <locale.h>
#include <wchar.h>

int unicode_width(int c) {
	int x = wcwidth(c);
	if (x < 0) return 0;
	return x;
}

#else

/* we're on our own */
#  include "wcwidthc.h"

int unicode_zero_width(int c) {
	unsigned int blk = (unsigned int)c >> 9;
	unsigned int base_info_shift = (sizeof(wczerowidth2_cctab[0]) * 8U) - 2;
	unsigned int ent,eno;

	if (blk >= wczerowidth2_cctab_max)
		blk = wczerowidth2_cctab_max - 1;

	ent = wczerowidth2_cctab[blk];

	/* best case: the whole 512-char range is constant */
	if (ent & (1U << (base_info_shift+1U)))
		return ((ent >> base_info_shift) & 1) + 1;

	eno = (unsigned int)c & 0x1FF;
	return ((wczerowidth2_ccblks[(ent*(1<<(9-3)))+(eno>>3)] >> (eno&7)) & 1);
}

int unicode_width(int c) {
	if (unicode_zero_width(c))
		return 0;

	unsigned int blk = (unsigned int)c >> 9;
	unsigned int base_info_shift = (sizeof(wcwidth2_cctab[0]) * 8U) - 2;
	unsigned int ent,eno;

	if (blk >= wcwidth2_cctab_max)
		blk = wcwidth2_cctab_max - 1;

	ent = wcwidth2_cctab[blk];

	/* best case: the whole 512-char range is constant */
	if (ent & (1U << (base_info_shift+1U)))
		return ((ent >> base_info_shift) & 1) + 1;

	eno = (unsigned int)c & 0x1FF;
	return ((wcwidth2_ccblks[(ent*(1<<(9-3)))+(eno>>3)] >> (eno&7)) & 1) + 1;
}

#endif

