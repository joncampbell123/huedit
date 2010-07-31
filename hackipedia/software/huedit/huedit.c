
#include "common.h"

/* character sets */
typedef unsigned int charset_t;

#define CHARSET_ASCII		0
#define CHARSET_UTF8		1
#define CHARSET_CP437		2

/* compression filters */
enum {
	COMPR_NONE=0,
	COMPR_GZIP,
	COMPR_BZIP2,
	COMPR_XZ	/* or LZMA */
};

/* one line */
struct file_line_t {
	char*			buffer;			/* NULL = read from file */
	unsigned short		alloc;			/* allocated length in bytes */
	unsigned short		chars;			/* length in characters (if == 0 when length > 0 then we don't know) */
};

void file_line_alloc(struct file_line_t *fl,unsigned int len,unsigned int chars) {
	if (fl->buffer != NULL)
		Fatal(_HERE_ "bug: allocating line already allocated");
	if ((fl->buffer = malloc(len+8)) == NULL)
		Fatal(_HERE_ "Cannot allocate memory for line");
	fl->alloc = len+8;
	fl->chars = chars;
}

/* for a file/buffer, map line numbers to offsets/lengths and/or hold modified lines in memory */
struct file_lines_t {
	unsigned int		lines;
	unsigned int		lines_alloc;
	struct file_line_t*	line;			/* array of line buffer directions */
};

struct position_t {
	unsigned int		x,y;
};

struct window_t {
	unsigned short		x,y,w,h;
};

/* one open file */
struct openfile_t {
	fd_t			fd;			/* source data, FD_CLOSED if not backed by a file */
	char*			name;			/* the name of the file */
	char*			path;			/* the full path of the file */
	unsigned long		size;			/* total size of the file */
#if _OS_linux == 1
	DIR*			path_dir;		/* handle of the path */
#endif
	struct file_lines_t	contents;
	unsigned short		index;
	unsigned char		compression;
	charset_t		charset;
	struct position_t	position;
	struct position_t	scroll;
	struct window_t		window;
	unsigned char		redraw;
};

struct openfile_t *open_files[MAX_FILES];
int active_open_file = 0;

typedef int (*charset_decoder_t)(char **,char *);

int csdec_ascii(char **pscan,char *fence) {
	int c = UTF8ERR_INVALID;
	char *scan = *pscan;
	char cv;

	if (scan >= fence)
		return UTF8ERR_NO_ROOM;

	cv = *scan;
	if ((signed char)cv >= 0) {
		c = (int)cv;
		scan++;
	}

	*pscan = scan;
	return c;
}

int csdec_cp437(char **pscan,char *fence) {
	int c = UTF8ERR_INVALID;
	char *scan = *pscan;

	if (scan >= fence)
		return UTF8ERR_NO_ROOM;

	*pscan = scan;
	return c;
}

int csdec_utf8(char **pscan,char *fence) {
	if (*pscan >= fence)
		return UTF8ERR_NO_ROOM;

	return utf8_decode(pscan,fence);
}

charset_decoder_t get_charset_decoder(charset_t cs) {
	switch (cs) {
		case CHARSET_ASCII:
			return csdec_ascii;
		case CHARSET_UTF8:
			return csdec_utf8;
		case CHARSET_CP437:
			return csdec_cp437;
	};

	return NULL;
}

void file_line_free(struct file_line_t *l) {
	if (l->buffer) free(l->buffer);
	l->buffer = NULL;
}

void file_lines_free(struct file_lines_t *l) {
	unsigned int i;

	if (l->line) {
		for (i=0;i < l->lines_alloc;i++) file_line_free(&l->line[i]);
		free(l->line);
	}
	l->line = NULL;
}

