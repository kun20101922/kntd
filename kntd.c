#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


#define EDITOR_VERSION "0.1.0"
#define EDITOR_TAB_STOP 4
#define EDITOR_QUIT_TIMES 2
#define UNDO_HISTORY 100

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

enum editorMode {
  MODE_NORMAL = 0,
  MODE_INSERT,
  MODE_VISUAL,
  MODE_VISUAL_LINE,
  MODE_COMMAND
};

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH,
  HL_VISUAL,
  HL_LINENR,
  HL_CURSORLINE,
  HL_STATUSBAR,
  HL_STATUSMODE,
  HL_CMDBAR,
  HL_TILDE,
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)


struct editorSyntax {
  char *filetype;
  char **filematch;
  char **keywords;
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  int flags;
};

typedef struct erow {
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
  int hl_open_comment;
} erow;

typedef struct undoEntry {
  int type;       /* 0=delete_row, 1=insert_row, 2=modify_row, 3=delete_char, 4=insert_char */
  int row;
  int col;
  char *data;
  int data_len;
} undoEntry;

struct editorConfig {
  int cx, cy;         /* cursor position in file */
  int rx;             /* render x */
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[256];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  struct termios orig_termios;

  /* Mode */
  enum editorMode mode;
  char modename[16];

  /* Command bar */
  char cmdbuf[256];
  int cmdlen;

  /* Search */
  char searchbuf[256];
  int search_direction;  /* 1 = forward, -1 = backward */
  int last_match;        /* row of last match */

  /* Visual mode */
  int vx, vy;           /* visual anchor */

  /* Yank buffer */
  char **yank_lines;
  int yank_count;
  int yank_is_line;     /* yanked whole lines? */

  /* Undo */
  undoEntry undo_stack[UNDO_HISTORY];
  int undo_top;

  /* Repeat count */
  int count_buf;
  int pending_count;

  /* Normal mode pending operator */
  char pending_op;

  /* Options */
  int opt_number;       /* show line numbers */
  int opt_relativenumber;
  int opt_syntax;
  int opt_cursorline;
};

struct editorConfig E;

/* ====== FILETYPES ====== */

char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".cc", ".cxx", NULL };
char *C_HL_keywords[] = {
  "switch","if","while","for","break","continue","return","else",
  "struct","union","typedef","static","enum","class","case","do",
  "default","goto","sizeof","volatile","const","extern","register",
  "inline","restrict","auto",
  "int|","long|","double|","float|","char|","unsigned|","signed|",
  "void|","short|","bool|","size_t|","ssize_t|","uint8_t|","uint16_t|",
  "uint32_t|","uint64_t|","int8_t|","int16_t|","int32_t|","int64_t|",
  "NULL|","true|","false|",
  NULL
};

char *PY_HL_extensions[] = { ".py", NULL };
char *PY_HL_keywords[] = {
  "False","None","True","and","as","assert","async","await","break",
  "class","continue","def","del","elif","else","except","finally",
  "for","from","global","if","import","in","is","lambda","nonlocal",
  "not","or","pass","raise","return","try","while","with","yield",
  "int|","str|","float|","bool|","list|","dict|","set|","tuple|",
  "bytes|","bytearray|","complex|","range|","type|","object|",
  "print|","len|","range|","enumerate|","zip|","map|","filter|",
  "open|","input|","isinstance|","issubclass|","hasattr|","getattr|",
  "setattr|","delattr|","super|","property|","staticmethod|","classmethod|",
  NULL
};

char *SH_HL_extensions[] = { ".sh", ".bash", ".zsh", NULL };
char *SH_HL_keywords[] = {
  "if","then","else","elif","fi","case","esac","for","while","do",
  "done","in","function","return","exit","break","continue","local",
  "export","readonly","declare","unset","shift","source","echo|",
  "printf|","read|","test|","cd|","ls|","mkdir|","rm|","cp|","mv|",
  NULL
};

struct editorSyntax HLDB[] = {
  {
    "c",
    C_HL_extensions,
    C_HL_keywords,
    "//", "/*", "*/",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
  {
    "python",
    PY_HL_extensions,
    PY_HL_keywords,
    "#", "\"\"\"", "\"\"\"",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
  {
    "shell",
    SH_HL_extensions,
    SH_HL_keywords,
    "#", NULL, NULL,
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ====== PROTOTYPES ====== */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
void editorProcessKeypress(void);
void editorInsertChar(int c);
void editorDelChar(void);
void editorInsertNewline(void);
void editorSearch(void);

/* ====== TERMINAL ====== */

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disableRawMode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[4];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  }
  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  }
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

/* ====== SYNTAX HIGHLIGHTING ====== */

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];{}\\|&!?:\"'`", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  if (!E.opt_syntax || E.syntax == NULL) return;

