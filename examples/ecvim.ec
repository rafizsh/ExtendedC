// ============================================================================
//  ecvim.ec  --  Vim-like terminal editor written in ExtendedC
//
//  Compile:
//    /exec/ec ecvim.ec -o ecvim.ll -I /exec/stdlib
//    clang -O2 ecvim.ll /exec/runtime.o -lm -o ecvim
//
//  Usage:
//    ./ecvim [file.ec ...]
//
//  Modes:
//    Normal   h j k l  w b e  0 ^ $  gg G  i a o I A O  v V  : / ?
//    Insert   type text  Esc to return  Ctrl-t for tab
//    Visual   d y > <  then Esc
//    Command  :w :q :wq :e :compile :run :make :set :syntax :help
//    Search   /pattern  ?pattern  n N  * #
//    Replace  R  r{c}  ~
//
//  F5 = compile + run current file
// ============================================================================

import "std.io";
import "std.string";
import "std.collections";

// ── Key code constants ────────────────────────────────────────────────────────
int KEY_ESC       = 27;
int KEY_ENTER     = 13;
int KEY_BACKSPACE = 127;
int KEY_UP        = 1000;
int KEY_DOWN      = 1001;
int KEY_LEFT      = 1002;
int KEY_RIGHT     = 1003;
int KEY_DEL       = 1004;
int KEY_HOME      = 1005;
int KEY_END       = 1006;
int KEY_PGUP      = 1007;
int KEY_PGDN      = 1008;
int KEY_F1        = 1009;
int KEY_F5        = 1013;

int CTRL_C = 3;
int CTRL_D = 4;
int CTRL_E = 5;
int CTRL_F = 6;
int CTRL_G = 7;
int CTRL_R = 18;
int CTRL_T = 20;
int CTRL_U = 21;
int CTRL_Y = 25;

// ── Mode constants ────────────────────────────────────────────────────────────
int MODE_NORMAL      = 0;
int MODE_INSERT      = 1;
int MODE_VISUAL      = 2;
int MODE_VISUAL_LINE = 3;
int MODE_COMMAND     = 4;
int MODE_SEARCH      = 5;
int MODE_REPLACE     = 6;

// ── Highlight type constants ──────────────────────────────────────────────────
int HL_NORMAL   = 0;
int HL_KEYWORD  = 1;
int HL_TYPE     = 2;
int HL_STRING   = 3;
int HL_CHAR     = 4;
int HL_COMMENT  = 5;
int HL_NUMBER   = 6;
int HL_OPERATOR = 7;
int HL_MATCH    = 8;

// ── ANSI colour codes ─────────────────────────────────────────────────────────
string ANSI_RESET    = "\x1b[0m";
string C_KEYWORD     = "\x1b[38;5;204m";
string C_TYPE        = "\x1b[38;5;81m";
string C_STRING      = "\x1b[38;5;186m";
string C_CHAR        = "\x1b[38;5;179m";
string C_COMMENT     = "\x1b[38;5;102m";
string C_NUMBER      = "\x1b[38;5;141m";
string C_OPERATOR    = "\x1b[38;5;215m";
string C_MATCH       = "\x1b[48;5;58m\x1b[38;5;226m";
string C_VISUAL      = "\x1b[48;5;237m";
string C_CURLINE     = "\x1b[48;5;235m";
string C_LINENO      = "\x1b[38;5;239m";
string C_LINENO_CUR  = "\x1b[38;5;250m";
string C_STATUS_N    = "\x1b[48;5;24m\x1b[38;5;255m";
string C_STATUS_I    = "\x1b[48;5;28m\x1b[38;5;255m";
string C_STATUS_V    = "\x1b[48;5;90m\x1b[38;5;255m";
string C_STATUS_C    = "\x1b[48;5;130m\x1b[38;5;255m";
string C_STATUS_R    = "\x1b[48;5;88m\x1b[38;5;255m";
string C_STATUS_FILE = "\x1b[48;5;237m\x1b[38;5;252m";
string C_STATUS_POS  = "\x1b[48;5;24m\x1b[38;5;255m";

// ── EC syntax keywords ────────────────────────────────────────────────────────
string[] KEYWORDS = {
    "fun", "return", "if", "else", "while", "for", "do", "break", "continue",
    "switch", "case", "default", "class", "extends", "super", "this", "new",
    "delete", "import", "export", "using", "with", "try", "catch", "throw",
    "throws", "instanceof", "typeof", "async", "await", "static", "final",
    "public", "private", "protected", "enum", "struct", "union", "var",
    "null", "true", "false", "const", "as"
};

string[] TYPES = {
    "int", "uint", "long", "ulong", "float", "ufloat", "double",
    "bool", "char", "string", "void"
};

// ── Undo record ───────────────────────────────────────────────────────────────
class UndoRecord {
    public int    utype;   // 0=ins_char 1=del_char 2=ins_line 3=del_line 4=replace_line
    public int    row;
    public int    col;
    public int    ch;
    public string text;

    public fun void init() {
        this.utype = 0; this.row = 0; this.col = 0;
        this.ch = 0; this.text = "";
    }
}

// ── Row (one line of text) ────────────────────────────────────────────────────
class Row {
    public string raw;          // actual content
    public string rendered;     // tabs expanded
    public int[]  hl;           // highlight type per rendered char
    public bool   hlOpenComment; // ends inside block comment

    public fun void init() {
        this.raw           = "";
        this.rendered      = "";
        this.hl            = new int[0];
        this.hlOpenComment = false;
    }

    public fun void initContent(string content) {
        this.raw           = content;
        this.rendered      = content;
        this.hl            = new int[0];
        this.hlOpenComment = false;
    }

    public fun int rawLen()      { return stringLen(this.raw); }
    public fun int renderedLen() { return stringLen(this.rendered); }
}

// ── Register (clipboard) ──────────────────────────────────────────────────────
class Register {
    public List  lines;     // List of string
    public bool  linewise;

    public fun void init() {
        this.lines    = new List();
        this.linewise = false;
    }

    public fun void clear() {
        this.lines    = new List();
        this.linewise = false;
    }

    public fun void yankLines(List rows, int y1, int y2) {
        this.clear();
        this.linewise = true;
        int i = y1;
        while (i <= y2) {
            Row r = rows.get(i) as Row;
            this.lines.add(r.raw);
            i = i + 1;
        }
    }
}

// ── Buffer (one open file) ────────────────────────────────────────────────────
class Buffer {
    public string filename;
    public List   rows;         // List of Row
    public bool   dirty;
    public bool   syntaxOn;

    // cursor
    public int    cx;           // column (raw)
    public int    cy;           // row
    public int    rx;           // rendered column
    public int    rowoff;       // scroll offset
    public int    coloff;

    // visual selection anchor
    public int    visRow;
    public int    visCol;

    // undo stack
    public List   undoStack;    // List of UndoRecord
    public int    undoSaved;    // stack length at last save

    // marks a-z
    public int[]  markRow;
    public int[]  markCol;

    public fun void init() {
        this.filename  = "";
        this.rows      = new List();
        this.dirty     = false;
        this.syntaxOn  = true;
        this.cx        = 0;
        this.cy        = 0;
        this.rx        = 0;
        this.rowoff    = 0;
        this.coloff    = 0;
        this.visRow    = 0;
        this.visCol    = 0;
        this.undoStack = new List();
        this.undoSaved = 0;
        this.markRow   = new int[26];
        this.markCol   = new int[26];
        int i = 0;
        while (i < 26) { this.markRow[i] = -1; this.markCol[i] = -1; i = i + 1; }
    }

    public fun int numRows() { return this.rows.length(); }

    public fun Row getRow(int y) {
        if (y < 0 || y >= this.rows.length()) { return null; }
        return this.rows.get(y) as Row;
    }

    public fun void insertRow(int at, string content) {
        Row r = new Row(); r.init(); r.initContent(content);
        if (at >= this.rows.length()) {
            this.rows.add(r);
        } else {
            this.rows.insert(at, r);
        }
        this.dirty = true;
    }

    public fun void deleteRow(int at) {
        if (at < 0 || at >= this.rows.length()) { return; }
        this.rows.remove(at);
        if (this.rows.length() == 0) { this.insertRow(0, ""); }
        if (this.cy >= this.rows.length()) { this.cy = this.rows.length() - 1; }
        this.dirty = true;
    }

    public fun void undoPush(int utype, int row, int col, int ch, string text) {
        if (this.undoStack.length() >= 100) {
            this.undoStack.remove(0);
            if (this.undoSaved > 0) { this.undoSaved = this.undoSaved - 1; }
        }
        UndoRecord u = new UndoRecord(); u.init();
        u.utype = utype; u.row = row; u.col = col;
        u.ch = ch; u.text = text;
        this.undoStack.add(u);
    }
}

// ── Editor (global state) ─────────────────────────────────────────────────────
class Editor {
    public List   bufs;         // List of Buffer
    public int    curBuf;
    public int    mode;
    public int    termRows;
    public int    termCols;

    // command line
    public string cmd;

