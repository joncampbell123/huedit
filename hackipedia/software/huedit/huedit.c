
#include "common.h"

int			exit_program = 0;
int			force_utf8 = 0;
int			DIE = 0;

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

/* tracking table, lookup table for col -> char and char -> offset
 * for a line, due to the variable-byte nature of UTF-8 */
struct file_line_qlookup_t {
	unsigned int		columns;
	unsigned short*		col2char;
	unsigned int		chars;
	unsigned short*		char2ofs;
};

#define QLOOKUP_COLUMN_NONE 0xFFFFU

void file_line_qlookup_free(struct file_line_qlookup_t *q) {
	if (q == NULL) return;
	if (q->col2char) free(q->col2char);
	if (q->char2ofs) free(q->char2ofs);
	q->columns = q->chars = 0UL;
	q->col2char = NULL;
	q->char2ofs = NULL;
}

void file_line_charlen(struct file_line_t *fl) {
	if (fl->chars == 0 && fl->alloc > 0) {
		char *p = fl->buffer,*f = p + fl->alloc;
		int c = 0;

		while (p < f) {
			if (utf8_decode(&p,f) >= 0)
				c++;
			else
				abort(); /* SHOULDN'T HAPPEN! */
		}

		fl->chars = (unsigned short)c;
	}
}

void file_line_qlookup_line(struct file_line_qlookup_t *q,struct file_line_t *l) {
	unsigned short *colend;
	unsigned int col=0;
	unsigned int cho=0;
	char *src,*srce,*p;
	int w,c;

	file_line_qlookup_free(q);
	if (l == NULL) return;
	file_line_charlen(l);
	q->chars = l->chars;
	if (q->chars == 0) return;
	srce = l->buffer + l->alloc;
	src = l->buffer;

	q->char2ofs = (unsigned short*)malloc(sizeof(unsigned short) * q->chars);
	if (q->char2ofs == NULL) Fatal(_HERE_ "cannot alloc lookup char2ofs");

	q->columns = q->chars * 2; /* worst case scenario since wcslen() == 0, 1 or 2 */
	q->col2char = (unsigned short*)malloc(sizeof(unsigned short) * q->columns);
	if (q->col2char == NULL) Fatal(_HERE_ "cannot alloc col2char");
	memset(q->col2char,0xFF,sizeof(unsigned short) * q->columns); /* NOTE: 0xFFFF = QLOOKUP_COLUMN_NONE */
	colend = q->col2char + q->columns;

	while (src < srce && col < q->columns) {
		p = src;
		c = utf8_decode(&src,srce);
		if (c < 0) break;

		if (cho >= q->chars) Fatal(_HERE_ "Too many chars");
		q->char2ofs[cho] = (unsigned short)(p - l->buffer);

		w = unicode_width(c);
		if ((col+w) > q->columns) Fatal(_HERE_ "Too many columns");
		q->col2char[col] = (unsigned short)cho;

		col += w;
		cho++;
	}

	q->columns = col;
}

void file_line_alloc(struct file_line_t *fl,unsigned int len,unsigned int chars) {
	if (fl->buffer != NULL)
		Fatal(_HERE_ "bug: allocating line already allocated");
	if ((fl->buffer = malloc(len+8)) == NULL) /* <- caller expects at least +1 bytes for NUL */
		Fatal(_HERE_ "Cannot allocate memory for line");
	fl->alloc = len;
	fl->chars = chars;
}

struct file_active_line {
	wchar_t*			buffer;
	wchar_t*			eol;
	wchar_t*			fence;
};

void file_active_line_free(struct file_active_line *l) {
	if (l->buffer) free(l->buffer);
	l->buffer = NULL;
	l->fence = NULL;
	l->eol = NULL;
}

int file_active_line_alloc(struct file_active_line *l,unsigned int chars) {
	file_active_line_free(l);
	l->buffer = (wchar_t*)malloc(sizeof(wchar_t) * (chars + 2));
	if (l->buffer == NULL) return 0;
	l->fence = l->buffer + chars;
	return 1;
}

