#include <stdio.h>
#include <wchar.h> /* for wcwidth() */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

/* width bitfield (bit set = 2 char, clear = 1 char) */
unsigned char bitfield[(0x10FFFF+1)>>3];

/* zero-width bitfield (if set, char is zero-width) */
unsigned char zbitfield[(0x10FFFF+1)>>3];

enum {
	FORM_STRAIGHT=0,
	FORM_COMPRESSED
};

void emit_straight(unsigned char *bitfield,char *name) {
	wchar_t c;
	int i;

	printf("/* form: straight bitfield */\n");
	printf("/* total memory: %u bytes */\n",0x110000>>3);
	printf("unsigned char %s_tab[] = {\n",name);
	for (c=(wchar_t)0;c < (wchar_t)(0x110000>>3);c += 16) {
		if (c == ((0x110000>>3)-16)) {
			for (i=0;i < 15;i++) {
				printf("0x%02x,",bitfield[c+i]);
			}
			printf("0x%02x ",bitfield[c+15]);
		}
		else {
			for (i=0;i < 16;i++) {
				printf("0x%02x,",bitfield[c+i]);
			}
		}
		printf("/*%06xh*/\n",(unsigned int)(c<<3));
	}
	printf("};\n\n");
}

void emit_compressed(unsigned char *bitfield,char *name) {
	wchar_t c;
	int i,b;

	printf("/* form: compressed bitfield */\n");

	/* WORD entry:
	 *   bits 0-13: block offset into wcwidthtab
	 *   bit 15: block is constant throughout
	 *   bit 14: bit value constant */
	int last_nondup = (0x110000>>9)-1;
	uint16_t ctab[0x110000>>9];
	uint16_t alloc=0;

	for (c=(wchar_t)0;c < (wchar_t)(0x110000);c += (1U << 9U)) {
		unsigned char cval = bitfield[c>>3] & 1 ? 0xFF : 0x00;

		for (i=0;i < (1U << 9U) && bitfield[(c+i)>>3] == cval;) i += 8;

		if (i == (1U << 9U)) {
			/* constant value */
			ctab[c>>9] = 0x8000 | (cval << 14);
		}
		else {
			ctab[c>>9] = alloc++;
		}
	}

	/* apparently many unicode chars out there in the extended plane are
	 * just 0x80 single-width, even with this compression scheme that's
	 * still a lot of waste. why not shorten the table and add a #define
	 * that's an index to the last entry that ISN'T the same as the last
	 * char? */
	uint16_t match = ctab[last_nondup--];
	while (last_nondup >= 0 && ctab[last_nondup] == match) last_nondup--;
	last_nondup++;

	/* hey, if the block mapping is small enough to fit into an 8-bit char
	 * array, then all the better for tight memory savings! */
	if (alloc < 0x40) {
		printf("/* this is a master lookup table. for each 512-char */\n");
		printf("/* range, lookup a word from here and look at bits  */\n");
		printf("/* 6-7. bit 7 means the whole range is one const    */\n");
		printf("/* value matching bit 6. else, bits 0-5 are a       */\n");
		printf("/* block index into the next table where the actual */\n");
		printf("/* bits are stored                                  */\n");

		printf("/* total memory: %u + %u = %u bytes */\n",(last_nondup+1),(alloc * 512) >> 3,
				(last_nondup+1) + ((alloc * 512) >> 3));
	}
	else {
		printf("/* this is a master lookup table. for each 512-char */\n");
		printf("/* range, lookup a word from here and look at bits  */\n");
		printf("/* 14-15. bit 15 means the whole range is one const */\n");
		printf("/* value matching bit 14. else, bits 0-13 are a     */\n");
		printf("/* block index into the next table where the actual */\n");
		printf("/* bits are stored                                  */\n");

		printf("/* total memory: %u + %u = %u bytes */\n",(last_nondup+1) * 2,(alloc * 512) >> 3,
				((last_nondup+1) * 2) + ((alloc * 512) >> 3));
	}

	printf("#define %s_cctab_max %u\n",name,last_nondup+1);
	if (alloc < 0x40)
		printf("unsigned char %s_cctab[] = {\n",name);
	else
		printf("unsigned short %s_cctab[] = {\n",name);

	for (c=(wchar_t)0;c < (wchar_t)(0x110000);c += (1U << 9U)) {
		if (alloc < 0x40)
			printf("0x%02x",(ctab[c>>9] & 0x3F) | (ctab[c>>9] >> 8));
		else
			printf("0x%04x",ctab[c>>9]);

		if (c == (0x110000-512) || c == (last_nondup << 9)) {
			printf(" /*%06x*/\n",(unsigned int)(c - (512*7)));
			break;
		}
		else if (((c>>9)&7) == 7) {
			printf(",/*%06x*/\n",(unsigned int)(c - (512*7)));
		}
		else {
			printf(",");
		}
	}
	printf("};\n");

	/* the compressed form divides the bitfield into 512-bit
	 * blocks. each block is checked to see if it is all zeros,
	 * all ones, or varying. this takes care of the long runs
	 * present in the table where many chars are zero followed
	 * by the huge block of CJK chars, etc.
	 *
	 * this is directed by another table of 16-bit words that
	 * indicate whether the block is constant and what value,
	 * OR the block offset to read the values */
	printf("unsigned char %s_ccblks[] = {\n",name);
	for (i=0;i < 0x110000;i += 512) {
		unsigned char *bt;
		uint16_t blk = ctab[i>>9];
		if (blk & 0x8000) continue;
		bt = bitfield + (i>>3);

		printf("/* block %u: %06xh-%06xh */\n",blk,i,i+512);
		for (b=0;b < 512;b += 8) {
			printf("0x%02x",bt[b>>3]);
			if (((b>>3)&7) == 7)
				printf(",\n");
			else
				printf(",");
		}
	}
	printf("};\n\n");
}

int main(int argc,char **argv) {
	int form = FORM_STRAIGHT;
	wchar_t c;

	if (argc > 1) {
		if (!strcmp(argv[1],"compressed"))
			form = FORM_COMPRESSED;
	}

	setlocale(LC_ALL,"");

	printf("/* auto-generated by wcwidgen.c do not modify */\n");
	printf("/* this table is a bitfield for unicode chars */\n");
	printf("/* 0x00-0x10FFFF indicating whether a char is */\n");
	printf("/* 1 column width or two. it is stored as a   */\n");
	printf("/* bitfield to occupy the least amount of     */\n");
	printf("/* memory                                     */\n");
	printf("\n");

	for (c=(wchar_t)0x0;c <= (wchar_t)0x10FFFF;c++) {
		int w = wcwidth(c);
		if (w < 0) w = 0;
		if (w < 0 || w > 2) {
			fprintf(stderr,"Wide char 0x%06X: width is neither 1 nor 2, it is %d\n",(unsigned int)c,(int)w);
			return 1;
		}

		if ((c&7) == 0) bitfield[c>>3] = zbitfield[c>>3] = (unsigned char)0;
		if (w == 2) bitfield[c>>3] |= (unsigned char)(1 << (c&7));
		if (w == 0) zbitfield[c>>3] |= (unsigned char)(1 << (c&7));
	}

	if (form == FORM_STRAIGHT) {
		emit_straight(bitfield,"wcwidth2");
		emit_straight(zbitfield,"wczerowidth2");
	}
	else if (form == FORM_COMPRESSED) {
		emit_compressed(bitfield,"wcwidth2");
		emit_compressed(zbitfield,"wczerowidth2");
	}

	return 0;
}