    // search
    public string  search;
    public bool    searchFwd;
    public int     searchMatchRow;
    public int     searchMatchCol;

    // find-char (f F t T)
    public int     findChar;
    public int     findOp;

    // count prefix
    public string  countStr;

    // default register
    public Register reg;

    // status message
    public string  statusMsg;
    public long    statusTime;

    // settings
    public int     tabWidth;
    public bool    showLineNums;
    public bool    relativeNums;
    public bool    autoIndent;
    public bool    showWhitespace;

    // render buffer (accumulated output flushed once per frame)
    public StringBuilder renderBuf;

    public fun void init() {
        this.bufs           = new List();
        this.curBuf         = 0;
        this.mode           = 0;
        this.termRows       = 24;
        this.termCols       = 80;
        this.cmd            = "";
        this.search         = "";
        this.searchFwd      = true;
        this.searchMatchRow = -1;
        this.searchMatchCol = -1;
        this.findChar       = 0;
        this.findOp         = 0;
        this.countStr       = "";
        this.reg            = new Register(); this.reg.init();
        this.statusMsg      = "";
        this.statusTime     = 0;
        this.tabWidth       = 4;
        this.showLineNums   = true;
        this.relativeNums   = false;
        this.autoIndent     = true;
        this.showWhitespace = false;
        this.renderBuf      = new StringBuilder();
    }

    public fun Buffer curBuffer() {
        return this.bufs.get(this.curBuf) as Buffer;
    }

    public fun void setStatus(string msg) {
        this.statusMsg  = msg;
        this.statusTime = timeMs() / 1000;
    }

    public fun void r(string s)  { this.renderBuf.append(s); }
    public fun void rc(char c)   { this.renderBuf.appendChar(c); }

    public fun void flush() {
        writeRaw(this.renderBuf.toString());
        flushOutput();
        this.renderBuf.clear();
    }

    public fun void updateTermSize() {
        this.termRows = getTermRows() - 2;
        this.termCols = getTermCols();
    }
}

// ── Global editor instance ────────────────────────────────────────────────────
Editor E;

// ── Utility helpers ───────────────────────────────────────────────────────────

fun bool isWordChar(char c) {
    return isAlnum(c) || c == '_';
}

fun bool isSep(char c) {
    if (c == '\0') { return true; }
    if (isSpace(c)) { return true; }
    string seps = "(){}[]<>=!&|^~;:,.+*/%?\"'";
    int i = 0;
    while (i < stringLen(seps)) {
        if (seps[i] == c) { return true; }
        i = i + 1;
    }
    return false;
}

fun bool matchKeyword(string text, int pos, string kw) {
    int klen = stringLen(kw);
    int tlen = stringLen(text);
    if (pos + klen > tlen) { return false; }
    // match characters
    int i = 0;
    while (i < klen) {
        if (text[pos + i] != kw[i]) { return false; }
        i = i + 1;
    }
    // check word boundary before
    if (pos > 0 && !isSep(text[pos - 1])) { return false; }
    // check word boundary after
    if (pos + klen < tlen && !isSep(text[pos + klen])) { return false; }
    return true;
}

fun string hlColor(int hl) {
    if (hl == 1) { return C_KEYWORD; }
    if (hl == 2) { return C_TYPE; }
    if (hl == 3) { return C_STRING; }
    if (hl == 4) { return C_CHAR; }
    if (hl == 5) { return C_COMMENT; }
    if (hl == 6) { return C_NUMBER; }
    if (hl == 7) { return C_OPERATOR; }
    if (hl == 8) { return C_MATCH; }
    return ANSI_RESET;
}

// ── Tab expansion ─────────────────────────────────────────────────────────────

fun string expandTabs(string raw, int tabW) {
    StringBuilder sb = new StringBuilder();
    int i = 0;
    int col = 0;
    while (i < stringLen(raw)) {
        char c = raw[i];
        if (c == '\t') {
            int spaces = tabW - (col % tabW);
            int s = 0;
            while (s < spaces) { sb.appendChar(' '); s = s + 1; col = col + 1; }
        } else {
            sb.appendChar(c);
            col = col + 1;
        }
        i = i + 1;
    }
    return sb.toString();
}

fun int cxToRx(string raw, int cx, int tabW) {
    int rx = 0;
    int i = 0;
    while (i < cx && i < stringLen(raw)) {
        if (raw[i] == '\t') {
            rx = rx + tabW - (rx % tabW);
        } else {
            rx = rx + 1;
        }
        i = i + 1;
    }
    return rx;
}

fun int rxToCx(string raw, int rx, int tabW) {
    int cur = 0;
    int cx = 0;
    int rlen = stringLen(raw);
    while (cx < rlen) {
        if (raw[cx] == '\t') {
            cur = cur + tabW - (cur % tabW);
        } else {
            cur = cur + 1;
        }
        if (cur > rx) { return cx; }
        cx = cx + 1;
    }
    return rlen;
}

// ── Syntax highlighting ───────────────────────────────────────────────────────