void file_lines_alloc(struct file_lines_t *l,unsigned int lines) {
	if (l->line == NULL) {
		l->lines_alloc = ((lines + 0x20) | 0x3FF) + 1;
		l->lines = lines;
		if ((l->line = malloc(sizeof(struct file_line_t) * l->lines_alloc)) == NULL)
			Fatal(_HERE_ "cannot allocate line array for %u lines\n",lines);

		memset(l->line,0,sizeof(struct file_line_t) * l->lines_alloc);
	}
	else if (lines < l->lines_alloc) {
		l->lines = lines;
	}
	else if (lines > l->lines_alloc ||
		(l->lines_alloc > 0x1000 && lines < (l->lines_alloc/2))) {
		unsigned int new_alloc = ((lines + 0x20) | 0x7F) + 1;
		struct file_line_t* x = realloc(l->line,sizeof(struct file_line_t) * new_alloc);
		if (x == NULL) Fatal(_HERE_ "cannot expand file line array to %u lines\n",lines);

		if (new_alloc > l->lines_alloc)
			memset(x + l->lines_alloc,0,
				sizeof(struct file_line_t) * (new_alloc - l->lines_alloc));

		l->lines_alloc = new_alloc;
		l->lines = lines;
		l->line = x;
	}
}

void openfile_zero(struct openfile_t *f) {
	memset(f,0,sizeof(*f));
	f->fd = FD_CLOSED;
}

void openfile_free(struct openfile_t *f) {
	if (f == NULL) Fatal(_HERE_ "openfile_free() NULL pointer");
	file_lines_free(&f->contents);
	if (f->fd != FD_CLOSED) {
		close(f->fd);
		f->fd = FD_CLOSED;
	}
	if (f->name) {
		free(f->name);
		f->name = NULL;
	}
	if (f->path) {
		free(f->path);
		f->path = NULL;
	}
#if _OS_linux == 1
	if (f->path_dir != NULL) {
		closedir(f->path_dir);
		f->path_dir = NULL;
	}
#endif
}

void InitFiles() {
	unsigned int i;

	for (i=0;i < MAX_FILES;i++)
		open_files[i] = NULL;
}

void CloseFiles() {
	unsigned int i;

	for (i=0;i < MAX_FILES;i++) {
		if (open_files[i] != NULL) {
			openfile_free(open_files[i]);
			free(open_files[i]);
			open_files[i] = NULL;
		}
	}
}

struct openfile_t *alloc_file() {
	unsigned int i=0;

	while (i < MAX_FILES) {
		if (open_files[i] == NULL) {
			struct openfile_t *file = malloc(sizeof(struct openfile_t));
			if (file == NULL) Fatal(_HERE_ "Cannot alloc openfile_t");
			file->index = i;
			openfile_zero(file);
			open_files[i] = file;
			return file;
		}

		i++;
	}

	return NULL;
}

void close_file(struct openfile_t *x) {
	if (x == NULL) Fatal(_HERE_ "close_file() given a NULL pointer");
	if (x->index < 0 || x->index >= MAX_FILES) Fatal(_HERE_ "close_file() given an invalid index");
	if (open_files[x->index] != x) Fatal(_HERE_ "close_file() given a file who's index doesn't correspond to myself");
	open_files[x->index] = NULL;
	openfile_free(x);
}

