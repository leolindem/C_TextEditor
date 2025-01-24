/*
 * Kilo Editor -- Modern C++ Port
 *
 * Original logic by Snaptoken (https://viewsourcecode.org/snaptoken/kilo/).
 * This version uses C++ containers (std::vector, std::string),
 * RAII, and <fstream> for file IO.
 *
 * Compile: g++ -std=c++17 kilo.cpp -o kilo
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

enum editorKeys
{
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

/*** data ***/

/*
 * Each line of text is stored in an ERow.
 *  - 'chars' holds the actual text of the line
 *  - 'render' is an expanded version that replaces tabs with spaces
 */
struct ERow
{
    std::string chars;  // The actual text of the row
    std::string render; // The rendered version (tabs expanded, etc.)
};

/*
 * The main editor configuration/state struct
 */
struct editorConfig
{
    int cx, cy;     // Cursor x (column) and y (row) position in the file
    int rx;         // Rendered x position (when we account for tabs, etc.)
    int rowoff;     // Offset of the row displayed (top of the screen)
    int coloff;     // Offset of the column displayed (left of the screen)
    int screenrows; // Number of rows we can display
    int screencols; // Number of columns we can display
    bool dirty;     // Track if the file is modified
    std::string filename;
    std::string statusmsg;
    time_t statusmsg_time;

    struct termios orig_termios;

    // Each line in the file is stored in a vector of ERow
    std::vector<ERow> rows;
};

/*** Global editor state ***/
static editorConfig E;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
std::string editorPrompt(const std::string &prompt);

/*** terminal ***/

/**
 * Print an error message (via perror) and exit.
 * Also reset the screen so it isn't left in a weird state.
 */
static void die(const char *s)
{
    // Clear screen and position cursor at home
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    std::exit(1);
}

/**
 * Restore original terminal mode on exit.
 */
static void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

/**
 * Switch the terminal into raw mode:
 *  - No canonical mode, no echo, no signals, etc.
 */
static void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    {
        die("tcgetattr");
    }

    // Ensure we restore normal mode on exit
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    // Input flags
    raw.c_iflag &= static_cast<unsigned int>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    // Output flags
    raw.c_oflag &= static_cast<unsigned int>(~(OPOST));
    // Control flags
    raw.c_cflag |= (CS8);
    // Local flags
    raw.c_lflag &= static_cast<unsigned int>(~(ECHO | ICANON | IEXTEN | ISIG));
    // VMIN/VTIME
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        die("tcsetattr");
    }
}

/**
 * Read one key from stdin, handling escape sequences.
 */
