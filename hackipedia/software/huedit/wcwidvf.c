#include <stdio.h>
#include <wchar.h> /* for wcwidth() */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "wcwidtht.h"
#include "wcwidthc.h"

#include "wcwidth.h"

static int wc_cctab_lookup(int c) {
	unsigned int blk = (unsigned int)c >> 9;
	unsigned int ent,eno;

	if (blk >= wcwidth2_cctab_max)
		blk = wcwidth2_cctab_max - 1;

	eno = (unsigned int)c & 0x1FF;
	ent = wcwidth2_cctab[blk];

	if (sizeof(wcwidth2_cctab[0]) == 1) { /* char table */
		if (ent & 0x80)
			return (ent & 0x40) >> 6;

		return (wcwidth2_ccblks[(ent*(1<<6))+(eno>>3)] >> (c&7)) & 1;
	}
	else { /* unsigned short table */
		if (ent & 0x8000)
			return (ent & 0x4000) >> 14;

		return (wcwidth2_ccblks[(ent*(1<<6))+(eno>>3)] >> (c&7)) & 1;
	}

	return 0;
}

int main(int argc,char **argv) {
	wchar_t c;

	setlocale(LC_ALL,"");

	printf("Testing uncompressed bitfield\n");
	for (c=0;c < 0x110000;c++) {
		int w = wcwidth((wchar_t)c);
		if (w < 1) w = 1;
		if (w > 2) {
			fprintf(stderr,"Wchar %u is more than 2 char widths in your library\n",w);
			return 1;
		}

		int b1 = (w == 2) ? 1 : 0;
		int b2 = (wcwidth2_tab[c>>3] >> (c&7)) & 1;
		if (b1 != b2) {
			fprintf(stderr,"Wchar %u table error: %u != %u\n",c,b1,b2);
			return 1;
		}
	}
	printf("  +-- OK!\n");

	printf("Testing compressed bitfield\n");
	for (c=0;c < 0x110000;c++) {
		int w = wcwidth((wchar_t)c);
		if (w < 1) w = 1;
		if (w > 2) {
			fprintf(stderr,"Wchar %u is more than 2 char widths in your library\n",w);
			return 1;
		}

		int b1 = (w == 2) ? 1 : 0;
		int b2 = wc_cctab_lookup(c);
		if (b1 != b2) {
			fprintf(stderr,"Wchar %u table error: %u != %u\n",c,b1,b2);
			return 1;
		}
	}
	printf("  +-- OK!\n");

	return 0;
}