int OpenInNewWindow(const char *path) {
	struct stat st;
	struct openfile_t *file = alloc_file();
	if (file == NULL) {
		Error(_HERE_ "No empty file slots");
		return 0;
	}

	{
		char combined[MAX_PATH+1];
		const char *work = combined;
		const char *rsh;
		size_t work_len;

#if _OS_linux == 1
		if (path[0] == '/') { /* absolute path: as-is */
			work = path;
			work_len = strlen(work);
		}
		else if (realpath(path,combined) != NULL) {
			work_len = strlen(work); /* work = combined */
			Debug(_HERE_ "%s => %s",path,combined);
		}
		else {
			Error_Errno(_HERE_ "Cannot resolve path");
			close_file(file);
			return 0;
		}
#else
		/* TODO: DOS/Windows drive letter handling? */
		work_len = snprintf(combined,sizeof(combined),"%s" PATH_SEP "%s",cwd,path);
#endif

		/* subdivide into path and name */
		rsh = strrchr(work,PATH_SEP_CHAR);
		if (rsh == NULL) Fatal(_HERE_ "Path %s cannot subdivide",work);
		{
			size_t name_len = strlen(rsh+1);
			if (name_len == 0) Fatal(_HERE_ "Path %s has no-length name",work);

			if ((file->name = malloc(name_len+1)) == NULL)
				Fatal(_HERE_ "Cannot allocate memory for name");

			memcpy(file->name,rsh+1,name_len+1);

			if ((file->path = malloc(work_len+1)) == NULL)
				Fatal(_HERE_ "Cannot allocate memory for full path");

			memcpy(file->path,work,work_len+1);
		}
	}

	if (lstat(file->path,&st)) {
		if (errno == ENOENT) {
			Warning(_HERE_ "File does not exist. Creating");
			file_lines_alloc(&file->contents,0);
		}
		else {
			Error_Errno(_HERE_ "Cannot stat file");
			close_file(file);
			return 0;
		}
	}
	else if (!S_ISREG(st.st_mode)) {
		Error_Errno(_HERE_ "Not a file");
		close_file(file);
		return 0;
	}
	else if (st.st_size >= (1UL << 30UL)) {
		Error_Errno(_HERE_ "File is way too big");
		close_file(file);
		return 0;
	}
	else {
		struct file_line_t *fline;
		charset_decoder_t csdec = NULL;
		unsigned int rc,est_lines,offset=0,rem,line=0;
		const unsigned int bufsize = 4096;
		const unsigned int maxline = 4096;
		char *buffer,*scan,*fence;
		char *in_line,*in_fence,*in_base;
		unsigned int in_chars;
		int rd,c;

		file->charset = CHARSET_ASCII;
		file->size = (unsigned int)st.st_size;
		file->fd = open(file->path,O_RDONLY | O_BINARY);
		if (file->fd < 0) {
			Error_Errno(_HERE_ "Cannot open %s",file->path);
			close_file(file);
			return 0;
		}

		if ((buffer = malloc(bufsize+maxline)) == NULL) {
			Error_Errno(_HERE_ "Cannot alloc read buffer for %s",file->path);
			close_file(file);
			return 0;
		}
		scan = buffer;
		fence = buffer;
		in_base = buffer+bufsize;
		in_line = in_base;
		in_fence = in_line+maxline;
		in_chars = 0;

		/* estimate the number of lines based on average line
		 * length of 90 chars. if we undershoot, the file alloc
		 * function will expand as necessary */
		est_lines = (unsigned int)((file->size + 89) / 90) + 10U;
		file_lines_alloc(&file->contents,est_lines);

		lseek(file->fd,0,SEEK_SET);
		if ((csdec = get_charset_decoder(file->charset)) == NULL)
			Fatal(_HERE_ "Charset not available");

		do {
			/* not only do we refill the buffer as needed, we make sure
			 * we have at least an 8 char lookahead for processing multibyte
			 * chars, especially UTF-8 */
			if ((scan+8) > fence) {
				rem = (unsigned int)(fence-scan);
				if (rem > 0) memmove(buffer,scan,rem);
				scan = buffer; fence = buffer+rem;
				rc = bufsize - rem;
				offset = (unsigned int)lseek(file->fd,0,SEEK_CUR) - rem;
				rd = read(file->fd,fence,rc);
				if (rd < 0) {
					Error_Errno(_HERE_ "Cannot read data");
					break;
				}
				else if (rd > 0) {
					fence += (unsigned int)rd;
					if (fence > (buffer+bufsize)) Fatal(_HERE_ "bug: read past buffer fence");
				}
				else if (rd == 0 && rem == 0) /* rem == 0 if scan == fence */
					break;

				/* if we're at the start, this is our chance to auto-identify UTF-8 */
				if (offset == 0 && rd >= 3) {
					if (!memcmp(scan,"\xEF\xBB\xBF",3)) {
						scan += 3;
						Debug(_HERE_ "File auto-identified as UTF-8 encoding");
						if ((csdec = get_charset_decoder(file->charset = CHARSET_UTF8)) == NULL)
							Fatal(_HERE_ "UTF-8 charset not available");
					}
				}
			}

			if (scan == fence)
				break;

			c = csdec(&scan,fence);
			if (c < 0) { /* ignore invalid char sequences */
				scan++;
			}
			else if (c == '\r') { /* to successfully handle DOS/Windows CR LF we just ignore CR */
			}
			else if (c == '\n') { /* line break */
				size_t in_len = (size_t)(in_line - in_base);
				if ((line+8) > file->contents.lines_alloc)
					file_lines_alloc(&file->contents,line+32);

				fline = &file->contents.line[line];
				file_line_alloc(fline,in_len,in_chars);
				memcpy(fline->buffer,in_base,in_len);
				fline->buffer[in_len] = 0;
				in_line = in_base;
				in_chars = 0;
				line++;
			}
			else {
				/* write as UTF-8 into our own buffer */
				/* we trust utf8_encode() will never overwrite past in_fence since that's how we coded it */
				if (utf8_encode(&in_line,in_fence,c) >= 0)
					in_chars++;
			}
		} while (1);

		if (in_line != in_base) {
			size_t in_len = (size_t)(in_line - in_base);
			if ((line+8) > file->contents.lines_alloc)
				file_lines_alloc(&file->contents,line+32);

			fline = &file->contents.line[line];
			file_line_alloc(fline,in_len,in_chars);
			memcpy(fline->buffer,in_base,in_len);
			fline->buffer[in_len] = 0;
			in_line = in_base;
			in_chars = 0;
			line++;
		}

		free(buffer);
	}

	/* default position and window */
	file->window.x = 0U;
	file->window.y = 1U;
	file->window.h = screen_height - 1;
	file->window.w = screen_width;
	file->position.x = 0U;
	file->position.y = 0U;
	file->redraw = 1;

	return 1;
}