/* for a file/buffer, map line numbers to offsets/lengths and/or hold modified lines in memory */
struct file_lines_t {
	unsigned int			lines;
	unsigned int			lines_alloc;
	struct file_line_t*		line;			/* array of line buffer directions */
	/* to efficiently map column to char, and char to byte offset, we need an "active line" concept */
	unsigned int			active_line;
	struct file_line_qlookup_t	active;
	/* and if the user chooses to edit this line, we need a buffer to hold the wide chars we're editing */
	struct file_active_line		active_edit;
	unsigned int			active_edit_line;
	unsigned char			modified;
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
	unsigned char		insert;			/* insert mode */
	unsigned short		page_width;
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

void file_lines_discard_edit(struct file_lines_t *l) {
	file_active_line_free(&l->active_edit);
	l->active_edit_line = 0U;
}

void file_lines_prepare_edit(struct file_lines_t *l,unsigned int line) {
	struct file_line_t *fl;
	wchar_t *o;
	char *i,*f;
	int c,w;

	file_lines_discard_edit(l);

	/* decode line and output wchar */
	/* for active editing purposes we allocate a fixed size line */
	if (line < 0 || line >= l->lines) return;
	fl = &l->line[line];
	if (fl->buffer == NULL) Fatal(_HERE_ "Line to edit is NULL");

	i = fl->buffer;
	f = i + fl->alloc;

	if (!file_active_line_alloc(&l->active_edit,514)) return;
	l->active_edit_line = line;

	o = l->active_edit.buffer;
	while (i < f && o < l->active_edit.fence) {
		c = utf8_decode(&i,f);
		if (c < 0) Fatal(_HERE_ "Invalid UTF-8 in file buffer");
		w = unicode_width(c);
		*o++ = (wchar_t)c;

		/* for sanity's sake we're expected to add padding if the char is wider than 1 cell */
		if (w > 1) *o++ = (wchar_t)(~0UL);
	}
	l->active_edit.eol = o;
	while (o < l->active_edit.fence)
		*o++ = (wchar_t)0;
}

void FlushActiveLine(struct openfile_t *of) {
	file_line_qlookup_free(&of->contents.active);
}

static char apply_edit_buffer[sizeof(wchar_t)*(512+4)]; /* enough for 512 UTF-8 chars at max length */

void file_lines_apply_edit(struct file_lines_t *l) {
	if (l->active_edit.buffer) {
		struct file_line_t *fl;
		wchar_t *i = l->active_edit.buffer;
		wchar_t *ifence = l->active_edit.eol;
		char *o = apply_edit_buffer;
		char *of = apply_edit_buffer + sizeof(apply_edit_buffer);
		int chars = 0,utf8len;

		if (l->active_edit_line >= l->lines)
			Fatal(_HERE_ "Somehow, active line is beyond end of file");

		fl = &l->line[l->active_edit_line];

		while (i < ifence) {
			/* skip padding */
			if (*i == ((wchar_t)(~0UL))) {
				i++;
				continue;
			}
			else if (*i == 0) {
				break;
			}

			if (utf8_encode(&o,of,*i++) < 0)
				Fatal(_HERE_ "UTF-8 encoding error");

			chars++;
		}

		utf8len = (int)(o - apply_edit_buffer);
		file_line_free(fl);
		file_line_alloc(fl,utf8len,chars);
		if (utf8len > 0) memcpy(fl->buffer,apply_edit_buffer,utf8len);
		fl->buffer[utf8len] = 0;
		file_lines_discard_edit(l);
		l->modified = 1;
	}
}

void file_lines_free(struct file_lines_t *l) {
	unsigned int i;

	file_active_line_free(&l->active_edit);
	file_line_qlookup_free(&l->active);
	file_lines_discard_edit(l);
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
	else {
		l->lines = lines;
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

		if (force_utf8)
			file->charset = CHARSET_UTF8;
		else
			file->charset = CHARSET_ASCII;

		file->page_width = 80;
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
				if (force_utf8) {
				}
				else if (offset == 0 && rd >= 3) {
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
			else if (c == 8 || c == 26 || c == 27) {
			}
			else if (c == 9) {
				int i;
				/* just convert TABs to 8 chars */
				for (i=0;i < 8;i++) {
					if (utf8_encode(&in_line,in_fence,' ') >= 0)
						in_chars++;
				}
			}
			else if (c == '\r') { /* to successfully handle DOS/Windows CR LF we just ignore CR */
			}
			else if (c == '\n') { /* line break */
				size_t in_len = (size_t)(in_line - in_base);
				if ((line+8) > file->contents.lines_alloc)
					file_lines_alloc(&file->contents,line+256);

				fline = &file->contents.line[line];
				file_line_alloc(fline,in_len,in_chars);
				if (in_len > 0) memcpy(fline->buffer,in_base,in_len);
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

		if (in_line != in_base || 1) {
			size_t in_len = (size_t)(in_line - in_base);
			if ((line+8) > file->contents.lines_alloc)
				file_lines_alloc(&file->contents,line+32);

			fline = &file->contents.line[line];
			file_line_alloc(fline,in_len,in_chars);
			if (in_len > 0) memcpy(fline->buffer,in_base,in_len);
			fline->buffer[in_len] = 0;
			in_line = in_base;
			in_chars = 0;
			line++;
		}

		file->contents.lines = line;
		free(buffer);
	}

	/* default position and window */
	file->window.x = 0U;
	file->window.y = 1U;
	file->window.h = screen_height - 1;
	file->window.w = screen_width;
	file->position.x = 0U;
	file->position.y = 0U;
	file->insert = 1;
	file->redraw = 1;

	return 1;
}

/* pair #1 is the status bar */
#define NCURSES_PAIR_STATUS		1
#define NCURSES_PAIR_ACTIVE_EDIT	2
#define NCURSES_PAIR_MENU_NS		3
#define NCURSES_PAIR_MENU_SEL		4
#define NCURSES_PAIR_PAGE_OVERRUN	5

int redraw_status = 0;
void InitStatusBar() {
	if (curses_with_color) {
		init_pair(NCURSES_PAIR_STATUS,COLOR_YELLOW,COLOR_BLUE);
		init_pair(NCURSES_PAIR_ACTIVE_EDIT,COLOR_YELLOW,COLOR_BLACK);
		init_pair(NCURSES_PAIR_MENU_NS,COLOR_WHITE,COLOR_GREEN);
		init_pair(NCURSES_PAIR_MENU_SEL,COLOR_YELLOW,COLOR_BLUE);
		init_pair(NCURSES_PAIR_PAGE_OVERRUN,COLOR_YELLOW,COLOR_RED);
	}

	redraw_status = 1;
}

void CloseStatusBar() {
}

void UpdateStatusBar() {
	redraw_status = 1;
}

void DrawStatusBar() {
	struct openfile_t *of = open_files[active_open_file];
	char status_temp[256];
	char *sp = status_temp;
	char *sf = sp + screen_width;

	if (!redraw_status)
		return;

	if (of != NULL) {
		{
			sp += sprintf(status_temp,"@ %u,%u ",of->position.y+1,of->position.x+1);

			if (of->insert) {
				char *i = "INS ";
				while (*i && sp < sf) *sp++ = *i++;
			}
			else {
				char *i = "OVR ";
				while (*i && sp < sf) *sp++ = *i++;
			}

			{
				char *i = of->name;
				while (*i && sp < sf) *sp++ = *i++;
			}
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

void DrawFile(struct openfile_t *file,int line) {
	unsigned int x,y,fy,fx;

	if (file == NULL)
		return;
	if (line == -1 && file->redraw == 0)
		return;

	/* hide cursor */
	curs_set(0);

	for (y=0;y < file->window.h;y++) {
		struct file_line_t *fline = NULL;
		fx = x + file->scroll.x;
		fy = y + file->scroll.y;

		if (line != -1 && fy != line)
			continue;

		if (fy < file->contents.lines)
			fline = &file->contents.line[fy];

		if (fline == NULL) {
			attrset(A_NORMAL);
			for (x=0;x < file->window.w;x++)
				mvaddstr(y+file->window.y,x+file->window.x," ");
		}
		/* the active editing line */
		else if (file->contents.active_edit.buffer != NULL && fy == file->contents.active_edit_line) {
			wchar_t *i = file->contents.active_edit.buffer;
			wchar_t *ifence = file->contents.active_edit.eol;
			size_t i_max = (size_t)(ifence - i);
			int w;
	
			attrset(A_BOLD);
			color_set(NCURSES_PAIR_ACTIVE_EDIT,NULL);

			for (x=0;x < file->window.w;) {
				wchar_t wc;

				if ((x+file->scroll.x) >= i_max)
					wc = ' ';
				else
					wc = i[x+file->scroll.x];

				/* hide our use of NUL as indication of EOL */
				if (wc == 0) wc = ' ';

				w = unicode_width(wc);
				if ((x+w) > file->page_width && (x+file->scroll.x) < i_max) {
					color_set(NCURSES_PAIR_PAGE_OVERRUN,NULL);
					attron(A_BOLD);
				}
				mvaddnwstr(y+file->window.y,x+file->window.x,&wc,1);
				x += w;
			}

			color_set(0,NULL);
		}
		else {
			char *i = fline->buffer;
			char *ifence = i + fline->alloc;
			unsigned int skipchar = (unsigned int)(file->scroll.x);
			int c,w;

			if (fy == file->position.y) {
				attrset(A_BOLD);
			}
			else {
				attrset(A_NORMAL);
			}

			for (x=0;x < file->window.w;) {
				wchar_t wc;
				int eos=0;

				c = utf8_decode(&i,ifence);
				if (c < 0) {
					if (i < ifence) i++;
					c = ' ';
					eos = 1;
				}
				else if (skipchar > 0) {
					w = unicode_width(c);
					if ((unsigned int)w > skipchar) {
						x += skipchar;
						skipchar = 0;
					}
					else {
						skipchar -= (unsigned int)w;
					}
					continue;
				}

				w = unicode_width(c);
				wc = (wchar_t)c;
				if ((x+w) > file->page_width && !eos) {
					color_set(NCURSES_PAIR_PAGE_OVERRUN,NULL);
					attron(A_BOLD);
				}
				mvaddnwstr(y+file->window.y,x+file->window.x,&wc,1);
				x += w;
			}
		}
	}

	file->redraw = 0;
}

struct openfile_t *ActiveOpenFile() {
	return open_files[active_open_file];
}

void GenerateActiveLine(struct openfile_t *of) {
	struct file_lines_t *c = &of->contents;
	struct file_line_t *l;
	FlushActiveLine(of);

	if (of->position.y >= c->lines) return;
	l = &c->line[of->position.y];
	c->active_line = of->position.y;
	file_line_qlookup_line(&c->active,l);
}

void UpdateActiveLive(struct openfile_t *of) {
	if (	of->contents.active.col2char == NULL ||
		of->contents.active.char2ofs == NULL ||
		of->contents.active_line != of->position.y)
		GenerateActiveLine(of);
}

void DoCursorPos(struct openfile_t *of) {
	if (of == NULL) {
		curs_set(0);
	}
	else {
		int dx = (int)of->position.x - (int)of->scroll.x;
		int dy = (int)of->position.y - (int)of->scroll.y;
		if (dx >= 0 && dx < of->window.w && dy >= 0 && dy < of->window.h) {
			move(dy+of->window.y,dx+of->window.x);
			curs_set(1);
		}
		else {
			curs_set(0);
		}
	}
}

void DoCursorMove(struct openfile_t *of,int old_y,int new_y) {
	of->position.y = new_y;
	if (new_y < of->scroll.y) {
		of->redraw = 1;
		of->scroll.y = new_y;
	}
	else if (new_y >= (of->scroll.y + of->window.h)) {
		of->redraw = 1;
		of->scroll.y = (new_y - of->window.h) + 1;
	}
	else {
		DrawFile(of,old_y);
		DrawFile(of,new_y);
	}

	DoCursorPos(of);
	UpdateStatusBar();
}

void DoCursorUp(struct openfile_t *of,int lines) {
	unsigned int ny;

	if (of == NULL || lines <= 0)
		return;
	
	file_lines_apply_edit(&of->contents);
	FlushActiveLine(of);

	if (of->position.y < lines)
		ny = 0;
	else
		ny = of->position.y - lines;

	if (ny != of->position.y)
		DoCursorMove(of,of->position.y,ny);
	else
		DoCursorPos(of);
}

void DoCursorDown(struct openfile_t *of,int lines) {
	unsigned int ny;

	if (of == NULL || lines <= 0)
		return;
	
	file_lines_apply_edit(&of->contents);
	FlushActiveLine(of);

	ny = of->position.y + lines;
	if (ny >= of->contents.lines)
		ny = of->contents.lines - 1;

	if (ny != of->position.y)
		DoCursorMove(of,of->position.y,ny);
	else
		DoCursorPos(of);
}

void DoPageDown(struct openfile_t *of) {
	file_lines_apply_edit(&of->contents);
	FlushActiveLine(of);

	if (of->position.y < (of->scroll.y+of->window.h-1) &&
		(of->scroll.y+of->window.h-1) < of->contents.lines)
		DoCursorMove(of,of->position.y,of->scroll.y+of->window.h-1);
	else
		DoCursorDown(of,of->window.h);
}

void DoPageUp(struct openfile_t *of) {
	file_lines_apply_edit(&of->contents);
	FlushActiveLine(of);

	if (of->position.y > of->scroll.y)
		DoCursorMove(of,of->position.y,of->scroll.y);
	else
		DoCursorUp(of,of->window.h);
}

void DoCursorRight(struct openfile_t *of,int count) {
	unsigned int nx;

	if (of == NULL || count <= 0)
		return;

	nx = of->position.x + (unsigned int)count;
	if (nx > 512) nx = 512; /* <- FIXME: where is the constant that says max line length? */

	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line == of->position.y) {
		/* use of wchar_t and arrangement in the buffer makes it easier to do this */
		unsigned int m = (unsigned int)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
		while (nx < m && of->contents.active_edit.buffer[nx] == ((wchar_t)(~0UL))) nx++;
	}
	else {
		/* we need to align the cursor so we're not in the middle of CJK characters */
		UpdateActiveLive(of);
		if (of->contents.active.col2char != NULL) {
			struct file_line_qlookup_t *q = &of->contents.active;
			if (nx < q->columns) {
				while (nx < q->columns && q->col2char[nx] == QLOOKUP_COLUMN_NONE) nx++;
			}
		}
	}

	if (nx >= (of->scroll.x+of->window.w)) {
		of->scroll.x = (nx+1)-of->window.w;
		of->position.x = nx;
		of->redraw = 1;
		UpdateStatusBar();
	}
	else if (nx != of->position.x) {
		of->position.x = nx;
		DoCursorPos(of);
		UpdateStatusBar();
	}
}

void DoCursorLeft(struct openfile_t *of,int count) {
	unsigned int nx;

	if (of == NULL || count <= 0)
		return;

	if (of->position.x < count)
		nx = 0;
	else
		nx = of->position.x - (unsigned int)count;

	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line == of->position.y) {
		/* use of wchar_t and arrangement in the buffer makes it easier to do this */
		while (nx > 0 && of->contents.active_edit.buffer[nx] == ((wchar_t)(~0UL))) nx--;
	}
	else {
		/* we need to align the cursor so we're not in the middle of CJK characters */
		UpdateActiveLive(of);
		if (of->contents.active.col2char != NULL) {
			struct file_line_qlookup_t *q = &of->contents.active;
			if (nx < q->columns) {
				while (nx > 0 && q->col2char[nx] == QLOOKUP_COLUMN_NONE) nx--;
			}
		}
	}

	if (nx < of->scroll.x) {
		of->position.x = nx;
		of->scroll.x = nx;
		of->redraw = 1;
		UpdateStatusBar();
	}
	else if (nx != of->position.x) {
		of->position.x = nx;
		DoCursorPos(of);
		UpdateStatusBar();
	}
}

void DoCursorHome(struct openfile_t *of) {
	if (of == NULL)
		return;

	if (of->position.x != 0) {
		of->position.x = 0;
		if (of->scroll.x != 0) {
			of->scroll.x = 0;
			of->redraw = 1;
		}
		else {
			DoCursorPos(of);
		}
		UpdateStatusBar();
	}
}

void DoCursorEndOfLine(struct openfile_t *of) {
	unsigned int nx = 0;

	if (of == NULL)
		return;

	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line == of->position.y) {
		nx = (unsigned int)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
	}
	else if (of->position.y < of->contents.lines) {
		struct file_line_t *fl = &of->contents.line[of->position.y];
		if (fl != NULL) {
			file_line_charlen(fl);
			UpdateActiveLive(of);

			if (of->contents.active.col2char != NULL) {
				nx = of->contents.active.columns - 1;
				while ((int)nx >= 0 && of->contents.active.col2char[nx] == QLOOKUP_COLUMN_NONE) nx--;
				if (nx < 0)
					nx = 0;
				else {
					unsigned short ch = of->contents.active.col2char[nx];
					unsigned short on = of->contents.active.char2ofs[ch];
					char *p = fl->buffer + on;
					int c = utf8_decode(&p,fl->buffer + fl->alloc);
					if (c < 0) c = 0;
					nx += unicode_width(c);
				}
			}
			else {
				nx = fl->chars;
			}
		}
	}

	if (nx != of->position.x) {
		if (nx < of->scroll.x) {
			of->position.x = nx;
			of->scroll.x = nx;
			of->redraw = 1;
		}
		else if (nx >= (of->scroll.x+of->window.w)) {
			of->position.x = nx;
			of->scroll.x = (nx+1)-of->window.w;
			of->redraw = 1;
		}
		else {
			of->position.x = nx;
			DoCursorPos(of);
		}
		UpdateStatusBar();
	}
}

void DoResizedScreen(int width,int height) {
	int old_width = screen_width,old_height = screen_height;
	unsigned int fi;

	for (fi=0;fi < MAX_FILES;fi++) {
		struct openfile_t *of = open_files[fi];
		if (of == NULL) continue;

		if (of->window.x >= width) {
			of->window.x = width - 1;
			of->window.w = 1;
		}
		if (of->window.y >= height) {
			of->window.y = height - 1;
			of->window.h = 1;
		}
		if ((of->window.x+of->window.w) > width)
			of->window.w = width - of->window.x;
		if ((of->window.y+of->window.h) > height)
			of->window.h = height - of->window.y;

		if (of->window.x == 0 && of->window.w == old_width)
			of->window.w = width;
		if (of->window.y <= 1 && of->window.h >= (old_height-1))
			of->window.h = height - of->window.y;

		of->redraw = 1;
	}

	screen_width = width;
	screen_height = height;
	UpdateStatusBar();
}

void DoMouseClick(int mx,int my) {
	/* if that is within the active file, then put the cursor there */
	struct openfile_t *of = ActiveOpenFile();
	if (of) {
		if (	mx >= of->window.x && mx < (of->window.x+of->window.w) &&
			my >= of->window.y && my < (of->window.y+of->window.h)) {
			unsigned int ny = of->scroll.y + my - of->window.y;
			unsigned int nx = of->scroll.x + mx - of->window.x;
			if (ny != of->position.y) {
				file_lines_apply_edit(&of->contents);
				DoCursorMove(of,of->position.y,ny);
			}

			if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line == of->position.y) {
				/* use of wchar_t and arrangement in the buffer makes it easier to do this */
				while (nx > 0 && of->contents.active_edit.buffer[nx] == ((wchar_t)(~0UL))) nx--;
			}
			else {
				/* we need to align the cursor so we're not in the middle of CJK characters */
				UpdateActiveLive(of);
				if (of->contents.active.col2char != NULL) {
					struct file_line_qlookup_t *q = &of->contents.active;
					if (nx < q->columns) {
						while (nx > 0 && q->col2char[nx] == QLOOKUP_COLUMN_NONE) nx--;
					}
				}
			}

			of->position.x = nx;
			DoCursorPos(of);
			UpdateStatusBar();
		}
	}
}

void DoInsertKey() {
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;
	of->insert = !of->insert;
	UpdateStatusBar();
}

void DoType(int c) { /* <- WARNING: "c" is a unicode char */
	int w = unicode_width(c);
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		FlushActiveLine(of);
		DrawFile(of,y);
	}

	if (of->position.y >= of->contents.lines)
		return;

	if (of->contents.active_edit.buffer == NULL)
		file_lines_prepare_edit(&of->contents,of->position.y);

	if (of->contents.active_edit.buffer == NULL)
		Fatal(_HERE_ "Active edit could not be engaged");

	/* apply the text */
	{
		wchar_t *p = of->contents.active_edit.buffer + of->position.x;

		if (p < of->contents.active_edit.fence) {
			while (p > of->contents.active_edit.eol)
				*(of->contents.active_edit.eol++) = (wchar_t)' ';

			if (of->insert) {
				size_t moveover = (size_t)(of->contents.active_edit.eol - p);
				if ((p+w+moveover) > of->contents.active_edit.fence)
					moveover = (size_t)(of->contents.active_edit.fence - (p+w));

				if (moveover != 0) {
					memmove(p+w,p,moveover * sizeof(wchar_t));
					of->contents.active_edit.eol += w;
				}
			}
			else if (p < (of->contents.active_edit.fence-1)) {
				if (w > 1 && p < (of->contents.active_edit.fence-2)) {
					if (p[2] == ((wchar_t)(~0UL)))
						p[2] = ' ';
				}
				else {
					/* if we're about to overwrite a CJK char we'd better
					 * make sure to pad out the gap this will create or
					 * else the user will see text shift around */
					if (p[1] == ((wchar_t)(~0UL)))
						p[1] = ' ';
				}
			}

			if (*p == ((wchar_t)(~0UL)))
				Fatal(_HERE_ "bug: overwrite of padding for wide char");

			*p++ = (wchar_t)c;
			if (w > 1 && p < of->contents.active_edit.fence) *p++ = (wchar_t)(~0UL);
			if (of->contents.active_edit.eol < p) of->contents.active_edit.eol = p;
			DrawFile(of,of->contents.active_edit_line);
			DoCursorRight(of,w);
			of->contents.modified = 1;
		}
	}
}

void DoEnterKey() {
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		DrawFile(of,y);
	}

