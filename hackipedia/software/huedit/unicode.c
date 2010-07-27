
/* this code is already provided in Linux by the isp-utils.
 * but it's here in case we're in another environment where
 * the library is not present. */

#if ISP_UTILS == 0

/* UTF-8 and UTF-16 Unicode encoder/decoder functions.
 * Impact Studio Pro utilities - Text processors
 * (C) 2008, 2009 Impact Studio Pro ALL RIGHTS RESERVED.
 * Written by Jonathan Campbell
 *
 * For each encode/decode function you pass the address
 * of the char pointer itself, not the value of the char
 * pointer. This is how the function uses the pointer,
 * and updates it before returning it to you. The code
 * is written to always move forward in memory, and to
 * never step past the 'fence' pointer value (returning
 * errors rather than cause memory access violations).
 *
 * This code is not for wussies who can't handle pointers.
 * If pointers scare you, then by all means run crying
 * back to the comfortable managed world of Java/C# you
 * big baby :)
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <assert.h>

#include "unicode.h"

int utf8_encode(char **ptr,char *fence,uint32_t code) {
	int uchar_size=1;
	char *p = *ptr;

	if (!p) return UTF8ERR_NO_ROOM;
	if (code >= 0x110000) return UTF8ERR_INVALID;
	if (p >= fence) return UTF8ERR_NO_ROOM;

	if (code >= 0x10000) uchar_size = 4;
	else if (code >= 0x800) uchar_size = 3;
	else if (code >= 0x80) uchar_size = 2;

	if ((p+uchar_size) > fence) return UTF8ERR_NO_ROOM;

	switch (uchar_size) {
		case 1:	*p++ = (char)code;
			break;
		case 2:	*p++ = (char)(0xC0 | (code >> 6));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
		case 3:	*p++ = (char)(0xE0 | (code >> 12));
			*p++ = (char)(0x80 | ((code >> 6) & 0x3F));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
		case 4:	*p++ = (char)(0xF0 | (code >> 18));
			*p++ = (char)(0x80 | ((code >> 12) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 6) & 0x3F));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
	};

	*ptr = p;
	return 0;
}

int utf8_decode(char **ptr,char *fence) {
	int uchar_size=1;
	char *p = *ptr;
	int ret = 0,c;

	if (!p) return UTF8ERR_NO_ROOM;
	if (p >= fence) return UTF8ERR_NO_ROOM;

	ret = (unsigned char)(*p);
	if (ret >= 0xF8) { p++; return UTF8ERR_INVALID; }
	else if (ret >= 0xF0) uchar_size=4;
	else if (ret >= 0xE0) uchar_size=3;
	else if (ret >= 0xC0) uchar_size=2;
	else if (ret >= 0x80) { p++; return UTF8ERR_INVALID; }

	switch (uchar_size) {
		case 1:	p++;
			break;
		case 2:	ret = (ret&0x1F)<<6; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
		case 3:	ret = (ret&0xF)<<12; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<6;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
		case 4:	ret = (ret&0x7)<<18; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<12;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<6;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
	};

	*ptr = p;
	return ret;
}

int utf16le_decode(char **ptr,char *fence) {
	char *p = *ptr;
	int ret,b=2;

	if (!p) return UTF8ERR_NO_ROOM;
	if ((p+1) >= fence) return UTF8ERR_NO_ROOM;

	ret = (unsigned char)p[0];
	ret |= ((unsigned int)((unsigned char)p[1])) << 8;
	if (ret >= 0xD800 && ret <= 0xDBFF)
		b=4;
	else if (ret >= 0xDC00 && ret <= 0xDFFF)
		{ p++; return UTF8ERR_INVALID; }

	if ((p+b) > fence)
		return UTF8ERR_NO_ROOM;

	p += 2;
	if (ret >= 0xD800 && ret <= 0xDBFF) {
		/* decode surrogate pair */
		int hi = ret & 0x3FF;
		int lo = (unsigned char)p[0];
		lo |= ((unsigned int)((unsigned char)p[1])) << 8;
		p += 2;
		if (lo < 0xDC00 || lo > 0xDFFF) return UTF8ERR_INVALID;
		lo &= 0x3FF;
		ret = ((hi << 10) | lo) + 0x10000;
	}

	*ptr = p;
	return ret;
}

#endif /* ISP_UTILS == 0 */