/* pair #1 is the status bar */
#define NCURSES_PAIR_STATUS		1

int redraw_status = 0;
void InitStatusBar() {
	if (curses_with_color)
		init_pair(NCURSES_PAIR_STATUS,COLOR_YELLOW,COLOR_BLUE);

	redraw_status = 1;
}

void CloseStatusBar() {
}

void DrawStatusBar() {
	struct openfile_t *of = open_files[active_open_file];
	char status_temp[256];
	char *sp = status_temp;
	char *sf = sp + screen_width;

	if (of != NULL) {
		sp += sprintf(status_temp,"@ %u,%u ",of->position.y+1,of->position.x+1);

		{
			char *i = of->name;
			while (*i && sp < sf) *sp++ = *i++;
		}
	}

	while (sp < sf) *sp++ = ' ';
	*sp = (char)0;

	attrset(A_BOLD);
	color_set(NCURSES_PAIR_STATUS,NULL);
	mvaddstr(0,0,status_temp);
	attrset(A_NORMAL);
	color_set(0,NULL);
	refresh();
	redraw_status = 0;
}

void DrawFile(struct openfile_t *file) {
	unsigned int x,y,fy,fx;

	for (y=0;y < file->window.h;y++) {
		struct file_line_t *fline = NULL;
		fx = x + file->scroll.x;
		fy = y + file->scroll.y;

		if (fy < file->contents.lines)
			fline = &file->contents.line[fy];

		if (fline == NULL) {
			attrset(A_NORMAL);
			for (x=0;x < file->window.w;x++)
				mvaddstr(y+file->window.y,x+file->window.x," ");
		}
		else {
			char *i = fline->buffer;
			char *ifence = i + fline->alloc;
			int c,w;

			if (fy == file->position.y) {
				attrset(A_BOLD);
			}
			else {
				attrset(A_NORMAL);
			}

			for (x=0;x < file->window.w;) {
				wchar_t wc;

				c = utf8_decode(&i,ifence);
				if (c < 0) {
					if (i < ifence) i++;
					c = ' ';
				}
				w = unicode_width(c);
				wc = (wchar_t)c;
				mvaddnwstr(y+file->window.y,x+file->window.x,&wc,1);
				x += w;
			}
		}
	}

	file->redraw = 0;
}

void DrawActiveFile() {
	struct openfile_t *of = open_files[active_open_file];
	if (of) DrawFile(of);
}

int main(int argc,char **argv) {
	if (argc < 2) {
		fprintf(stderr,"You must specify a file to edit\n");
		return 1;
	}

#if _OS_linux == 1
	if (setlocale(LC_ALL,"") == NULL)
		Debug_Errno(_HERE_ "setlocale failed");
#endif

	OpenCwd();
	InitVid();
	InitFiles();
	InitStatusBar();

	OpenInNewWindow(argv[1]);

	if (redraw_status) DrawStatusBar();
	DrawActiveFile();

	getch();

	CloseStatusBar();
	CloseFiles();
	FreeVid();
	CloseCwd();
	return 0;
}