  char **keywords = E.syntax->keywords;
  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;

  int prev_sep = 1;
  int in_string = 0;
  int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL;

    if (scs && !in_string && !in_comment) {
      if (!strncmp(&row->render[i], scs, strlen(scs))) {
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    if (mcs && mce && !in_string) {
      if (in_comment) {
        row->hl[i] = HL_MLCOMMENT;
        if (!strncmp(&row->render[i], mce, strlen(mce))) {
          memset(&row->hl[i], HL_MLCOMMENT, strlen(mce));
          i += strlen(mce);
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (!strncmp(&row->render[i], mcs, strlen(mcs))) {
        memset(&row->hl[i], HL_MLCOMMENT, strlen(mcs));
        i += strlen(mcs);
        in_comment = 1;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        row->hl[i] = HL_STRING;
        if (c == '\\' && i + 1 < row->rsize) {
          row->hl[i+1] = HL_STRING;
          i += 2;
          continue;
        }
        if (c == in_string) in_string = 0;
        i++;
        prev_sep = 1;
        continue;
      } else if (c == '"' || c == '\'') {
        in_string = c;
        row->hl[i] = HL_STRING;
        i++;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
          (c == '.' && prev_hl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen-1] == '|';
        if (kw2) klen--;

        if (!strncmp(&row->render[i], keywords[j], klen) &&
            is_separator(row->render[i + klen])) {
          memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }

  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if (changed && row->idx + 1 < E.numrows)
    editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl) {
  switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 90;   /* dark gray */
    case HL_KEYWORD1:  return 33;   /* yellow */
    case HL_KEYWORD2:  return 36;   /* cyan */
    case HL_STRING:    return 32;   /* green */
    case HL_NUMBER:    return 31;   /* red */
    case HL_MATCH:     return 34;   /* blue (search) */
    default:           return 37;   /* white */
  }
}

void editorSelectSyntaxHighlight(void) {
  E.syntax = NULL;
  if (E.filename == NULL) return;

  char *ext = strrchr(E.filename, '.');

  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = &HLDB[j];
    unsigned int i = 0;
    while (s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.');
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
          (!is_ext && strstr(E.filename, s->filematch[i]))) {
        E.syntax = s;
        for (int filerow = 0; filerow < E.numrows; filerow++)
          editorUpdateSyntax(&E.row[filerow]);
        return;
      }
      i++;
    }
  }
}

/* ====== ROW OPERATIONS ====== */

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
    rx++;
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      cur_rx += (EDITOR_TAB_STOP - 1) - (cur_rx % EDITOR_TAB_STOP);
    cur_rx++;
    if (cur_rx > rx) return cx;
  }
  return cx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs * (EDITOR_TAB_STOP - 1) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  E.row[at].idx = at;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].hl_open_comment = 0;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/* ====== EDITOR OPERATIONS ====== */

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline(void) {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar(void) {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/* ====== FILE I/O ====== */

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (int j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    /* New file */
    editorSetStatusMessage("New file: \"%s\"", filename);
    return;
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave(void) {
  if (E.filename == NULL) {
    /* Prompt for filename */
    editorSetStatusMessage("No filename. Use :w <filename>");
    return;
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("\"%s\" %dL, %dB written", E.filename, E.numrows, len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorSaveAs(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  editorSelectSyntaxHighlight();
  editorSave();
}

/* ====== SEARCH ====== */

void editorSearchCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;
  static int saved_hl_line = -1;
  static unsigned char *saved_hl = NULL;

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN || key == 'n') {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP || key == 'N') {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) direction = 1;
  int current = last_match;

  for (int i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = editorRowRxToCx(row, match - row->render);
      E.rowoff = E.numrows;

      saved_hl_line = current;
      saved_hl = malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

void editorSearchPrompt(void) {
  int saved_cx = E.cx, saved_cy = E.cy;
  int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

  char query[256] = "";
  int qlen = 0;
  char prompt[320];

  while (1) {
    snprintf(prompt, sizeof(prompt), "/%s", query);
    editorSetStatusMessage("%s", prompt);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (qlen > 0) query[--qlen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      E.cx = saved_cx; E.cy = saved_cy;
      E.coloff = saved_coloff; E.rowoff = saved_rowoff;
      editorSearchCallback(query, c);
      return;
    } else if (c == '\r') {
      editorSetStatusMessage("");
      editorSearchCallback(query, c);
      strncpy(E.searchbuf, query, sizeof(E.searchbuf)-1);
      return;
    } else if (c == ARROW_UP || c == ARROW_DOWN || c == 'n' || c == 'N') {
      editorSearchCallback(query, c);
    } else if (!iscntrl(c) && c < 128) {
      if (qlen < 255) {
        query[qlen++] = c;
        query[qlen] = '\0';
      }
    }
    editorSearchCallback(query, -1);
  }
}

void editorFindNext(int direction) {
  if (E.searchbuf[0] == '\0') return;
  int start = E.cy + direction;
  if (start < 0) start = E.numrows - 1;
  if (start >= E.numrows) start = 0;

  for (int i = 0; i < E.numrows; i++) {
    int row_idx = (start + i * direction + E.numrows * E.numrows) % E.numrows;
    erow *row = &E.row[row_idx];
    char *match = strstr(row->render, E.searchbuf);
    if (match) {
      E.cy = row_idx;
      E.cx = editorRowRxToCx(row, match - row->render);
      E.rowoff = E.numrows;
      return;
    }
  }
  editorSetStatusMessage("Pattern not found: %s", E.searchbuf);
}

/* ====== MODE HELPERS ====== */

void editorSetMode(enum editorMode mode) {
  E.mode = mode;
  switch (mode) {
    case MODE_NORMAL:     strcpy(E.modename, "NORMAL");      break;
    case MODE_INSERT:     strcpy(E.modename, "INSERT");      break;
    case MODE_VISUAL:     strcpy(E.modename, "VISUAL");      break;
    case MODE_VISUAL_LINE:strcpy(E.modename, "VISUAL LINE"); break;
    case MODE_COMMAND:    strcpy(E.modename, "COMMAND");     break;
  }
}

/* ====== APPEND BUFFER ====== */

struct abuf {
  char *b;
  int len;
};
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/* ====== OUTPUT ====== */

void editorScroll(void) {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
  if (E.rx < E.coloff) E.coloff = E.rx;
  if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab) {
  /* Decide line number width */
  int lnw = 0;
  if (E.opt_number || E.opt_relativenumber) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", E.numrows);
    lnw = strlen(tmp) + 1;
    if (lnw < 4) lnw = 4;
  }

  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;

    if (filerow >= E.numrows) {
      /* Draw ~ for empty lines */
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "nvim-c v%s", EDITOR_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        /* line number column blank */
        if (lnw > 0) {
          abAppend(ab, "\x1b[38;5;240m", 11);
          for (int i = 0; i < lnw; i++) abAppend(ab, " ", 1);
          abAppend(ab, "\x1b[0m", 4);
        }
        if (padding) {
          abAppend(ab, "\x1b[38;5;240m~\x1b[0m", 16);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, "\x1b[1;36m", 7);
        abAppend(ab, welcome, welcomelen);
        abAppend(ab, "\x1b[0m", 4);
      } else {
        if (lnw > 0) {
          abAppend(ab, "\x1b[38;5;240m", 11);
          for (int i = 0; i < lnw; i++) abAppend(ab, " ", 1);
          abAppend(ab, "\x1b[0m", 4);
        }
        abAppend(ab, "\x1b[38;5;240m~\x1b[0m", 16);
      }
    } else {
      /* Draw line number */
      if (lnw > 0) {
        char lnbuf[16];
        int lineno;
        if (E.opt_relativenumber && filerow != E.cy) {
          lineno = abs(filerow - E.cy);
        } else {
          lineno = filerow + 1;
        }
        int is_cur = (filerow == E.cy);
        if (is_cur)
          abAppend(ab, "\x1b[33m", 5);  /* yellow for current line */
        else
          abAppend(ab, "\x1b[38;5;240m", 11);
        snprintf(lnbuf, sizeof(lnbuf), "%*d ", lnw - 1, lineno);
        abAppend(ab, lnbuf, strlen(lnbuf));
        abAppend(ab, "\x1b[0m", 4);
      }

      /* Cursor line highlight bg */
      int is_curline = (E.opt_cursorline && filerow == E.cy);

      /* Draw row content */
      erow *row = &E.row[filerow];
      int len = row->rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols - lnw) len = E.screencols - lnw;

      if (is_curline) abAppend(ab, "\x1b[48;5;236m", 11);

      char *c = &row->render[E.coloff];
      unsigned char *hl = &row->hl[E.coloff];

      /* Visual selection range */
      int vis_start = -1, vis_end = -1;
      if (E.mode == MODE_VISUAL || E.mode == MODE_VISUAL_LINE) {
        int vy_min = E.vy < E.cy ? E.vy : E.cy;
        int vy_max = E.vy > E.cy ? E.vy : E.cy;
        if (filerow >= vy_min && filerow <= vy_max) {
          if (E.mode == MODE_VISUAL_LINE) {
            vis_start = 0;
            vis_end = row->rsize;
          } else {
            if (vy_min == vy_max) {
              int vx_min = E.vx < E.cx ? E.vx : E.cx;
              int vx_max = E.vx > E.cx ? E.vx : E.cx;
              vis_start = vx_min - E.coloff;
              vis_end = vx_max - E.coloff + 1;
            } else if (filerow == vy_min) {
              vis_start = (E.vy < E.cy ? E.vx : E.cx) - E.coloff;
              vis_end = row->rsize;
            } else if (filerow == vy_max) {
              vis_start = 0;
              vis_end = (E.vy > E.cy ? E.vx : E.cx) - E.coloff + 1;
            } else {
              vis_start = 0;
              vis_end = row->rsize;
            }
          }
        }
      }

      int current_color = -1;
      for (int j = 0; j < len; j++) {
        /* Visual selection */
        int in_vis = (vis_start >= 0 && j >= vis_start && j < vis_end);
        if (in_vis) {
          abAppend(ab, "\x1b[48;5;59m", 10);
        }

        if (iscntrl((unsigned char)c[j])) {
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          abAppend(ab, "\x1b[7m", 4);
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3);
          if (is_curline) abAppend(ab, "\x1b[48;5;236m", 11);
          current_color = -1;
        } else if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(ab, "\x1b[0m", 4);
            if (is_curline) abAppend(ab, "\x1b[48;5;236m", 11);
            current_color = -1;
          }
          abAppend(ab, c + j, 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) {
            current_color = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, clen);
          }
          abAppend(ab, c + j, 1);
        }

        if (in_vis) {
          abAppend(ab, "\x1b[0m", 4);
          if (is_curline) abAppend(ab, "\x1b[48;5;236m", 11);
          current_color = -1;
        }
      }
      abAppend(ab, "\x1b[0m", 4);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[0m", 4);

  /* Mode indicator colors */
  const char *modecolor;
  switch (E.mode) {
    case MODE_NORMAL:      modecolor = "\x1b[1;44;37m"; break;
    case MODE_INSERT:      modecolor = "\x1b[1;42;30m"; break;
    case MODE_VISUAL:
    case MODE_VISUAL_LINE: modecolor = "\x1b[1;45;37m"; break;
    case MODE_COMMAND:     modecolor = "\x1b[1;43;30m"; break;
    default:               modecolor = "\x1b[1;47;30m"; break;
  }

  abAppend(ab, modecolor, strlen(modecolor));
  char modebuf[32];
  snprintf(modebuf, sizeof(modebuf), " %s ", E.modename);
  abAppend(ab, modebuf, strlen(modebuf));

  abAppend(ab, "\x1b[0;7m", 6);

  char status[128], rstatus[64];
  int len = snprintf(status, sizeof(status), " %.30s%s",
    E.filename ? E.filename : "[No Name]",
    E.dirty ? " [+]" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d:%d ",
    E.syntax ? E.syntax->filetype : "plain",
    E.cy + 1, E.cx + 1);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[0m", 4);
  abAppend(ab, "\r\n", 2);
}

void editorDrawCommandBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  if (E.mode == MODE_COMMAND) {
    char buf[270];
    snprintf(buf, sizeof(buf), ":%.*s", E.cmdlen, E.cmdbuf);
    abAppend(ab, buf, strlen(buf));
  } else if (E.statusmsg[0] && time(NULL) - E.statusmsg_time < 5) {
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    abAppend(ab, E.statusmsg, msglen);
  }
}

void editorRefreshScreen(void) {
  editorScroll();

  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6);  /* hide cursor */
  abAppend(&ab, "\x1b[H", 3);     /* top-left */

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawCommandBar(&ab);

  /* Position cursor */
  int lnw = 0;
  if (E.opt_number || E.opt_relativenumber) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", E.numrows);
    lnw = strlen(tmp) + 1;
    if (lnw < 4) lnw = 4;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
    (E.cy - E.rowoff) + 1,
    (E.rx - E.coloff) + 1 + lnw);