	if (of->insert) {
		struct file_line_t *fl;
		int cutpoint = of->position.x,len;
		wchar_t copy[513];
		int ny,cl;

		if (of->contents.active_edit.buffer == NULL || of->contents.active_edit_line != of->position.y)
			file_lines_prepare_edit(&of->contents,of->position.y);
		if (of->contents.active_edit.buffer == NULL)
			Fatal(_HERE_ "Active edit could not be engaged");

		len = (int)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
		assert(len >= 0 && len <= 512);
		if (cutpoint > len) cutpoint = len;
		if (cutpoint < len) memcpy(copy,of->contents.active_edit.buffer+cutpoint,sizeof(wchar_t) * (len - cutpoint));

		/* cut THIS line */
		of->contents.active_edit.eol = of->contents.active_edit.buffer + cutpoint;
		file_lines_apply_edit(&of->contents);

		/* and insert into the next line */
		ny = of->position.y+1;
		file_lines_alloc(&of->contents,of->contents.lines+1);
		cl = (int)(of->contents.lines - ny);
		if (cl > 0) memmove(of->contents.line+ny+1,of->contents.line+ny,sizeof(struct file_line_t) * cl);
		fl = &of->contents.line[ny];
		memset(fl,0,sizeof(*fl));
		file_line_alloc(fl,0,0);

		DoCursorDown(of,1);
		DoCursorHome(of);

		file_lines_prepare_edit(&of->contents,ny);
		if (of->contents.active_edit.buffer == NULL)
			Fatal(_HERE_ "Active edit could not be engaged");

		if (cutpoint < len) {
			memcpy(of->contents.active_edit.buffer,copy,(len - cutpoint) * sizeof(wchar_t));
			of->contents.active_edit.eol = of->contents.active_edit.buffer + (len - cutpoint);
		}

		of->redraw = 1;
	}
	else {
		if (of->position.y < (of->contents.lines-1)) {
			DoCursorDown(of,1);
			DoCursorHome(of);
		}
		else {
			/* add a new line */
			{
				struct file_line_t *fl;
				unsigned int nl = of->position.y+1;
				file_lines_alloc(&of->contents,nl+1);
				fl = &of->contents.line[nl];
				file_line_alloc(fl,0,0);
			}

			/* and put the cursor there */
			of->contents.modified = 1;
			DoCursorDown(of,1);
			DoCursorHome(of);
		}
	}
}

