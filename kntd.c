#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>


struct termios orig_termios;

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

struct editorConfig {
	int cx, cy;
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig E;

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

void editorDrawRows() {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		write(STDOUT_FILENO, "~\r\n", 3);
	}
}

void editorReFreshScreen() {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	editorDrawRows();

	write(STDOUT_FILENO, "\x1b[H", 3);

	editorDrawRows();
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


int main() {

	enableRawMode();
	intiEditor();
	
	while (1) {
		editorReFreshScreen();

		int c = editorReadKey();
		if (c == 'q') break;

		editorMoveCursor(c);
	}

		return 0;

}