  if (E.mode == MODE_COMMAND) {
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
      E.screenrows + 2, E.cmdlen + 2);
  }

  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);  /* show cursor */

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/* ====== CURSOR MOVEMENT ====== */

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case 'h':
    case ARROW_LEFT:
      if (E.cx != 0) E.cx--;
      else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].size; }
      break;
    case 'l':
    case ARROW_RIGHT:
      if (row && E.cx < row->size) E.cx++;
      else if (row && E.cx == row->size && E.cy < E.numrows - 1) {
        E.cy++; E.cx = 0;
      }
      break;
    case 'k':
    case ARROW_UP:
      if (E.cy != 0) E.cy--;
      break;
    case 'j':
    case ARROW_DOWN:
      if (E.cy < E.numrows - 1) E.cy++;
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) E.cx = rowlen;
  /* In normal mode, don't go past last char */
  if (E.mode == MODE_NORMAL && E.cx > 0 && E.cx == rowlen) E.cx--;
}

/* Word motions */
int isword(char c) { return isalnum(c) || c == '_'; }

void editorMoveWordForward(void) {
  if (E.cy >= E.numrows) return;
  erow *row = &E.row[E.cy];
  if (E.cx < row->size && isword(row->chars[E.cx])) {
    while (E.cx < row->size && isword(row->chars[E.cx])) E.cx++;
  } else {
    while (E.cx < row->size && !isword(row->chars[E.cx]) && !isspace(row->chars[E.cx])) E.cx++;
  }
  while (E.cx < row->size && isspace(row->chars[E.cx])) E.cx++;
  if (E.cx >= row->size && E.cy < E.numrows - 1) { E.cy++; E.cx = 0; }
}