static int editorReadKey()
{
    int nread;
    char c;
    while ((nread = (int)read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\x1b')
    {
        // Possible escape sequence
        char seq[3];
        // If we can't read 2 more bytes, it's just ESC.
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    }

    // Not an escape sequence, just return the character
    return c;
}

/**
 * Use a sequence trick to query the terminal for the cursor position.
 */
static int getCursorPosition(int &rows, int &cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", &rows, &cols) != 2)
        return -1;

    return 0;
}

/**
 * Get the size of the terminal window (rows & columns).
 */
static int getWindowSize(int &rows, int &cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // Fallback if ws is not supported
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    else
    {
        cols = ws.ws_col;
        rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

/**
 * Given a cursor x position (E.cx) in 'row->chars', this computes the
 * corresponding x position in the rendered version (row->render),
 * which accounts for tabs.
 */
static int editorRowCxToRx(const ERow &row, int cx)
{
    int rx = 0;
    for (int j = 0; j < cx; j++)
    {
        if (row.chars[j] == '\t')
        {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

/**
 * Build the 'render' string from 'chars' by expanding tabs into
 * the appropriate number of spaces.
 */
static void editorUpdateRow(ERow &row)
{
    std::ostringstream render;
    for (char c : row.chars)
    {
        if (c == '\t')
        {
            // Insert at least one space
            render << ' ';
            // Insert additional spaces until we hit a tab stop
            while (render.tellp() % KILO_TAB_STOP != 0)
            {
                render << ' ';
            }
        }
        else
        {
            render << c;
        }
    }
    row.render = render.str();
}

/**
 * Insert a new row into E.rows at index 'at'.
 */
static void editorInsertRow(int at, const std::string &s)
{
    if (at < 0 || at > (int)E.rows.size())
        return;

    ERow newRow;
    newRow.chars = s;
    editorUpdateRow(newRow);

    E.rows.insert(E.rows.begin() + at, newRow);
    E.dirty = true;
}

/**
 * Delete row at index 'at'.
 */
static void editorDelRow(int at)
{
    if (at < 0 || at >= (int)E.rows.size())
        return;
    E.rows.erase(E.rows.begin() + at);
    E.dirty = true;
}

/**
 * Insert a single character 'c' into row 'row' at position 'at'.
 */
static void editorRowInsertChar(ERow &row, int at, char c)
{
    if (at < 0 || at > (int)row.chars.size())
    {
        at = (int)row.chars.size();
    }
    row.chars.insert(row.chars.begin() + at, c);
    editorUpdateRow(row);
    E.dirty = true;
}

/**
 * Append a string 's' to row 'row'.
 */
static void editorRowAppendString(ERow &row, const std::string &s)
{
    row.chars += s;
    editorUpdateRow(row);
    E.dirty = true;
}

/**
 * Delete the character at position 'at' in row 'row'.
 */
static void editorRowDelChar(ERow &row, int at)
{
    if (at < 0 || at >= (int)row.chars.size())
        return;
    row.chars.erase(row.chars.begin() + at);
    editorUpdateRow(row);
    E.dirty = true;
}

/*** editor operations ***/

/**
 * Insert a character at the current cursor position. If the cursor
 * is beyond the last row, create a new row first.
 */
static void editorInsertChar(char c)
{
    if (E.cy == (int)E.rows.size())
    {
        // We are on a 'virtual' row past the end, so create a new empty row
        editorInsertRow((int)E.rows.size(), "");
    }
    editorRowInsertChar(E.rows[E.cy], E.cx, c);
    E.cx++;
}

/**
 * Insert a newline at the current cursor. If E.cx == 0,
 * just insert a blank row above the current row; otherwise,
 * split the current line at E.cx.
 */
static void editorInsertNewline()
{
    if (E.cx == 0)
    {
        // Insert an empty row before this one
        editorInsertRow(E.cy, "");
    }
    else
    {
        ERow &row = E.rows[E.cy];
        // The substring from E.cx onward
        std::string splitText = row.chars.substr(E.cx);
        // Trim the current row
        row.chars.erase(E.cx);
        editorUpdateRow(row);

        // Insert the new row below
        editorInsertRow(E.cy + 1, splitText);
    }
    E.cy++;
    E.cx = 0;
}

/**
 * Delete the character behind the cursor. If we're at the start
 * of a line, then we merge with the previous line.
 */
static void editorDelChar()
{
    if (E.cy == (int)E.rows.size())
        return;
    if (E.cx == 0 && E.cy == 0)
        return;

    ERow &row = E.rows[E.cy];
    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        // Merge current row into previous row
        E.cx = (int)E.rows[E.cy - 1].chars.size();
        editorRowAppendString(E.rows[E.cy - 1], row.chars);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

/**
 * Convert all rows to a single string buffer, each row followed by a newline.
 */
static std::string editorRowsToString()
{
    std::ostringstream out;
    for (auto &row : E.rows)
    {
        out << row.chars << "\n";
    }
    return out.str();
}

/**
 * Open a file, read its contents line by line into E.rows.
 */
static void editorOpen(const std::string &filename)
{
    E.filename = filename;

    std::ifstream file(filename);
    if (!file.is_open())
    {
        die(("fopen: " + filename).c_str());
    }

    std::string line;
    while (std::getline(file, line))
    {
        // Strip out any trailing carriage return
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        editorInsertRow((int)E.rows.size(), line);
    }
    file.close();
    E.dirty = false;
}

/**
 * Save the current text buffer to disk. If no filename is set, prompt for one.
 */
static void editorSave()
{
    if (E.filename.empty())
    {
        std::string newName = editorPrompt("Save as: %s (ESC to cancel)");
        if (newName.empty())
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
        E.filename = newName;
    }

    std::ofstream out(E.filename, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
        return;
    }
    std::string buffer = editorRowsToString();
    out << buffer;
    out.close();

    E.dirty = false;
    editorSetStatusMessage("%lu bytes written to disk", (unsigned long)buffer.size());
}

/*** append buffer for rendering ***/

/*
 * A small struct for building output strings more efficiently,
 * similar to a dynamic string or std::stringstream usage.
 */
struct abuf
{
    std::string b;
};

/**
 * Append a string 's' of length 'len' to abuf 'ab'.
 */
static void abAppend(abuf &ab, const char *s, int len)
{
    ab.b.append(s, len);
}

/**
 * Append a C-string (null-terminated) to abuf 'ab'.
 */
static void abAppend(abuf &ab, const char *s)
{
    ab.b.append(s);
}

/*** output ***/

/**
 * Update E.rowoff and E.coloff so that E.cx and E.cy are within
 * the visible window on screen.
 */
static void editorScroll()
{
    E.rx = 0;
    if (E.cy < (int)E.rows.size())
    {
        E.rx = editorRowCxToRx(E.rows[E.cy], E.cx);
    }

    // Vertical scrolling
    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    // Horizontal scrolling
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

/**
 * Draw the rows of text (or '~' / welcome message) for each row on screen.
 */
static void editorDrawRows(abuf &ab)
{
    for (int y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff;
        if (filerow >= (int)E.rows.size())
        {
            // Display welcome message or '~'
            if (E.rows.empty() && y == E.screenrows / 3)
            {
                std::ostringstream ss;
                ss << "Kilo editor -- version " << KILO_VERSION;
                std::string welcome = ss.str();
                if ((int)welcome.size() > E.screencols)
                {
                    welcome.resize(E.screencols);
                }
                int padding = (E.screencols - (int)welcome.size()) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding-- > 0)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome.c_str(), (int)welcome.size());
            }
            else
            {
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            const ERow &row = E.rows[filerow];
            int len = (int)row.render.size() - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;

            if (len > 0)
            {
                abAppend(ab, &row.render[E.coloff], len);
            }
        }

        // Clear to end of line
        abAppend(ab, "\x1b[K", 3);
        // Newline
        abAppend(ab, "\r\n", 2);
    }
}

/**
 * Draw the status bar (filename, dirty status, line count, etc.).
 */
static void editorDrawStatusBar(abuf &ab)
{
    abAppend(ab, "\x1b[7m"); // Invert colors

    // Left status
    std::ostringstream leftStatus;
    leftStatus << (E.filename.empty() ? "[No Name]" : E.filename)
               << " - " << E.rows.size() << " lines"
               << (E.dirty ? " (modified)" : "");

    // Right status
    std::ostringstream rightStatus;
    rightStatus << (E.cy + 1) << "/" << E.rows.size();

    std::string lStr = leftStatus.str();
    std::string rStr = rightStatus.str();

    int len = (int)lStr.size();
    int rlen = (int)rStr.size();

    if (len > E.screencols)
    {
        lStr.resize(E.screencols);
        len = E.screencols;
    }

    abAppend(ab, lStr.c_str(), len);

    // Fill the remainder of the screen with spaces,
    // but place the rightStatus on the right side
    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rStr.c_str(), rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m"); // End invert
    abAppend(ab, "\r\n", 2);
}

/**
 * Draw the message bar (e.statusmsg) at the bottom.
 */
static void editorDrawMessageBar(abuf &ab)
{
    abAppend(ab, "\x1b[K", 3); // clear line
    if (!E.statusmsg.empty())
    {
        int msglen = (int)E.statusmsg.size();
        if (msglen > E.screencols)
            msglen = E.screencols;
        if (time(nullptr) - E.statusmsg_time < 5)
        {
            abAppend(ab, E.statusmsg.c_str(), msglen);
        }
    }
}

/**
 * Refresh the screen: hide cursor, position cursor at top-left,
 * draw rows, status bar, and message bar, then move cursor
 * to the correct position and show it again.
 */
void editorRefreshScreen()
{
    editorScroll();

    abuf ab;
    // Hide cursor
    abAppend(ab, "\x1b[?25l");
    // Move cursor to top-left
    abAppend(ab, "\x1b[H");

    // Draw text and bars
    editorDrawRows(ab);
    editorDrawStatusBar(ab);
    editorDrawMessageBar(ab);

    // Move cursor to correct position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + 1);
    abAppend(ab, buf);

    // Show cursor
    abAppend(ab, "\x1b[?25h");

    write(STDOUT_FILENO, ab.b.data(), ab.b.size());
}

/**
 * Set a timed status message (fades out after a few seconds).
 */
void editorSetStatusMessage(const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    E.statusmsg = buf;
    E.statusmsg_time = time(nullptr);
}

/*** input ***/

/**
 * Prompt the user for input in the status bar. The prompt must contain
 * a "%s" where we insert the user's input. Returns the input string,
 * or empty if the user cancels with ESC.
 */
std::string editorPrompt(const std::string &prompt)
{
    std::string input;
    while (true)
    {
        // Build the status message: prompt + current input
        char status[256];
        snprintf(status, sizeof(status), prompt.c_str(), input.c_str());
        editorSetStatusMessage("%s", status);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (!input.empty())
            {
                input.pop_back();
            }
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            return std::string();
        }
        else if (c == '\r')
        {
            if (!input.empty())
            {
                editorSetStatusMessage("");
                return input;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            input.push_back((char)c);
        }
    }
}

/**
 * Move the cursor in response to arrow keys, page keys, etc.
 */
static void editorMoveCursor(int key)
{
    if (E.rows.empty())
    {
        // No lines, nothing to move over
        return;
    }
    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx > 0)
        {
            E.cx--;
        }
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = (int)E.rows[E.cy].chars.size();
        }
        break;
    case ARROW_RIGHT:
    {
        int rowLen = (int)E.rows[E.cy].chars.size();
        if (E.cx < rowLen)
        {
            E.cx++;
        }
        else if (E.cy < (int)E.rows.size() - 1)
        {
            E.cy++;
            E.cx = 0;
        }
    }
    break;
    case ARROW_UP:
        if (E.cy > 0)
        {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < (int)E.rows.size() - 1)
        {
            E.cy++;
        }
        break;
    default:
        break;
    }
    // Clamp cursor x to the length of the current row
    int rowlen = (int)E.rows[E.cy].chars.size();
    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

/**
 * Process a single keypress from the user: inserts chars,
 * handles special commands, movement, etc.
 */
static void editorProcessKeypress()
{
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();
    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage(
                "WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.",
                quit_times);
            quit_times--;
            return;
        }
        // Clear screen
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        std::exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        E.cx = (int)E.rows[E.cy].chars.size();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
        {
            // By default, DEL key moves cursor right, then backspace.
            // This is a design choice; you could move left.
            editorMoveCursor(ARROW_RIGHT);
        }
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
        {
            E.cy = E.rowoff;
        }
        else
        {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > (int)E.rows.size())
                E.cy = (int)E.rows.size();
        }
        int times = E.screenrows;
        while (times--)
        {
            editorMoveCursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
        }
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        // Do nothing
        break;

    default:
        // Insert normal character
        editorInsertChar((char)c);
        break;
    }
    // Reset quit_times if the user does anything else
    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

/**
 * Initialize the editor by reading the screen size and
 * resetting state variables.
 */
static void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.dirty = false;
    E.filename.clear();
    E.statusmsg.clear();
    E.statusmsg_time = 0;

    if (getWindowSize(E.screenrows, E.screencols) == -1)
    {
        die("getWindowSize");
    }
    // Make room for status bar and message bar
    E.screenrows -= 2;
}

/*** main ***/
int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();

    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while (true)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