void DoBackspaceKey() {
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		FlushActiveLine(of);
		DrawFile(of,y);
	}

	if (of->position.y >= of->contents.lines)
		return;

	if (of->contents.active_edit.buffer == NULL)
		file_lines_prepare_edit(&of->contents,of->position.y);

	if (of->contents.active_edit.buffer == NULL)
		Fatal(_HERE_ "Active edit could not be engaged");

	/* delete the character behind me. if that char was wide, then replace it with narrower space
	 * if insert mode, shift the whole string back as well */
	if (of->position.x != 0) {
		wchar_t *p = of->contents.active_edit.buffer + of->position.x;
		if (p > of->contents.active_edit.eol) {
			/* well then just step back one */
			DoCursorLeft(of,1);
		}
		else {
			wchar_t *src = p--,*pchar = p;
			size_t len = (size_t)(of->contents.active_edit.eol - src);

			/* if the previous is a wide char, rub it out and replace it with spaces */
			if (*pchar == ((wchar_t)(~0UL))) {
				while (pchar >= of->contents.active_edit.buffer && *pchar == ((wchar_t)(~0UL)))
					*pchar-- = ' ';

				*pchar = ' ';
			}

			if (of->insert) {
				if (len != 0)
					memmove(p,src,len * sizeof(wchar_t));

				of->contents.active_edit.eol--;
				DrawFile(of,of->contents.active_edit_line);
				DoCursorLeft(of,1);
			}
			else {
				if (src == of->contents.active_edit.eol)
					of->contents.active_edit.eol--;

				*p = ' ';
				DrawFile(of,of->contents.active_edit_line);
				DoCursorLeft(of,1);
			}
		}
	}
	/* if the cursor is leftmost and insert mode is enabled, then the user wants
	 * to move the line contents up to the previous line. if the line is empty,
	 * then just delete the line */
	else if (of->insert) {
		if (of->position.y > 0 && of->contents.lines > 0) {
			size_t active_max = 512;//(size_t)(of->contents.active_edit_fence - of->contents.active_edit);
			wchar_t p1[512],p2[512];
			size_t p1_len,p2_len;
			int p1rem;

			/* copy the wchar[] off, then throw away the edit */
			p1_len = (size_t)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
			if (p1_len != 0) memcpy(p1,of->contents.active_edit.buffer,p1_len * sizeof(wchar_t));
			file_lines_discard_edit(&of->contents);

			/* put the previous line into edit mode, parsing into wchar_t and combine */
			file_lines_prepare_edit(&of->contents,--of->position.y);
			p2_len = (size_t)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
			if (p2_len != 0) memcpy(p2,of->contents.active_edit.buffer,p2_len * sizeof(wchar_t));

			p1rem = active_max - p2_len;

			/* if the combined string is too long, then leave the two lines alone */
			if ((p1_len+p2_len) > active_max) {
				memcpy(of->contents.active_edit.buffer,       p2,              p2_len  * sizeof(wchar_t));
				if (p1rem > 0)
					memcpy(of->contents.active_edit.buffer+p2_len,p1,              p1rem   * sizeof(wchar_t));

				of->contents.active_edit.eol = of->contents.active_edit.buffer + active_max;
				file_lines_apply_edit(&of->contents);

				/* and the remaining text on the next line */
				file_lines_prepare_edit(&of->contents,++of->position.y);
				memcpy(of->contents.active_edit.buffer,p1+p1rem,               (p1_len - p1rem) * sizeof(wchar_t));
				of->contents.active_edit.eol = of->contents.active_edit.buffer + (p1_len - p1rem);
				of->position.x = p1_len - p1rem;
			}
			else {
				memcpy(of->contents.active_edit.buffer,       p2,              p2_len  * sizeof(wchar_t));
				memcpy(of->contents.active_edit.buffer+p2_len,p1,              p1_len  * sizeof(wchar_t));
				of->contents.active_edit.eol = of->contents.active_edit.buffer + p1_len + p2_len;

				/* and then we need to shift up the other lines */
				int remline = of->position.y+1;
				int lines = (int)of->contents.lines - (remline+1);
				struct file_line_t *del_fl = &of->contents.line[remline];
				file_line_free(del_fl);
				if (lines > 0) {
					memmove(of->contents.line+remline,of->contents.line+remline+1,
						lines*sizeof(struct file_line_t));
				}
				/* we memmove'd the list, leaving an extra elem. don't free it */
				del_fl = &of->contents.line[--of->contents.lines];
				memset(del_fl,0,sizeof(*del_fl));
				of->position.x = p2_len;
			}

			/* make sure cursor is scrolled into place */
			if (of->position.x < of->scroll.x)
				of->scroll.x = of->position.x;
			else if ((of->position.x+of->scroll.x) >= (of->scroll.x+of->window.w))
				of->scroll.x = (of->position.x+1)-of->window.w;

			/* redraw the whole screen */
			of->redraw = 1;
		}
	}
}