void editorMoveWordBackward(void) {
  if (E.cy >= E.numrows) return;
  if (E.cx == 0) {
    if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].size - 1; if (E.cx < 0) E.cx = 0; }
    return;
  }
  E.cx--;
  erow *row = &E.row[E.cy];
  while (E.cx > 0 && isspace(row->chars[E.cx])) E.cx--;
  if (isword(row->chars[E.cx])) {
    while (E.cx > 0 && isword(row->chars[E.cx-1])) E.cx--;
  } else {
    while (E.cx > 0 && !isword(row->chars[E.cx-1]) && !isspace(row->chars[E.cx-1])) E.cx--;
  }
}

void editorMoveWordEnd(void) {
  if (E.cy >= E.numrows) return;
  erow *row = &E.row[E.cy];
  if (E.cx < row->size - 1) E.cx++;
  while (E.cx < row->size && isspace(row->chars[E.cx])) E.cx++;
  if (isword(row->chars[E.cx])) {
    while (E.cx < row->size - 1 && isword(row->chars[E.cx+1])) E.cx++;
  } else {
    while (E.cx < row->size - 1 && !isword(row->chars[E.cx+1]) && !isspace(row->chars[E.cx+1])) E.cx++;
  }
}

/* ====== YANK / DELETE ====== */

