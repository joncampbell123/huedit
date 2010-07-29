all: huedit

# this is Linux
OS=linux

# VDRV = video driver
#  [linux]
#   ncursesw       wide ncurses
#   ncurses        non-wide ncurses
#   vga            direct communication with VGA alphanumeric memory in Linux (/dev/mem and ioport)
#   vcsa           direct communication with Linux /dev/vcsa driver (console)
#   vt100          direct communication with Linux console
#   fbdev          direct communication with Linux framebuffer
#   svgalib        direct communication with VGA using svgalib
#   sdl            framebuffer mode using SDL
#   x11            graphical mode using X-Windows
VDRV=ncursesw

# gzip = whether or not to compile in support for gzipped text (and saving back)
GZIP=1

# bzip2 = whether or not to compile in support for bzipped text (and saving back)
BZIP2=1

# xz = whether or not to compile in support for xz/lzma'd text (and saving back)
XZ=1

# isp_utils = whether or not to use the ISP utils library
ISP_UTILS=1

# final CFLAGS/LDFLAGS
CFLAGS=-g3 -O0 -D_V_$(VDRV)=1 -DGZIP=$(GZIP) -DBZIP2=$(BZIP2) -DXZ=$(XZ) -DISP_UTILS=$(ISP_UTILS) -D_OS_$(OS)=1 -Wall -pedantic -std=c99
LDFLAGS=

ifeq ($(OS),linux)
CFLAGS+= -D_FILE_OFFSET_BITS=64
endif

ifeq ($(VDRV),ncursesw)
LDFLAGS+=-lncursesw
endif

ifeq ($(VDRV),ncurses)
LDFLAGS+=-lncurses
endif

ifeq ($(VDRV),sdl)
LDFLAGS+=-lSDL
endif

ifeq ($(VDRV),x11)
# TODO
endif

ifeq ($(GZIP),1)
LDFLAGS+=-lz
endif

ifeq ($(BZIP2),1)
LDFLAGS+=-lbz2
endif

ifeq ($(ISP_UTILS),1)
LDFLAGS+=-lisp-utils-text
endif

ifeq ($(XZ),1)
# TODO
endif

EDITOR_OBJS=huedit.o
COMMON_OBJS=errors.o cwd.o vid.o

ifneq ($(ISP_UTILS),1)
COMMON_OBJS+=unicode.o
endif

huedit: $(COMMON_OBJS) $(EDITOR_OBJS) $(LDFLAGS)
	$(CC) -o $@ $^

.c.o:
	$(CC) -c -o $@ $^ $(CFLAGS)

clean:
	rm -f *.o huedit

distclean: clean
	find -name \*~ -delete