void DoDeleteKey() {
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit.buffer != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		FlushActiveLine(of);
		DrawFile(of,y);
	}

	if (of->position.y >= of->contents.lines)
		return;

	if (of->contents.active_edit.buffer == NULL)
		file_lines_prepare_edit(&of->contents,of->position.y);

	if (of->contents.active_edit.buffer == NULL)
		Fatal(_HERE_ "Active edit could not be engaged");

	/* delete the one char and shift all other chars back.
	 * if we just deleted a wide char (such as CJK) then replace
	 * the padding with a space. */
	{
		wchar_t *p = of->contents.active_edit.buffer + of->position.x;

		if (p < of->contents.active_edit.eol) {
			size_t len = (size_t)(of->contents.active_edit.eol - (p+1));

			of->contents.active_edit.eol--;
			if (len != 0) {
				memmove(p,p+1,len * sizeof(wchar_t));
				if (*p == ((wchar_t)(~0UL))) *p = ' ';
			}

			DrawFile(of,of->contents.active_edit_line);
		}
		else if (p < of->contents.active_edit.fence) {
			size_t active_max = 512;//(size_t)(of->contents.active_edit_fence - of->contents.active_edit);
			wchar_t p1[512],p2[512];
			size_t p1_len,p2_len;
			int p2rem;

			while (p > of->contents.active_edit.eol)
				*(of->contents.active_edit.eol++) = ' ';

			/* copy the wchar[] off, then throw away the edit */
			p1_len = (size_t)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
			if (p1_len != 0) memcpy(p1,of->contents.active_edit.buffer,p1_len * sizeof(wchar_t));
			file_lines_discard_edit(&of->contents);

			/* put the next line into edit mode, parsing into wchar_t and combine */
			file_lines_prepare_edit(&of->contents,++of->position.y);
			p2_len = (size_t)(of->contents.active_edit.eol - of->contents.active_edit.buffer);
			if (p2_len != 0) memcpy(p2,of->contents.active_edit.buffer,p2_len * sizeof(wchar_t));

			p2rem = active_max - p1_len;

			/* if the combined string is too long, then leave the two lines alone */
			if ((p1_len+p2_len) > active_max) {
				file_lines_discard_edit(&of->contents);
				file_lines_prepare_edit(&of->contents,--of->position.y);

				memcpy(of->contents.active_edit.buffer,       p1,              p1_len  * sizeof(wchar_t));
				memcpy(of->contents.active_edit.buffer+p1_len,p2,              p2rem   * sizeof(wchar_t));
				of->contents.active_edit.eol = of->contents.active_edit.buffer + active_max;
				file_lines_apply_edit(&of->contents);
				file_lines_prepare_edit(&of->contents,++of->position.y);
				memcpy(of->contents.active_edit.buffer,       p2+p2rem,        (p2_len - p2rem) * sizeof(wchar_t));
				of->contents.active_edit.eol = of->contents.active_edit.buffer + (p2_len - p2rem);

				of->position.x = p2_len - p2rem;
			}
			else {
				file_lines_discard_edit(&of->contents);
				file_lines_prepare_edit(&of->contents,--of->position.y);

				memcpy(of->contents.active_edit.buffer,       p1,              p1_len  * sizeof(wchar_t));
				memcpy(of->contents.active_edit.buffer+p1_len,p2,              p2_len  * sizeof(wchar_t));
				of->contents.active_edit.eol = of->contents.active_edit.buffer + p1_len + p2_len;

				/* and then we need to shift up the other lines */
				int remline = of->position.y+1;
				int lines = (int)of->contents.lines - (remline+1);
				struct file_line_t *del_fl = &of->contents.line[remline];
				file_line_free(del_fl);
				if (lines > 0) {
					memmove(of->contents.line+remline,of->contents.line+remline+1,
						lines*sizeof(struct file_line_t));
				}
				/* we memmove'd the list, leaving an extra elem. don't free it */
				del_fl = &of->contents.line[--of->contents.lines];
				memset(del_fl,0,sizeof(*del_fl));
				of->position.x = p1_len;
			}

			/* make sure cursor is scrolled into place */
			if (of->position.x < of->scroll.x)
				of->scroll.x = of->position.x;
			else if ((of->position.x+of->scroll.x) >= (of->scroll.x+of->window.w))
				of->scroll.x = (of->position.x+1)-of->window.w;

			/* redraw the whole screen */
			of->redraw = 1;
		}
	}
}