void editorFreeYank(void) {
  if (E.yank_lines) {
    for (int i = 0; i < E.yank_count; i++) free(E.yank_lines[i]);
    free(E.yank_lines);
    E.yank_lines = NULL;
    E.yank_count = 0;
  }
}

void editorYankLine(int row, int count) {
  editorFreeYank();
  E.yank_count = 0;
  E.yank_lines = malloc(sizeof(char*) * count);
  for (int i = 0; i < count && row + i < E.numrows; i++) {
    E.yank_lines[E.yank_count++] = strdup(E.row[row + i].chars);
  }
  E.yank_is_line = 1;
  editorSetStatusMessage("%d line%s yanked", E.yank_count, E.yank_count>1?"s":"");
}

void editorDeleteLine(int row, int count) {
  editorYankLine(row, count);
  for (int i = 0; i < count && E.numrows > 0; i++) {
    editorDelRow(row < E.numrows ? row : E.numrows - 1);
  }
  if (E.cy >= E.numrows && E.cy > 0) E.cy = E.numrows - 1;
  E.cx = 0;
}

void editorPut(int after) {
  if (!E.yank_lines || E.yank_count == 0) return;
  if (E.yank_is_line) {
    int insert_at = after ? E.cy + 1 : E.cy;
    for (int i = 0; i < E.yank_count; i++) {
      editorInsertRow(insert_at + i, E.yank_lines[i], strlen(E.yank_lines[i]));
    }
    E.cy = insert_at;
    E.cx = 0;
  }
}

