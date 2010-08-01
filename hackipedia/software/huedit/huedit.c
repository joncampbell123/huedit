
#include "common.h"

int			exit_program = 0;
int			force_utf8 = 0;

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

	q->columns = q->chars * 2; /* worst case scenario */
	q->col2char = (unsigned short*)malloc(sizeof(unsigned short) * q->columns);
	if (q->col2char == NULL) Fatal(_HERE_ "cannot alloc col2char");
	memset(q->col2char,0xFF,sizeof(unsigned short) * q->columns);
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

/* for a file/buffer, map line numbers to offsets/lengths and/or hold modified lines in memory */
struct file_lines_t {
	unsigned int			lines;
	unsigned int			lines_alloc;
	struct file_line_t*		line;			/* array of line buffer directions */
	/* to efficiently map column to char, and char to byte offset, we need an "active line" concept */
	unsigned int			active_line;
	struct file_line_qlookup_t	active;
	/* and if the user chooses to edit this line, we need a buffer to hold the wide chars we're editing */
	wchar_t*			active_edit;
	wchar_t*			active_edit_eol;
	unsigned int			active_edit_line;
	wchar_t*			active_edit_fence;
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
	if (l->active_edit) {
		free(l->active_edit);
		l->active_edit_fence = NULL;
		l->active_edit_eol = NULL;
		l->active_edit_line = 0U;
		l->active_edit = NULL;
	}
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

	l->active_edit = (wchar_t*)malloc(sizeof(wchar_t) * 514);
	if (l->active_edit == NULL) return;
	l->active_edit_fence = l->active_edit + 512;
	l->active_edit_line = line;
	o = l->active_edit;

	while (i < f && o < l->active_edit_fence) {
		c = utf8_decode(&i,f);
		if (c < 0) Fatal(_HERE_ "Invalid UTF-8 in file buffer");
		w = unicode_width(c);
		*o++ = (wchar_t)c;

		/* for sanity's sake we're expected to add padding if the char is wider than 1 cell */
		if (w > 1) *o++ = (wchar_t)(~0UL);
	}
	l->active_edit_eol = o;
	while (o < l->active_edit_fence)
		*o++ = (wchar_t)0;
}

void FlushActiveLine(struct openfile_t *of) {
	file_line_qlookup_free(&of->contents.active);
}

static char apply_edit_buffer[sizeof(wchar_t)*(512+4)]; /* enough for 512 UTF-8 chars at max length */

void file_lines_apply_edit(struct file_lines_t *l) {
	if (l->active_edit) {
		struct file_line_t *fl;
		wchar_t *i = l->active_edit;
		wchar_t *ifence = l->active_edit_eol;
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
	}
}