void help() {
	fprintf(stderr,"huedit [options] [file [file ...]]\n");
	fprintf(stderr,"Multi-platform unicode text editor\n");
	fprintf(stderr,"(C) 2008-2010 Jonathan Campbell ALL RIGHTS RESERVED\n");
	fprintf(stderr,"The official text editor of Hackipedia.org\n");
	fprintf(stderr,"\n");
	fprintf(stderr," --utf8         Assume text file is UTF-8\n");
}

int safe_getch() {
	int c = getch();

	/* *SIGH* this is needed because apparently ncurses won't interpret F1...F10 keys for us */
	if (c == 27) {
		c = getch();
		if (c == '[') {
			/* function keys look like <ESC>[n~ where
			 * n = 11    F1
			 *     12    F2
			 *     13    F3
			 *     14    F4
			 *     15    F5
			 */
			int dec = 0;
			while (isdigit(c=getch()))
				dec = (dec * 10) + c - '0';

			if (c == '~') {
				c = -1;
				switch (dec) {
					case 11:	c = KEY_F(1); break;
					case 12:	c = KEY_F(2); break;
					case 13:	c = KEY_F(3); break;
					case 14:	c = KEY_F(4); break;
					case 15:	c = KEY_F(5); break;
					case 17:	c = KEY_F(6); break;
					case 18:	c = KEY_F(7); break;
					case 19:	c = KEY_F(8); break;
					case 20:	c = KEY_F(9); break;
					case 21:	c = KEY_F(10); break;
					case 23:	c = KEY_F(11); break;
					case 24:	c = KEY_F(12); break;
				};
			}
			else {
				c = -1;
			}
		}
		else if (c == -1 || c == 27) {
			/* then we give back 27 */
			c = 27;
		}
		else {
			c = -1;
		}
	}

	return c;
}

void sigma(int x) {
	if (++DIE >= 10) abort();
}

void draw_single_box(WINDOW *sw,int px,int py,int w,int h) {
	int x,y;

	mvwaddch(sw,py,px,ACS_ULCORNER);
	for (x=1;x < (w-1);x++) mvwaddch(sw,py,px+x,ACS_HLINE);
	mvwaddch(sw,py,px+w-1,ACS_URCORNER);

	mvwaddch(sw,py+h-1,px,ACS_LLCORNER);
	for (x=1;x < (w-1);x++) mvwaddch(sw,py+h-1,px+x,ACS_HLINE);
	mvwaddch(sw,py+h-1,px+w-1,ACS_LRCORNER);

	for (y=1;y < (h-1);y++) {
		mvwaddch(sw,py+y,px,ACS_VLINE);
		mvwaddch(sw,py+y,px+w-1,ACS_VLINE);
	}
}

void draw_single_box_with_fill(WINDOW *sw,int px,int py,int w,int h) {
	int x,y;

	draw_single_box(sw,px,py,w,h);
	for (y=1;y < (h-1);y++) {
		for (x=1;x < (w-1);x++) {
			mvwaddch(sw,py+y,px+x,' ');
		}
	}
}

struct menu_item_t {
	const char*		str;
	unsigned char		shortcut;
	signed short		menucode;
};

int MenuBox(struct menu_item_t *menu,const char *msg,int def) {
	int px = 4,py = 1,w = strlen(msg)+2,h;
	int items = 0,ret = 0,selection = 0;
	int redraw = 1,dismiss = 0;
	int x,y,c;

	while (menu[items].str != NULL) {
		if (menu[items].menucode == def) selection = items;
		items++;
	}
	h = items + 2;

	for (y=0;y < items;y++) {
		int len = strlen(menu[y].str) + 2;
		if (w < len) w = len;
	}

	w += 2;
	if ((w+px) > screen_width)
		w = screen_width - px;

	py = screen_height - (h + 4);

	WINDOW *sw = newwin(h,w,py,px);
	if (sw == NULL) Fatal(_HERE_ "Cannot make window");

	PANEL *pan = new_panel(sw);
	if (pan == NULL) Fatal(_HERE_ "Cannot make panel");

	attrset(0);
	curs_set(0);
	draw_single_box(sw,0,0,w,h);
	wattron(sw,A_BOLD);
	mvwaddnstr(sw,0,2,msg,w-2);
	show_panel(pan);

	while (!dismiss) {
		if (redraw) {
			for (y=0;y < (h-2);y++) {
				const char *str = menu[y].str;
				int strl = strlen(str);

				if (strl > w-2)
					strl = w-2;

				if (y == selection) {
					wattrset(sw,A_BOLD);
					wcolor_set(sw,NCURSES_PAIR_MENU_SEL,NULL);
				}
				else {
					wattrset(sw,A_BOLD);
					wcolor_set(sw,NCURSES_PAIR_MENU_NS,NULL);
				}

				mvwaddch(sw,y+1,1,' ');
				mvwaddnstr(sw,y+1,2,str,strl);
				for (x=strl;x < (w-3);x++) mvwaddch(sw,y+1,x+2,' ');
			}
			wrefresh(sw);
			update_panels();
			redraw = 0;
		}

		c = safe_getch();
		if (c == 27) {
			ret = -1;
			break;
		}
		else if (c == 13 || c == 10 || c == KEY_ENTER) {
			ret = menu[selection].menucode;
			break;
		}
		else if (c >= 32 && c < 127) {
			int i=0;

			while (i < items) {
				struct menu_item_t *m = &menu[i];
				if (c == m->shortcut) {
					selection = i;
					redraw = 1;
					break;
				}
				else {
					i++;
				}
			}
		}
		else if (c == KEY_UP) {
			if (selection > 0) {
				selection--;
				redraw=1;
			}
		}
		else if (c == KEY_DOWN) {
			if (selection < (items-1)) {
				selection++;
				redraw=1;
			}
		}
		else if (c == KEY_PPAGE || c == KEY_HOME) {
			if (selection > 0) {
				selection = 0;
				redraw=1;
			}
		}
		else if (c == KEY_NPAGE || c == KEY_END) {
			if (selection < (items-1)) {
				selection = items-1;
				redraw=1;
			}
		}
	}

	hide_panel(pan);
	del_panel(pan);
	delwin(sw);
	update_panels();
	refresh();
	return ret;
}

void console_beep() {
	beep();
}

#define PROMPT_YES		 1
#define PROMPT_NO		 0
#define PROMPT_CANCEL		-1

int PromptYesNoCancel(const char *msg,int def) {
	static struct menu_item_t menu[] = {
		{"Yes",		'y',	PROMPT_YES		},
		{"No",		'n',	PROMPT_NO		},
		{"Cancel",	'c',	PROMPT_CANCEL		},
		{NULL,		0,	0			}
	};
	int i = MenuBox(menu,msg,def);
	return i;
}