/* ====== COMMAND EXECUTION ====== */

void editorExecuteCommand(void) {
  char *cmd = E.cmdbuf;
  editorSetMode(MODE_NORMAL);

  if (!strcmp(cmd, "w")) {
    editorSave();
  } else if (!strncmp(cmd, "w ", 2)) {
    editorSaveAs(cmd + 2);
  } else if (!strcmp(cmd, "q")) {
    if (E.dirty) {
      editorSetStatusMessage("No write since last change (add ! to override)");
    } else {
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
    }
  } else if (!strcmp(cmd, "q!") || !strcmp(cmd, "qa!")) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
  } else if (!strcmp(cmd, "wq") || !strcmp(cmd, "x")) {
    editorSave();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
  } else if (!strcmp(cmd, "set nu") || !strcmp(cmd, "set number")) {
    E.opt_number = 1; E.opt_relativenumber = 0;
    editorSetStatusMessage("Line numbers enabled");
  } else if (!strcmp(cmd, "set nonu") || !strcmp(cmd, "set nonumber")) {
    E.opt_number = 0; E.opt_relativenumber = 0;
    editorSetStatusMessage("Line numbers disabled");
  } else if (!strcmp(cmd, "set rnu") || !strcmp(cmd, "set relativenumber")) {
    E.opt_relativenumber = 1; E.opt_number = 1;
    editorSetStatusMessage("Relative line numbers enabled");
  } else if (!strcmp(cmd, "set nornu") || !strcmp(cmd, "set norelativenumber")) {
    E.opt_relativenumber = 0;
    editorSetStatusMessage("Relative line numbers disabled");
  } else if (!strcmp(cmd, "set cursorline") || !strcmp(cmd, "set cul")) {
    E.opt_cursorline = 1;
    editorSetStatusMessage("Cursorline enabled");
  } else if (!strcmp(cmd, "set nocursorline") || !strcmp(cmd, "set nocul")) {
    E.opt_cursorline = 0;
    editorSetStatusMessage("Cursorline disabled");
  } else if (!strcmp(cmd, "syntax on")) {
    E.opt_syntax = 1;
    editorSelectSyntaxHighlight();
    editorSetStatusMessage("Syntax highlighting on");
  } else if (!strcmp(cmd, "syntax off")) {
    E.opt_syntax = 0;
    for (int i = 0; i < E.numrows; i++)
      memset(E.row[i].hl, HL_NORMAL, E.row[i].rsize);
    editorSetStatusMessage("Syntax highlighting off");
  } else if (!strncmp(cmd, "e ", 2)) {
    /* Open file */
    for (int i = E.numrows - 1; i >= 0; i--) editorDelRow(i);
    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.dirty = 0;
    editorOpen(cmd + 2);
  } else if (cmd[0] >= '1' && cmd[0] <= '9') {
    int line = atoi(cmd);
    if (line > 0 && line <= E.numrows) {
      E.cy = line - 1;
      E.cx = 0;
    }
  } else {
    editorSetStatusMessage("Unknown command: %s", cmd);
  }
}

/* ====== INPUT ====== */

static int quit_times = EDITOR_QUIT_TIMES;

