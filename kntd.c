#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

#define DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#define ABUF_INIT {NULL, 0}

struct termios orig_termios;

struct abuf {
	char *b;
	int len;
};

typedef struct {
	int size;
	char *chars;
} erow;

struct editorConfig {
	int cx, cy;
	int rowoff;
	int screenrows;
	int screencols;
	struct termios orig_termios;
	int numrows;
	erow *row;
};

struct editorConfig E;

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

void disableRawMode() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(disableRawMode);

	struct termios raw = orig_termios;

	raw.c_lflag &= ~(ECHO | ICANON);

	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		return -1;
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		if (y < E.numrows) {
			int len = E.row[y].size;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, E.row[y].chars, len);
		} else {
			abAppend(ab, "~", 1);
		}

		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\nl", 2);
		}
	}
}

void editorReFreshScreen() {
	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);
	write(STDIN_FILENO, ab.b, ab.len);
	abFree(&ab);

}

void intiEditor() {
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
		perror("getWindowSize");
		_exit(1);
	}
}

void editorMoveCursor(char key) {
	switch (key) {
		case 'a':
			E.cx--;
			break;
		case 'd':
			E.cx++;
			break;
		case 'w':
			E.cy--;
			break;
		case 's':
			E.cy++;
			break;
	}
}

void editorOpen(char *filename) {
	FILE *fp = fopen(filename, "r");
	if (!fp) return;

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
							             line[linelen - 1] == '\n'))
			linelen--;

		E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
		int at = E.numrows;
		E.row[at].size = linelen;
		E.row[at].chars = malloc(linelen + 1);
		memcpy(E.row[at].chars, line, linelen);
		E.row[at].chars[linelen] = '\0';
		E.numrows++;
	}
	free(line);
	fclose(fp);
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1);

	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			switch (seq[1]) {
				case 'A': return 'w';
				case 'B': return 's';
				case 'C': return 'd';
				case 'D': return 'a';
			}
		}
		return '\x1b';
	} else {
		return c;
	}
}

int main(int argc, char*argv[]) {

	enableRawMode();
	intiEditor();
	editorOpen(argv[1]);
	
	while (1) {
		editorReFreshScreen();

		int c = editorReadKey();
		if (c == 'q') break;

		editorMoveCursor(c);
	}
	return 0;

}