void TempStatus(const char *msg,int count,int total) {
	char tmp[1024];
	char percent[64] = {0};
	int tmp_max = screen_width;
	int msg_len = strlen(msg);

	memset(tmp,' ',tmp_max);
	tmp[tmp_max] = 0;

	if (count >= 0 && total > 0) {
		if (count > total) count = total;
		int perc = (1000 * count) / total;
		size_t percent_len = sprintf(percent," %%%u.%u",perc/10,perc%10);
		tmp_max -= percent_len;
		memcpy(tmp+tmp_max,percent,percent_len);
	}

	if (msg_len > tmp_max) {
		tmp_max -= 3;
		memset(tmp+tmp_max,'.',3);
	}

	if (tmp_max > 0) {
		memcpy(tmp,msg,msg_len);
	}

	attrset(A_BOLD);
	color_set(NCURSES_PAIR_MENU_NS,NULL);
	mvaddnstr(0,0,tmp,screen_width);
	refresh();

	UpdateStatusBar();
}

void SaveFile(struct openfile_t *of) {
	struct file_lines_t *c;
	struct file_line_t *fl;
	char msg[PATH_MAX+64];
	unsigned int line;
	int fd;

	sprintf(msg,"Saving: %s",of->name);
	TempStatus(msg,0,0);

	/* make sure the user's edits are applied! */
	file_lines_apply_edit(&of->contents);

	fd = open(of->path,O_WRONLY|O_TRUNC|O_CREAT,0644);
	if (fd < 0) {
		Error_Errno(_HERE_ "Cannot write file %s\n",of->path);
		return;
	}

	/* UTF-8 BOM */
	write(fd,"\xEF\xBB\xBF",3);

	c = &of->contents;
	for (line=0;line < c->lines;line++) {
		fl = &c->line[line];
		if (fl->buffer == NULL) Fatal(_HERE_ "Line %u is null",line);

		/* TODO: non-UTF encodings? */
		write(fd,fl->buffer,fl->alloc);

		/* DOS-style CR LF */
		if (line < (c->lines-1))
			write(fd,"\r\n",2);

		if ((line%100) == 0)
			TempStatus(msg,line,c->lines);
	}

	close(fd);
	sprintf(msg,"Written: %s",of->name);
	TempStatus(msg,0,0);
}

void DoExitProgram();

void QuitFile(struct openfile_t *of) {
	int i;

	if (of == NULL) return;
	if (of->index >= MAX_FILES) Fatal(_HERE_ "Invalid open file index");
	if (open_files[of->index] != of) Fatal(_HERE_ "Index does not represent this file");

	active_open_file = of->index;
	i = of->index;

	if (of->contents.modified) {
		int ans;

		of->redraw = 1;
		UpdateStatusBar();
		DrawStatusBar();
		DrawFile(of,-1);
		DoCursorPos(of);
		console_beep();
		refresh();
		ans = PromptYesNoCancel("File has been modified! Save?",PROMPT_YES);
		if (ans == PROMPT_YES) {
			/* save the file then */
			SaveFile(of);
		}
		else if (ans == PROMPT_CANCEL) {
			return;
		}
	}

	openfile_free(open_files[i]);
	free(open_files[i]);
	open_files[i] = NULL;

	active_open_file = 0;
	while (active_open_file < MAX_FILES && open_files[active_open_file] == NULL)
		active_open_file++;

	if (active_open_file == MAX_FILES)
		DoExitProgram();
	else {
		UpdateStatusBar();
		open_files[active_open_file]->redraw = 1;
	}
}

void DoExitProgram() {
	int i;

	DIE = 0;
	for (i=0;i < MAX_FILES;i++) {
		struct openfile_t *of = open_files[i];
		if (of == NULL) continue;
		active_open_file = i;

		if (of->contents.modified) {
			int ans;

			of->redraw = 1;
			UpdateStatusBar();
			DrawStatusBar();
			DrawFile(of,-1);
			DoCursorPos(of);
			console_beep();
			refresh();
			ans = PromptYesNoCancel("File has been modified! Save?",PROMPT_YES);
			if (ans == PROMPT_CANCEL) {
				/* user wants to abort shutdown */
				return;
			}
			else if (ans == PROMPT_YES) {
				/* save the file then */
				SaveFile(of);
			}
		}

		openfile_free(open_files[i]);
		free(open_files[i]);
		open_files[i] = NULL;
	}

	exit_program = 1;
}

struct main_submenu_item_t {
	char*				title;
	unsigned char			shortcut;
	unsigned short			item;
};

struct main_menu_item_t {
	char*				title;
	unsigned char			shortcut;
	struct main_submenu_item_t*	submenu;
};

enum {
	MM_FILE_QUITALL=1,
	MM_FILE_SAVE,
	MM_FILE_QUIT,
	MM_OS_SHELL
};

struct main_submenu_item_t main_file_menu[] = {
	{"Save",		's',		MM_FILE_SAVE},
	{"Quitfile",		'q',		MM_FILE_QUIT},
	{"Os Shell",		'o',		MM_OS_SHELL},
	{NULL,			0,		0}
};

struct main_submenu_item_t main_quit_menu[] = {
	{"Quit all files",	'q',		MM_FILE_QUITALL},
	{NULL,			0,		0}
};

struct main_submenu_item_t main_util_menu[] = {
	{NULL,			0,		0}
};

struct main_menu_item_t main_menu[] = {
	{"File",		'f',		main_file_menu},
	{"Util",		'u',		main_util_menu},
	{"Quit",		'q',		main_quit_menu},
	{NULL,			0,		NULL}
};

void DoShell() {
	endwin();
	system("/bin/bash");
	initscr();
}