void editorProcessNormal(int c) {
  static char last_cmd = 0;

  /* Count prefix */
  if (c >= '1' && c <= '9' && last_cmd != 'g') {
    E.count_buf = E.count_buf * 10 + (c - '0');
    last_cmd = c;
    return;
  }
  if (c == '0' && E.count_buf > 0) {
    E.count_buf = E.count_buf * 10;
    last_cmd = c;
    return;
  }
  int count = E.count_buf > 0 ? E.count_buf : 1;
  E.count_buf = 0;

  switch (c) {
    /* Mode switches */
    case 'i': editorSetMode(MODE_INSERT); break;
    case 'I':
      E.cx = 0;
      editorSetMode(MODE_INSERT);
      break;
    case 'a':
      if (E.cy < E.numrows && E.cx < E.row[E.cy].size) E.cx++;
      editorSetMode(MODE_INSERT);
      break;
    case 'A':
      if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
      editorSetMode(MODE_INSERT);
      break;
    case 'o':
      if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
      else E.cx = 0;
      editorInsertNewline();
      editorSetMode(MODE_INSERT);
      break;
    case 'O':
      E.cx = 0;
      editorInsertRow(E.cy, "", 0);
      editorSetMode(MODE_INSERT);
      break;
    case 'v': editorSetMode(MODE_VISUAL); E.vx = E.cx; E.vy = E.cy; break;
    case 'V': editorSetMode(MODE_VISUAL_LINE); E.vx = 0; E.vy = E.cy; break;
    case ':':
      editorSetMode(MODE_COMMAND);
      E.cmdlen = 0;
      E.cmdbuf[0] = '\0';
      break;

    /* Motion */
    case 'h': case ARROW_LEFT:
    case 'j': case ARROW_DOWN:
    case 'k': case ARROW_UP:
    case 'l': case ARROW_RIGHT:
      for (int i = 0; i < count; i++) editorMoveCursor(c);
      break;
    case 'w': for (int i = 0; i < count; i++) editorMoveWordForward(); break;
    case 'b': for (int i = 0; i < count; i++) editorMoveWordBackward(); break;
    case 'e': for (int i = 0; i < count; i++) editorMoveWordEnd(); break;
    case '0': case HOME_KEY: E.cx = 0; break;
    case '$': case END_KEY:
      if (E.cy < E.numrows) {
        E.cx = E.row[E.cy].size;
        if (E.cx > 0) E.cx--;
      }
      break;
    case 'G':
      E.cy = E.numrows - 1;
      E.cx = 0;
      break;
    case 'g':
      if (last_cmd == 'g') {
        E.cy = 0; E.cx = 0;
        last_cmd = 0;
        return;
      }
      break;
    case PAGE_UP:
      E.cy = E.rowoff;
      for (int i = 0; i < E.screenrows; i++) editorMoveCursor('k');
      break;
    case PAGE_DOWN:
      E.cy = E.rowoff + E.screenrows - 1;
      if (E.cy > E.numrows) E.cy = E.numrows;
      for (int i = 0; i < E.screenrows; i++) editorMoveCursor('j');
      break;

    /* Editing */
    case 'x':
      if (E.cy < E.numrows && E.cx < E.row[E.cy].size)
        editorRowDelChar(&E.row[E.cy], E.cx);
      if (E.cx >= E.row[E.cy].size && E.cx > 0) E.cx--;
      break;
    case 'r': {
      int rc = editorReadKey();
      if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
        E.row[E.cy].chars[E.cx] = rc;
        editorUpdateRow(&E.row[E.cy]);
        E.dirty++;
      }
      break;
    }
    case 'd':
      if (last_cmd == 'd') {
        editorDeleteLine(E.cy, count);
        last_cmd = 0;
        return;
      }
      break;
    case 'D':
      if (E.cy < E.numrows) {
        E.row[E.cy].size = E.cx;
        E.row[E.cy].chars[E.cx] = '\0';
        editorUpdateRow(&E.row[E.cy]);
        E.dirty++;
      }
      break;
    case 'y':
      if (last_cmd == 'y') {
        editorYankLine(E.cy, count);
        last_cmd = 0;
        return;
      }
      break;
    case 'p': editorPut(1); break;
    case 'P': editorPut(0); break;

    /* Search */
    case '/': editorSearchPrompt(); break;
    case 'n': editorFindNext(1); break;
    case 'N': editorFindNext(-1); break;

    /* Misc */
    case CTRL_KEY('l'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      break;
    case CTRL_KEY('c'):
      break;
  }

  last_cmd = c;
}

void editorProcessInsert(int c) {
  switch (c) {
    case '\x1b':
    case CTRL_KEY('c'):
      editorSetMode(MODE_NORMAL);
      if (E.cx > 0) E.cx--;
      break;
    case '\r': editorInsertNewline(); break;
    case BACKSPACE:
    case CTRL_KEY('h'):
      editorDelChar();
      break;
    case DEL_KEY:
      editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;
    case CTRL_KEY('w'): {
      /* delete word */
      while (E.cx > 0 && (E.cy >= E.numrows || E.cx > 0)) {
        if (E.cy < E.numrows && E.cx > 0 &&
            isspace(E.row[E.cy].chars[E.cx-1])) editorDelChar();
        else break;
      }
      while (E.cy < E.numrows && E.cx > 0 &&
             !isspace(E.row[E.cy].chars[E.cx-1])) editorDelChar();
      break;
    }
    case ARROW_LEFT:  editorMoveCursor(ARROW_LEFT); break;
    case ARROW_RIGHT: editorMoveCursor(ARROW_RIGHT); break;
    case ARROW_UP:    editorMoveCursor(ARROW_UP); break;
    case ARROW_DOWN:  editorMoveCursor(ARROW_DOWN); break;
    case HOME_KEY: E.cx = 0; break;
    case END_KEY:
      if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
      break;
    case '\t': editorInsertChar('\t'); break;
    default:
      if (!iscntrl(c)) editorInsertChar(c);
      break;
  }
}