void file_lines_free(struct file_lines_t *l) {
	unsigned int i;

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

		if (in_line != in_base) {
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

int redraw_status = 0;
void InitStatusBar() {
	if (curses_with_color) {
		init_pair(NCURSES_PAIR_STATUS,COLOR_YELLOW,COLOR_BLUE);
		init_pair(NCURSES_PAIR_ACTIVE_EDIT,COLOR_YELLOW,COLOR_BLACK);
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
		else if (file->contents.active_edit != NULL && fy == file->contents.active_edit_line) {
			wchar_t *i = file->contents.active_edit;
			wchar_t *ifence = file->contents.active_edit_eol;
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

				c = utf8_decode(&i,ifence);
				if (c < 0) {
					if (i < ifence) i++;
					c = ' ';
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

	if (of->contents.active_edit != NULL && of->contents.active_edit_line == of->position.y) {
		/* use of wchar_t and arrangement in the buffer makes it easier to do this */
		unsigned int m = (unsigned int)(of->contents.active_edit_eol - of->contents.active_edit);
		while (nx < m && of->contents.active_edit[nx] == ((wchar_t)(~0UL))) nx++;
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

	if (of->contents.active_edit != NULL && of->contents.active_edit_line == of->position.y) {
		/* use of wchar_t and arrangement in the buffer makes it easier to do this */
		while (nx > 0 && of->contents.active_edit[nx] == ((wchar_t)(~0UL))) nx--;
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

	if (of->contents.active_edit != NULL && of->contents.active_edit_line == of->position.y) {
		nx = (unsigned int)(of->contents.active_edit_eol - of->contents.active_edit);
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

			if (of->contents.active_edit != NULL && of->contents.active_edit_line == of->position.y) {
				/* use of wchar_t and arrangement in the buffer makes it easier to do this */
				while (nx > 0 && of->contents.active_edit[nx] == ((wchar_t)(~0UL))) nx--;
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
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		FlushActiveLine(of);
		DrawFile(of,y);
	}

	if (of->position.y >= of->contents.lines)
		return;

	if (of->contents.active_edit == NULL)
		file_lines_prepare_edit(&of->contents,of->position.y);

	if (of->contents.active_edit == NULL)
		Fatal(_HERE_ "Active edit could not be engaged");

	/* apply the text */
	{
		wchar_t *p = of->contents.active_edit + of->position.x;

		if (p < of->contents.active_edit_fence) {
			while (p >= of->contents.active_edit_eol)
				*(of->contents.active_edit_eol++) = (wchar_t)' ';

			if (of->insert) {
				size_t moveover = (size_t)(of->contents.active_edit_eol - p);
				if ((p+1+moveover) > of->contents.active_edit_fence)
					moveover = (size_t)(of->contents.active_edit_fence - (p+1));

				if (moveover != 0) {
					memmove(p+1,p,moveover * sizeof(wchar_t));
					of->contents.active_edit_eol++;
				}
			}
			else if (p < (of->contents.active_edit_fence-1)) {
				/* if we're about to overwrite a CJK char we'd better
				 * make sure to pad out the gap this will create or
				 * else the user will see text shift around */
				if (p[1] == ((wchar_t)(~0UL)))
					p[1] = ' ';
			}

			if (*p == ((wchar_t)(~0UL)))
				Fatal(_HERE_ "bug: overwrite of padding for wide char");

			*p = (wchar_t)c;
			DrawFile(of,of->contents.active_edit_line);
			DoCursorRight(of,1);
		}
	}
}

void DoEnterKey() {
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		DrawFile(of,y);
	}

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
		DoCursorDown(of,1);
		DoCursorHome(of);
	}
}

void DoBackspaceKey() {
	struct openfile_t *of = ActiveOpenFile();
	if (of == NULL) return;

	/* if the active edit line is elsewhere, then flush it back to the
	 * contents struct and flush it for editing THIS line */
	if (of->contents.active_edit != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		FlushActiveLine(of);
		DrawFile(of,y);
	}

	if (of->position.y >= of->contents.lines)
		return;

	if (of->contents.active_edit == NULL)
		file_lines_prepare_edit(&of->contents,of->position.y);

	if (of->contents.active_edit == NULL)
		Fatal(_HERE_ "Active edit could not be engaged");

	/* delete the character behind me. if that char was wide, then replace it with narrower space
	 * if insert mode, shift the whole string back as well */
	if (of->position.x != 0) {
		wchar_t *p = of->contents.active_edit + of->position.x;
		if (p > of->contents.active_edit_eol) {
			/* well then just step back one */
			DoCursorLeft(of,1);
		}
		else {
			wchar_t *src = p--,*pchar = p;
			size_t len = (size_t)(of->contents.active_edit_eol - src);

			/* if the previous is a wide char, rub it out and replace it with spaces */
			if (*pchar == ((wchar_t)(~0UL))) {
				while (pchar >= of->contents.active_edit && *pchar == ((wchar_t)(~0UL)))
					*pchar-- = ' ';

				*pchar = ' ';
			}

			if (of->insert) {
				if (len != 0)
					memmove(p,src,len * sizeof(wchar_t));

				of->contents.active_edit_eol--;
				DrawFile(of,of->contents.active_edit_line);
				DoCursorLeft(of,1);
			}
			else {
				if (src == of->contents.active_edit_eol)
					of->contents.active_edit_eol--;

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
			p1_len = (size_t)(of->contents.active_edit_eol - of->contents.active_edit);
			if (p1_len != 0) memcpy(p1,of->contents.active_edit,p1_len * sizeof(wchar_t));
			file_lines_discard_edit(&of->contents);

			/* put the previous line into edit mode, parsing into wchar_t and combine */
			file_lines_prepare_edit(&of->contents,--of->position.y);
			p2_len = (size_t)(of->contents.active_edit_eol - of->contents.active_edit);
			if (p2_len != 0) memcpy(p2,of->contents.active_edit,p2_len * sizeof(wchar_t));

			p1rem = active_max - p2_len;

			/* if the combined string is too long, then leave the two lines alone */
			if ((p1_len+p2_len) > active_max) {
				memcpy(of->contents.active_edit,       p2,              p2_len  * sizeof(wchar_t));
				if (p1rem > 0)
					memcpy(of->contents.active_edit+p2_len,p1,              p1rem   * sizeof(wchar_t));

				of->contents.active_edit_eol = of->contents.active_edit + active_max;
				file_lines_apply_edit(&of->contents);

				/* and the remaining text on the next line */
				file_lines_prepare_edit(&of->contents,++of->position.y);
				memcpy(of->contents.active_edit,p1+p1rem,               (p1_len - p1rem) * sizeof(wchar_t));
				of->contents.active_edit_eol = of->contents.active_edit + (p1_len - p1rem);
				of->position.x = p1_len - p1rem;
			}
			else {
				memcpy(of->contents.active_edit,       p2,              p2_len  * sizeof(wchar_t));
				memcpy(of->contents.active_edit+p2_len,p1,              p1_len  * sizeof(wchar_t));
				of->contents.active_edit_eol = of->contents.active_edit + p1_len + p2_len;

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
	if (of->contents.active_edit != NULL && of->contents.active_edit_line != of->position.y) {
		unsigned int y = of->contents.active_edit_line;
		file_lines_apply_edit(&of->contents);
		FlushActiveLine(of);
		DrawFile(of,y);
	}

	if (of->position.y >= of->contents.lines)
		return;

	if (of->contents.active_edit == NULL)
		file_lines_prepare_edit(&of->contents,of->position.y);

	if (of->contents.active_edit == NULL)
		Fatal(_HERE_ "Active edit could not be engaged");

	/* delete the one char and shift all other chars back.
	 * if we just deleted a wide char (such as CJK) then replace
	 * the padding with a space. */
	{
		wchar_t *p = of->contents.active_edit + of->position.x;

		if (p < of->contents.active_edit_eol) {
			size_t len = (size_t)(of->contents.active_edit_eol - (p+1));

			of->contents.active_edit_eol--;
			if (len != 0) {
				memmove(p,p+1,len * sizeof(wchar_t));
				if (*p == ((wchar_t)(~0UL))) *p = ' ';
			}

			DrawFile(of,of->contents.active_edit_line);
		}
		else if (p < of->contents.active_edit_fence) {
			size_t active_max = 512;//(size_t)(of->contents.active_edit_fence - of->contents.active_edit);
			wchar_t p1[512],p2[512];
			size_t p1_len,p2_len;
			int p2rem;

			while (p > of->contents.active_edit_eol)
				*(of->contents.active_edit_eol++) = ' ';

			/* copy the wchar[] off, then throw away the edit */
			p1_len = (size_t)(of->contents.active_edit_eol - of->contents.active_edit);
			if (p1_len != 0) memcpy(p1,of->contents.active_edit,p1_len * sizeof(wchar_t));
			file_lines_discard_edit(&of->contents);

			/* put the next line into edit mode, parsing into wchar_t and combine */
			file_lines_prepare_edit(&of->contents,++of->position.y);
			p2_len = (size_t)(of->contents.active_edit_eol - of->contents.active_edit);
			if (p2_len != 0) memcpy(p2,of->contents.active_edit,p2_len * sizeof(wchar_t));

			p2rem = active_max - p1_len;

			/* if the combined string is too long, then leave the two lines alone */
			if ((p1_len+p2_len) > active_max) {
				file_lines_discard_edit(&of->contents);
				file_lines_prepare_edit(&of->contents,--of->position.y);

				memcpy(of->contents.active_edit,       p1,              p1_len  * sizeof(wchar_t));
				memcpy(of->contents.active_edit+p1_len,p2,              p2rem   * sizeof(wchar_t));
				of->contents.active_edit_eol = of->contents.active_edit + active_max;
				file_lines_apply_edit(&of->contents);
				file_lines_prepare_edit(&of->contents,++of->position.y);
				memcpy(of->contents.active_edit,       p2+p2rem,        (p2_len - p2rem) * sizeof(wchar_t));
				of->contents.active_edit_eol = of->contents.active_edit + (p2_len - p2rem);

				of->position.x = p2_len - p2rem;
			}
			else {
				file_lines_discard_edit(&of->contents);
				file_lines_prepare_edit(&of->contents,--of->position.y);

				memcpy(of->contents.active_edit,       p1,              p1_len  * sizeof(wchar_t));
				memcpy(of->contents.active_edit+p1_len,p2,              p2_len  * sizeof(wchar_t));
				of->contents.active_edit_eol = of->contents.active_edit + p1_len + p2_len;

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

		int key = getch();
		if (key >= ' ' && key < 127) {
			DoType(key);
		}
		else if (key == KEY_ENTER || key == 10 || key == 13) {
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
		else if (key == 2) { /* CTRL+B */
			/* DEBUG */
			exit_program = 1;
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