fun void updateHighlight(Buffer b, int y) {
    Row row = b.getRow(y);
    if (row == null) { return; }

    string text = row.rendered;
    int    tlen = stringLen(text);
    row.hl = new int[tlen + 1];

    if (!b.syntaxOn) { return; }

    bool inComment = (y > 0 && (b.getRow(y - 1) as Row).hlOpenComment);
    bool inString  = false;
    char strChar   = '\0';
    int  i = 0;

    while (i < tlen) {
        char c = text[i];
        int prevHl = (i > 0) ? row.hl[i - 1] : 0;

        // Inside block comment
        if (inComment) {
            row.hl[i] = 5;
            if (c == '*' && i + 1 < tlen && text[i + 1] == '/') {
                row.hl[i + 1] = 5;
                i = i + 2;
                inComment = false;
            } else {
                i = i + 1;
            }
            continue;
        }

        // Line comment
        if (!inString && c == '/' && i + 1 < tlen && text[i + 1] == '/') {
            while (i < tlen) { row.hl[i] = 5; i = i + 1; }
            break;
        }

        // Block comment start
        if (!inString && c == '/' && i + 1 < tlen && text[i + 1] == '*') {
            row.hl[i] = 5; row.hl[i + 1] = 5;
            i = i + 2; inComment = true;
            continue;
        }

        // String/char literal
        if (inString) {
            row.hl[i] = (strChar == '"') ? 3 : 4;
            if (c == '\\' && i + 1 < tlen) {
                row.hl[i + 1] = (strChar == '"') ? 3 : 4;
                i = i + 2;
            } else {
                if (c == strChar) { inString = false; }
                i = i + 1;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            inString = true; strChar = c;
            row.hl[i] = (c == '"') ? 3 : 4;
            i = i + 1;
            continue;
        }

        // Number
        if (isDigit(c) && (i == 0 || isSep(text[i - 1]))) {
            while (i < tlen && (isAlnum(text[i]) || text[i] == '_' || text[i] == '.')) {
                row.hl[i] = 6; i = i + 1;
            }
            continue;
        }

        // Keywords and types
        if (isAlpha(c) || c == '_') {
            // check types first
            bool matched = false;
            int ti = 0;
            while (ti < TYPES.length) {
                if (matchKeyword(text, i, TYPES[ti])) {
                    int kl = stringLen(TYPES[ti]);
                    int ki = 0;
                    while (ki < kl) { row.hl[i + ki] = 2; ki = ki + 1; }
                    i = i + kl; matched = true; break;
                }
                ti = ti + 1;
            }
            if (!matched) {
                int ki2 = 0;
                while (ki2 < KEYWORDS.length) {
                    if (matchKeyword(text, i, KEYWORDS[ki2])) {
                        int kl = stringLen(KEYWORDS[ki2]);
                        int ki = 0;
                        while (ki < kl) { row.hl[i + ki] = 1; ki = ki + 1; }
                        i = i + kl; matched = true; break;
                    }
                    ki2 = ki2 + 1;
                }
            }
            if (!matched) {
                while (i < tlen && (isAlnum(text[i]) || text[i] == '_')) { i = i + 1; }
            }
            continue;
        }

        // Operators
        string ops = "+-*/%&|^~<>=!.,:;@#?";
        int oi = 0;
        bool isOp = false;
        while (oi < stringLen(ops)) {
            if (ops[oi] == c) { isOp = true; break; }
            oi = oi + 1;
        }
        if (isOp) { row.hl[i] = 7; }

        i = i + 1;
    }

    bool wasOpen = row.hlOpenComment;
    row.hlOpenComment = inComment;
    // propagate to next row if comment state changed
    if (wasOpen != inComment && y + 1 < b.numRows()) {
        updateHighlight(b, y + 1);
    }
}

fun void updateRowRender(Buffer b, int y) {
    Row row = b.getRow(y);
    if (row == null) { return; }
    row.rendered = expandTabs(row.raw, E.tabWidth);
    updateHighlight(b, y);
}

fun void updateAllRows(Buffer b) {
    int i = 0;
    while (i < b.numRows()) { updateRowRender(b, i); i = i + 1; }
}

// ── Row text operations ───────────────────────────────────────────────────────

fun void rowInsertChar(Buffer b, int y, int at, char c) {
    Row row = b.getRow(y);
    if (row == null) { return; }
    string raw = row.raw;
    int rlen = stringLen(raw);
    if (at < 0)    { at = 0; }
    if (at > rlen) { at = rlen; }
    row.raw = substring(raw, 0, at) + charToString(c) + substring(raw, at, rlen);
    updateRowRender(b, y);
    b.dirty = true;
}

fun void rowDeleteChar(Buffer b, int y, int at) {
    Row row = b.getRow(y);
    if (row == null) { return; }
    string raw = row.raw;
    int rlen = stringLen(raw);
    if (at < 0 || at >= rlen) { return; }
    row.raw = substring(raw, 0, at) + substring(raw, at + 1, rlen);
    updateRowRender(b, y);
    b.dirty = true;
}

fun int firstNonBlank(Row row) {
    if (row == null) { return 0; }
    int i = 0;
    while (i < stringLen(row.raw) && isSpace(row.raw[i])) { i = i + 1; }
    return i;
}

// ── File I/O ──────────────────────────────────────────────────────────────────

fun Buffer newBuffer() {
    Buffer b = new Buffer();
    b.init();
    return b;
}

fun void loadFile(Buffer b, string filename) {
    b.filename = filename;
    string content = readFile(filename);
    if (stringLen(content) == 0) {
        b.insertRow(0, "");
        b.dirty = false;
        return;
    }
    // Split on newlines
    int start = 0;
    int i = 0;
    int clen = stringLen(content);
    while (i < clen) {
        char c = content[i];
        if (c == '\n' || c == '\r') {
            string line = substring(content, start, i);
            // strip \r if present
            int ll = stringLen(line);
            if (ll > 0 && line[ll - 1] == '\r') {
                line = substring(line, 0, ll - 1);
            }
            b.insertRow(b.numRows(), line);
            if (c == '\r' && i + 1 < clen && content[i + 1] == '\n') {
                i = i + 1;
            }
            start = i + 1;
        }
        i = i + 1;
    }
    // Last line if no trailing newline
    if (start < clen) {
        b.insertRow(b.numRows(), substring(content, start, clen));
    }
    if (b.numRows() == 0) { b.insertRow(0, ""); }
    b.dirty = false;
    b.undoStack = new List();
    b.undoSaved = 0;
    updateAllRows(b);
}

fun bool saveFile(Buffer b, string filename) {
    if (stringLen(filename) > 0) { b.filename = filename; }
    if (stringLen(b.filename) == 0) { return false; }
    StringBuilder sb = new StringBuilder();
    int i = 0;
    while (i < b.numRows()) {
        Row row = b.getRow(i);
        sb.append(row.raw);
        sb.appendChar('\n');
        i = i + 1;
    }
    int ok = writeFile(b.filename, sb.toString());
    if (ok != 0) {
        b.dirty = false;
        b.undoSaved = b.undoStack.length();
        return true;
    }
    return false;
}

// ── Cursor / scroll ───────────────────────────────────────────────────────────

fun void clampCursor(Buffer b) {
    if (b.cy < 0) { b.cy = 0; }
    if (b.cy >= b.numRows()) { b.cy = b.numRows() - 1; }
    int rlen = stringLen(b.getRow(b.cy).raw);
    if (b.cx < 0) { b.cx = 0; }
    int maxCx = (E.mode == MODE_INSERT) ? rlen : (rlen > 0 ? rlen - 1 : 0);
    if (b.cx > maxCx) { b.cx = maxCx; }
}

fun void scroll(Buffer b) {
    Row row = b.getRow(b.cy);
    if (row == null) { return; }
    b.rx = cxToRx(row.raw, b.cx, E.tabWidth);
    int lnWidth = E.showLineNums ? 5 : 0;
    int vwidth  = E.termCols - lnWidth;

    if (b.cy < b.rowoff) { b.rowoff = b.cy; }
    if (b.cy >= b.rowoff + E.termRows) { b.rowoff = b.cy - E.termRows + 1; }
    if (b.rx < b.coloff) { b.coloff = b.rx; }
    if (b.rx >= b.coloff + vwidth) { b.coloff = b.rx - vwidth + 1; }
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fun void drawRows(Buffer b) {
    int lnWidth = E.showLineNums ? 5 : 0;
    int vwidth  = E.termCols - lnWidth;

    bool inVis   = (E.mode == MODE_VISUAL || E.mode == MODE_VISUAL_LINE);
    int visR1 = b.visRow; int visR2 = b.cy;
    int visC1 = b.visCol; int visC2 = b.cx;
    if (visR1 > visR2) {
        int tmp = visR1; visR1 = visR2; visR2 = tmp;
        tmp = visC1; visC1 = visC2; visC2 = tmp;
    }

    int screenY = 0;
    while (screenY < E.termRows) {
        int fileY = screenY + b.rowoff;

        // Position and erase line
        E.r("\x1b[");
        E.r(toString(screenY + 1));
        E.r(";1H\x1b[K");

        if (fileY >= b.numRows()) {
            if (E.showLineNums) { E.r(C_LINENO); E.r("    ~"); E.r(ANSI_RESET); }
            else { E.r(C_LINENO); E.r("~"); E.r(ANSI_RESET); }
            screenY = screenY + 1;
            continue;
        }

        // Line number
        if (E.showLineNums) {
            int displayNum = fileY + 1;
            if (E.relativeNums && fileY != b.cy) {
                displayNum = fileY - b.cy;
                if (displayNum < 0) { displayNum = 0 - displayNum; }
            }
            string lnStr = toString(displayNum);
            while (stringLen(lnStr) < 4) { lnStr = " " + lnStr; }
            E.r(fileY == b.cy ? C_LINENO_CUR : C_LINENO);
            E.r(lnStr); E.r(" "); E.r(ANSI_RESET);
        }

        // Current line bg
        if (fileY == b.cy && E.mode != MODE_VISUAL && E.mode != MODE_VISUAL_LINE) {
            E.r(C_CURLINE);
        }

        Row row = b.getRow(fileY);
        int curHl = -1;
        int rendered = 0;

        int rx = b.coloff;
        while (rx < row.renderedLen() && rendered < vwidth) {
            char c = row.rendered[rx];

            // Visual selection
            bool inSel = false;
            if (inVis) {
                int cx = rxToCx(row.raw, rx, E.tabWidth);
                if (E.mode == MODE_VISUAL_LINE) {
                    inSel = (fileY >= visR1 && fileY <= visR2);
                } else {
                    if (visR1 == visR2) {
                        inSel = (fileY == visR1 && cx >= visC1 && cx <= visC2);
                    } else if (fileY == visR1) {
                        inSel = (cx >= visC1);
                    } else if (fileY == visR2) {
                        inSel = (cx <= visC2);
                    } else {
                        inSel = (fileY > visR1 && fileY < visR2);
                    }
                }
            }

            // Search match
            bool inMatch = false;
            if (stringLen(E.search) > 0 && fileY == E.searchMatchRow) {
                int cx = rxToCx(row.raw, rx, E.tabWidth);
                int mlen = stringLen(E.search);
                inMatch = (cx >= E.searchMatchCol && cx < E.searchMatchCol + mlen);
            }

            int hl = (row.hl.length > rx) ? row.hl[rx] : 0;

            if (inSel) {
                E.r(C_VISUAL);
            } else if (inMatch) {
                E.r(C_MATCH); curHl = -1;
            } else if (hl != curHl) {
                curHl = hl;
                E.r(hlColor(hl));
            }

            if (E.showWhitespace && c == ' ') {
                E.r("\xc2\xb7");
            } else if (isAlpha(c) || isDigit(c) || c == ' ' || c == '\t'
                       || charCode(c) >= 32) {
                E.rc(c);
            } else {
                E.r("?");
            }

            if (inSel || inMatch) { E.r(ANSI_RESET); curHl = -1; }

            rendered = rendered + 1;
            rx = rx + 1;
        }
        E.r(ANSI_RESET);
        screenY = screenY + 1;
    }
}

fun void drawStatusBar(Buffer b) {
    E.r("\x1b[");
    E.r(toString(E.termRows + 1));
    E.r(";1H");

    string modeStr;
    string modeColor;
    if (E.mode == MODE_INSERT)      { modeStr = " INSERT ";  modeColor = C_STATUS_I; }
    else if (E.mode == MODE_VISUAL || E.mode == MODE_VISUAL_LINE) {
                                      modeStr = " VISUAL ";  modeColor = C_STATUS_V; }
    else if (E.mode == MODE_COMMAND){ modeStr = " COMMAND "; modeColor = C_STATUS_C; }
    else if (E.mode == MODE_REPLACE){ modeStr = " REPLACE "; modeColor = C_STATUS_R; }
    else                            { modeStr = " NORMAL ";  modeColor = C_STATUS_N; }

    E.r(modeColor); E.r(modeStr); E.r(ANSI_RESET);
    E.r(C_STATUS_FILE);

    string fname = stringLen(b.filename) > 0 ? b.filename : "[No Name]";
    string dirty = b.dirty ? " [+]" : "";
    E.r("  "); E.r(fname); E.r(dirty); E.r("  ");

    if (stringLen(E.countStr) > 0) {
        E.r("\x1b[38;5;226m"); E.r(E.countStr); E.r(C_STATUS_FILE);
    }

    // Right side: line:col pct
    int pct = b.numRows() > 0 ? (b.cy * 100 / b.numRows()) : 0;
    string right = "  " + toString(b.cy + 1) + ":" + toString(b.cx + 1) +
                   "  " + toString(pct) + "%  ";

    int used = stringLen(modeStr) + stringLen(fname) + stringLen(dirty) +
               4 + stringLen(E.countStr);
    int rlen  = stringLen(right);
    int pad   = E.termCols - used - rlen;
    int p = 0;
    while (p < pad) { E.rc(' '); p = p + 1; }

    E.r(C_STATUS_POS); E.r(right); E.r(ANSI_RESET);
    E.r("\x1b[K");
}

fun void drawCommandLine() {
    E.r("\x1b[");
    E.r(toString(E.termRows + 2));
    E.r(";1H\x1b[K");

    long now = timeMs() / 1000;
    if (now - E.statusTime < 4 && stringLen(E.statusMsg) > 0) {
        E.r("\x1b[38;5;226m");
        E.r(E.statusMsg);
        E.r(ANSI_RESET);
        return;
    }

    if (E.mode == MODE_COMMAND) {
        E.r("\x1b[38;5;255m:");
        E.r(E.cmd);
        E.r(ANSI_RESET);
    } else if (E.mode == MODE_SEARCH) {
        E.r("\x1b[38;5;255m");
        E.rc(E.searchFwd ? '/' : '?');
        E.r(E.search);
        E.r(ANSI_RESET);
    }
}

fun void positionCursor(Buffer b) {
    int lnWidth = E.showLineNums ? 5 : 0;
    string pos;

    if (E.mode == MODE_COMMAND) {
        pos = "\x1b[" + toString(E.termRows + 2) + ";" +
              toString(stringLen(E.cmd) + 2) + "H";
    } else if (E.mode == MODE_SEARCH) {
        pos = "\x1b[" + toString(E.termRows + 2) + ";" +
              toString(stringLen(E.search) + 2) + "H";
    } else {
        int sr = b.cy - b.rowoff + 1;
        int sc = b.rx - b.coloff + 1 + lnWidth;
        pos = "\x1b[" + toString(sr) + ";" + toString(sc) + "H";
    }
    E.r(pos);
}

fun void refreshScreen() {
    Buffer b = E.curBuffer();
    scroll(b);
    E.r("\x1b[?25l\x1b[H");
    drawRows(b);
    drawStatusBar(b);
    drawCommandLine();
    positionCursor(b);
    E.r("\x1b[?25h");
    E.flush();
}

// ── Search ────────────────────────────────────────────────────────────────────

fun void searchFind(Buffer b, bool forward) {
    if (stringLen(E.search) == 0) { return; }
    int nrows = b.numRows();
    int startRow = b.cy;
    int slen = stringLen(E.search);

    int pass = 0;
    while (pass < 2) {
        int row = (pass == 0) ? startRow : (forward ? 0 : nrows - 1);
        int col = (pass == 0) ? (forward ? b.cx + 1 : b.cx - 1)
                               : (forward ? 0 : stringLen(b.getRow(row).raw));

        while (forward ? row < nrows : row >= 0) {
            Row r = b.getRow(row);
            string rawText = r.raw;
            int rlen = stringLen(rawText);

            if (forward) {
                if (col < 0) { col = 0; }
                int found = -1;
                int i = col;
                while (i <= rlen - slen) {
                    if (substring(rawText, i, i + slen) == E.search) { found = i; break; }
                    i = i + 1;
                }
                if (found >= 0) {
                    b.cy = row; b.cx = found;
                    E.searchMatchRow = row; E.searchMatchCol = found;
                    E.setStatus("/" + E.search);
                    return;
                }
                col = 0; row = row + 1;
            } else {
                if (col >= rlen) { col = rlen - 1; }
                int found = -1;
                int i = col - slen + 1;
                if (i < 0) { i = 0; }
                while (i <= col) {
                    if (i + slen <= rlen &&
                        substring(rawText, i, i + slen) == E.search) { found = i; }
                    i = i + 1;
                }
                if (found >= 0) {
                    b.cy = row; b.cx = found;
                    E.searchMatchRow = row; E.searchMatchCol = found;
                    return;
                }
                row = row - 1;
                if (row >= 0) { col = stringLen(b.getRow(row).raw); }
            }
        }

        if (pass == 0) {
            startRow = forward ? 0 : nrows - 1;
        }
        pass = pass + 1;
    }
    E.setStatus("Pattern not found: " + E.search);
    E.searchMatchRow = -1;
}

// ── Edit operations ───────────────────────────────────────────────────────────

fun void insertChar(Buffer b, char c) {
    if (b.cy >= b.numRows()) { b.insertRow(b.numRows(), ""); }
    b.undoPush(0, b.cy, b.cx, charCode(c), "");
    rowInsertChar(b, b.cy, b.cx, c);
    b.cx = b.cx + 1;
}

fun void insertNewline(Buffer b) {
    Row r = b.getRow(b.cy);
    b.undoPush(2, b.cy, b.cx, 0, r.raw);

    // Build indent for new line
    string indent = "";
    if (E.autoIndent) {
        int ii = 0;
        while (ii < stringLen(r.raw) && isSpace(r.raw[ii])) {
            indent = indent + charToString(r.raw[ii]);
            ii = ii + 1;
        }
        // Extra indent after {
        int lx = stringLen(r.raw) - 1;
        while (lx >= 0 && isSpace(r.raw[lx])) { lx = lx - 1; }
        if (lx >= 0 && r.raw[lx] == '{') {
            int s = 0;
            while (s < E.tabWidth) { indent = indent + " "; s = s + 1; }
        }
    }

    // Split current line
    string rest = substring(r.raw, b.cx, stringLen(r.raw));
    r.raw = substring(r.raw, 0, b.cx);
    updateRowRender(b, b.cy);

    b.cy = b.cy + 1;
    b.insertRow(b.cy, indent + rest);

    // De-indent if new line starts with }
    if (E.autoIndent && stringLen(indent) >= E.tabWidth) {
        Row newRow = b.getRow(b.cy);
        int nx = firstNonBlank(newRow);
        if (nx < stringLen(newRow.raw) && newRow.raw[nx] == '}') {
            newRow.raw = substring(newRow.raw, E.tabWidth, stringLen(newRow.raw));
            updateRowRender(b, b.cy);
            indent = substring(indent, E.tabWidth, stringLen(indent));
        }
    }

    b.cx = stringLen(indent);
    b.dirty = true;
}

fun void deleteCharBefore(Buffer b) {
    if (b.cx == 0) {
        if (b.cy == 0) { return; }
        // Join with previous line
        b.undoPush(3, b.cy, b.cx, 0, b.getRow(b.cy).raw);
        int prevLen = stringLen(b.getRow(b.cy - 1).raw);
        Row prevRow = b.getRow(b.cy - 1);
        prevRow.raw = prevRow.raw + b.getRow(b.cy).raw;
        updateRowRender(b, b.cy - 1);
        b.deleteRow(b.cy);
        b.cy = b.cy - 1;
        b.cx = prevLen;
    } else {
        Row r = b.getRow(b.cy);
        b.undoPush(1, b.cy, b.cx - 1, charCode(r.raw[b.cx - 1]), "");
        rowDeleteChar(b, b.cy, b.cx - 1);
        b.cx = b.cx - 1;
    }
}

fun void deleteToEOL(Buffer b) {
    Row r = b.getRow(b.cy);
    if (b.cx >= stringLen(r.raw)) { return; }
    b.undoPush(4, b.cy, b.cx, 0, r.raw);
    r.raw = substring(r.raw, 0, b.cx);
    updateRowRender(b, b.cy);
    b.dirty = true;
}

fun void deleteLine(Buffer b, int y) {
    b.undoPush(3, y, 0, 0, b.getRow(y).raw);
    b.deleteRow(y);
    b.cx = 0;
}

fun void indentLine(Buffer b, int y, int dir) {
    Row r = b.getRow(y);
    b.undoPush(4, y, 0, 0, r.raw);
    if (dir > 0) {
        string spaces = "";
        int i = 0;
        while (i < E.tabWidth) { spaces = spaces + " "; i = i + 1; }
        r.raw = spaces + r.raw;
    } else {
        int remove = 0;
        while (remove < E.tabWidth && remove < stringLen(r.raw) && r.raw[remove] == ' ') {
            remove = remove + 1;
        }
        if (remove > 0) { r.raw = substring(r.raw, remove, stringLen(r.raw)); }
    }
    updateRowRender(b, y);
    b.dirty = true;
}

// ── Motion helpers ────────────────────────────────────────────────────────────

fun void motionW(Buffer b, bool bigWord) {
    Row r = b.getRow(b.cy);
    int x = b.cx;
    int rlen = stringLen(r.raw);

    if (x < rlen) {
        char c = r.raw[x];
        bool onWord = bigWord ? !isSpace(c) : isWordChar(c);
        if (onWord) {
            while (x < rlen) {
                char cc = r.raw[x];
                bool still = bigWord ? !isSpace(cc) : isWordChar(cc);
                if (!still) { break; }
                x = x + 1;
            }
        } else if (!isSpace(c)) {
            while (x < rlen) {
                char cc = r.raw[x];
                if (isSpace(cc) || (bigWord ? false : isWordChar(cc))) { break; }
                x = x + 1;
            }
        }
    }
    while (x < rlen && isSpace(r.raw[x])) { x = x + 1; }
    if (x >= rlen && b.cy + 1 < b.numRows()) {
        b.cy = b.cy + 1; x = 0;
        r = b.getRow(b.cy);
        while (x < stringLen(r.raw) && isSpace(r.raw[x])) { x = x + 1; }
    }
    b.cx = x;
}

fun void motionB(Buffer b, bool bigWord) {
    int x = b.cx;
    if (x == 0 && b.cy > 0) {
        b.cy = b.cy - 1;
        x = stringLen(b.getRow(b.cy).raw);
        if (x > 0) { x = x - 1; }
    } else if (x > 0) {
        x = x - 1;
    }
    Row r = b.getRow(b.cy);
    while (x > 0 && isSpace(r.raw[x])) { x = x - 1; }
    char c = r.raw[x];
    bool onWord = bigWord ? !isSpace(c) : isWordChar(c);
    if (onWord) {
        while (x > 0) {
            char cc = r.raw[x - 1];
            bool prev = bigWord ? !isSpace(cc) : isWordChar(cc);
            if (!prev) { break; }
            x = x - 1;
        }
    } else {
        while (x > 0) {
            char cc = r.raw[x - 1];
            if (isSpace(cc) || (bigWord ? false : isWordChar(cc))) { break; }
            x = x - 1;
        }
    }
    b.cx = x;
}

fun void motionE(Buffer b, bool bigWord) {
    Row r = b.getRow(b.cy);
    int x = b.cx;
    int rlen = stringLen(r.raw);
    if (x + 1 < rlen) { x = x + 1; }
    while (x + 1 < rlen && isSpace(r.raw[x])) { x = x + 1; }
    char c = r.raw[x];
    bool onWord = bigWord ? !isSpace(c) : isWordChar(c);
    if (onWord) {
        while (x + 1 < rlen) {
            char cc = r.raw[x + 1];
            bool next = bigWord ? !isSpace(cc) : isWordChar(cc);
            if (!next) { break; }
            x = x + 1;
        }
    } else {
        while (x + 1 < rlen) {
            char cc = r.raw[x + 1];
            if (isSpace(cc) || (bigWord ? false : isWordChar(cc))) { break; }
            x = x + 1;
        }
    }
    b.cx = x;
}

fun bool findCharFwd(Buffer b, char c, bool till) {
    Row r = b.getRow(b.cy);
    int x = b.cx + 1;
    while (x < stringLen(r.raw)) {
        if (r.raw[x] == c) { b.cx = till ? x - 1 : x; return true; }
        x = x + 1;
    }
    return false;
}

fun bool findCharBwd(Buffer b, char c, bool till) {
    Row r = b.getRow(b.cy);
    int x = b.cx - 1;
    while (x >= 0) {
        if (r.raw[x] == c) { b.cx = till ? x + 1 : x; return true; }
        x = x - 1;
    }
    return false;
}

// ── Compile / run ─────────────────────────────────────────────────────────────

fun void runShellCmd(string cmd) {
    termRestore();
    writeRaw("\x1b[2J\x1b[H");
    writeRaw("\x1b[1;33m─── Running: ");
    writeRaw(cmd);
    writeRaw(" ───\x1b[0m\n\n");
    flushOutput();

    int ret = runSystem(cmd);

    writeRaw("\n\x1b[1;33m─── Exit: ");
    writeRaw(toString(ret));
    writeRaw(" — Press ENTER ───\x1b[0m\n");
    flushOutput();
    while (readKey() != KEY_ENTER) {}

    termRaw();
    E.updateTermSize();
}

fun string stripExt(string fname) {
    int i = stringLen(fname) - 1;
    while (i >= 0 && fname[i] != '.') { i = i - 1; }
    if (i > 0) { return substring(fname, 0, i); }
    return fname;
}

fun void doCompile(Buffer b) {
    if (stringLen(b.filename) == 0) { E.setStatus("No filename — :w first"); return; }
    if (b.dirty) { saveFile(b, ""); }
    string base   = stripExt(b.filename);
    string irPath = base + ".ll";
    string binPath = base;
    string cmd = "/exec/ec '" + b.filename + "' -o '" + irPath +
                 "' -I /exec/stdlib && clang -O2 '" + irPath +
                 "' /exec/runtime.o -lm -o '" + binPath + "'";
    runShellCmd(cmd);
    E.setStatus("Compiled: " + binPath);
}

fun void doRun(Buffer b) {
    if (stringLen(b.filename) == 0) { E.setStatus("No filename"); return; }
    string binPath = stripExt(b.filename);
    runShellCmd(binPath);
}

// ── Command processing ────────────────────────────────────────────────────────

fun void processCommand(Buffer b) {
    string cmd = E.cmd;
    // Trim leading spaces
    while (stringLen(cmd) > 0 && cmd[0] == ' ') {
        cmd = substring(cmd, 1, stringLen(cmd));
    }

    // :N — go to line
    if (stringLen(cmd) > 0 && isDigit(cmd[0])) {
        int n = parseInt(cmd) - 1;
        if (n < 0) { n = 0; }
        if (n >= b.numRows()) { n = b.numRows() - 1; }
        b.cy = n; b.cx = firstNonBlank(b.getRow(n));
        E.mode = MODE_NORMAL; return;
    }

    if (cmd == "q") {
        if (b.dirty) { E.setStatus("Unsaved changes — :q! to force"); E.mode = MODE_NORMAL; return; }
        termRestore(); writeRaw("\x1b[2J\x1b[H"); flushOutput(); exit(0);
    }
    if (cmd == "q!") {
        termRestore(); writeRaw("\x1b[2J\x1b[H"); flushOutput(); exit(0);
    }
    if (cmd == "wq" || cmd == "x") {
        saveFile(b, ""); termRestore(); writeRaw("\x1b[2J\x1b[H"); flushOutput(); exit(0);
    }
    if (cmd == "w" || startsWith(cmd, "w ")) {
        string fname = "";
        if (stringLen(cmd) > 2) { fname = substring(cmd, 2, stringLen(cmd)); }
        bool ok = saveFile(b, fname);
        if (ok) { E.setStatus("Written: " + b.filename + " (" + toString(b.numRows()) + " lines)"); }
        else    { E.setStatus("Write failed"); }
    }
    if (startsWith(cmd, "e ")) {
        string fname = substring(cmd, 2, stringLen(cmd));
        Buffer nb = newBuffer();
        loadFile(nb, fname);
        E.bufs.add(nb);
        E.curBuf = E.bufs.length() - 1;
    }
    if (cmd == "bn") {
        E.curBuf = (E.curBuf + 1) % E.bufs.length();
    }
    if (cmd == "bp") {
        int nb2 = E.curBuf - 1;
        if (nb2 < 0) { nb2 = E.bufs.length() - 1; }
        E.curBuf = nb2;
    }
    if (cmd == "compile") { doCompile(b); E.mode = MODE_NORMAL; return; }
    if (cmd == "run")     { doRun(b);     E.mode = MODE_NORMAL; return; }
    if (cmd == "make")    { doCompile(b); doRun(b); E.mode = MODE_NORMAL; return; }
    if (startsWith(cmd, "set ")) {
        string opt = substring(cmd, 4, stringLen(cmd));
        while (stringLen(opt) > 0 && opt[0] == ' ') {
            opt = substring(opt, 1, stringLen(opt));
        }
        if (opt == "nu")          { E.showLineNums = true; }
        else if (opt == "nonu")   { E.showLineNums = false; }
        else if (opt == "rnu")    { E.relativeNums = true;  E.showLineNums = true; }
        else if (opt == "nornu")  { E.relativeNums = false; }
        else if (opt == "ai")     { E.autoIndent = true; }
        else if (opt == "noai")   { E.autoIndent = false; }
        else if (opt == "list")   { E.showWhitespace = true; }
        else if (opt == "nolist") { E.showWhitespace = false; }
        else if (startsWith(opt, "ts=")) {
            int ts = parseInt(substring(opt, 3, stringLen(opt)));
            if (ts >= 1 && ts <= 16) {
                E.tabWidth = ts;
                updateAllRows(b);
            }
        }
        E.setStatus("set " + opt);
    }
    if (startsWith(cmd, "syntax ")) {
        string arg = substring(cmd, 7, stringLen(cmd));
        b.syntaxOn = (arg == "on");
        updateAllRows(b);
        E.setStatus("syntax " + arg);
    }
    if (cmd == "help") {
        E.setStatus("i=Insert v=Visual :=Cmd /=Search  :compile :run :make  F5=make");
    }

    E.mode = MODE_NORMAL;
}

// ── Normal mode ───────────────────────────────────────────────────────────────

fun int getCount() {
    if (stringLen(E.countStr) == 0) { return 1; }
    int n = parseInt(E.countStr);
    E.countStr = "";
    return n > 0 ? n : 1;
}

fun void processNormal(int key) {
    Buffer b = E.curBuffer();

    // Count prefix
    if (key >= 49 && key <= 57) {  // '1'-'9'
        E.countStr = E.countStr + charToString(charFromCode(key));
        return;
    }
    if (key == 48 && stringLen(E.countStr) > 0) {  // '0' in a count
        E.countStr = E.countStr + "0";
        return;
    }

    int n = getCount();

    // Movement
    if (key == 104 || key == KEY_LEFT)  { int i=0; while(i<n){if(b.cx>0)b.cx=b.cx-1;i=i+1;} }  // h
    if (key == 108 || key == KEY_RIGHT) {  // l
        int i = 0;
        while (i < n) {
            int rlen = stringLen(b.getRow(b.cy).raw);
            int mx = rlen > 0 ? rlen - 1 : 0;
            if (b.cx < mx) { b.cx = b.cx + 1; }
            i = i + 1;
        }
    }
    if (key == 106 || key == KEY_DOWN)  { int i=0; while(i<n){if(b.cy<b.numRows()-1)b.cy=b.cy+1;i=i+1;} }  // j
    if (key == 107 || key == KEY_UP)    { int i=0; while(i<n){if(b.cy>0)b.cy=b.cy-1;i=i+1;} }  // k

    if (key == 48 || key == KEY_HOME)   { b.cx = 0; }                               // 0
    if (key == 94)                      { b.cx = firstNonBlank(b.getRow(b.cy)); }   // ^
    if (key == 36 || key == KEY_END) {                                               // $
        int rlen = stringLen(b.getRow(b.cy).raw);
        b.cx = rlen > 0 ? rlen - 1 : 0;
    }

    if (key == 103) {  // g — gg
        int k2 = readKey();
        if (k2 == 103) { b.cy = 0; b.cx = firstNonBlank(b.getRow(0)); }
    }
    if (key == 71) {  // G
        int line = n > 1 ? n - 1 : b.numRows() - 1;
        if (line >= b.numRows()) { line = b.numRows() - 1; }
        b.cy = line; b.cx = firstNonBlank(b.getRow(b.cy));
    }

    if (key == CTRL_F || key == KEY_PGDN) { int i=0; while(i<n){b.cy=b.cy+E.termRows; if(b.cy>=b.numRows())b.cy=b.numRows()-1; i=i+1;} }
    if (key == CTRL_C || key == KEY_PGUP) { int i=0; while(i<n){b.cy=b.cy-E.termRows; if(b.cy<0)b.cy=0; i=i+1;} }  // reusing CTRL_C for page-up (safe in normal mode)
    if (key == CTRL_D) { b.cy=b.cy+E.termRows/2; if(b.cy>=b.numRows())b.cy=b.numRows()-1; }
    if (key == CTRL_U) { b.cy=b.cy-E.termRows/2; if(b.cy<0)b.cy=0; }
    if (key == CTRL_E) { if(b.rowoff<b.numRows()-1)b.rowoff=b.rowoff+1; }
    if (key == CTRL_Y) { if(b.rowoff>0)b.rowoff=b.rowoff-1; }

    // Word motions
    if (key == 119) { int i=0; while(i<n){motionW(b,false);i=i+1;} }  // w
    if (key == 87)  { int i=0; while(i<n){motionW(b,true);i=i+1;} }   // W
    if (key == 98)  { int i=0; while(i<n){motionB(b,false);i=i+1;} }  // b
    if (key == 66)  { int i=0; while(i<n){motionB(b,true);i=i+1;} }   // B
    if (key == 101) { int i=0; while(i<n){motionE(b,false);i=i+1;} }  // e
    if (key == 69)  { int i=0; while(i<n){motionE(b,true);i=i+1;} }   // E

    // Find char
    if (key == 102 || key == 70 || key == 116 || key == 84) {  // f F t T
        int ch2 = readKey();
        E.findChar = ch2; E.findOp = key;
        bool fwd  = (key == 102 || key == 116);
        bool till = (key == 116 || key == 84);
        char fc = charFromCode(ch2);
        int i = 0;
        while (i < n) {
            if (fwd) { findCharFwd(b, fc, till); } else { findCharBwd(b, fc, till); }
            i = i + 1;
        }
    }
    if (key == 59 && E.findChar > 0) {  // ;
        bool fwd  = (E.findOp == 102 || E.findOp == 116);
        bool till = (E.findOp == 116 || E.findOp == 84);
        char fc = charFromCode(E.findChar);
        if (fwd) { findCharFwd(b, fc, till); } else { findCharBwd(b, fc, till); }
    }
    if (key == 44 && E.findChar > 0) {  // ,
        bool fwd  = (E.findOp == 102 || E.findOp == 116);
        bool till = (E.findOp == 116 || E.findOp == 84);
        char fc = charFromCode(E.findChar);
        if (!fwd) { findCharFwd(b, fc, till); } else { findCharBwd(b, fc, till); }
    }

    // Enter insert
    if (key == 105) { E.mode = MODE_INSERT; }  // i
    if (key == 73)  { b.cx = firstNonBlank(b.getRow(b.cy)); E.mode = MODE_INSERT; }  // I
    if (key == 97)  { if(stringLen(b.getRow(b.cy).raw)>0)b.cx=b.cx+1; E.mode=MODE_INSERT; }  // a
    if (key == 65)  { b.cx = stringLen(b.getRow(b.cy).raw); E.mode = MODE_INSERT; }  // A
    if (key == 111) { b.cx = stringLen(b.getRow(b.cy).raw); insertNewline(b); E.mode = MODE_INSERT; }  // o
    if (key == 79)  {  // O
        if (b.cy > 0) { b.cy = b.cy - 1; }
        b.cx = stringLen(b.getRow(b.cy).raw);
        insertNewline(b);
        E.mode = MODE_INSERT;
    }

    // Replace mode
    if (key == 82) { E.mode = MODE_REPLACE; }  // R

    // Replace single char
    if (key == 114) {  // r
        int ch2 = readKey();
        if (ch2 >= 32 && ch2 < 127) {
            Row r = b.getRow(b.cy);
            if (b.cx < stringLen(r.raw)) {
                b.undoPush(4, b.cy, b.cx, 0, r.raw);
                r.raw = substring(r.raw, 0, b.cx) + charToString(charFromCode(ch2)) +
                        substring(r.raw, b.cx + 1, stringLen(r.raw));
                updateRowRender(b, b.cy);
                b.dirty = true;
            }
        }
    }

    // Toggle case ~
    if (key == 126) {
        Row r = b.getRow(b.cy);
        if (b.cx < stringLen(r.raw)) {
            char c = r.raw[b.cx];
            char nc = isLower(c) ? toUpperChar(c) : (isUpper(c) ? toLowerChar(c) : c);
            b.undoPush(4, b.cy, b.cx, 0, r.raw);
            r.raw = substring(r.raw, 0, b.cx) + charToString(nc) +
                    substring(r.raw, b.cx + 1, stringLen(r.raw));
            updateRowRender(b, b.cy);
            if (b.cx < stringLen(r.raw) - 1) { b.cx = b.cx + 1; }
            b.dirty = true;
        }
    }

    // Visual
    if (key == 118) { E.mode=MODE_VISUAL;      b.visRow=b.cy; b.visCol=b.cx; }  // v
    if (key == 86)  { E.mode=MODE_VISUAL_LINE; b.visRow=b.cy; b.visCol=0; }     // V

    // Delete operations
    if (key == 120) {  // x
        int i = 0;
        while (i < n) {
            Row r = b.getRow(b.cy);
            if (b.cx < stringLen(r.raw)) { rowDeleteChar(b, b.cy, b.cx); }
            i = i + 1;
        }
        int rlen = stringLen(b.getRow(b.cy).raw);
        if (b.cx >= rlen && b.cx > 0) { b.cx = b.cx - 1; }
    }
    if (key == 88) {  // X
        int i = 0;
        while (i < n) { deleteCharBefore(b); i = i + 1; }
    }
    if (key == 100) {  // d
        int k2 = readKey();
        if (k2 == 100) {  // dd
            int y2 = b.cy + n - 1;
            if (y2 >= b.numRows()) { y2 = b.numRows() - 1; }
            E.reg.yankLines(b.rows, b.cy, y2);
            int i = 0;
            while (i < n) { deleteLine(b, b.cy); i = i + 1; }
        } else if (k2 == 36 || k2 == 68) {  // d$ or dD
            deleteToEOL(b);
        } else if (k2 == 48) {  // d0
            Row r = b.getRow(b.cy);
            b.undoPush(4, b.cy, 0, 0, r.raw);
            r.raw = substring(r.raw, b.cx, stringLen(r.raw));
            updateRowRender(b, b.cy); b.cx = 0; b.dirty = true;
        } else if (k2 == 119) {  // dw
            int oldCx = b.cx;
            motionW(b, false);
            if (b.cx > oldCx) {
                Row r = b.getRow(b.cy);
                b.undoPush(4, b.cy, oldCx, 0, r.raw);
                r.raw = substring(r.raw,0,oldCx) + substring(r.raw,b.cx,stringLen(r.raw));
                updateRowRender(b, b.cy); b.dirty = true;
            }
            b.cx = oldCx;
        }
    }
    if (key == 68) { deleteToEOL(b); }  // D

    // Yank
    if (key == 121) {  // y
        int k2 = readKey();
        if (k2 == 121) {  // yy
            int y2 = b.cy + n - 1;
            if (y2 >= b.numRows()) { y2 = b.numRows() - 1; }
            E.reg.yankLines(b.rows, b.cy, y2);
            E.setStatus("Yanked " + toString(y2 - b.cy + 1) + " line(s)");
        }
    }
    if (key == 89) {  // Y
        E.reg.yankLines(b.rows, b.cy, b.cy);
    }

    // Paste
    if (key == 112 || key == 80) {  // p or P
        if (E.reg.lines.length() > 0 && E.reg.linewise) {
            int at = (key == 112) ? b.cy + 1 : b.cy;
            int i = 0;
            while (i < E.reg.lines.length()) {
                b.insertRow(at + i, E.reg.lines.get(i) as string);
                updateRowRender(b, at + i);
                i = i + 1;
            }
            b.cy = at;
            b.cx = firstNonBlank(b.getRow(b.cy));
        }
    }

    // Change
    if (key == 99) {  // c
        int k2 = readKey();
        if (k2 == 99) {  // cc
            Row r = b.getRow(b.cy);
            b.undoPush(4, b.cy, 0, 0, r.raw);
            r.raw = ""; updateRowRender(b, b.cy);
            b.cx = 0; b.dirty = true; E.mode = MODE_INSERT;
        } else if (k2 == 119) {  // cw
            int oldCx = b.cx;
            motionE(b, false);
            if (b.cx < stringLen(b.getRow(b.cy).raw)) { b.cx = b.cx + 1; }
            Row r = b.getRow(b.cy);
            b.undoPush(4, b.cy, oldCx, 0, r.raw);
            r.raw = substring(r.raw, 0, oldCx) + substring(r.raw, b.cx, stringLen(r.raw));
            updateRowRender(b, b.cy); b.cx = oldCx;
            b.dirty = true; E.mode = MODE_INSERT;
        }
    }
    if (key == 67) { deleteToEOL(b); E.mode = MODE_INSERT; }  // C

    // Indent
    if (key == 62) {  // >
        int k2 = readKey();
        if (k2 == 62) { int i=0; while(i<n){indentLine(b,b.cy,1);i=i+1;} }
    }
    if (key == 60) {  // <
        int k2 = readKey();
        if (k2 == 60) { int i=0; while(i<n){indentLine(b,b.cy,-1);i=i+1;} }
    }

    // Undo
    if (key == 117) {  // u
        int ulen = b.undoStack.length();
        if (ulen > 0) {
            UndoRecord u = b.undoStack.get(ulen - 1) as UndoRecord;
            b.undoStack.remove(ulen - 1);
            if (u.utype == 0) {  // insert_char → delete it
                rowDeleteChar(b, u.row, u.col);
                b.cy = u.row; b.cx = u.col;
            } else if (u.utype == 1) {  // delete_char → insert it
                rowInsertChar(b, u.row, u.col, charFromCode(u.ch));
                b.cy = u.row; b.cx = u.col;
            } else if (u.utype == 4 || u.utype == 3) {  // replace/delete line
                if (u.utype == 3) { b.insertRow(u.row, ""); }
                Row r = b.getRow(u.row);
                if (r != null) { r.raw = u.text; updateRowRender(b, u.row); }
                b.cy = u.row; b.cx = u.col;
            } else if (u.utype == 2) {  // insert_line → delete next
                if (u.row + 1 < b.numRows()) { b.deleteRow(u.row + 1); }
                Row r = b.getRow(u.row);
                if (r != null) { r.raw = u.text; updateRowRender(b, u.row); }
                b.cy = u.row; b.cx = u.col;
            }
            b.dirty = true;
            E.setStatus("1 change undone");
        } else {
            E.setStatus("Nothing to undo");
        }
    }

    // Search
    if (key == 47) { E.mode = MODE_SEARCH; E.searchFwd = true;  E.search = ""; }  // /
    if (key == 63) { E.mode = MODE_SEARCH; E.searchFwd = false; E.search = ""; }  // ?
    if (key == 110) { int i=0; while(i<n){searchFind(b,E.searchFwd);i=i+1;} }     // n
    if (key == 78)  { int i=0; while(i<n){searchFind(b,!E.searchFwd);i=i+1;} }    // N

    // * — search word under cursor
    if (key == 42) {
        Row r = b.getRow(b.cy);
        int lo = b.cx; int hi = b.cx;
        while (lo > 0 && isWordChar(r.raw[lo - 1])) { lo = lo - 1; }
        while (hi < stringLen(r.raw) && isWordChar(r.raw[hi])) { hi = hi + 1; }
        if (hi > lo) {
            E.search = substring(r.raw, lo, hi);
            E.searchFwd = true;
            searchFind(b, true);
        }
    }
    if (key == 35) {  // # — search word backward
        Row r = b.getRow(b.cy);
        int lo = b.cx; int hi = b.cx;
        while (lo > 0 && isWordChar(r.raw[lo - 1])) { lo = lo - 1; }
        while (hi < stringLen(r.raw) && isWordChar(r.raw[hi])) { hi = hi + 1; }
        if (hi > lo) {
            E.search = substring(r.raw, lo, hi);
            E.searchFwd = false;
            searchFind(b, false);
        }
    }

    // Marks
    if (key == 109) {  // m
        int k2 = readKey();
        if (k2 >= 97 && k2 <= 122) {
            b.markRow[k2 - 97] = b.cy;
            b.markCol[k2 - 97] = b.cx;
            E.setStatus("Mark '" + charToString(charFromCode(k2)) + "' set");
        }
    }
    if (key == 39) {  // '
        int k2 = readKey();
        if (k2 >= 97 && k2 <= 122) {
            int mi = k2 - 97;
            if (b.markRow[mi] >= 0) { b.cy = b.markRow[mi]; b.cx = b.markCol[mi]; }
        }
    }

    // Bracket match %
    if (key == 37) {
        Row r = b.getRow(b.cy);
        if (b.cx < stringLen(r.raw)) {
            char c = r.raw[b.cx];
            string opens = "({[";
            string closes = ")}]";
            int oidx = indexOf(opens, charToString(c));
            int cidx = indexOf(closes, charToString(c));
            if (oidx >= 0 || cidx >= 0) {
                char match = cidx >= 0 ? opens[cidx] : closes[oidx];
                int dir    = cidx >= 0 ? -1 : 1;
                int depth = 1;
                int row = b.cy; int col = b.cx + dir;
                bool found = false;
                while (!found && row >= 0 && row < b.numRows()) {
                    Row rr = b.getRow(row);
                    while (!found && col >= 0 && col < stringLen(rr.raw)) {
                        if (rr.raw[col] == c) { depth = depth + 1; }
                        else if (rr.raw[col] == match) {
                            depth = depth - 1;
                            if (depth == 0) { b.cy = row; b.cx = col; found = true; }
                        }
                        col = col + dir;
                    }
                    row = row + dir;
                    if (!found && row >= 0 && row < b.numRows()) {
                        col = dir > 0 ? 0 : stringLen(b.getRow(row).raw) - 1;
                    }
                }
            }
        }
    }

    // File info
    if (key == CTRL_G) {
        string mod = b.dirty ? " [Modified]" : "";
        E.setStatus("\"" + b.filename + "\" " + toString(b.numRows()) +
                    " lines" + mod + "  " + toString(b.cy+1) + ":" + toString(b.cx+1));
    }

    // Command mode
    if (key == 58) { E.mode = MODE_COMMAND; E.cmd = ""; }  // :

    // Quick save+quit ZZ / ZQ
    if (key == 90) {  // Z
        int k2 = readKey();
        if (k2 == 90) { saveFile(b,""); termRestore(); writeRaw("\x1b[2J\x1b[H"); flushOutput(); exit(0); }
        if (k2 == 81) { termRestore(); writeRaw("\x1b[2J\x1b[H"); flushOutput(); exit(0); }
    }

    // F5 = compile + run
    if (key == KEY_F5) { doCompile(b); doRun(b); }

    // Escape
    if (key == KEY_ESC) {
        E.countStr = ""; E.searchMatchRow = -1;
        if (E.mode != MODE_NORMAL) { E.mode = MODE_NORMAL; }
    }

    clampCursor(b);
}

// ── Insert mode ───────────────────────────────────────────────────────────────

fun void processInsert(int key) {
    Buffer b = E.curBuffer();
    if (key == KEY_ESC) { if(b.cx>0)b.cx=b.cx-1; E.mode=MODE_NORMAL; return; }
    if (key == CTRL_C)  { E.mode = MODE_NORMAL; return; }
    if (key == KEY_ENTER)     { insertNewline(b); return; }
    if (key == KEY_BACKSPACE) { deleteCharBefore(b); return; }
    if (key == KEY_DEL)       { if(b.cx<stringLen(b.getRow(b.cy).raw))rowDeleteChar(b,b.cy,b.cx); return; }
    if (key == KEY_UP)    { if(b.cy>0)b.cy=b.cy-1; }
    if (key == KEY_DOWN)  { if(b.cy<b.numRows()-1)b.cy=b.cy+1; }
    if (key == KEY_LEFT)  { if(b.cx>0)b.cx=b.cx-1; }
    if (key == KEY_RIGHT) { if(b.cx<stringLen(b.getRow(b.cy).raw))b.cx=b.cx+1; }
    if (key == KEY_HOME)  { b.cx=0; }
    if (key == KEY_END)   { b.cx=stringLen(b.getRow(b.cy).raw); }
    if (key == CTRL_T) {
        int i = 0; while (i < E.tabWidth) { insertChar(b, ' '); i = i + 1; }
        return;
    }
    if (key >= 32 && key < 127) { insertChar(b, charFromCode(key)); return; }
    if (key == 9) {  // tab
        int i = 0; while (i < E.tabWidth) { insertChar(b, ' '); i = i + 1; }
        return;
    }
    clampCursor(b);
}

// ── Replace mode ──────────────────────────────────────────────────────────────

fun void processReplace(int key) {
    Buffer b = E.curBuffer();
    if (key == KEY_ESC || key == CTRL_C) { E.mode = MODE_NORMAL; return; }
    if (key == KEY_BACKSPACE) { if(b.cx>0)b.cx=b.cx-1; return; }
    if (key >= 32 && key < 127) {
        Row r = b.getRow(b.cy);
        if (b.cx < stringLen(r.raw)) {
            b.undoPush(4, b.cy, b.cx, 0, r.raw);
            r.raw = substring(r.raw,0,b.cx) + charToString(charFromCode(key)) +
                    substring(r.raw, b.cx+1, stringLen(r.raw));
            updateRowRender(b, b.cy);
            b.cx = b.cx + 1;
        } else {
            insertChar(b, charFromCode(key));
        }
        b.dirty = true;
    }
}

// ── Visual mode ───────────────────────────────────────────────────────────────

fun void processVisual(int key) {
    Buffer b = E.curBuffer();
    bool vline = (E.mode == MODE_VISUAL_LINE);

    if (key == KEY_ESC || key == CTRL_C) { E.mode = MODE_NORMAL; return; }
    if (key == 104 || key == KEY_LEFT)  { if(b.cx>0)b.cx=b.cx-1; }
    if (key == 108 || key == KEY_RIGHT) { if(b.cx<stringLen(b.getRow(b.cy).raw)-1)b.cx=b.cx+1; }
    if (key == 106 || key == KEY_DOWN)  { if(b.cy<b.numRows()-1)b.cy=b.cy+1; }
    if (key == 107 || key == KEY_UP)    { if(b.cy>0)b.cy=b.cy-1; }
    if (key == 119) { motionW(b, false); }
    if (key == 98)  { motionB(b, false); }
    if (key == 101) { motionE(b, false); }
    if (key == 36)  { b.cx = stringLen(b.getRow(b.cy).raw) - 1; }
    if (key == 48)  { b.cx = 0; }
    if (key == 71)  { b.cy = b.numRows() - 1; }

    if (key == 100 || key == 120) {  // d or x — delete selection
        int r1 = b.visRow; int r2 = b.cy;
        if (r1 > r2) { int t=r1; r1=r2; r2=t; }
        E.reg.yankLines(b.rows, r1, r2);
        int i = r2;
        while (i >= r1) { deleteLine(b, i); i = i - 1; }
        E.mode = MODE_NORMAL;
        return;
    }
    if (key == 121) {  // y — yank selection
        int r1 = b.visRow; int r2 = b.cy;
        if (r1 > r2) { int t=r1; r1=r2; r2=t; }
        E.reg.yankLines(b.rows, r1, r2);
        E.setStatus("Yanked " + toString(r2 - r1 + 1) + " line(s)");
        E.mode = MODE_NORMAL;
        return;
    }
    if (key == 62) {  // > indent
        int r1 = b.visRow; int r2 = b.cy;
        if (r1 > r2) { int t=r1; r1=r2; r2=t; }
        int i = r1;
        while (i <= r2) { indentLine(b, i, 1); i = i + 1; }
        E.mode = MODE_NORMAL; return;
    }
    if (key == 60) {  // < de-indent
        int r1 = b.visRow; int r2 = b.cy;
        if (r1 > r2) { int t=r1; r1=r2; r2=t; }
        int i = r1;
        while (i <= r2) { indentLine(b, i, -1); i = i + 1; }
        E.mode = MODE_NORMAL; return;
    }
    if (key == 58) { E.mode = MODE_COMMAND; E.cmd = ""; return; }  // :

    clampCursor(b);
}

// ── Command mode ──────────────────────────────────────────────────────────────

fun void processCommandKey(int key) {
    Buffer b = E.curBuffer();
    if (key == KEY_ENTER) {
        processCommand(b);
        return;
    }
    if (key == KEY_ESC || key == CTRL_C) { E.mode = MODE_NORMAL; return; }
    if (key == KEY_BACKSPACE) {
        if (stringLen(E.cmd) > 0) {
            E.cmd = substring(E.cmd, 0, stringLen(E.cmd) - 1);
        } else {
            E.mode = MODE_NORMAL;
        }
        return;
    }
    if (key >= 32 && key < 127) {
        E.cmd = E.cmd + charToString(charFromCode(key));
    }
}

// ── Search mode ───────────────────────────────────────────────────────────────

fun void processSearchKey(int key) {
    Buffer b = E.curBuffer();
    if (key == KEY_ENTER) {
        if (stringLen(E.search) > 0) { searchFind(b, E.searchFwd); }
        E.mode = MODE_NORMAL; return;
    }
    if (key == KEY_ESC || key == CTRL_C) {
        E.mode = MODE_NORMAL; E.search = ""; return;
    }
    if (key == KEY_BACKSPACE) {
        if (stringLen(E.search) > 0) {
            E.search = substring(E.search, 0, stringLen(E.search) - 1);
        } else {
            E.mode = MODE_NORMAL;
        }
        return;
    }
    if (key >= 32 && key < 127) {
        E.search = E.search + charToString(charFromCode(key));
        searchFind(b, E.searchFwd);
    }
}

// ── Main entry point ──────────────────────────────────────────────────────────

fun int main() {
    E = new Editor();
    E.init();

    // Get arguments via command-line (argv access via a simple trick:
    // read from a temp arg file if provided, else start empty)
    // Since EC doesn't expose argc/argv yet, check for a file arg
    // by trying to read a well-known temp path written by a wrapper script.
    // For now, open files passed via a temp argument file:
    string argFile = "/tmp/.ecvim_args";
    string argsContent = readFile(argFile);
    if (stringLen(argsContent) > 0) {
        // Parse newline-separated filenames
        int start = 0;
        int i = 0;
        while (i < stringLen(argsContent)) {
            if (argsContent[i] == '\n') {
                string fname = substring(argsContent, start, i);
                if (stringLen(fname) > 0 && fname[0] != '#') {
                    Buffer b = newBuffer();
                    loadFile(b, fname);
                    E.bufs.add(b);
                }
                start = i + 1;
            }
            i = i + 1;
        }
        // last line
        if (start < stringLen(argsContent)) {
            string fname = substring(argsContent, start, stringLen(argsContent));
            if (stringLen(fname) > 0 && fname[0] != '#') {
                Buffer b = newBuffer();
                loadFile(b, fname);
                E.bufs.add(b);
            }
        }
    }

    if (E.bufs.length() == 0) {
        Buffer b = newBuffer();
        b.insertRow(0, "");
        E.bufs.add(b);
    }

    // Terminal setup
    atexitRestore();
    termRaw();
    registerWinch();
    E.updateTermSize();

    E.setStatus("ecvim — :help for keys  |  F5 = compile+run  |  :q to quit");

    // Main loop
    while (true) {
        int wp = winchPending(); if (wp == 1) {
            E.updateTermSize();
        }

        refreshScreen();

        int key = readKey();

        if (E.mode == MODE_NORMAL)           { processNormal(key); }
        else if (E.mode == MODE_INSERT)      { processInsert(key); }
        else if (E.mode == MODE_REPLACE)     { processReplace(key); }
        else if (E.mode == MODE_VISUAL || E.mode == MODE_VISUAL_LINE) { processVisual(key); }
        else if (E.mode == MODE_COMMAND)     { processCommandKey(key); }
        else if (E.mode == MODE_SEARCH)      { processSearchKey(key); }
    }

    return 0;
}
