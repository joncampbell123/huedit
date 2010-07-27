
#if ISP_UTILS == 1
#  include <isp-utils/text/unicode.h>
#else
#  include <stdint.h>

#  ifndef UNICODE_BOM
#    define UNICODE_BOM 0xFEFF
#  endif

int utf8_encode(char **ptr,char *fence,uint32_t code);
int utf8_decode(char **ptr,char *fence);
int utf16le_decode(char **ptr,char *fence);

typedef char utf8_t;
typedef uint16_t utf16_t;

enum {
	UTF8ERR_INVALID=-1,
	UTF8ERR_NO_ROOM=-2
};

#endif