void DoMainMenu() {
	static int selected = 0;
	int max_items = 0;
	int dismiss = 0;
	int redraw = 1;
	int c,ret = -1;
	int do_sub = 0;
	int sel_x = 0;
	int item = -1;

	WINDOW *sw = newwin(3,screen_width,1,0);
	if (sw == NULL) Fatal(_HERE_ "Cannot make window");

	PANEL *pan = new_panel(sw);
	if (pan == NULL) Fatal(_HERE_ "Cannot make panel");

	attrset(0);
	curs_set(0);
	wattrset(sw,A_BOLD);
	wcolor_set(sw,NCURSES_PAIR_ACTIVE_EDIT,NULL);
	draw_single_box_with_fill(sw,0,0,screen_width,3);
	show_panel(pan);

	while (!dismiss) {
		if (redraw) {
			int i=0,x=2;

			for (i=0;main_menu[i].title != NULL;i++) {
				struct main_menu_item_t *mi = &main_menu[i];
				int len = strlen(mi->title);

				wattrset(sw,A_BOLD);
				if (i == selected) {
					sel_x = x;
					wcolor_set(sw,NCURSES_PAIR_MENU_SEL,NULL);
				}
				else {
					wcolor_set(sw,NCURSES_PAIR_ACTIVE_EDIT,NULL);
				}

				mvwaddstr(sw,1,x,mi->title);
				x += len + 2;
			}

			max_items = i;
			wrefresh(sw);
			update_panels();
			redraw = 0;
		}

		if (!do_sub) {
			c = safe_getch();
			if (c == 27 || c == KEY_F(2)) {
				ret = -1;
				break;
			}
			else if (c >= 32 && c < 127) {
				int i=0,x=2;

				while (i < max_items) {
					struct main_menu_item_t *mi = &main_menu[i];
					if (mi->shortcut == c) {
						selected = i;
						redraw = 1;
						do_sub = 1;
						sel_x = x;
						break;
					}
					else {
						i++;
					}

					x += strlen(mi->title) + 2;
				}

				if (redraw)
					continue;
			}
			else if (c == KEY_DOWN || c == 13 || c == 10 || c == KEY_ENTER) {
				do_sub = 1;
			}
			else if (c == KEY_LEFT) {
				if (selected > 0)
					selected--;
				else
					selected = max_items - 1;

				redraw = 1;
			}
			else if (c == KEY_RIGHT) {
				if (selected < (max_items - 1))
					selected++;
				else
					selected = 0;

				redraw = 1;
			}
		}
		else {
			c = -1;
		}

		if (do_sub) {
			int s_redraw = 1;
			int s_dismiss = 0;
			int s_selected = 0;
			int s_width = strlen(main_menu[selected].title) + 4;
			struct main_submenu_item_t *s_menu = main_menu[selected].submenu;
			int s_height = 2;
			int s_items = 0;

			do_sub = 0;
			while (s_menu[s_items].title != NULL) {
				struct main_submenu_item_t *s_i = &s_menu[s_items];
				int w = strlen(s_i->title) + 4;
				s_height++;
				s_items++;

				if (s_width < w)
					s_width = w;
			}

			WINDOW *ssw = newwin(s_height,s_width,3,sel_x-2);
			if (ssw == NULL) Fatal(_HERE_ "Cannot make window");

			PANEL *span = new_panel(ssw);
			if (span == NULL) Fatal(_HERE_ "Cannot make panel");

			attrset(0);
			curs_set(0);
			wattrset(ssw,A_BOLD);
			wcolor_set(ssw,NCURSES_PAIR_ACTIVE_EDIT,NULL);
			draw_single_box(ssw,0,0,s_width,s_height);
			if (sel_x > 2)	mvwaddch(ssw,0,0,ACS_TTEE);
			else		mvwaddch(ssw,0,0,ACS_LTEE);
			mvwaddch(ssw,0,s_width-1,ACS_TTEE);
			show_panel(span);

			while (!s_dismiss) {
				if (s_redraw) {
					int i=0;

					for (i=0;s_menu[i].title != NULL;i++) {
						struct main_submenu_item_t *mi = &s_menu[i];

						wattrset(ssw,A_BOLD);
						if (i != s_selected)
							wcolor_set(ssw,NCURSES_PAIR_ACTIVE_EDIT,NULL);
						else
							wcolor_set(ssw,NCURSES_PAIR_MENU_SEL,NULL);

						mvwaddstr(ssw,i+1,2,mi->title);
					}

					wrefresh(ssw);
					update_panels();
					s_redraw = 0;
				}

				c = safe_getch();
				if (c == 27 || c == KEY_F(2)) {
					ret = -1;
					break;
				}
				else if (c >= 32 && c < 127) {
					int i=0;

					while (i < s_items) {
						struct main_submenu_item_t *mi = &s_menu[i];
						if (mi->shortcut == c) {
							s_selected = i;
							s_redraw = 1;
							break;
						}
						else {
							i++;
						}
					}
				}
				else if (c == KEY_UP) {
					if (s_selected > 0)
						s_selected--;
					else
						s_selected = s_items - 1;

					s_redraw = 1;
				}
				else if (c == KEY_DOWN) {
					if (s_selected < (s_items - 1))
						s_selected++;
					else
						s_selected = 0;

					s_redraw = 1;
				}
				else if (c == KEY_LEFT) {
					if (selected == 0)
						selected = max_items - 1;
					else
						selected--;

					redraw = 1;
					do_sub = 1;
					break;
				}
				else if (c == KEY_RIGHT) {
					if (selected == (max_items - 1))
						selected = 0;
					else
						selected++;

					redraw = 1;
					do_sub = 1;
					break;
				}
				else if (c == 13 || c == 10 || c == KEY_ENTER) {
					struct main_submenu_item_t *mi = &s_menu[s_selected];
					item = mi->item;
					dismiss = 1;
					break;
				}
			}

			del_panel(span);
			delwin(ssw);
			update_panels();
			refresh();
		}
	}

	del_panel(pan);
	delwin(sw);
	update_panels();
	refresh();

	switch (item) {
		case MM_FILE_QUITALL:
			DoExitProgram();
			break;
		case MM_FILE_SAVE:
			SaveFile(ActiveOpenFile());
			break;
		case MM_FILE_QUIT:
			QuitFile(ActiveOpenFile());
			break;
		case MM_OS_SHELL:
			DoShell();
			break;
	};
}

#define KEY_CTRL(x)	(x + 1 - 'A')

int main(int argc,char **argv) {
	char *file2open[MAX_FILES] = {NULL};
	int files2open=0;
	int i;

	for (i=1;i < argc;) {
		char *a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"help")) {
				help();
				return 1;
			}
			else if (!strcmp(a,"utf8")) {
				force_utf8 = 1;
			}
		}
		else {
			if (files2open >= MAX_FILES) {
				fprintf(stderr,"Too many files!\n");
				return 1;
			}
			file2open[files2open++] = a;
		}
	}

	if (files2open < 1) {
		fprintf(stderr,"You must specify a file to edit\n");
		fprintf(stderr,"Use --help switch for more information\n");
		return 1;
	}

#if _OS_linux == 1
	if (setlocale(LC_ALL,"") == NULL)
		Debug_Errno(_HERE_ "setlocale failed");

	signal(SIGINT,sigma);
	signal(SIGQUIT,sigma);
#endif

	OpenCwd();
	InitVid();
	InitFiles();
	InitStatusBar();

	for (i=0;i < files2open;i++)
		OpenInNewWindow(file2open[i]);

	while (!exit_program) {
		DrawStatusBar();
		DrawFile(ActiveOpenFile(),-1);
		DoCursorPos(ActiveOpenFile());

		int key = safe_getch();
		if (key == 3 || DIE) {
			DoExitProgram();
		}
		else if (key >= ' ' && key < 127) {
			DoType(key);
		}
		else if (key == 10 || key == 13) {
			DoEnterKey();
		}
		else if (key == KEY_DOWN) {
			DoCursorDown(ActiveOpenFile(),1);
		}
		else if (key == KEY_UP) {
			DoCursorUp(ActiveOpenFile(),1);
		}
		else if (key == KEY_RIGHT) {
			DoCursorRight(ActiveOpenFile(),1);
		}
		else if (key == KEY_LEFT) {
			DoCursorLeft(ActiveOpenFile(),1);
		}
		else if (key == KEY_HOME) {
			DoCursorHome(ActiveOpenFile());
		}
		else if (key == KEY_END) {
			DoCursorEndOfLine(ActiveOpenFile());
		}
		else if (key == KEY_NPAGE) {
			DoPageDown(ActiveOpenFile());
		}
		else if (key == KEY_PPAGE) {
			DoPageUp(ActiveOpenFile());
		}
		/* NTS: XFCE's Terminal takes F1 for itself, and ESC is no good, so use CTRL+P or F2 */
		else if (key == KEY_F(2) || key == KEY_CTRL('P')) {
			DoMainMenu();
		}
		else if (key == KEY_MOUSE) {
			MEVENT event;
			if (getmouse(&event) == OK) {
				if (event.bstate & BUTTON1_PRESSED) {
					DoMouseClick(event.x,event.y);
				}
			}
		}
		else if (key == KEY_RESIZE) {
			DoResizedScreen(COLS,LINES);
		}
		else if (key == KEY_IC) {
			DoInsertKey();
		}
		else if (key == KEY_DC) {
			DoDeleteKey();
		}
		else if (key == KEY_BACKSPACE || key == 8 || key == 127) {
			DoBackspaceKey();
		}
	}

	CloseStatusBar();
	CloseFiles();
	FreeVid();
	CloseCwd();
	return 0;
}