void editorProcessVisual(int c) {
  switch (c) {
    case '\x1b':
    case CTRL_KEY('c'):
    case 'v':
    case 'V':
      editorSetMode(MODE_NORMAL);
      break;
    case 'h': case 'j': case 'k': case 'l':
    case ARROW_LEFT: case ARROW_RIGHT: case ARROW_UP: case ARROW_DOWN:
    case 'w': case 'b': case 'e':
    case '0': case '$': case 'G':
      editorProcessNormal(c);
      editorSetMode(E.mode == MODE_NORMAL ? MODE_VISUAL : E.mode);
      break;
    case 'd': case 'x': {
      /* Delete selection */
      int vy_min = E.vy < E.cy ? E.vy : E.cy;
      int vy_max = E.vy > E.cy ? E.vy : E.cy;
      if (E.mode == MODE_VISUAL_LINE) {
        editorDeleteLine(vy_min, vy_max - vy_min + 1);
      } else {
        editorDeleteLine(vy_min, vy_max - vy_min + 1);
      }
      editorSetMode(MODE_NORMAL);
      break;
    }
    case 'y': {
      int vy_min = E.vy < E.cy ? E.vy : E.cy;
      int vy_max = E.vy > E.cy ? E.vy : E.cy;
      editorYankLine(vy_min, vy_max - vy_min + 1);
      editorSetMode(MODE_NORMAL);
      E.cy = vy_min; E.cx = 0;
      break;
    }
    case ':':
      editorSetMode(MODE_COMMAND);
      E.cmdlen = 0;
      E.cmdbuf[0] = '\0';
      break;
    default:
      editorMoveCursor(c);
      break;
  }
}

void editorProcessCommand(int c) {
  switch (c) {
    case '\r':
      editorExecuteCommand();
      break;
    case '\x1b':
    case CTRL_KEY('c'):
      editorSetMode(MODE_NORMAL);
      E.cmdlen = 0;
      E.cmdbuf[0] = '\0';
      break;
    case BACKSPACE:
    case CTRL_KEY('h'):
      if (E.cmdlen > 0) E.cmdbuf[--E.cmdlen] = '\0';
      else editorSetMode(MODE_NORMAL);
      break;
    default:
      if (!iscntrl(c) && E.cmdlen < 255) {
        E.cmdbuf[E.cmdlen++] = c;
        E.cmdbuf[E.cmdlen] = '\0';
      }
      break;
  }
}

void editorProcessKeypress(void) {
  int c = editorReadKey();

  switch (E.mode) {
    case MODE_NORMAL:      editorProcessNormal(c); break;
    case MODE_INSERT:      editorProcessInsert(c); break;
    case MODE_VISUAL:
    case MODE_VISUAL_LINE: editorProcessVisual(c); break;
    case MODE_COMMAND:     editorProcessCommand(c); break;
  }
}

/* ====== INIT ====== */

void initEditor(void) {
  E.cx = 0; E.cy = 0; E.rx = 0;
  E.rowoff = 0; E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = NULL;
  E.mode = MODE_NORMAL;
  strcpy(E.modename, "NORMAL");
  E.cmdbuf[0] = '\0';
  E.cmdlen = 0;
  E.searchbuf[0] = '\0';
  E.search_direction = 1;
  E.last_match = -1;
  E.vx = 0; E.vy = 0;
  E.yank_lines = NULL;
  E.yank_count = 0;
  E.yank_is_line = 0;
  E.undo_top = 0;
  E.count_buf = 0;
  E.pending_op = 0;
  E.opt_number = 1;
  E.opt_relativenumber = 0;
  E.opt_syntax = 1;
  E.opt_cursorline = 1;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;  /* status bar + command bar */
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("nvim-c ready | :w save | :q quit | /search | i insert | v visual");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
