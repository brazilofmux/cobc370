/* cobc -- a COBOL cross-compiler for MVS 3.8j, slice 0.
 *
 * Reads fixed-format COBOL on the Mac and emits S/370 assembler source, which
 * is assembled and link-edited on the guest. The compiler never runs on MVS.
 *
 * Accepts, so far: the divisions, and a PROCEDURE DIVISION containing
 * DISPLAY of a nonnumeric literal and STOP RUN. Anything else is a diagnostic
 * naming the limit rather than silence.
 *
 * Nothing in SYS1.COBLIB is ever referenced. The runtime is ours: COBDISP and
 * COBTERM are emitted as a COBRT CSECT alongside the program, and they reach
 * the operating system directly through the QSAM macros (OPEN/PUT/CLOSE), the
 * same access-method path IKFCBL00 itself uses for file I/O. SYS1.MACLIB is
 * used for those macros, which is the OS interface, not IBM's COBOL runtime.
 *
 * Runtime calling convention -- ours, but deliberately the OS one, so that
 * COBOL CALL can use it unchanged later:
 *   R1  -> parameter list, high-order bit set on the last entry
 *   R13 -> caller's save area;  R14 return;  R15 entry / return code
 *   COBDISP parameter list:  A(text), A(halfword length)
 *
 *   cc -O2 -o cobc370 cobc370.c
 *   ./cobc370 prog.cbl -o prog.asm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "picture.h"

#define MAXLINE 256
/* 1 NUC 1,2 puts a nonnumeric literal at 1 through 120 characters, so a token
 * has to be able to hold one -- with room for the terminator and for a word
 * that runs long enough to be worth diagnosing rather than silently cutting. */
#define MAXTOK  132

/* ---- source reader: fixed format ------------------------------------- */
/* cols 1-6 sequence, 7 indicator, 8-72 code, 73-80 sequence. A '*' or '/'
 * in column 7 makes the line a comment. */

typedef struct {
    FILE *fp;
    char  buf[MAXLINE];
    char *p;            /* cursor within the current line's code area */
    int   line;
    int   cont;         /* this line carries a hyphen in the indicator area */
    const char *name;
    /* COPY ... REPLACING: applied to every line of this copybook as it is
     * read. A pseudo-text operand replaces wherever its characters occur; a
     * word or literal operand replaces whole words only. */
    int   nrep;
    struct { char from[120], to[120]; int pseudo; } rep[16];
} Src;

/* COPY nests: the source being read, and the sources it was copied into. The
 * copybook is found on the -I directories, then beside the program. */
#define MAXCOPY 4
static Src copy_stack[MAXCOPY];
static int copy_depth;
static const char *copy_dirs[16];
static int ncopy_dirs;
static char src_dir[512];

static void die(const char *msg);

/* One copybook line through its REPLACING pairs. Pseudo-text is matched as
 * characters, blanks collapsed on both sides; a word operand only between
 * separators. The line may grow past column 72 -- the scanner reads to its
 * end, and a card image is what it was before the copy, not after. */
static void copy_replace(Src *s)
{
    char out[MAXLINE * 2];
    for (int k = 0; k < s->nrep; k++) {
        const char *from = s->rep[k].from, *to = s->rep[k].to;
        size_t fl = strlen(from);
        if (!fl) continue;
        const char *p = s->buf + 7; char *o = out; int changed = 0;
        while (*p) {
            int hit = 0;
            if (s->rep[k].pseudo) {
                /* compare with runs of blanks collapsed */
                const char *a = p, *b = from;
                while (*b) {
                    if (*b == ' ') { if (*a != ' ') break; while (*a == ' ') a++; while (*b == ' ') b++; continue; }
                    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) break;
                    a++; b++;
                }
                if (!*b) { hit = 1; p = a; }
            } else if (!strncasecmp(p, from, fl)
                       && (p == s->buf + 7 || strchr(" .,;()'\"", p[-1]))
                       && (!p[fl] || strchr(" .,;()'\"", p[fl]))) {
                hit = 1; p += fl;
            }
            if (hit) { size_t tl = strlen(to); if ((size_t)(o - out) + tl >= sizeof out - 1) die("COPY REPLACING made a line too long"); memcpy(o, to, tl); o += tl; changed = 1; }
            else { if ((size_t)(o - out) >= sizeof out - 1) die("COPY REPLACING made a line too long"); *o++ = *p++; }
        }
        *o = 0;
        if (changed) {
            if (strlen(out) >= MAXLINE - 8) die("COPY REPLACING made a line too long");
            strcpy(s->buf + 7, out);
        }
    }
}

static int src_fill(Src *s)
{
    for (;;) {
        if (!fgets(s->buf, sizeof s->buf, s->fp)) return 0;
        s->line++;
        size_t n = strlen(s->buf);
        while (n && (s->buf[n-1] == '\n' || s->buf[n-1] == '\r')) s->buf[--n] = 0;
        if (n < 7) continue;                       /* blank or short: skip */
        if (s->buf[6] == '*' || s->buf[6] == '/') continue;   /* comment */
        if (n > 72) s->buf[72] = 0;                /* drop cols 73-80 */
        s->cont = (s->buf[6] == '-');
        s->p = s->buf + 7;                         /* code starts col 8 */
        if (s->nrep) copy_replace(s);
        int blank = 1;
        for (char *q = s->p; *q; q++) if (!isspace((unsigned char)*q)) blank = 0;
        if (blank) continue;
        return 1;
    }
}

/* ---- tokenizer -------------------------------------------------------- */
/* COBOL words, and '.' as a distinct token. Case-insensitive; folded up. */

typedef struct {
    char text[MAXTOK];
    int  len;           /* significant for literals */
    int  literal;       /* nonzero if this token was a quoted literal */
    int  line;
    int  eof;
} Tok;

static Src src;
static Tok tok;

/* Parentheses are separators in the PROCEDURE DIVISION, but part of the word
 * in a PICTURE -- S9(7)V99 has to survive as one token. So the word scanner
 * only breaks on them once the procedure division has started. */
static int lex_parens;

/* A PICTURE character-string is one word even when it contains a period --
 * PIC 9(2).99 must survive whole. The scanner cannot tell that from the
 * characters alone: a ')' before a decimal point is legal in a picture and
 * nowhere else. So the parser says when a picture is coming. */
static int lex_picture;

/* SPECIAL-NAMES clauses that change how every PICTURE and numeric literal is
 * read. CURRENCY SIGN names the character that stands where '$' stands in the
 * standard's own examples; DECIMAL-POINT IS COMMA exchanges the roles of the
 * period and the comma in pictures and in numeric literals. Both are Nucleus
 * level 1. */
static char currency_sym = '$';
static int  decimal_is_comma;

/* Host character to EBCDIC (CP037), for the one byte a CURRENCY SIGN puts in
 * an ED pattern. Printable ASCII only; the standard forbids everything else. */
static unsigned char host_ebcdic(char c)
{
    static const unsigned char t[95] = {
        0x40,0x5A,0x7F,0x7B,0x5B,0x6C,0x50,0x7D,0x4D,0x5D,0x5C,0x4E,0x6B,0x60,0x4B,0x61,
        0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0x7A,0x5E,0x4C,0x7E,0x6E,0x6F,
        0x7C,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,
        0xD7,0xD8,0xD9,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xBA,0xE0,0xBB,0xB0,0x6D,
        0x79,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x91,0x92,0x93,0x94,0x95,0x96,
        0x97,0x98,0x99,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xC0,0x4F,0xD0,0xA1 };
    unsigned char u = (unsigned char)c;
    return (u >= 0x20 && u <= 0x7E) ? t[u - 0x20] : 0x40;
}

/* Move a nonnumeric literal onto its continuation line, or return 0 if the
 * next line is not one. I-106, 5.8.2.2: a hyphen in the indicator area, area A
 * blank, and -- because the literal has no closing quotation mark yet -- the
 * first nonblank character in area B must be a quotation mark, with the
 * literal resuming at the character after it. */
static int lit_continue(Src *s)
{
    if (!fgets(s->buf, sizeof s->buf, s->fp)) return 0;
    s->line++;
    size_t n = strlen(s->buf);
    while (n && (s->buf[n-1] == '\n' || s->buf[n-1] == '\r')) s->buf[--n] = 0;
    if (n < 12 || s->buf[6] != '-') return 0;
    if (n > 72) { s->buf[72] = 0; n = 72; }
    for (size_t k = 7; k < 11 && k < n; k++)
        if (s->buf[k] != ' ') return 0;            /* area A must be blank */
    char *q = s->buf + 11;                         /* area B starts at col 12 */
    while (*q == ' ') q++;
    if (*q != '"' && *q != '\'') return 0;
    s->cont = 1;
    s->p = q + 1;
    return 1;
}

/* The line is exhausted in the middle of a word or a numeric literal. If the
 * next line is a continuation line -- hyphen in the indicator area, area A
 * blank -- the word goes on from the first nonblank character in area B, with
 * nothing between (I-106, 5.8.2.2). Otherwise the word ends here, and the line
 * just read is left ready for the scanner exactly as src_fill would leave it:
 * this is level 2 of the Nucleus, where level 1 continues only nonnumeric
 * literals. */
static int word_continue(Src *s)
{
    if (!src_fill(s)) return 0;
    if (!s->cont) return 0;
    for (int k = 7; k < 11 && s->buf[k]; k++)
        if (s->buf[k] != ' ') return 0;            /* area A must be blank */
    s->p = s->buf + 11;
    while (*s->p == ' ') s->p++;
    return 1;
}

static void die(const char *msg)
{
    fprintf(stderr, "%s:%d: %s\n", src.name, tok.line ? tok.line : src.line, msg);
    exit(1);
}

/* COBOL lets a comma or semicolon stand in for a space between operands --
 * OPEN INPUT A, B and DISPLAY X, Y both appear in the corpus. It is a separator
 * only when a space follows it, which is exactly what keeps the commas inside
 * PIC ZZZ,ZZ9.99 and PIC ---,---,--9 part of the picture. */
static int sep_punct(const char *p)
{
    if (*p != ',' && *p != ';') return 0;
    return p[1] == 0 || isspace((unsigned char)p[1]);
}

static void scan_token(void);
static void copy_statement(void);

/* The scanner the parser sees: the raw one, with COPY taken out. A COPY
 * statement is read and its copybook opened here, and the first token the
 * parser gets after COPY is the first token of the copybook. */
static void next(void)
{
    scan_token();
    while (!tok.eof && !tok.literal && !lex_picture && !strcmp(tok.text, "COPY")) {
        copy_statement();
        scan_token();
    }
}

static void scan_token(void)
{
    tok.eof = 0;
    for (;;) {
        if (!src.p || !*src.p) {
            if (!src_fill(&src)) {
                if (copy_depth > 0) {
                    /* end of a copybook: back to the source it was copied into */
                    fclose(src.fp);
                    src = copy_stack[--copy_depth];
                    continue;
                }
                tok.eof = 1; tok.text[0]=0; return;
            }
            /* A hyphen joins the first nonblank in area B to the last nonblank
             * of the line before it, with no space between. Reaching here means
             * the previous token already ended, so this is a word or a numeric
             * literal being continued -- level 2 of the Nucleus, where level 1
             * allows only a nonnumeric literal to be broken across lines. */
            if (src.cont)
                die("continuation of a word or a numeric literal is not "
                    "implemented; only a nonnumeric literal may be continued");
        }
        while (*src.p && (isspace((unsigned char)*src.p) || sep_punct(src.p))) src.p++;
        if (*src.p) break;
    }
    tok.line = src.line;
    tok.literal = 0;
    /* A period immediately followed by a digit begins a numeric literal:
     * "the decimal point must not be the rightmost character" is the only
     * placement rule, so .1 is legal. A period used as a separator is always
     * followed by a space, so the two cannot be confused. */
    if (*src.p == '.' && (decimal_is_comma || !isdigit((unsigned char)src.p[1]))) {
        src.p++; strcpy(tok.text, "."); tok.len = 1; return;
    }
    if (*src.p == '(' || *src.p == ')') {
        tok.text[0] = *src.p++; tok.text[1] = 0; tok.len = 1; return;
    }
    if (*src.p == '\'' || *src.p == '"') {
        /* Nonnumeric literal. A doubled quote is one quote character; a period
           inside a literal does not end the sentence. */
        char q = *src.p++;
        int i = 0;
        for (;;) {
            if (!*src.p) {
                /* "All spaces at the end of the continued line are considered
                 * part of the literal" -- the line runs to margin R whether or
                 * not the file carries the blanks, so pad to column 72 before
                 * moving on. */
                int col = (int)(src.p - src.buf);
                while (col++ < 72) {
                    if (i >= MAXTOK - 1)
                        die("nonnumeric literal too long -- the standard's "
                            "limit is 120 characters");
                    tok.text[i++] = ' ';
                }
                if (!lit_continue(&src))
                    die("unterminated literal: a continuation line needs a "
                        "hyphen in column 7, a blank area A, and a quotation "
                        "mark as the first nonblank character in area B");
                continue;
            }
            if (*src.p == q) {
                if (*(src.p + 1) == q) { src.p += 2; }
                else { src.p++; break; }
            } else { src.p++; }
            if (i >= MAXTOK - 1)
                die("nonnumeric literal too long -- the standard's limit is "
                    "120 characters");
            tok.text[i++] = *(src.p - 1);
        }
        tok.text[i] = 0; tok.len = i; tok.literal = 1;
        return;
    }
    int i = 0;
    for (;;) {
        if (!*src.p || isspace((unsigned char)*src.p)) {
            /* The word ends -- unless nothing but blanks remain on the line
             * and the next line continues it. */
            const char *q = src.p;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q || i == 0 || !word_continue(&src)) break;
            continue;
        }
        if (sep_punct(src.p)) break;
        if (lex_parens && (*src.p == '(' || *src.p == ')')) break;
        if (*src.p == '.') {
            /* A period is a decimal point only when it sits between digits;
               otherwise it ends the sentence. Inside a PICTURE the rule is the
               standard's own: the period is a separator only when a space
               follows it, so PIC 9(2).99 and PIC -.9(18) stay one word. */
            if (lex_picture) {
                char nx = *(src.p + 1);
                if (!nx || isspace((unsigned char)nx)) break;
            } else if (decimal_is_comma
                       || !(isdigit((unsigned char)*(src.p + 1))
                            && (i == 0 || isdigit((unsigned char)tok.text[i-1]))))
                break;   /* a decimal point: leading, as in .1, or between digits */
        }
        if (decimal_is_comma && !lex_picture && *src.p == ','
            && isdigit((unsigned char)*(src.p + 1))
            && i > 0 && isdigit((unsigned char)tok.text[i-1])) {
            /* DECIMAL-POINT IS COMMA: a comma between digits is the decimal
             * point. It is stored as the period so that nothing downstream
             * has to know -- every place that scales a literal looks for '.'.
             * A comma followed by a space is still a separator, and never
             * reaches here. */
            tok.text[i++] = '.';
            src.p++;
            continue;
        }
        if (i >= MAXTOK - 1) die("word too long");
        tok.text[i++] = (char)toupper((unsigned char)*src.p);
        src.p++;
    }
    tok.text[i] = 0; tok.len = i;
}

static int is(const char *w) { return strcmp(tok.text, w) == 0; }

/* The words that end a list of names inside a data description. */
static int is_data_clause(void)
{
    static const char *w[] = {
        "PIC", "PICTURE", "VALUE", "REDEFINES", "OCCURS", "USAGE", "COMP",
        "COMP-3", "COMPUTATIONAL", "COMPUTATIONAL-3", "DISPLAY", "INDEX",
        "SYNC", "SYNCHRONIZED", "JUST", "JUSTIFIED", "SIGN", "BLANK",
        "ASCENDING", "DESCENDING", "INDEXED", "DEPENDING", "TIMES", "KEY",
        "RENAMES", "IS", 0
    };
    for (int i = 0; w[i]; i++) if (is(w[i])) return 1;
    return 0;
}

/* pic_analyse with the SPECIAL-NAMES substitutions applied on the way in and
 * undone on the way out. The picture module itself knows only the standard's
 * own spellings: '$' for the currency symbol, '.' for the decimal point and ','
 * for the insertion comma. A program that says otherwise has its picture
 * rewritten into those spellings here, and the ED pattern that comes back has
 * the bytes exchanged again -- so the printed result carries the program's
 * character and the arithmetic is unaffected. */
static int analyse_picture(const char *pic, PicInfo *pi)
{
    char p[PIC_MAXITEM * 4];
    snprintf(p, sizeof p, "%s", pic);
    for (char *q = p; *q; q++) {
        char c = (char)toupper((unsigned char)*q);
        if (currency_sym != '$' && c == currency_sym) *q = '$';
        else if (decimal_is_comma && *q == '.') *q = ',';
        else if (decimal_is_comma && *q == ',') *q = '.';
    }
    int rc = pic_analyse(p, pi);
    if (rc < 0) return rc;
    for (int k = 0; k < pi->masklen; k++) {
        unsigned char m = pi->mask[k];
        if (m == 0x5B && currency_sym != '$') pi->mask[k] = host_ebcdic(currency_sym);
        else if (decimal_is_comma && m == 0x4B) pi->mask[k] = 0x6B;
        else if (decimal_is_comma && m == 0x6B) pi->mask[k] = 0x4B;
    }
    return 0;
}

/* Step past PIC / PICTURE [IS] and leave the character-string in tok. */
static void next_pic(void)
{
    lex_picture = 1;
    next();
    if (is("IS")) next();
    lex_picture = 0;
}

static void expect(const char *w)
{
    if (!is(w)) {
        char m[128];
        snprintf(m, sizeof m, "expected %s, found '%s'", w, tok.text[0] ? tok.text : "end of file");
        die(m);
    }
    next();
}

/* ---- emitter ---------------------------------------------------------- */
/* Assembler F source: name in col 1, operation col 10, operands col 16. */

static FILE *out;

static void asm_line(const char *name, const char *op, const char *operand,
                     const char *comment)
{
    char b[128];
    memset(b, ' ', sizeof b);
    size_t n;
    if (name && *name) {
        n = strlen(name);
        if (n > 8) die("internal: assembler label longer than 8 characters");
        memcpy(b + 0, name, n);
    }
    size_t opl = 0;
    if (op && *op) { opl = strlen(op); memcpy(b + 9, op, opl); }
    /* Operands start in column 16, but an opcode longer than six characters
     * (GETMAIN, FREEDBUF) would otherwise have its tail overwritten -- leave
     * it one blank instead. */
    int ostart = 15;
    if (9 + (int)opl + 1 > ostart) ostart = 9 + (int)opl + 1;
    if (operand && *operand) {
        n = strlen(operand);
        /* A statement must end by column 71 -- column 72 is the continuation
         * indicator. Truncating an operand here once produced a DC that
         * assembled cleanly and held the wrong bytes, so this is fatal. */
        if (ostart + (int)n > 71)
            die("internal: assembler operand runs past column 71");
        memcpy(b + ostart, operand, n);
    }
    int end = ostart + (operand ? (int)strlen(operand) : 0);
    if (end < 15) end = 15;
    if (comment && *comment) {
        /* Column 72 is the continuation indicator: a statement must stop at
           column 71 or the assembler reads the next line as a continuation
           and reports IFO026. Truncate the comment rather than the code. */
        int c = end + 2; if (c < 35) c = 35;
        int room = 71 - c;
        if (room > 0) {
            int n2 = (int)strlen(comment);
            if (n2 > room) n2 = room;
            memcpy(b + c, comment, (size_t)n2);
            end = c + n2;
        }
    }
    if (end > 71) end = 71;
    b[end] = 0;
    for (int i = end - 1; i >= 0 && b[i] == ' '; i--) b[i] = 0;
    fprintf(out, "%s\n", b);
}

static void asm_comment(const char *text) { fprintf(out, "*%s\n", text); }

/* A quote or an ampersand has to be doubled inside an assembler character
 * constant, so how much room a literal needs is not its length. */
static int escaped_len(const char *v)
{
    int n = 0;
    for (const char *q = v; *q; q++) n += (*q == '\'' || *q == '&') ? 2 : 1;
    return n;
}

/* Emit a character constant as one or more adjacent DC statements. A statement
 * must end by column 71, and a nonnumeric literal may be 120 characters, so a
 * long one will not fit on a line. Adjacent DCs occupy contiguous storage, so
 * the field is identical either way -- and the declared length of the last
 * chunk carries any blank padding the value does not fill. */
static void emit_split_dc(const char *label, const char *value, int total,
                          const char *cmt)
{
    enum { ROOM = 48 };                 /* escaped characters per statement; the
                                         * operand has 71-15 columns and CLnnn''
                                         * takes up to 7 of them */
    const char *q = value;
    int left = total;
    int first = 1;
    while (*q && left > 0) {
        char op[96]; int j = 0, taken = 0, used = 0;
        char body[64]; int bj = 0;
        while (*q && taken < left && used < ROOM) {
            int w = (*q == '\'' || *q == '&') ? 2 : 1;
            if (used + w > ROOM) break;
            if (w == 2) body[bj++] = *q;
            body[bj++] = *q++;
            used += w; taken++;
        }
        body[bj] = 0;
        j = snprintf(op, sizeof op, "CL%d'%s'", taken, body);
        (void)j;
        asm_line(first ? label : "", "DC", op, first ? cmt : "");
        first = 0;
        left -= taken;
    }
    if (left > 0) {                     /* the value stopped short of the field */
        char op[32];
        snprintf(op, sizeof op, "CL%d' '", left);
        asm_line(first ? label : "", "DC", op, first ? cmt : "");
    }
}

/* ---- parser ----------------------------------------------------------- */

static char progid[9];
static int uses_switches;   /* a SPECIAL-NAMES switch condition is tested */

/* ---- data model ------------------------------------------------------- */
/* Elementary items only, so far. Storage sits inside the program CSECT and is
 * reached off base register 12, which caps WORKING-STORAGE at one 4K
 * displacement. That is exactly the limit ANS COBOL solves with BL cells, and
 * it is the next structural thing this compiler will need. */

enum { U_DISPLAY, U_COMP, U_COMP3 };

typedef struct {
    char name[31];
    char label[9];    /* assembler labels are 8 characters; COBOL names are 30 */
    int  level;
    int  is_group;    /* has subordinates, so no PICTURE of its own */
    int  is_alpha;    /* PIC X */
    int  usage;
    int  digits;      /* total digit positions */
    int  scale;       /* digits to the right of V */
    int  is_signed;
    int  bytes;
    int  offset;
    int  has_value;
    char value[MAXTOK]; /* the VALUE literal; numerics already scaled */
    int  edited;      /* needs an ED pattern */
    int  floating;    /* floating insertion -> EDMK */
    int  masklen;
    char sign_char;
    int  sign_pos, first_sel, need_lead_start;
    unsigned char mask[PIC_MAXMASK];
    int  occurs;      /* OCCURS count, 0 when not a table */
    int  elem;        /* size of one element */
    int  occ_parent;  /* the innermost table this item sits inside, -1 if none */
    int  occ_chain[3];/* every enclosing OCCURS, outermost first */
    int  occ_depth;   /* how many -- and how many subscripts a reference needs */
    int  alias;       /* REDEFINES: shares storage, so emit a label not a DC */
    int  just;        /* JUSTIFIED RIGHT: an alphanumeric receiver right-aligns */
    int  sgn_lead;    /* SIGN IS LEADING; the default is trailing */
    int  sgn_sep;     /* SIGN IS ... SEPARATE CHARACTER: its own position */
    int  is_alphabetic; /* PIC A: the category ALPHABETIC, narrower than is_alpha */
    int  is_index;    /* an index-name, or an item with USAGE IS INDEX */
    int  fd_file;     /* an 01 under an FD: which file's record area, else -1 */
    int  redef_from;  /* REDEFINES: cursor to resume at when this item closes */
    int  redef_cap;   /* REDEFINES: one past the last byte it may occupy */
    int  is_88;       /* condition name: no storage, tests its parent */
    int  is_switch;   /* SPECIAL-NAMES condition name: tests a UPSI bit */
    int  sw_bit;      /* which switch, 0 through 7, leftmost first */
    int  sw_on;       /* 1 for ON STATUS, 0 for OFF STATUS */
    int  parent;
    int  gparent;     /* enclosing group, -1 at 01/77 -- for OF/IN qualification */
    int  sync;        /* SYNCHRONIZED: alignment is promised, so it is checked */
    int  index_sym;   /* INDEXED BY item for this OCCURS table, -1 if none */
    int  askey_sym;   /* the first KEY field, -1 if none */
    int  keys[8], keydesc[8], nkeys;  /* ASCENDING/DESCENDING KEY series, in order */
    int  odo_dep;     /* OCCURS ... DEPENDING ON: the count item, -1 if fixed */
    int  odo_min;     /* the lower bound of the OCCURS */
    int  odo_tab;     /* a group: the DEPENDING ON table inside it, 0 if none (sym 0 cannot be one) */
    int  linkage;     /* declared in the LINKAGE SECTION: storage belongs to the caller */
    int  link_area;   /* which linkage 01 it sits in, so it gets the right base */
    char cvalue[MAXTOK];
    int  cvalue_len, cvalue_str;
    char chi[MAXTOK];   /* VALUE ... THRU: the upper bound, when has_hi */
    int  chi_len, has_hi;
    int  c88_next;      /* the next value of a VALUE series: a hidden 88, or -1 */
} Sym;

#define MAXSYM 1024
static Sym syms[MAXSYM];
static int nsym, wslen;

/* The innermost REDEFINES still open on the group stack, or -1. A redefinition
 * inside a redefining group is bounded by the inner one; when that closes, the
 * outer bound comes back. */
static int enclosing_cap(const int *stack, int sp)
{
    for (int k = sp - 1; k >= 0; k--)
        if (syms[stack[k]].redef_cap >= 0) return syms[stack[k]].redef_cap;
    return -1;
}

/* Close the innermost open group: its size is what the cursor walked over. */
static void close_group(int *cursor, const int *stack, int *sp)
{
    Sym *g = &syms[stack[--*sp]];
    g->elem = *cursor - g->offset;
    if (g->occurs > 0) { g->bytes = g->elem * g->occurs; *cursor = g->offset + g->bytes; }
    else g->bytes = g->elem;
    /* A group REDEFINES stays open across all of its subordinates, and this is
     * where it ends. What follows belongs after the item that was redefined --
     * not after however far into it the redefinition happened to reach. */
    if (g->redef_from > *cursor) *cursor = g->redef_from;
}

/* ---- expressions ------------------------------------------------------
 * A small AST, evaluated onto a stack of packed work areas. Scales are
 * tracked at compile time; see gen_expr for the intermediate-result rules,
 * which are where this will diverge from GnuCOBOL's unbounded intermediates
 * if it diverges anywhere.
 */
enum { N_SYM, N_LIT, N_STR, N_ADD, N_SUB, N_MUL, N_DIV, N_NEG, N_POW, N_TRUNC };

typedef struct Node {
    int kind;
    int sym;
    char lit[MAXTOK];   /* N_LIT: scaled digits.  N_STR: the text */
    int litscale;
    int litlen;         /* N_STR */
    int fig;            /* N_STR: a figurative constant; for FIG_ALL, lit is the unit */
    struct Node *sub;   /* subscript list on an N_SYM reference */
    struct Node *next;  /* the next subscript of a multi-dimensional reference */
    struct Node *l, *r;
} Node;

/* Conditions. Relations compare two expressions; AND/OR short-circuit. */
enum { REL_EQ, REL_LT, REL_GT, REL_NE, REL_NGT, REL_NLT };
enum { C_REL, C_AND, C_OR, C_NOT, C_CLASS, C_SWITCH };

typedef struct Cond {
    int kind, op;
    Node *l, *r;
    struct Cond *cl, *cr;
    int  cls_sym;       /* C_CLASS: the item under test */
    Node *cls_sub;
    int  cls_alpha;     /* 1 for ALPHABETIC, 0 for NUMERIC */
    int  cls_not;       /* the NOT was written before the class name */
    int  sw_bit, sw_on; /* C_SWITCH: which UPSI bit, and which way round */
} Cond;

#define MAXCOND 256
static Cond conds[MAXCOND];
static int ncond;

static Cond *cnode(int kind)
{
    if (ncond >= MAXCOND) die("condition too complex");
    Cond *c = &conds[ncond++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    return c;
}

#define MAXNODE 4096
static Node nodes[MAXNODE];
static int nnode;

static Node *node(int kind)
{
    if (nnode >= MAXNODE) die("expression too complex");
    Node *n = &nodes[nnode++];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    return n;
}

enum { ST_DISPLAY_LIT, ST_DISPLAY_ID, ST_MOVE, ST_ADD, ST_SUB, ST_COMPUTE,
       ST_PARA, ST_PERFORM, ST_STOP, ST_EXIT, ST_INSPECT, ST_ALTER, ST_ACCEPT,
       ST_LABEL, ST_BRANCH, ST_IFTEST,
       ST_OPEN, ST_READ, ST_WRITE, ST_CLOSE, ST_GOTO, ST_GODEP, ST_STRING, ST_UNSTRING, ST_CANCEL, ST_EXITPGM,
       ST_INITIATE, ST_GENERATE, ST_TERMINATE, ST_CALL, ST_SEARCH,
       ST_REWRITE, ST_DELETE, ST_START };

typedef struct {
    int  op;
    int  dst, src;          /* symbol indices, -1 when unused */
    char lit[MAXTOK];       /* DISPLAY literal */
    int  litlen;
    int  imm;               /* source is a numeric literal */
    int  fig;               /* source is a figurative constant: FIG_SPACE/FIG_ZERO */
    char immdigits[34];     /* that literal, scaled to an integer */
    int  immscale;
    Node *expr;             /* COMPUTE */
    int  rounded;
    int  size_err;          /* ON SIZE ERROR governs this statement */
    int  sop_first, sop_n;  /* STRING: sending items; UNSTRING: receivers */
    int  sdl_first, sdl_n;  /* UNSTRING: delimiters */
    int  ptr_sym, tally_sym; /* WITH POINTER, TALLYING IN: -1 when absent */
    Node *ptr_sub, *tally_sub;
    int  ovf;               /* ON OVERFLOW present; lab2 is the continue label */
    int  size_first, size_last; /* first and last of the series it governs */
    char para[31];          /* ST_PARA name, or PERFORM's first paragraph */
    char thru[31];          /* PERFORM ... THRU */
    Cond *cond;             /* ST_IFTEST */
    int  lab1, lab2;        /* ST_READ: the AT END and continue labels */
    int  read_next;         /* ST_READ: READ NEXT on an ACCESS IS DYNAMIC file */
    int  rec;               /* WRITE/REWRITE: the record named, of an FD's several */
    int  adv;               /* WRITE ADVANCING: -2 none, -1 PAGE, else lines */
    int  adv_before;        /* the phrase was BEFORE, so the advance is held */
    int  adv_sym; Node *adv_sub;  /* ADVANCING identifier LINES, -1 if not */
    int  eop;               /* WRITE ... AT END-OF-PAGE; lab2 continues */
    int  close_opt;         /* CLOSE: 0 plain, 1 NO REWIND (LEAVE), 2 REEL/UNIT (FEOV) */
    int  had_atend;         /* ST_READ: the statement carried AT END itself */
    int  had_invalid;       /* the statement carried INVALID KEY itself */
    int  ins_first, ins_n;  /* ST_INSPECT: its slice of the operation table */
    int  line;              /* the source line, for the program-check table */
    int  lab3;              /* ST_SEARCH: the end label */
    struct Cond *cond2;     /* ST_SEARCH: the same operands compared with < */
    Node *dsub, *ssub;      /* subscripts on dst and src */
    int  vary_sym;          /* PERFORM ... VARYING identifier, -1 if none */
    Node *vary_from, *vary_by, *times_expr;
    int  vary2_sym, vary3_sym;  /* PERFORM ... VARYING ... AFTER: two more levels */
    Node *vary2_from, *vary2_by, *vary3_from, *vary3_by;
    Cond *acond2, *acond3;
    int  ndop;              /* DISPLAY operands */
    struct { int sym; char lit[MAXTOK]; int litlen; Node *sub; int part_off, part_len; } dop[8];
    int  upon_console;      /* DISPLAY UPON CONSOLE: a WTO rather than SYSOUT */
    int  serial;            /* ST_SEARCH: a serial SEARCH rather than SEARCH ALL */
    struct Cond *whens[8]; int when_lab[8]; int nwhen;   /* serial SEARCH: WHEN series */
    int  acc_from;          /* ACCEPT FROM: 0 SYSIN, 1 DATE, 2 DAY, 3 TIME, 4 CONSOLE */
} Stmt;

/* Files. One DCB each, emitted into the program CSECT. QSAM move mode: the
 * 01 record under the FD is a real area and GET/PUT move into and out of it. */
typedef struct {
    char name[31];
    char label[9];     /* DCB label */
    char ddname[9];
    int  rec_sym;      /* the 01 record beneath the FD */
    int  reclen;
    int  opened_input; /* which OPEN modes appear, so MACRF can be set */
    int  opened_output;
    int  opened_io;    /* OPEN I-O: retrieval and update through one RPL */
    int  has_write;    /* a WRITE names this file, so an insert RPL is needed */
    int  has_start;    /* a START names it, so the RPL needs a search argument */
    int  opened_extend;/* OPEN EXTEND: append to what is already there */
    int  update;       /* a plain sequential file opened I-O: QSAM update mode */
    int  report;       /* the RD this FD carries, or -1 */
    int  print;        /* a WRITE ... ADVANCING names it: ASA control, RECFM=A */
    char pbuf[9];      /* that file's print buffer: control byte + the record */
    int  isam;         /* 0 none, 1 QISAM sequential, 2 BISAM random */
    int  key_sym, nominal_sym;
    int  rrn_via_cell; /* the RELATIVE KEY is not a fullword, so convert it */
    int  blk_records;  /* BLOCK CONTAINS n RECORDS, 0 when unblocked */
    int  blk_chars;    /* BLOCK CONTAINS n CHARACTERS: the block size outright */
    int  linage, footing, top, bottom;  /* LINAGE clause; linage 0 when absent */
    int  lc_sym;       /* the LINAGE-COUNTER item, -1 if none */
    int  optional;     /* SELECT OPTIONAL: the DD may be absent */
    int  reserve;      /* RESERVE n AREAS: BUFNO, 0 for the default */
    int  reversed;     /* OPEN INPUT ... REVERSED: RDBACK */
    /* VSAM. The ANS COBOL system-name says which access method is meant:
     * UT-S-x is QSAM, DA-I-x is ISAM, and a bare name -- or AS-x -- is VSAM.
     * ORGANIZATION then picks the cluster type. */
    int  vsam;         /* 1 when the ASSIGN name says VSAM */
    int  org;          /* 0 sequential/ESDS, 1 indexed/KSDS, 2 relative/RRDS */
    int  access;       /* 0 sequential, 1 random, 2 dynamic */
    int  status_sym;   /* FILE STATUS field, -1 if none */
} File;

#define MAXFILE 16
static File files[MAXFILE];
static int nfile;
/* RECORD KEY and NOMINAL KEY name items declared later, so hold the names and
 * resolve once the data division has been read. */
static char keyname[MAXFILE][31], nomname[MAXFILE][31], statname[MAXFILE][31];

static int file_index(const char *n)
{
    for (int i = 0; i < nfile; i++) if (!strcmp(files[i].name, n)) return i;
    return -1;
}

/* ---- Report Writer -----------------------------------------------------
 * The subset the corpus actually uses: page-heading and detail groups, LINE
 * and LINE PLUS, COLUMN with SOURCE or VALUE, driven by INITIATE / GENERATE /
 * TERMINATE. No CONTROL clauses, no control heading or footing groups, no SUM
 * counters -- control breaks are done by hand in the procedure division, so
 * none of that machinery is needed.
 *
 * Each COLUMN entry becomes an ordinary hidden data item with its own
 * PICTURE, so SOURCE placement reuses the whole existing MOVE path, editing
 * included. Building a line is then: move the sources, then MVC each field
 * into the print buffer at its column.
 */
enum { RG_PAGE_HEADING, RG_DETAIL };

typedef struct { int column, sym, src; char lit[MAXTOK]; int litlen; } RField;
typedef struct { int absolute, n, first_fld, nfld; } RLine;
typedef struct { char name[31]; int report, type, first_line, nline; } RGroup;
typedef struct {
    char name[31];
    int  file;
    int  page_limit, heading, first_detail, last_detail;
    char lbl_line[9], lbl_page[9], lbl_first[9];
} Report;

#define MAXREPORT 4
#define MAXRGROUP 32
#define MAXRLINE  128
#define MAXRFIELD 512
static Report reports[MAXREPORT]; static int nreport;
static RGroup rgroups[MAXRGROUP]; static int nrgroup;
static RLine  rlines[MAXRLINE];   static int nrline;
static RField rfields[MAXRFIELD]; static int nrfield;

static int report_index(const char *n)
{
    for (int i = 0; i < nreport; i++) if (!strcmp(reports[i].name, n)) return i;
    return -1;
}
static int rgroup_index(const char *n)
{
    for (int i = 0; i < nrgroup; i++) if (!strcmp(rgroups[i].name, n)) return i;
    return -1;
}

/* INSPECT. Each operation scans the item for a string -- one character at
 * level 1, any length at level 2 -- and tallies or replaces it; CHARACTERS
 * takes every position. BEFORE and AFTER INITIAL bound the scan at the first
 * occurrence of another string, on each operation separately. An operand is
 * an interned constant (a literal or figurative) or a symbol; the length is
 * fixed at compile time either way. */
enum { INS_T_ALL, INS_T_LEAD, INS_T_CHARS,
       INS_R_ALL, INS_R_LEAD, INS_R_FIRST, INS_R_CHARS };
typedef struct {
    int  kind;
    int  tally;         /* INS_T_*: the counter */
    int  c_sym;  char c_lab[12];  int c_len;    /* the string compared */
    int  by_sym; char by_lab[12]; int by_len;   /* the replacement */
    int  bf_sym; char bf_lab[12]; int bf_len;   /* BEFORE/AFTER INITIAL, bf_len 0 = none */
    int  bf_after;
} InsOp;
#define MAXINSOP 64
static InsOp insops[MAXINSOP];
static int ninsop;

/* Paragraphs, resolved after parsing so a PERFORM may name one that has not
 * been seen yet. */
typedef struct {
    char name[31];
    int is_range_end, is_section;
    int altered;        /* an ALTER names it, so its GO TO is compiled indirect */
    int alter_to;       /* the target that GO TO had when the program was written */
} Para;

/* A declarative section and what calls it. General rule 1 on IV-32: the
 * procedure runs after the standard error routine, or on AT END when the
 * statement carried no AT END phrase. */
#define MAXDECL 16
typedef struct {
    int  sect;          /* the section's index in paras[] */
    int  nfiles;        /* files named, or 0 when a mode was named instead */
    int  file[8];
    int  mode;          /* 0 none, 1 INPUT, 2 OUTPUT, 3 I-O, 4 EXTEND */
} Decl;
static Decl decls[MAXDECL];
static int ndecl;
static int decl_end_para = -1;   /* first procedure after END DECLARATIVES */
#define MAXPARA 1024
static Para paras[MAXPARA];
static int npara;

/* The last procedure in the section that opens at i. A PERFORM of a section
 * runs through everything up to the next section header, so this is where its
 * range ends. */
static int section_end(int i);

static int para_index(const char *n)
{
    for (int i = 0; i < npara; i++) if (!strcmp(paras[i].name, n)) return i;
    return -1;
}

#define MAXSTMT 4096
static Stmt stmts[MAXSTMT];

/* Operands of STRING and UNSTRING, in a side table the statement indexes --
 * a literal or figurative is an interned constant, an identifier is a symbol
 * and subscript whose address is stored into the parameter block at run
 * time. For a STRING sending item the delimiter rides alongside; for an
 * UNSTRING receiver, its DELIMITER IN and COUNT IN items do. */
typedef struct {
    int  sym; Node *sub; char lab[12]; int len;   /* the operand itself */
    int  dsym; Node *dsub; char dlab[12]; int dlen; int dsize; /* STRING: DELIMITED BY; dsize = SIZE */
    int  all;                                     /* UNSTRING delimiter: ALL */
    int  insym; Node *insub;                      /* UNSTRING: DELIMITER IN */
    int  cntsym; Node *cntsub;                    /* UNSTRING: COUNT IN */
} SOp;
#define MAXSOP 2048
static SOp sops[MAXSOP];
static int nsops;
static const char *intern_str(const char *text, int len, int pad);
static int use_hvals, use_lvals, use_qvals, use_spcs;
static int nstmt;

/* Each LINKAGE SECTION 01 is a separate area: the caller owns the storage and
 * passes its address, so offsets restart at 0 and it gets a DSECT of its own
 * plus a cell holding whatever address arrived in the parameter list. */
/* INDEXED BY names a new data item, but it appears inside an OCCURS clause on
 * a group whose subordinates are still to come -- creating it there would put
 * it inside the table's own storage. So they are recorded and appended to
 * WORKING-STORAGE once the data division is finished. */
#define MAXIDX 16
static struct { char name[31], key[31]; int table, desc; } pend_idx[MAXIDX];
/* DEPENDING ON names, resolved once the data division is complete: the count
 * item is commonly declared after the table. */
static struct { char name[31]; int table; } pend_odo[MAXIDX];
static int npend_odo;
static int npend_idx;

#define MAXLINK 16
static int nlinkarea;
static int link_root[MAXLINK];      /* the 01 sym for each area */
static int using_parm[MAXLINK];     /* PROCEDURE DIVISION USING, in order */
static int nusing;
static int is_subprogram;

static int lookup(const char *n)
{
    for (int i = 0; i < nsym; i++) if (!strcmp(syms[i].name, n)) return i;
    return -1;
}

/* CURRENT-DATE is a special register, not something the program declares: an
 * 8-character MM/DD/YY filled from the system clock. It is only created when
 * the source actually mentions it, so programs that do not pay nothing. */
static int uses_curdate;
static int curdate_sym = -1;

static int need_sym(const char *n)
{
    int i = lookup(n);
    if (i < 0) {
        char m[96];
        snprintf(m, sizeof m, "undeclared identifier '%s'", n);
        die(m);
    }
    return i;
}

/* ---- qualification with OF / IN ---------------------------------------
 * COBOL allows the same data name in different groups; a reference then has to
 * name enough enclosing groups to be unique. The corpus does this constantly
 * without ever qualifying anything -- every DYNALOAD control block carries its
 * own WS-MODULE-NAME and WS-MODULE-ADDR, and only the 01 group is ever passed
 * to CALL. So duplicates are legal at declaration and ambiguity is an error
 * only where a name is actually USED. */

#define MAXQUAL 4

static int has_ancestor(int i, const char *q)
{
    for (int p = syms[i].gparent; p >= 0; p = syms[p].gparent)
        if (!strcmp(syms[p].name, q)) return 1;
    return 0;
}

/* Qualifiers may skip levels: B OF D is valid for A > B under C > D. */
static int resolve_sym(const char *name, char q[][31], int nq)
{
    int found = -1, count = 0;
    for (int i = 0; i < nsym; i++) {
        if (strcmp(syms[i].name, name)) continue;
        int ok = 1;
        for (int k = 0; k < nq; k++) if (!has_ancestor(i, q[k])) { ok = 0; break; }
        if (!ok) continue;
        if (count++ == 0) found = i;
    }
    char m[160];
    if (count == 0) {
        if (nq) snprintf(m, sizeof m, "no '%s' is inside '%s'", name, q[0]);
        else    snprintf(m, sizeof m, "undeclared identifier '%s'", name);
        die(m);
    }
    if (count > 1) {
        snprintf(m, sizeof m, "'%s' is ambiguous -- %d items share that name; "
                 "qualify it with OF or IN", name, count);
        die(m);
    }
    return found;
}

static void expect(const char *w);

/* Reads any OF/IN chain that follows a name already consumed. */
static int consume_quals(char q[][31])
{
    int nq = 0;
    while (is("OF") || is("IN")) {
        next();
        if (nq >= MAXQUAL) die("too many levels of qualification");
        if (strlen(tok.text) > 30) die("data name too long");
        strcpy(q[nq++], tok.text);
        next();
    }
    return nq;
}

/* Consumes  name [OF group]...  and resolves it. */
static int consume_sym(void)
{
    char name[31], q[MAXQUAL][31];
    if (strlen(tok.text) > 30) die("data name too long");
    strcpy(name, tok.text);
    next();
    int nq = consume_quals(q);
    return resolve_sym(name, q, nq);
}

/* Open text-name from the copy directories: as given, then with the usual
 * suffixes; under a library subdirectory when OF/IN names one. */
static FILE *copy_open(const char *name, const char *lib, char *path, size_t pn)
{
    static const char *sfx[] = { "", ".cpy", ".CPY", ".cob", ".COB", ".cbl", ".txt", NULL };
    const char *dirs[20]; int nd = 0;
    for (int i = 0; i < ncopy_dirs; i++) dirs[nd++] = copy_dirs[i];
    if (src_dir[0]) dirs[nd++] = src_dir;
    dirs[nd++] = ".";
    for (int d = 0; d < nd; d++)
        for (int k = 0; sfx[k]; k++) {
            if (lib && *lib) snprintf(path, pn, "%s/%s/%s%s", dirs[d], lib, name, sfx[k]);
            else snprintf(path, pn, "%s/%s%s", dirs[d], name, sfx[k]);
            FILE *fp = fopen(path, "r");
            if (fp) return fp;
        }
    return NULL;
}

/* A REPLACING operand, from the raw source. Pseudo-text is everything
 * between == and ==, on one line or several; anything else is one token. */
static void copy_operand(char *out, size_t on, int *pseudo)
{
    for (;;) {
        while (*src.p == ' ') src.p++;
        if (*src.p) break;
        if (!src_fill(&src)) die("COPY REPLACING ends inside the statement");
    }
    if (src.p[0] == '=' && src.p[1] == '=') {
        *pseudo = 1;
        src.p += 2;
        size_t n = 0;
        for (;;) {
            if (!*src.p) {
                if (!src_fill(&src)) die("COPY REPLACING: pseudo-text is not closed");
                if (n < on - 1) out[n++] = ' ';
                continue;
            }
            if (src.p[0] == '=' && src.p[1] == '=') { src.p += 2; break; }
            if (n < on - 1) out[n++] = *src.p;
            src.p++;
        }
        while (n && out[n-1] == ' ') n--;
        out[n] = 0;
        return;
    }
    *pseudo = 0;
    scan_token();
    if (tok.literal) snprintf(out, on, "'%s'", tok.text);
    else snprintf(out, on, "%s", tok.text);
}

static void copy_statement(void)
{
    /* COPY text-name [OF/IN library-name] [REPLACING a BY b ...] . */
    char name[64], lib[64] = "";
    scan_token();
    if (tok.eof || tok.literal) die("COPY needs a text-name");
    snprintf(name, sizeof name, "%s", tok.text);
    scan_token();
    if (is("OF") || is("IN")) {
        scan_token();
        snprintf(lib, sizeof lib, "%s", tok.text);
        scan_token();
    }
    Src cb; memset(&cb, 0, sizeof cb);
    if (is("REPLACING")) {
        for (;;) {
            if (cb.nrep >= 16) die("too many COPY REPLACING pairs");
            int ps;
            copy_operand(cb.rep[cb.nrep].from, sizeof cb.rep[0].from, &ps);
            cb.rep[cb.nrep].pseudo = ps;
            scan_token();
            if (!is("BY")) die("COPY REPLACING wants  operand BY operand");
            copy_operand(cb.rep[cb.nrep].to, sizeof cb.rep[0].to, &ps);
            cb.nrep++;
            /* another pair, or the period */
            while (*src.p == ' ') src.p++;
            if (*src.p == '.' ) break;
            if (!*src.p) { if (!src_fill(&src)) die("COPY has no period"); while (*src.p == ' ') src.p++; if (*src.p == '.') break; }
        }
        if (*src.p != '.') die("COPY has no period");
        src.p++;
    } else {
        if (!is(".")) die("COPY has no period");
    }
    if (copy_depth >= MAXCOPY) die("COPY nests too deeply");
    char path[600];
    FILE *fp = copy_open(name, lib, path, sizeof path);
    if (!fp) {
        char m[200];
        snprintf(m, sizeof m, "COPY %s: no copybook found on the -I directories or beside the program", name);
        die(m);
    }
    copy_stack[copy_depth++] = src;
    cb.fp = fp; cb.p = NULL; cb.line = 0;
    cb.name = strdup(path);
    src = cb;
}

static void parse_program_id(void)
{
    expect("IDENTIFICATION"); expect("DIVISION"); expect(".");
    expect("PROGRAM-ID"); expect(".");
    if (!tok.text[0]) die("PROGRAM-ID has no name");
    if (strlen(tok.text) > 8) die("PROGRAM-ID longer than 8 characters "
                                  "(a CSECT name will not fit)");
    if (!isalpha((unsigned char)tok.text[0]))
        die("PROGRAM-ID must start with a letter");
    for (char *q = tok.text; *q; q++)
        if (!isalnum((unsigned char)*q))
            die("PROGRAM-ID must be alphanumeric for slice 0 "
                "(hyphens need a name-mangling rule)");
    strcpy(progid, tok.text);
    next();
    expect(".");
    /* skip the rest of IDENTIFICATION DIVISION's optional paragraphs */
    while (!tok.eof && !is("ENVIRONMENT") && !is("DATA") && !is("PROCEDURE")) next();
}


/* SPECIAL-NAMES. The 1974 standard says only "implementor-name IS mnemonic-name
 * [ON STATUS IS condition-name] [OFF STATUS IS condition-name]" and leaves the
 * implementor-names themselves open.
 *
 * IKFCBL00 -- asked directly -- accepts SYSIN/SYSIPT, SYSOUT/SYSLST,
 * SYSPUNCH/SYSPCH, CONSOLE, C01 through C12, CSP and S01/S02, and rejects
 * UPSI-n and SWITCH-n outright: OS/360 ANS COBOL has no external switches at
 * all. So the switch spellings here are a deliberate extension past the
 * compiler this one replaces, taken from the IBM systems that do have them,
 * and both are accepted for the sake of source that came from either.
 *
 * The eight bits live in one byte, leftmost digit first, exactly as the UPSI
 * string in a PARM is written. */
/* Mnemonic-names from SPECIAL-NAMES, with the implementor-name each stands
 * for. What DISPLAY UPON and ACCEPT FROM look up. */
static struct { char name[31]; char dev[9]; } mnems[16];
static int nmnem;
static const char *mnem_dev(const char *nm)
{
    for (int i = 0; i < nmnem; i++) if (!strcmp(mnems[i].name, nm)) return mnems[i].dev;
    return NULL;
}

static void parse_special_names(void)
{
    while (!tok.eof && !is("INPUT-OUTPUT") && !is("DATA") && !is("PROCEDURE")) {
        if (is(".")) { next(); continue; }
        if (is("CURRENCY")) {
            next();
            if (is("SIGN")) next();
            if (is("IS")) next();
            if (!tok.literal || tok.len != 1)
                die("CURRENCY SIGN IS takes a one-character nonnumeric literal");
            char c = (char)toupper((unsigned char)tok.text[0]);
            /* II-8: not a digit, not A B C D P R S V X Z, not space, and not
             * any of the editing and punctuation characters. */
            if (isdigit((unsigned char)c) || strchr("ABCDPRSVXZ +-,.*/;()\"'=", c))
                die("that character cannot be a CURRENCY SIGN: it already means "
                    "something in a PICTURE");
            currency_sym = c;
            next();
            continue;
        }
        if (is("DECIMAL-POINT")) {
            next();
            if (is("IS")) next();
            expect("COMMA");
            decimal_is_comma = 1;
            continue;
        }
        char nm[31];
        snprintf(nm, sizeof nm, "%s", tok.text);
        int bit = -1;
        if ((!strncmp(nm, "UPSI-", 5) || !strncmp(nm, "SWITCH-", 7))) {
            const char *d = strrchr(nm, '-') + 1;
            if (d[0] >= '0' && d[0] <= '7' && !d[1]) bit = d[0] - '0';
            else die("a switch is UPSI-0 through UPSI-7, or SWITCH-0 through "
                     "SWITCH-7");
        }
        next();
        if (is("IS")) next();
        if (bit < 0) {
            /* A device or channel name and its mnemonic, remembered for
             * DISPLAY UPON and ACCEPT FROM. */
            if (nmnem < 16 && !tok.eof && !is(".")) {
                snprintf(mnems[nmnem].name, sizeof mnems[nmnem].name, "%s", tok.text);
                snprintf(mnems[nmnem].dev, sizeof mnems[nmnem].dev, "%.8s", nm);
                nmnem++;
            }
            if (!tok.eof && !is(".")) next();
            /* The next clause may follow in the same sentence: SYSOUT IS
             * PRINTER SYSIN IS CARDS. -- so nothing is skipped here. */
            continue;
        }
        if (!tok.eof && !is(".")) next();      /* the mnemonic-name */
        while (is("ON") || is("OFF")) {
            int on = is("ON");
            next();
            if (is("STATUS")) next();
            if (is("IS")) next();
            if (nsym >= MAXSYM) die("too many data items");
            Sym *cn = &syms[nsym];
            memset(cn, 0, sizeof *cn);
            cn->level = 88; cn->is_switch = 1; uses_switches = 1;
            cn->sw_bit = bit; cn->sw_on = on;
            cn->occ_parent = cn->gparent = cn->index_sym = cn->askey_sym = -1;
            cn->fd_file = cn->redef_from = cn->redef_cap = -1;
            cn->parent = -1;
            snprintf(cn->label, sizeof cn->label, "C%04d", nsym);
            snprintf(cn->name, sizeof cn->name, "%s", tok.text);
            if (lookup(cn->name) >= 0) die("duplicate condition name");
            nsym++;
            next();
        }
    }
}

/* ---- PICTURE ----------------------------------------------------------- */
/* Numeric pictures only: optional S, then 9s and one V, with (n) repetition.
 * Editing characters are a later slice and are rejected by name here. */


/* A numeric literal, rescaled to an integer at the given scale. */
static void scale_literal(const char *lit, int scale, char *out, size_t outsz)
{
    char digits[40]; int nd = 0, seen_dot = 0, frac = 0, neg = 0;
    const char *p = lit;
    if (*p == '+') p++; else if (*p == '-') { neg = 1; p++; }
    for (; *p; p++) {
        if (*p == '.') { if (seen_dot) die("two decimal points in a literal"); seen_dot = 1; continue; }
        if (!isdigit((unsigned char)*p)) die("malformed numeric literal");
        if (nd < (int)sizeof digits - 1) digits[nd++] = *p;
        if (seen_dot) frac++;
    }
    digits[nd] = 0;
    while (frac > scale) { if (nd) digits[--nd] = 0; frac--; }     /* truncate */
    while (frac < scale) { if (nd < (int)sizeof digits - 1) { digits[nd++] = '0'; digits[nd] = 0; } frac++; }
    if (nd == 0) { digits[0] = '0'; digits[1] = 0; }
    snprintf(out, outsz, "%s%s", neg ? "-" : "", digits);
}

static int is_numeric_literal(const char *t)
{
    const char *p = t;
    if (*p == '+' || *p == '-') p++;
    if (!*p) return 0;
    int dot = 0, digits = 0;
    for (; *p; p++) {
        if (*p == '.') { if (dot) return 0; dot = 1; continue; }
        if (!isdigit((unsigned char)*p)) return 0;
        digits++;
    }
    return digits > 0;      /* "." alone is the sentence terminator, not a number */
}

/* ---- DATA DIVISION ----------------------------------------------------- */

/* REPORT SECTION. Structure is recognised by the clauses rather than by fixed
 * level numbers, which is what the standard actually means. */
static void parse_report_section(void)
{
    int cur_report = -1, cur_group = -1, cur_line = -1;

    while (!tok.eof && !is("PROCEDURE") && !is("WORKING-STORAGE")) {
        if (is("RD")) {
            next();
            cur_report = report_index(tok.text);
            if (cur_report < 0)
                die("RD names a report that no FD declared with REPORT IS");
            cur_group = cur_line = -1;
            next();
            Report *rp = &reports[cur_report];
            while (!tok.eof && !is(".")) {
                if (is("PAGE")) {
                    next(); if (is("LIMIT")) next(); if (is("IS")) next();
                    rp->page_limit = atoi(tok.text); next();
                    if (is("LINE") || is("LINES")) next();
                } else if (is("HEADING")) { next(); rp->heading = atoi(tok.text); next(); }
                else if (is("FIRST")) { next(); expect("DETAIL"); rp->first_detail = atoi(tok.text); next(); }
                else if (is("LAST"))  { next(); expect("DETAIL"); rp->last_detail  = atoi(tok.text); next(); }
                else if (is("CONTROL") || is("CONTROLS"))
                    die("CONTROL clauses and control break groups are not "
                        "implemented; the corpus does its control breaks by hand");
                else die("this RD clause is not implemented yet");
            }
            expect(".");
            continue;
        }

        if (!isdigit((unsigned char)tok.text[0]))
            die("expected a level number or RD in the REPORT SECTION");
        next();                                   /* the level number itself */

        char nm[31] = "";
        if (!is("LINE") && !is("COLUMN") && !is("TYPE")) {
            snprintf(nm, sizeof nm, "%s", tok.text);
            next();
        }

        if (is("TYPE")) {
            next(); if (is("IS")) next();
            int ty;
            if (is("DETAIL") || is("DE")) { ty = RG_DETAIL; next(); }
            else if (is("PAGE")) { next(); expect("HEADING"); ty = RG_PAGE_HEADING; }
            else die("only TYPE DETAIL and TYPE PAGE HEADING are implemented");
            if (cur_report < 0) die("a report group outside any RD");
            if (nrgroup >= MAXRGROUP) die("too many report groups");
            RGroup *g = &rgroups[nrgroup];
            memset(g, 0, sizeof *g);
            snprintf(g->name, sizeof g->name, "%s", nm);
            g->report = cur_report; g->type = ty;
            g->first_line = nrline;
            cur_group = nrgroup++; cur_line = -1;
            expect(".");
            continue;
        }

        if (is("LINE")) {
            next(); if (is("NUMBER")) next(); if (is("IS")) next();
            if (cur_group < 0) die("a LINE outside any report group");
            if (nrline >= MAXRLINE) die("too many report lines");
            RLine *l = &rlines[nrline];
            memset(l, 0, sizeof *l);
            if (is("PLUS")) { next(); l->absolute = 0; }
            else l->absolute = 1;
            l->n = atoi(tok.text); next();
            l->first_fld = nrfield;
            cur_line = nrline++;
            rgroups[cur_group].nline++;
            expect(".");
            continue;
        }

        if (is("COLUMN")) {
            next(); if (is("NUMBER")) next(); if (is("IS")) next();
            if (cur_line < 0) die("a COLUMN outside any LINE");
            if (nrfield >= MAXRFIELD) die("too many report fields");
            RField *fl = &rfields[nrfield];
            memset(fl, 0, sizeof *fl);
            fl->column = atoi(tok.text); fl->src = -1;
            next();
            char pic[64] = "";
            while (!tok.eof && !is(".")) {
                if (is("PIC") || is("PICTURE")) {
                    next_pic();
                    snprintf(pic, sizeof pic, "%s", tok.text); next();
                } else if (is("SOURCE")) {
                    next(); if (is("IS")) next();
                    fl->src = consume_sym();
                } else if (is("VALUE")) {
                    next(); if (is("IS")) next();
                    if (!tok.literal) die("a report VALUE must be a nonnumeric literal");
                    memcpy(fl->lit, tok.text, (size_t)tok.len + 1);
                    fl->litlen = tok.len; next();
                } else if (is("GROUP") || is("BLANK") || is("JUSTIFIED"))
                    die("this COLUMN clause is not implemented yet");
                else die("this COLUMN clause is not implemented yet");
            }
            expect(".");
            if (!pic[0]) die("a COLUMN entry needs a PICTURE");
            /* The field becomes an ordinary hidden item, so SOURCE placement
               reuses the existing MOVE path, editing and all. */
            if (nsym >= MAXSYM) die("too many data items");
            Sym *sy = &syms[nsym];
            memset(sy, 0, sizeof *sy);
            snprintf(sy->label, sizeof sy->label, "D%04d", nsym);
            snprintf(sy->name, sizeof sy->name, "RPT%d", nrfield);
            sy->level = 49; sy->occ_parent = -1;
            PicInfo pi;
            if (analyse_picture(pic, &pi) < 0) die(pi.err);
            sy->digits = pi.digits; sy->scale = pi.scale;
            sy->is_signed = pi.is_signed; sy->is_alpha = pi.is_alpha;
            sy->is_alphabetic = pi.is_alphabetic;
            sy->edited = pi.edited; sy->floating = pi.floating;
            sy->masklen = pi.masklen; sy->sign_char = pi.sign_char;
            sy->sign_pos = pi.sign_pos; sy->first_sel = pi.first_sel;
            sy->need_lead_start = pi.need_lead_start;
            memcpy(sy->mask, pi.mask, sizeof sy->mask);
            sy->bytes = pi.is_alpha || pi.edited ? pi.bytes : pi.digits;
            sy->elem = sy->bytes;
            sy->offset = wslen; wslen += sy->bytes;
            fl->sym = nsym++;
            rlines[cur_line].nfld++;
            nrfield++;
            continue;
        }
        die("unexpected entry in the REPORT SECTION");
    }
}

/* CURRENT-DATE lives at the head of WORKING-STORAGE. Declaring it as an
 * ordinary symbol rather than special-casing the resolver means it addresses,
 * moves and compares like anything else, and its PICTURE goes through
 * pic_analyse so there is no second path to keep in step. */
static void make_curdate(int *cursor)
{
    if (!uses_curdate || curdate_sym >= 0) return;
    Sym *sy = &syms[nsym];
    memset(sy, 0, sizeof *sy);
    snprintf(sy->name, sizeof sy->name, "CURRENT-DATE");
    snprintf(sy->label, sizeof sy->label, "D%04d", nsym);
    PicInfo pi;
    if (pic_analyse("X(8)", &pi) < 0) die(pi.err);
    sy->is_alpha = pi.is_alpha;
    sy->bytes = pi.bytes;
    sy->elem  = sy->bytes;
    sy->usage = U_DISPLAY;
    sy->level = 1;
    sy->offset = *cursor;
    *cursor += sy->bytes;
    if (*cursor > wslen) wslen = *cursor;
    curdate_sym = nsym;
    nsym++;
}

/* One literal of a level-88 VALUE, into a condition name's value slots. */
static void read_88_literal(char *buf, int *len, int *isstr)
{
    *isstr = 0; *len = 0;
    if (is("ZERO") || is("ZEROS") || is("ZEROES")) { strcpy(buf, "0"); next(); return; }
    if (is("SPACE") || is("SPACES")) { *isstr = 1; strcpy(buf, " "); *len = 1; next(); return; }
    if (is("LOW-VALUE") || is("LOW-VALUES"))   { *isstr = 1; buf[0] = 0; *len = 1; buf[0] = 0; next(); return; }
    if (is("HIGH-VALUE") || is("HIGH-VALUES")) { *isstr = 1; buf[0] = (char)0xFF; buf[1] = 0; *len = 1; next(); return; }
    if (tok.literal) { *isstr = 1; memcpy(buf, tok.text, (size_t)tok.len + 1); *len = tok.len; next(); return; }
    if (is_numeric_literal(tok.text)) { snprintf(buf, MAXTOK, "%s", tok.text); next(); return; }
    die("level 88 VALUE must be a literal");
}

static void parse_data_division(void)
{
    if (!is("DATA")) return;
    next(); expect("DIVISION"); expect(".");
    int opened_ws = 0;
    if (is("FILE")) { next(); expect("SECTION"); expect("."); }
    else if (is("WORKING-STORAGE")) { next(); expect("SECTION"); expect("."); opened_ws = 1; }
    else if (is("LINKAGE")) { next(); expect("SECTION"); expect("."); }
    else { while (!tok.eof && !is("PROCEDURE")) next(); return; }
    int cur_file = -1;

    /* Open groups, innermost last. A group's size is not known until an item
     * at the same or a lower level closes it. */
    int stack[32], sp = 0;
    int redef_limit = -1;
    int cursor = 0;
    /* Share the real cursor, so the register claims storage nothing else can
     * also claim -- the offset drives base-locator addressing, not just the
     * order the DCs come out in. */
    if (opened_ws) make_curdate(&cursor);

    int in_linkage = is("LINKAGE") || 0;
    while (!tok.eof && !is("PROCEDURE")) {
        if (is("LINKAGE")) {
            next(); expect("SECTION"); expect(".");
            while (sp > 0) {
                Sym *g = &syms[stack[--sp]];
                g->elem = cursor - g->offset;
                if (g->occurs > 0) { g->bytes = g->elem * g->occurs; cursor = g->offset + g->bytes; }
                else g->bytes = g->elem;
            }
            if (!in_linkage && cursor > wslen) wslen = cursor;
            in_linkage = 1;
            cursor = 0;
            continue;
        }
        if (is("WORKING-STORAGE")) {
            next(); expect("SECTION"); expect(".");
            while (sp > 0) {
                Sym *g = &syms[stack[--sp]];
                g->elem = cursor - g->offset;
                if (g->occurs > 0) { g->bytes = g->elem * g->occurs; cursor = g->offset + g->bytes; }
                else g->bytes = g->elem;
            }
            if (cursor > wslen) wslen = cursor;
            cur_file = -1;
            make_curdate(&cursor);
            continue;
        }

        if (is("REPORT")) {
            next(); expect("SECTION"); expect(".");
            while (sp > 0) {
                Sym *g = &syms[stack[--sp]];
                g->elem = cursor - g->offset;
                if (g->occurs > 0) { g->bytes = g->elem * g->occurs; cursor = g->offset + g->bytes; }
                else g->bytes = g->elem;
            }
            if (cursor > wslen) wslen = cursor;
            parse_report_section();
            cursor = wslen;
            cur_file = -1;
            continue;
        }
        if (is("FD")) {
            next();
            while (sp > 0) {
                Sym *g = &syms[stack[--sp]];
                g->elem = cursor - g->offset;
                if (g->occurs > 0) { g->bytes = g->elem * g->occurs; cursor = g->offset + g->bytes; }
                else g->bytes = g->elem;
            }
            if (cursor > wslen) wslen = cursor;
            cur_file = file_index(tok.text);
            if (cur_file < 0) die("FD names a file that was not named in a SELECT");
            next();
            /* The FD clauses describe what the DD statement and the label
             * already carry, so accept and ignore them -- except REPORT IS,
             * which says this file carries a report and therefore has no
             * record description of its own. */
            while (!tok.eof && !is(".")) {
                if (is("REPORT")) {
                    next(); if (is("IS")) next();
                    snprintf(files[cur_file].name + 0, 0, "%s", "");   /* no-op */
                    if (nreport >= MAXREPORT) die("too many reports");
                    Report *rp = &reports[nreport];
                    memset(rp, 0, sizeof *rp);
                    snprintf(rp->name, sizeof rp->name, "%s", tok.text);
                    snprintf(rp->lbl_line, sizeof rp->lbl_line, "RL%03d", nreport);
                    snprintf(rp->lbl_first, sizeof rp->lbl_first, "RF%03d", nreport);
                    snprintf(rp->lbl_page, sizeof rp->lbl_page, "RP%03d", nreport);
                    rp->file = cur_file;
                    rp->page_limit = 66; rp->heading = 1;
                    rp->first_detail = 1; rp->last_detail = 66;
                    files[cur_file].report = nreport;
                    nreport++;
                    next();
                    continue;
                }
                if (is("LINAGE")) {
                    /* LINAGE n LINES [WITH FOOTING AT f] [LINES AT TOP t]
                     * [LINES AT BOTTOM b], IV-15: a logical page the runtime
                     * keeps a line count for. Integers only here; the
                     * identifier forms are refused with a message. */
                    next(); if (is("IS")) next();
                    if (!is_numeric_literal(tok.text)) die("LINAGE by an identifier is not implemented; give an integer");
                    files[cur_file].linage = atoi(tok.text); next();
                    if (is("LINES")) next();
                    for (;;) {
                        if (is("WITH")) next();
                        if (is("FOOTING")) {
                            next(); if (is("AT")) next();
                            if (!is_numeric_literal(tok.text)) die("FOOTING by an identifier is not implemented; give an integer");
                            files[cur_file].footing = atoi(tok.text); next();
                            continue;
                        }
                        if (is("LINES")) {
                            next(); if (is("AT")) next();
                            int *w = is("TOP") ? &files[cur_file].top : is("BOTTOM") ? &files[cur_file].bottom : NULL;
                            if (!w) die("LINAGE: LINES AT TOP or LINES AT BOTTOM");
                            next();
                            if (!is_numeric_literal(tok.text)) die("LINES AT TOP/BOTTOM by an identifier is not implemented; give an integer");
                            *w = atoi(tok.text); next();
                            continue;
                        }
                        break;
                    }
                    if (files[cur_file].footing > files[cur_file].linage)
                        die("FOOTING must not exceed LINAGE -- IV-15");
                    files[cur_file].print = 1;
                    snprintf(files[cur_file].pbuf, sizeof files[cur_file].pbuf, "FP%03d", cur_file);
                    continue;
                }
                if (is("BLOCK")) {
                    /* Ignorable for QSAM and for reading ISAM, where OPEN takes
                     * everything from the label -- but an ISAM file opened
                     * OUTPUT is being *created*, so there is no label yet and
                     * the DCB has to carry BLKSIZE itself. */
                    next(); if (is("CONTAINS")) next();
                    if (!is_numeric_literal(tok.text))
                        die("BLOCK CONTAINS wants a number");
                    int nrec = atoi(tok.text); next();
                    if (is("TO")) {            /* n TO m RECORDS: m is the size */
                        next();
                        if (!is_numeric_literal(tok.text)) die("BLOCK CONTAINS n TO m");
                        nrec = atoi(tok.text); next();
                    }
                    /* General rule 3 on IV-11: CHARACTERS states the physical
                     * record size outright, in character positions. RECORDS
                     * states how many logical records it holds. Rule 1 says the
                     * clause may be omitted when a block is one record, which
                     * is what a missing clause means here. */
                    if (is("CHARACTERS")) {
                        next();
                        files[cur_file].blk_chars = nrec;
                        continue;
                    }
                    if (is("RECORDS")) next();
                    files[cur_file].blk_records = nrec;
                    continue;
                }
                next();
            }
            expect(".");
            continue;
        }
        if (!isdigit((unsigned char)tok.text[0]))
            die("expected a level number, FD, or a section header");
        int level = atoi(tok.text);
        if (level != 66 && level != 77 && level != 88 && (level < 1 || level > 49))
            die("level number out of range (01-49, 66, 77, or 88)");
        next();
        if (level == 66) {
            /* 66 name RENAMES d1 [THRU d2], II-29: another name for a range of
             * the record's storage, from d1's first byte to d2's last. With
             * THRU, or when d1 is a group, the new item is a group; alone
             * and elementary, it takes d1's description. It is an alias, so
             * it defines no bytes -- the emitter gives it an EQU. */
            if (nsym >= MAXSYM) die("too many data items");
            Sym *rn = &syms[nsym];
            char nm[31]; snprintf(nm, sizeof nm, "%s", tok.text);
            if (lookup(nm) >= 0) die("duplicate data name");
            next();
            expect("RENAMES");
            int d1 = lookup(tok.text);
            if (d1 < 0) die("RENAMES names an item that was not declared");
            next();
            int d2 = d1;
            if (is("THRU") || is("THROUGH")) {
                next();
                d2 = lookup(tok.text);
                if (d2 < 0) die("RENAMES ... THRU names an item that was not declared");
                next();
            }
            if (syms[d1].level == 1 || syms[d1].level == 77 || syms[d1].is_88 ||
                syms[d2].level == 1 || syms[d2].level == 77 || syms[d2].is_88)
                die("RENAMES may not name a level 01, 77 or 88 item -- II-29");
            if (syms[d1].occurs || syms[d2].occurs || syms[d1].occ_parent >= 0 || syms[d2].occ_parent >= 0)
                die("RENAMES may not name a table or anything inside one -- II-29");
            int end = syms[d2].offset + syms[d2].bytes;
            if (d2 != d1 && end <= syms[d1].offset)
                die("RENAMES ... THRU must run forward through the record");
            if (d2 == d1) *rn = syms[d1];             /* d1's own description */
            else { memset(rn, 0, sizeof *rn); rn->is_group = 1; rn->usage = U_DISPLAY; }
            snprintf(rn->name, sizeof rn->name, "%s", nm);
            snprintf(rn->label, sizeof rn->label, "D%04d", nsym);
            rn->level = 66; rn->alias = 1; rn->has_value = 0;
            rn->offset = syms[d1].offset; rn->bytes = rn->elem = end - syms[d1].offset;
            rn->occurs = 0; rn->occ_depth = 0; rn->occ_parent = -1;
            rn->parent = rn->gparent = rn->index_sym = rn->askey_sym = -1;
            rn->fd_file = syms[d1].fd_file; rn->redef_from = rn->redef_cap = -1;
            rn->linkage = syms[d1].linkage; rn->link_area = syms[d1].link_area;
            expect(".");
            nsym++;
            continue;
        }

        if (level == 88) {
            /* A condition name: no storage, it tests the item it follows. */
            if (nsym == 0) die("level 88 must follow a data item");
            if (nsym >= MAXSYM) die("too many data items");
            Sym *cn = &syms[nsym];
            memset(cn, 0, sizeof *cn);
            cn->level = 88; cn->is_88 = 1;
            snprintf(cn->label, sizeof cn->label, "C%04d", nsym);
            snprintf(cn->name, sizeof cn->name, "%s", tok.text);
            if (lookup(cn->name) >= 0) die("duplicate data name");
            int p = nsym - 1;
            while (p >= 0 && (syms[p].is_88 || syms[p].is_group)) p--;
            if (p < 0) die("level 88 must follow an elementary item");
            cn->parent = p;
            cn->c88_next = -1;
            next();
            expect("VALUE"); if (is("IS")) next();
            if (is("ARE")) next();
            /* VALUE literal-1 [THRU literal-2] [, literal-3 [THRU literal-4]] ...
             * Level 1 takes one literal; the range and the series are level
             * 2. Each further value is a hidden condition name chained from
             * the first, so the test is an OR over the chain and nothing
             * else has to know how many there were. */
            int first = nsym, prev = -1;
            for (;;) {
                Sym *e = &syms[nsym];
                if (nsym != first) {
                    if (nsym >= MAXSYM) die("too many data items");
                    memset(e, 0, sizeof *e);
                    e->level = 88; e->is_88 = 1; e->parent = p; e->c88_next = -1;
                    snprintf(e->label, sizeof e->label, "C%04d", nsym);
                    snprintf(e->name, sizeof e->name, "*88-%d", nsym); /* unnameable */
                    syms[prev].c88_next = nsym;
                }
                read_88_literal(e->cvalue, &e->cvalue_len, &e->cvalue_str);
                if (is("THRU") || is("THROUGH")) {
                    next();
                    int hs;
                    read_88_literal(e->chi, &e->chi_len, &hs);
                    if (hs != e->cvalue_str)
                        die("VALUE ... THRU needs both literals of the same class");
                    e->has_hi = 1;
                }
                prev = nsym++;
                if (is(".")) break;
            }
            expect(".");
            continue;
        }

        while (sp > 0 && syms[stack[sp-1]].level >= level) close_group(&cursor, stack, &sp);
        redef_limit = enclosing_cap(stack, sp);
        int fd_from = -1;
        if (level == 1 || level == 77) {
            while (sp > 0) close_group(&cursor, stack, &sp);
            redef_limit = -1;                      /* a new 01 area starts clean */
            if (in_linkage) {
                cursor = 0;                 /* the caller owns this storage */
            } else {
                /* A group at 01 never ran through the elementary path that
                   advances wslen, so record its extent before starting the next
                   01 -- otherwise the next item is laid down on top of it.
                   01 items start on a doubleword, exactly as IKFCBL00 places
                   them: interiors stay tight, so a control block handed to an
                   assembler routine has the layout that routine was written
                   for, and COMP items land aligned in the common case. */
                if (cursor > wslen) wslen = cursor;
                wslen = (wslen + 7) & ~7;
                cursor = wslen;
            }
            /* An FD may describe its record more than one way. They are not
             * separate areas: every 01 under the FD describes the same buffer,
             * implicitly redefining the first, and the record length is the
             * longest of them. */
            if (level == 1 && cur_file >= 0 && files[cur_file].rec_sym >= 0) {
                fd_from = cursor;
                cursor = syms[files[cur_file].rec_sym].offset;
            }
        }

        if (nsym >= MAXSYM) die("too many data items");
        Sym *sy = &syms[nsym];
        memset(sy, 0, sizeof *sy);
        snprintf(sy->label, sizeof sy->label, "D%04d", nsym);
        sy->occ_parent = -1;
        sy->index_sym = sy->askey_sym = -1;
        sy->odo_dep = -1; sy->odo_tab = 0;
        sy->redef_from = sy->redef_cap = -1;
        sy->fd_file = -1;
        for (int k = sp - 1; k >= 0; k--)
            if (syms[stack[k]].occurs > 0) { sy->occ_parent = stack[k]; break; }
        /* The chain runs the other way: a reference names its subscripts
         * outermost first, and the address is the sum of one term per level. */
        for (int k = 0; k < sp; k++)
            if (syms[stack[k]].occurs > 0) {
                if (sy->occ_depth >= 3)
                    die("COBOL-74 subscripts and indexes to three levels; COBOL-85 raised that to seven, and this compiler targets the earlier standard");
                sy->occ_chain[sy->occ_depth++] = stack[k];
            }
        /* Anything under a REDEFINES shares that storage as well. */
        for (int k = sp - 1; k >= 0; k--)
            if (syms[stack[k]].alias) { sy->alias = 1; break; }
        sy->gparent = sp ? stack[sp-1] : -1;
        sy->linkage = in_linkage;
        if (in_linkage) {
            if (sp == 0) {                  /* a new 01: a new area */
                if (nlinkarea >= MAXLINK) die("too many LINKAGE SECTION items");
                sy->link_area = nlinkarea;
                link_root[nlinkarea++] = nsym;
            } else sy->link_area = syms[stack[0]].link_area;
        }
        sy->level = level;
        if (strlen(tok.text) > 30) die("data name too long");
        if (!strcmp(tok.text, "FILLER")) snprintf(sy->name, sizeof sy->name, "FILL%04d", nsym);
        else {
            strcpy(sy->name, tok.text);
            /* Legal in a different group; only a clash inside the SAME group is
             * an error, because no qualification could ever separate those. */
            for (int k = 0; k < nsym; k++)
                if (!strcmp(syms[k].name, sy->name) && syms[k].gparent == sy->gparent)
                    die("duplicate data name in the same group");
        }
        next();

        char pic[64] = "";
        int item_redef = (fd_from >= 0);  /* REDEFINES, or an FD's later record */
        if (fd_from >= 0) { sy->alias = 1; sy->redef_from = fd_from; }
        sy->usage = U_DISPLAY;
        while (!tok.eof && !is(".")) {
            if (is("PIC") || is("PICTURE")) {
                next_pic();
                if (strlen(tok.text) >= sizeof pic) die("PICTURE too long");
                strcpy(pic, tok.text); next();
            } else if (is("OCCURS")) {
                next();
                if (!is_numeric_literal(tok.text) || strchr(tok.text, '.'))
                    die("OCCURS needs a whole-number literal");
                sy->occurs = atoi(tok.text);
                if (sy->occurs < 1) die("OCCURS count must be positive");
                next();
                sy->odo_dep = -1;
                if (is("TO")) {
                    /* OCCURS integer-1 TO integer-2 TIMES DEPENDING ON name,
                     * III-2. Storage is laid out for integer-2; the count item
                     * says how many occurrences exist at any moment, and with
                     * that the size of every group containing the table. */
                    next();
                    if (!is_numeric_literal(tok.text) || strchr(tok.text, '.'))
                        die("OCCURS ... TO needs a whole-number literal");
                    sy->odo_min = sy->occurs;
                    sy->occurs = atoi(tok.text);
                    if (sy->occurs < sy->odo_min) die("OCCURS ... TO must not go backwards");
                    next();
                    if (is("TIMES")) next();
                    if (!is("DEPENDING")) die("OCCURS ... TO needs DEPENDING ON");
                    next(); if (is("ON")) next();
                    if (npend_odo >= MAXIDX) die("too many DEPENDING ON tables");
                    snprintf(pend_odo[npend_odo].name, sizeof pend_odo[0].name, "%s", tok.text);
                    pend_odo[npend_odo].table = nsym;
                    npend_odo++;
                    sy->odo_dep = -2;                /* resolved later */
                    if (cur_file >= 0) die("OCCURS DEPENDING ON in a file record means variable-length records, which are not implemented; it is supported in WORKING-STORAGE and LINKAGE");
                    next();
                } else if (is("TIMES")) next();
                if (is("DEPENDING")) die("OCCURS ... DEPENDING ON needs the integer-1 TO integer-2 form -- III-2");
                for (;;) {
                    if (is("ASCENDING") || is("DESCENDING")) {
                        /* A series of key names under one word, and any
                         * number of the clauses: the keys are ranked in the
                         * order written. */
                        int desc = is("DESCENDING");
                        next(); if (is("KEY")) next(); if (is("IS")) next();
                        do {
                            if (npend_idx >= MAXIDX) die("too many indexed tables");
                            snprintf(pend_idx[npend_idx].key,
                                     sizeof pend_idx[0].key, "%s", tok.text);
                            pend_idx[npend_idx].table = nsym;
                            pend_idx[npend_idx].desc = desc;
                            npend_idx++;
                            next();
                        } while (!tok.eof && !is(".") && !is_data_clause());
                        continue;
                    }
                    if (is("INDEXED")) {
                        next(); if (is("BY")) next();
                        /* A series: INDEXED BY IA, IB names two indexes for
                         * one table, and the scanner has already dropped the
                         * comma. Keep taking names until a clause word turns
                         * up. */
                        do {
                            if (npend_idx >= MAXIDX) die("too many indexed tables");
                            snprintf(pend_idx[npend_idx].name,
                                     sizeof pend_idx[0].name, "%s", tok.text);
                            pend_idx[npend_idx].table = nsym;
                            npend_idx++;
                            next();
                        } while (!tok.eof && !is(".") && !is_data_clause());
                        continue;
                    }
                    break;
                }
            } else if (is("REDEFINES")) {
                /* The redefining item is laid down on top of the redefined one:
                 * point the cursor at it so this item and everything
                 * subordinate to it takes the same offsets. */
                next();
                int t = lookup(tok.text);
                if (t < 0) die("REDEFINES names an item that was not declared");
                if (syms[t].level != level)
                    die("REDEFINES must name an item at the same level");
                sy->alias = 1;
                item_redef = 1;
                /* The state belongs to the item, not to the parse: a group
                 * REDEFINES stays open across all of its subordinates, and
                 * redefinitions nest. */
                sy->redef_from = cursor;
                cursor = syms[t].offset;
                sy->redef_cap = syms[t].offset + syms[t].bytes;
                redef_limit = sy->redef_cap;
                next();
            } else if (is("JUST") || is("JUSTIFIED")) {
                /* JUSTIFIED RIGHT, II-16: on the way in the data is aligned at
                 * the right of the receiver and space filled on the left, and
                 * a sender that is too long loses its LEFT-hand characters
                 * rather than its right. Only an alphabetic or alphanumeric
                 * receiver may carry it. */
                next(); if (is("RIGHT")) next();
                sy->just = 1;
            } else if (is("SIGN")) {
                /* [SIGN IS] {LEADING|TRAILING} [SEPARATE CHARACTER], II-31.
                 * Without SEPARATE the sign is an overpunch on the leading or
                 * trailing digit and the S costs nothing; with it the sign is
                 * its own character position, '+' or '-', and the S is counted
                 * in the size. Trailing overpunch is what this compiler does
                 * when no clause is given, which is the implementor's choice
                 * general rule 2 leaves open. */
                next(); if (is("IS")) next();
                if (is("LEADING")) { sy->sgn_lead = 1; next(); }
                else if (is("TRAILING")) next();
                else die("SIGN IS needs LEADING or TRAILING");
                if (is("SEPARATE")) { sy->sgn_sep = 1; next(); if (is("CHARACTER")) next(); }
            } else if (is("SYNC") || is("SYNCHRONIZED")) {
                /* Accepted, then verified. This compiler lays items out with no
                 * padding, so SYNC is a no-op exactly when every binary item
                 * under the group already sits on its natural boundary. Where
                 * that is not so the layout would silently differ from what the
                 * clause asks for, which is refused rather than guessed at. */
                sy->sync = 1;
                next();
                if (is("LEFT") || is("RIGHT")) next();
            } else if (is("USAGE")) { next(); if (is("IS")) next(); }
            else if (is("COMP") || is("COMPUTATIONAL")) { sy->usage = U_COMP; next(); }
            else if (is("COMP-3") || is("COMPUTATIONAL-3")) { sy->usage = U_COMP3; next(); }
            else if (is("INDEX")) {
                /* An index data item holds a value corresponding to an
                 * occurrence number, in a form the implementor chooses. This
                 * compiler keeps the occurrence number itself, exactly as an
                 * index-name does, so SET between the two is a plain move.
                 * It has no PICTURE -- the standard forbids one. */
                sy->usage = U_COMP; sy->is_index = 1; next();
            }
            else if (is("DISPLAY")) { sy->usage = U_DISPLAY; next(); }
            else if (is("VALUE")) {
                next(); if (is("IS")) next();
                if (is("ZERO") || is("ZEROS") || is("ZEROES")) { strcpy(sy->value, "0"); sy->has_value = 1; next(); }
                else if (is("SPACE") || is("SPACES")) { sy->has_value = 2; next(); }
                else if (is("LOW-VALUE")  || is("LOW-VALUES"))  { sy->has_value = 4; next(); }
                else if (is("HIGH-VALUE") || is("HIGH-VALUES")) { sy->has_value = 5; next(); }
                else if (is("QUOTE")      || is("QUOTES"))      { sy->has_value = 6; next(); }
                else if (is("ALL") && !tok.literal) {
                    /* ALL literal, level 2: the unit is kept and repeated to
                     * the item's width when the storage is laid down. */
                    next();
                    if (!tok.literal) die("VALUE ALL must be followed by a nonnumeric literal");
                    sy->has_value = 7;
                    snprintf(sy->value, sizeof sy->value, "%s", tok.text);
                    next();
                }
                else if (tok.literal) {
                    sy->has_value = 3;
                    snprintf(sy->value, sizeof sy->value, "%s", tok.text);
                    next();
                } else if (is_numeric_literal(tok.text)) {
                    sy->has_value = 1;
                    snprintf(sy->value, sizeof sy->value, "%s", tok.text);
                    next();
                } else die("VALUE must be a numeric or nonnumeric literal");
            } else {
                char m[96];
                snprintf(m, sizeof m, "clause '%s' is not implemented yet", tok.text);
                die(m);
            }
        }
        expect(".");

        if (!pic[0] && sy->is_index) {
            /* USAGE IS INDEX carries no PICTURE but is elementary all the
             * same: a signed fullword holding an occurrence number. */
            if (sy->has_value) die("VALUE is not allowed on a USAGE INDEX item");
            sy->digits = 9; sy->scale = 0; sy->is_signed = 1;
            sy->bytes = 4;
            sy->elem = sy->bytes;
            if (sy->occurs > 0) { sy->occ_parent = nsym;
            if (sy->occ_depth >= 3) die("COBOL-74 subscripts and indexes to three levels; COBOL-85 raised that to seven, and this compiler targets the earlier standard");
            sy->occ_chain[sy->occ_depth++] = nsym; sy->bytes = sy->elem * sy->occurs; }
            sy->offset = cursor;
            cursor += sy->bytes;
            if (!in_linkage && (level == 1 || level == 77) && cursor > wslen) wslen = cursor;
            nsym++;
            continue;
        }
        if (!pic[0]) {
            /* No PICTURE: this is a group, and the items that follow are its
             * subordinates. Its size is filled in when it closes. */
            sy->is_group = 1;
            /* A group that itself carries OCCURS is the innermost table of a
             * reference to it, so it goes on the end of its own chain. Its
             * element size is not known until it closes, which is fine: the
             * chain stores the symbol, and the size is read at codegen. */
            if (sy->occurs > 0) {
                sy->occ_parent = nsym;
                if (sy->occ_depth >= 3) die("COBOL-74 subscripts and indexes to three levels; COBOL-85 raised that to seven, and this compiler targets the earlier standard");
                sy->occ_chain[sy->occ_depth++] = nsym;
            }
            sy->offset = cursor;
            if (sy->has_value) die("a group item may not carry VALUE here");
            if (sp >= 32) die("group nesting too deep");
            if (cur_file >= 0 && level == 1) {
                sy->fd_file = cur_file;
                if (files[cur_file].rec_sym < 0) files[cur_file].rec_sym = nsym;
            }
            stack[sp++] = nsym;
            nsym++;
            if (level == 1 || level == 77) { /* wslen advances when it closes */ }
            continue;
        }

        {
            PicInfo pi;
            if (analyse_picture(pic, &pi) < 0) die(pi.err);
            sy->digits = pi.digits; sy->scale = pi.scale;
            sy->is_signed = pi.is_signed; sy->is_alpha = pi.is_alpha;
            sy->is_alphabetic = pi.is_alphabetic;
            sy->edited = pi.edited; sy->floating = pi.floating;
            sy->masklen = pi.masklen;
            sy->sign_char = pi.sign_char;
            sy->sign_pos = pi.sign_pos;
            sy->first_sel = pi.first_sel;
            sy->need_lead_start = pi.need_lead_start;
            memcpy(sy->mask, pi.mask, sizeof sy->mask);
            if (pi.is_alpha) {
                if (sy->has_value == 1)
                    die("a numeric VALUE on a PIC X item is not implemented yet");
                sy->bytes = pi.bytes;
            } else {
                if (sy->has_value == 3)
                    die("a nonnumeric VALUE on a numeric item is not implemented yet");
                if (sy->has_value >= 4)
                    die("VALUE LOW-VALUES, HIGH-VALUES and QUOTES are only "
                        "implemented on PIC X items");
                if (sy->has_value == 1) {
                    char scaled[34];
                    scale_literal(sy->value, sy->scale, scaled, sizeof scaled);
                    strcpy(sy->value, scaled);
                }
                if (sy->just && !pi.is_alpha)
                    die("JUSTIFIED applies only to an alphabetic or "
                        "alphanumeric item -- syntax rule 1 on II-16");
                if (pi.edited) {
                    if (sy->usage != U_DISPLAY)
                        die("an edited PICTURE must be USAGE DISPLAY");
                    if (sy->has_value == 1)
                        die("VALUE on an edited item is not implemented yet");
                    sy->bytes = pi.bytes;
                } else switch (sy->usage) {
                case U_DISPLAY:
                    if ((sy->sgn_lead || sy->sgn_sep) && !pi.is_signed)
                        die("a SIGN clause needs an S in the PICTURE -- syntax "
                            "rule 1 on II-31");
                    if (pi.digits > 18)
                        die("18 digits is the standard's ceiling");
                    if (pi.digits > 16 && (sy->sgn_lead || sy->sgn_sep))
                        die("a SIGN clause on an item of more than 16 digits is "
                            "not implemented: the conversion is already split");
                    sy->bytes = pi.digits + (sy->sgn_sep ? 1 : 0);
                    break;
                case U_COMP3:
                    if (sy->sgn_lead || sy->sgn_sep)
                        die("the SIGN clause applies only to USAGE DISPLAY -- "
                            "syntax rule 2 on II-31");
                    sy->bytes = pi.digits / 2 + 1; break;
                case U_COMP:
                    if (pi.digits <= 4)      sy->bytes = 2;
                    else if (pi.digits <= 9) sy->bytes = 4;
                    else die("COMP wider than 9 digits needs doubleword "
                             "arithmetic, which S/370 does not have and this "
                             "compiler does not emulate");
                    break;
                }
            }
        }
        sy->elem = sy->bytes;
        if (sy->occurs > 0) {
            sy->occ_parent = nsym;
            if (sy->occ_depth >= 3) die("COBOL-74 subscripts and indexes to three levels; COBOL-85 raised that to seven, and this compiler targets the earlier standard");
            sy->occ_chain[sy->occ_depth++] = nsym;          /* an elementary table is its own */
            sy->bytes = sy->elem * sy->occurs;
        }
        sy->offset = cursor;
        cursor += sy->bytes;
        if (item_redef) {
            /* Only the item that carries REDEFINES resumes the cursor. A group
             * redefinition's SUBORDINATES must keep walking forward through the
             * aliased area -- resuming there would drop every field after the
             * first on top of the end of the redefined item. A group resumes
             * when it closes, in the pop below.
             *
             * An elementary redefinition may also be shorter than what it
             * covers, and the item after it still follows the ORIGINAL. */
            if (sy->redef_from > cursor) cursor = sy->redef_from;
            redef_limit = enclosing_cap(stack, sp);
        }
        if (redef_limit >= 0 && cursor > redef_limit)
            die("a REDEFINES may not be longer than the item it redefines");
        if (!in_linkage && (level == 1 || level == 77) && cursor > wslen) wslen = cursor;
        if (cur_file >= 0 && level == 1) {
            sy->fd_file = cur_file;
            if (files[cur_file].rec_sym < 0) files[cur_file].rec_sym = nsym;
        }
        nsym++;
    }
    while (sp > 0) {
        close_group(&cursor, stack, &sp);
    }
    if (cursor > wslen) wslen = cursor;

    /* SYNCHRONIZED is a promise about alignment; check it holds. */
    for (int i = 0; i < nsym; i++) {
        if (!syms[i].sync) continue;
        int base = syms[i].offset;
        for (int k = i + 1; k < nsym && syms[k].level > syms[i].level; k++) {
            const Sym *e = &syms[k];
            if (e->is_group || e->is_88 || e->usage != U_COMP) continue;
            int need = e->bytes;                      /* 2 or 4 */
            if ((e->offset - base) % need == 0) continue;
            char m[160];
            snprintf(m, sizeof m, "%s is SYNCHRONIZED but %s would need %d bytes "
                     "of padding to reach its boundary, which this compiler does "
                     "not insert", syms[i].name, e->name,
                     need - (e->offset - base) % need);
            die(m);
        }
    }

    /* DEPENDING ON: find the count item, and mark every group that contains
     * the table so its size is known to be a run-time quantity. Rule 5 on
     * III-2 puts the table last in its record, so nothing after it moves. */
    for (int k = 0; k < npend_odo; k++) {
        int t = pend_odo[k].table;
        int dep = lookup(pend_odo[k].name);
        if (dep < 0) die("DEPENDING ON names an item that was not declared");
        if (syms[dep].is_alpha || syms[dep].is_group || syms[dep].scale)
            die("DEPENDING ON needs an integer item");
        syms[t].odo_dep = dep;
        for (int g = syms[t].gparent; g >= 0; g = syms[g].gparent) {
            if (syms[g].odo_tab > 0) die("a group may contain only one OCCURS DEPENDING ON table");
            syms[g].odo_tab = t;
        }
        /* the table must be the last thing in its record */
        int lvl = syms[t].level;
        for (int j = t + 1; j < nsym && syms[j].level > lvl; j++) ;
        int rec = t; while (syms[rec].gparent >= 0) rec = syms[rec].gparent;
        int endrec = rec + 1;
        while (endrec < nsym && syms[endrec].level > syms[rec].level && syms[endrec].level != 77) endrec++;
        for (int j = t + 1; j < endrec; j++)
            if (syms[j].level <= lvl && !syms[j].is_88)
                die("an OCCURS DEPENDING ON table must be the last item in its record -- rule 5 on III-2");
    }

    /* LINAGE-COUNTER: one hidden COMP halfword per LINAGE file, starting at 1.
     * Only the first is reachable by name -- LINAGE-COUNTER OF file-name is
     * not implemented -- so a program with two LINAGE files is told so. */
    {
        int nlin = 0;
        for (int i = 0; i < nfile; i++) {
            if (!files[i].linage) continue;
            if (nsym >= MAXSYM) die("too many data items");
            Sym *lc = &syms[nsym];
            memset(lc, 0, sizeof *lc);
            lc->level = 77;
            lc->occ_parent = lc->index_sym = lc->askey_sym = lc->gparent = lc->parent = -1;
            lc->odo_dep = -1; lc->fd_file = lc->redef_from = lc->redef_cap = -1;
            snprintf(lc->label, sizeof lc->label, "D%04d", nsym);
            if (nlin++ == 0) snprintf(lc->name, sizeof lc->name, "LINAGE-COUNTER");
            else snprintf(lc->name, sizeof lc->name, "*LINAGE-COUNTER-%d", i);
            lc->usage = U_COMP; lc->digits = 4; lc->bytes = lc->elem = 2;
            wslen = (wslen + 7) & ~7;
            lc->offset = wslen; wslen += 2;
            lc->has_value = 1; strcpy(lc->value, "1");
            files[i].lc_sym = nsym;
            nsym++;
        }
    }

    /* Index items, appended after everything the program declared. COBOL says
     * an index holds a displacement; this one holds the occurrence number,
     * which is what the subscript machinery already expects and is
     * indistinguishable from outside since nothing else may touch it. */
    for (int k = 0; k < npend_idx; k++) {
        if (!pend_idx[k].name[0]) {
            /* a KEY entry: no index item, just the key on the table */
            int ks = lookup(pend_idx[k].key);
            if (ks < 0) die("ASCENDING/DESCENDING KEY names an item that was not declared");
            Sym *tb = &syms[pend_idx[k].table];
            if (tb->nkeys >= 8) die("too many KEY items on one table");
            if (tb->askey_sym < 0) tb->askey_sym = ks;
            tb->keys[tb->nkeys] = ks; tb->keydesc[tb->nkeys] = pend_idx[k].desc; tb->nkeys++;
            continue;
        }
        if (nsym >= MAXSYM) die("too many data items");
        Sym *ix = &syms[nsym];
        memset(ix, 0, sizeof *ix);
        ix->level = 77;
        ix->occ_parent = ix->index_sym = ix->askey_sym = ix->gparent = -1;
        snprintf(ix->label, sizeof ix->label, "D%04d", nsym);
        snprintf(ix->name, sizeof ix->name, "%s", pend_idx[k].name);
        if (lookup(ix->name) >= 0) die("INDEXED BY name is already declared");
        ix->usage = U_COMP; ix->digits = 4; ix->is_signed = 1; ix->is_index = 1;
        ix->bytes = ix->elem = 2;
        wslen = (wslen + 7) & ~7;
        ix->offset = wslen; wslen += 2;
        ix->has_value = 1; strcpy(ix->value, "0");
        if (syms[pend_idx[k].table].index_sym < 0)
            syms[pend_idx[k].table].index_sym = nsym;   /* the first one wins */
        nsym++;
    }

    for (int i = 0; i < nfile; i++) {
        if (files[i].report >= 0) { files[i].reclen = 133; continue; }
        if (files[i].rec_sym < 0) die("an FD has no record description");
        files[i].reclen = 0;
        for (int k = 0; k < nsym; k++)
            if (syms[k].fd_file == i && syms[k].bytes > files[i].reclen)
                files[i].reclen = syms[k].bytes;
        if (keyname[i][0]) files[i].key_sym = need_sym(keyname[i]);
        if (nomname[i][0]) files[i].nominal_sym = need_sym(nomname[i]);
        if (statname[i][0]) {
            files[i].status_sym = need_sym(statname[i]);
            const Sym *st = &syms[files[i].status_sym];
            if (!(st->is_alpha || st->is_group) || st->bytes != 2)
                die("FILE STATUS must be a two-character alphanumeric item");
        }
        if (files[i].isam == 2 && files[i].nominal_sym < 0)
            die("ACCESS IS RANDOM needs a NOMINAL KEY");
        if (files[i].vsam) {
            /* What is not implemented is refused by name, so the gap is
             * obvious rather than mysterious. */
            if (files[i].access == 2 && files[i].org == 1 && files[i].key_sym < 0)
                die("ACCESS IS DYNAMIC needs a RECORD KEY");
            if (files[i].org == 2) {
                /* An RRDS is addressed by record number. VSAM wants that as a
                 * fullword binary, which is what a COBOL RELATIVE KEY declared
                 * PIC 9(8) COMP already is -- so the search argument can point
                 * straight at the program's own field, with no conversion and
                 * nothing to keep in step. */
                if (files[i].key_sym < 0 &&
                    (files[i].access != 0 || files[i].has_start))
                    die("a VSAM RRDS needs a RELATIVE KEY to be read by number");
                if (files[i].key_sym >= 0) {
                    /* The standard asks only for an unsigned integer item. A
                     * fullword COMP is what VSAM's search argument already is,
                     * so that one is used in place; anything else is converted
                     * through the compiler's own cell around each request. */
                    const Sym *k = &syms[files[i].key_sym];
                    if (k->is_alpha || k->is_group || k->scale)
                        die("RELATIVE KEY must be an elementary integer item");
                    files[i].rrn_via_cell = !(k->usage == U_COMP && k->bytes == 4);
                }
            }
            if (files[i].org == 1 && files[i].key_sym < 0)
                die("a VSAM KSDS needs a RECORD KEY");
            if (files[i].org == 0) {
                /* An entry-sequenced dataset has no key: records are found by
                 * where they are, not by what is in them. So the clauses that
                 * name a key have nothing to name, and the access modes that
                 * use one have nothing to use. */
                if (files[i].key_sym >= 0)
                    die("a VSAM ESDS has no key, so no RECORD KEY");
                /* VSAMIOS refuses DYNAMIC for an ESDS too, and for the same
                 * reason: there is no key to read one by. */
                if (files[i].access != 0)
                    die("a VSAM ESDS can only be ACCESS IS SEQUENTIAL");
                if (files[i].has_start)
                    die("START needs a key, which a VSAM ESDS does not have");
            }
        }
    }
    if (wslen > 64 * 1024)
        die("WORKING-STORAGE beyond 64K would need more base locator cells "
            "than this emits");
}

/* SELECT f ASSIGN TO UT-S-DDNAME.  The ddname is the part after the last
 * hyphen of the ANS COBOL system-name. */
static void parse_environment(void)
{
    if (!is("ENVIRONMENT")) return;
    next(); expect("DIVISION"); expect(".");
    while (!tok.eof && !is("DATA") && !is("PROCEDURE")) {
        if (is("CONFIGURATION")) {
            next(); expect("SECTION"); expect(".");
            while (!tok.eof && !is("INPUT-OUTPUT") && !is("DATA") && !is("PROCEDURE")) {
                if (!is("SPECIAL-NAMES")) { next(); continue; }
                next(); expect(".");
                parse_special_names();
            }
            continue;
        }
        if (is("INPUT-OUTPUT")) {
            next(); expect("SECTION"); expect(".");
            if (is("FILE-CONTROL")) { next(); expect("."); }
            while (is("SELECT")) {
                next();
                if (nfile >= MAXFILE) die("too many files");
                File *f = &files[nfile];
                memset(f, 0, sizeof *f);
                if (is("OPTIONAL")) { f->optional = 1; next(); }   /* SELECT OPTIONAL name */
                snprintf(f->name, sizeof f->name, "%s", tok.text);
                if (file_index(f->name) >= 0) die("duplicate file name");
                snprintf(f->label, sizeof f->label, "FD%03d", nfile);
                f->rec_sym = -1;
                f->report = -1;
                f->key_sym = f->nominal_sym = f->status_sym = -1;
                next();
                f->lc_sym = -1;
                while (!tok.eof && !is(".")) {
                    if (is("ASSIGN")) {
                        /* Syntax rule 1: only SELECT has to come first; the
                         * clauses after it may appear in any order, so ASSIGN
                         * can turn up after ACCESS or ORGANIZATION.
                         *
                         * A traditional system-name leads with a device class:
                         * UT-S-x, DA-I-x, UR-S-x. VSAM has no device to name,
                         * so it is written bare or as AS-x. That is how one
                         * INDEXED file is told from another. */
                        next(); if (is("TO")) next();
                        const char *dash = strrchr(tok.text, '-');
                        const char *dd = dash ? dash + 1 : tok.text;
                        if (strlen(dd) > 8) die("ddname longer than 8 characters");
                        snprintf(f->ddname, sizeof f->ddname, "%s", dd);
                        if (!dash) f->vsam = 1;
                        else if (!strncmp(tok.text, "AS-", 3)) f->vsam = 1;
                        next();
                        continue;
                    }
                    if (is("RESERVE")) {
                        /* RESERVE integer AREA(S): the buffer count, which is
                         * BUFNO on the DCB. RESERVE NO ALTERNATE AREA(S) is the
                         * older spelling of the default. */
                        next();
                        if (is_numeric_literal(tok.text)) { f->reserve = atoi(tok.text); next(); }
                        while (!tok.eof && !is(".") && !is("ACCESS") &&
                               !is("ORGANIZATION") && !is("FILE") && !is("RECORD")) next();
                        continue;
                    }
                    if (is("ACCESS")) { next(); if (is("IS")) next();
                        if (is("MODE")) next(); if (is("IS")) next();
                        if (is("SEQUENTIAL")) { f->access = 0; next(); continue; }
                        if (is("RANDOM"))     { f->access = 1; next(); continue; }
                        if (is("DYNAMIC"))    { f->access = 2; next(); continue; }
                        die("ACCESS must be SEQUENTIAL, RANDOM or DYNAMIC"); }
                    if (is("ORGANIZATION")) { next(); if (is("IS")) next();
                        if (is("INDEXED")) {
                            f->org = 1;
                            if (!f->vsam && !f->isam) f->isam = 1;
                            next(); continue;
                        }
                        if (is("RELATIVE")) {
                            if (!f->vsam) die("ORGANIZATION RELATIVE needs a VSAM file");
                            f->org = 2; next();
                            continue;
                        }
                        if (!is("SEQUENTIAL")) die("ORGANIZATION must be SEQUENTIAL, INDEXED or RELATIVE");
                        f->org = 0; next(); continue; }
                    if (is("FILE")) {
                        next(); expect("STATUS"); if (is("IS")) next();
                        snprintf(statname[nfile], sizeof statname[0], "%s", tok.text);
                        next(); continue;
                    }
                    /* RELATIVE KEY names the record number, which is the
                     * search argument for an RRDS exactly as RECORD KEY is for
                     * a KSDS. It is kept in the same place. */
                    if (is("RELATIVE")) {
                        next(); if (is("KEY")) next(); if (is("IS")) next();
                        if (f->org != 2) die("RELATIVE KEY needs ORGANIZATION RELATIVE");
                        snprintf(keyname[nfile], sizeof keyname[0], "%s", tok.text);
                        next(); continue;
                    }
                    if (is("RECORD")) {      /* RECORD KEY IS x -- resolved later */
                        next();
                        if (is("DELIMITER"))
                            die("RECORD DELIMITER is a COBOL-85 clause; this "
                                "compiler targets COBOL-74");
                        if (is("KEY")) next(); if (is("IS")) next();
                        /* RECORD KEY is written the same way for ISAM and for
                         * a VSAM KSDS, so it cannot be what marks a file as
                         * ISAM -- only the absence of a VSAM assign name can. */
                        if (!f->vsam && !f->isam) f->isam = 1;
                        snprintf(keyname[nfile], sizeof keyname[0], "%s", tok.text);
                        next(); continue; }
                    if (is("NOMINAL")) {
                        next(); expect("KEY"); if (is("IS")) next();
                        if (f->vsam) die("NOMINAL KEY is an ISAM clause; VSAM uses RECORD KEY");
                        f->isam = 2;
                        snprintf(nomname[nfile], sizeof nomname[0], "%s", tok.text);
                        next(); continue; }
                    die("this SELECT clause is not implemented yet");
                }
                expect(".");
                if (!f->ddname[0]) die("SELECT needs an ASSIGN clause");
                nfile++;
            }
            continue;
        }
        next();
    }
}


/* ---- expression parser -------------------------------------------------
 * COBOL requires spaces around the arithmetic operators, which is what makes
 * WS-TOTAL one word and A - B three. Parentheses are separators and tokenize
 * on their own.
 */
static Node *parse_expr(void);

static int fig_code(const char *w);
static Node *parse_primary(void)
{
    if (is("(")) { next(); Node *n = parse_expr(); expect(")"); return n; }
    if (is("ALL") && !tok.literal) {
        next();
        if (!tok.literal) die("ALL must be followed by a nonnumeric literal");
        Node *n = node(N_STR);
        memcpy(n->lit, tok.text, (size_t)tok.len + 1);
        n->litlen = tok.len; n->fig = 6 /* FIG_ALL */;
        next();
        return n;
    }
    if (!tok.literal) {
        int fg = fig_code(tok.text);
        if (fg >= 3) {                     /* HIGH-VALUE, LOW-VALUE, QUOTE */
            Node *n = node(N_STR);
            n->lit[0] = 0; n->litlen = 0; n->fig = fg;
            next();
            return n;
        }
    }
    /* The quotes decide, not the characters between them. '24' is an
     * alphanumeric literal and 24 is a numeric one, and COBOL draws no other
     * distinction -- which matters most for FILE STATUS, whose values are
     * two-character alphanumeric and all look like numbers. */
    if (tok.literal) {
        Node *n = node(N_STR);
        memcpy(n->lit, tok.text, (size_t)tok.len + 1);
        n->litlen = tok.len;
        next();
        return n;
    }
    if (is_numeric_literal(tok.text)) {
        Node *n = node(N_LIT);
        const char *dot = strchr(tok.text, '.');
        n->litscale = dot ? (int)strlen(dot + 1) : 0;
        scale_literal(tok.text, n->litscale, n->lit, sizeof n->lit);
        next();
        return n;
    }
    if (is("ZERO") || is("ZEROS") || is("ZEROES")) {
        Node *n = node(N_LIT); strcpy(n->lit, "0"); n->litscale = 0; next(); return n;
    }
    if (is("SPACE") || is("SPACES")) {
        Node *n = node(N_STR); strcpy(n->lit, " "); n->litlen = 1; next(); return n;
    }
    if (!tok.text[0]) die("expression ended unexpectedly");
    Node *n = node(N_SYM);
    n->sym = consume_sym();
    if (is("(")) {                       /* a subscript, not a grouping paren */
        next();
        n->sub = parse_expr();
        if (is(",")) die("only one-dimensional tables are implemented");
        expect(")");
    }
    return n;
}

static Node *parse_unary(void)
{
    if (is("-")) { next(); Node *n = node(N_NEG); n->l = parse_unary(); return n; }
    if (is("+")) { next(); return parse_unary(); }
    return parse_primary();
}

/* Exponentiation binds tighter than multiplication and associates to the
 * right, and a unary sign binds tighter still: -A ** 2 is (-A) ** 2. II-40. */
static Node *parse_power(void)
{
    Node *l = parse_unary();
    if (!is("**")) return l;
    next();
    Node *r = parse_power();
    if (l->kind == N_LIT && r->kind == N_LIT && l->litscale == 0 && r->litscale == 0) {
        /* Both integer literals: fold it here. 2 ** 3 ** 2 is 2 ** 9, and the
         * generator would otherwise see an exponent that is an expression. */
        long long base = atoll(l->lit), e = atoll(r->lit), v = 1;
        for (long long k = 0; k < e; k++) {
            v *= base;
            if (v > 999999999999999999LL) die("a literal ** literal exceeds eighteen digits");
        }
        Node *n = node(N_LIT);
        snprintf(n->lit, sizeof n->lit, "%lld", v);
        n->litscale = 0;
        return n;
    }
    Node *n = node(N_POW); n->l = l; n->r = r;
    return n;
}

static Node *parse_term(void)
{
    Node *l = parse_power();
    for (;;) {
        if (is("*")) { next(); Node *n = node(N_MUL); n->l = l; n->r = parse_power(); l = n; }
        else if (is("/")) { next(); Node *n = node(N_DIV); n->l = l; n->r = parse_power(); l = n; }
        else return l;
    }
}

static Node *parse_expr(void)
{
    Node *l = parse_term();
    for (;;) {
        if (is("+")) { next(); Node *n = node(N_ADD); n->l = l; n->r = parse_term(); l = n; }
        else if (is("-")) { next(); Node *n = node(N_SUB); n->l = l; n->r = parse_term(); l = n; }
        else return l;
    }
}

/* Figurative constants. SPACE and ZERO are level 1; HIGH-VALUE, LOW-VALUE,
 * QUOTE and ALL literal are level 2, and each stands for a string of its
 * character as long as the item it meets. A MOVE sets the first byte (or the
 * first copy of an ALL unit) and lets an overlapping MVC carry it across the
 * rest; a comparison reads against a 256-byte run of the character, or for
 * ALL against the unit repeated to the item's length. QUOTE is the
 * apostrophe, X'7D': that is what IBM's compilers mean by it and what every
 * literal in this compiler's world is delimited with. */
enum { FIG_NONE = 0, FIG_SPACE = 1, FIG_ZERO = 2, FIG_HIGH = 3, FIG_LOW = 4,
       FIG_QUOTE = 5, FIG_ALL = 6 };
static int fig_code(const char *w)
{
    if (!strcmp(w, "SPACE")  || !strcmp(w, "SPACES"))  return FIG_SPACE;
    if (!strcmp(w, "ZERO")   || !strcmp(w, "ZEROS") ||
        !strcmp(w, "ZEROES"))                          return FIG_ZERO;
    if (!strcmp(w, "LOW-VALUE")  || !strcmp(w, "LOW-VALUES"))  return FIG_LOW;
    if (!strcmp(w, "HIGH-VALUE") || !strcmp(w, "HIGH-VALUES")) return FIG_HIGH;
    if (!strcmp(w, "QUOTE")      || !strcmp(w, "QUOTES"))      return FIG_QUOTE;
    return FIG_NONE;
}

/* The byte a one-character figurative stands for, as assembler self-defining
 * text. Hex for the three that are not printable in a C'' constant. */
static const char *fig_byte(int fig)
{
    switch (fig) {
    case FIG_SPACE: return "C' '";
    case FIG_ZERO:  return "C'0'";
    case FIG_HIGH:  return "X'FF'";
    case FIG_LOW:   return "X'00'";
    case FIG_QUOTE: return "X'7D'";
    }
    return "C' '";
}
static const char *fig_name(int fig)
{
    switch (fig) {
    case FIG_SPACE: return "SPACES";   case FIG_ZERO:  return "ZEROS";
    case FIG_HIGH:  return "HIGH-VALUES"; case FIG_LOW: return "LOW-VALUES";
    case FIG_QUOTE: return "QUOTES";   case FIG_ALL:   return "ALL literal";
    }
    return "";
}
/* The 256-byte run a comparison reads against; emitted only when used. */
static const char *fig_run(int fig)
{
    switch (fig) {
    case FIG_HIGH:  use_hvals = 1; return "HVALS";
    case FIG_LOW:   use_lvals = 1; return "LVALS";
    case FIG_QUOTE: use_qvals = 1; return "QVALS";
    }
    return NULL;
}

static Stmt *new_stmt(int op)
{
    if (nstmt >= MAXSTMT) die("too many statements");
    Stmt *st = &stmts[nstmt++];
    memset(st, 0, sizeof *st);
    st->op = op; st->dst = st->src = -1; st->vary_sym = -1; st->rec = -1; st->adv = -2;
    st->line = tok.line;
    return st;
}

/* COBOL-74 has no END-IF: a period ends the whole sentence, unwinding every
 * open IF. at_period carries that up through the nested statement lists. */
static int at_period;
static int nlabel;

static void parse_stmt_list(int allow_else);

/* True when the current token begins a new statement. DISPLAY takes a list of
 * operands with no separator, so without this it happily swallows the verb of
 * the statement that follows it -- two consecutive DISPLAYs inside an INVALID
 * KEY clause is exactly the shape that exposed it. */
static int starts_statement(void)
{
    static const char *verbs[] = {
        "MOVE", "ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "COMPUTE", "IF",
        "ELSE", "DISPLAY", "PERFORM", "EXIT", "STOP", "GO", "GOBACK",
        "READ", "WRITE", "OPEN", "CLOSE", "INITIATE", "GENERATE",
        "TERMINATE", "SET", "ACCEPT", "NEXT", "WHEN", "SEARCH", "CALL",
        "REWRITE", "DELETE", "START", "ENTER", "ALTER", "INSPECT",
        "STRING", "UNSTRING", "CANCEL", 0
    };
    if (tok.literal) return 0;             /* a quoted literal is an operand */
    for (int i = 0; verbs[i]; i++) if (is(verbs[i])) return 1;
    return 0;
}

/* ---- conditions -------------------------------------------------------- */

static Cond *parse_cond(void);

static int relop(int *op)
{
    int neg = 0;
    if (is("IS")) next();
    if (is("NOT")) { neg = 1; next(); }
    if (is("=") || is("EQUAL") || is("EQUALS")) {
        next(); if (is("TO")) next();
        *op = neg ? REL_NE : REL_EQ; return 1;
    }
    if (is(">") || is("GREATER")) {
        next(); if (is("THAN")) next();
        *op = neg ? REL_NGT : REL_GT; return 1;
    }
    if (is("<") || is("LESS")) {
        next(); if (is("THAN")) next();
        *op = neg ? REL_NLT : REL_LT; return 1;
    }
    /* Sign conditions. IS [NOT] POSITIVE / NEGATIVE / ZERO compare against an
     * implicit zero, so there is no right operand to parse -- return 2 and let
     * the caller supply it. */
    if (is("POSITIVE")) { next(); *op = neg ? REL_NGT : REL_GT; return 2; }
    if (is("NEGATIVE")) { next(); *op = neg ? REL_NLT : REL_LT; return 2; }
    if (is("ZERO") || is("ZEROS") || is("ZEROES"))
                        { next(); *op = neg ? REL_NE  : REL_EQ; return 2; }
    /* Class conditions are not relations: there is no right operand and the
     * test is on the item's characters, so the caller builds a C_CLASS. 3 says
     * "a class condition follows, and the NOT is part of it". */
    if (is("NUMERIC") || is("ALPHABETIC")) {
        *op = is("ALPHABETIC");
        next();
        return neg ? 4 : 3;
    }
    if (neg) die("NOT must be followed by a relational operator");
    return 0;
}

static Node *opt_subscript(void);

/* GIVING turns an arithmetic verb into an assignment, so it is built as an
 * expression tree and handed to the COMPUTE path -- which already does the
 * scaling, the packed arithmetic and the store. Only the forms the corpus uses
 * are accepted: no REMAINDER, and ROUNDED only where COMPUTE already takes it.
 */
static Node *operand_node(const char *save, char q[][31], int nq, Node *sub)
{
    Node *n;
    if (is_numeric_literal(save)) {
        n = node(N_LIT);
        const char *dot = strchr(save, '.');
        n->litscale = dot ? (int)strlen(dot + 1) : 0;
        scale_literal(save, n->litscale, n->lit, sizeof n->lit);
        return n;
    }
    n = node(N_SYM);
    n->sym = resolve_sym(save, q, nq);
    n->sub = sub;
    return n;
}

static Node *binop(int kind, Node *l, Node *r)
{
    Node *t = node(kind); t->l = l; t->r = r; return t;
}

/* dst [ROUNDED] for the GIVING target, then the expression. */
/* A literal zero as an expression node, for SUBTRACT a b GIVING c. */
static Node *node_zero(void)
{
    Node *z = node(N_LIT);
    z->litscale = 0;
    scale_literal("0", 0, z->lit, sizeof z->lit);
    return z;
}

/* One INSPECT operand: a nonnumeric literal, a one-character figurative, or
 * an alphanumeric item. */
static void ins_operand(int *sym, char *lab, int *len)
{
    *sym = -1; lab[0] = 0;
    if (tok.literal) {
        if (tok.len < 1) die("an INSPECT operand cannot be empty");
        snprintf(lab, 12, "%s", intern_str(tok.text, tok.len, tok.len));
        *len = tok.len; next();
        return;
    }
    int fg = fig_code(tok.text);
    if (fg == FIG_SPACE) { snprintf(lab, 12, "%s", intern_str(" ", 1, 1)); *len = 1; next(); return; }
    if (fg == FIG_ZERO)  { snprintf(lab, 12, "%s", intern_str("0", 1, 1)); *len = 1; next(); return; }
    if (fg == FIG_HIGH)  { use_hvals = 1; strcpy(lab, "HVALS"); *len = 1; next(); return; }
    if (fg == FIG_LOW)   { use_lvals = 1; strcpy(lab, "LVALS"); *len = 1; next(); return; }
    if (fg == FIG_QUOTE) { use_qvals = 1; strcpy(lab, "QVALS"); *len = 1; next(); return; }
    int i = consume_sym();
    if (!(syms[i].is_alpha || syms[i].is_group) && syms[i].usage != U_DISPLAY)
        die("an INSPECT operand must be a DISPLAY item");
    *sym = i; *len = syms[i].bytes;
}

static void giving_target(Stmt *st)
{
    st->dst = consume_sym();
    st->dsub = opt_subscript();
    if (is("ROUNDED")) { st->rounded = 1; next(); }
}

static void eat_period(void);
/* CORRESPONDING (II-51). Two items correspond when they have the same name
 * and the same qualifiers up to, but not including, the two groups named in
 * the statement; neither is FILLER; and for MOVE at least one is elementary,
 * for ADD and SUBTRACT both are elementary numeric. An item that is a
 * REDEFINES, a table, or an index is left out together with everything
 * beneath it. Pairs come back in the order of the sending group. */
static int corr_skip(const Sym *y)
{
    return y->is_88 || y->alias || y->occurs > 0 || y->is_index
        || (!strncmp(y->name, "FILL", 4) && isdigit((unsigned char)y->name[4]));
}
/* The names from g (exclusive) down to i, outermost first. */
static int corr_path(int i, int g, const char **out, int max)
{
    const char *tmp[32]; int n = 0;
    for (int k = i; k >= 0 && k != g; k = syms[k].gparent) {
        if (n >= 32) die("CORRESPONDING: nesting too deep");
        tmp[n++] = syms[k].name;
    }
    if (n > max) die("CORRESPONDING: nesting too deep");
    for (int k = 0; k < n; k++) out[k] = tmp[n - 1 - k];
    return n;
}
static int corr_pairs(int g1, int g2, int arith, int *xs, int *ys, int max)
{
    int n = 0;
    int l1 = syms[g1].level, l2 = syms[g2].level;
    int skip = 0;                       /* level of an excluded item, or 0 */
    for (int i = g1 + 1; i < nsym && syms[i].level > l1 && syms[i].level != 77; i++) {
        const Sym *x = &syms[i];
        if (skip && x->level > skip) continue;
        skip = 0;
        if (corr_skip(x)) { if (x->is_group) skip = x->level; continue; }
        const char *px[32]; int npx = corr_path(i, g1, px, 32);
        int skip2 = 0;
        for (int j = g2 + 1; j < nsym && syms[j].level > l2 && syms[j].level != 77; j++) {
            const Sym *y = &syms[j];
            if (skip2 && y->level > skip2) continue;
            skip2 = 0;
            if (corr_skip(y)) { if (y->is_group) skip2 = y->level; continue; }
            const char *py[32]; int npy = corr_path(j, g2, py, 32);
            if (npx != npy) continue;
            int same = 1;
            for (int k = 0; k < npx && same; k++) if (strcmp(px[k], py[k])) same = 0;
            if (!same) continue;
            if (arith) { if (x->is_group || y->is_group || x->is_alpha || y->is_alpha || y->edited) continue; }
            else if (x->is_group && y->is_group) continue;
            if (n >= max) die("CORRESPONDING: too many pairs");
            xs[n] = i; ys[n] = j; n++;
            break;
        }
    }
    return n;
}

/* One operand of STRING or UNSTRING: an identifier, a nonnumeric literal, or a
 * one-character figurative constant. A literal becomes an interned constant
 * of exactly its own length; a figurative becomes the first byte of the run
 * a comparison would use. */
static void parse_sop(SOp *o, int is_delim)
{
    o->sym = -1; o->sub = NULL;
    if (tok.literal) {
        snprintf(o->lab, sizeof o->lab, "%s", intern_str(tok.text, tok.len, tok.len));
        o->len = tok.len;
        next();
        return;
    }
    int fg = fig_code(tok.text);
    if (fg == FIG_SPACE) { snprintf(o->lab, sizeof o->lab, "%s", intern_str(" ", 1, 1)); o->len = 1; next(); return; }
    if (fg == FIG_ZERO)  { snprintf(o->lab, sizeof o->lab, "%s", intern_str("0", 1, 1)); o->len = 1; next(); return; }
    if (fg == FIG_HIGH)  { use_hvals = 1; strcpy(o->lab, "HVALS"); o->len = 1; next(); return; }
    if (fg == FIG_LOW)   { use_lvals = 1; strcpy(o->lab, "LVALS"); o->len = 1; next(); return; }
    if (fg == FIG_QUOTE) { use_qvals = 1; strcpy(o->lab, "QVALS"); o->len = 1; next(); return; }
    if (is("ALL")) die("ALL literal as a STRING or UNSTRING operand is not implemented");
    if (is_numeric_literal(tok.text)) die("STRING and UNSTRING operands are nonnumeric; a numeric literal is not");
    o->sym = consume_sym(); o->sub = opt_subscript();
    if (!(syms[o->sym].is_alpha || syms[o->sym].is_group))
        die(is_delim ? "a delimiter must be an alphanumeric item"
                     : "a STRING sending item must be alphanumeric");
}

/* Every arithmetic statement ends the same way: perhaps more receivers, then
 * perhaps ON SIZE ERROR. Level 2 allows a series of results on all of them. */
static int at_arith_end(void)
{
    return tok.eof || is(".") || is("ON") || is("SIZE") || is("GIVING")
        || is("REMAINDER") || is("TO") || is("FROM") || starts_statement();
}

static Node *mk_add(Node *r, Node *x) { return binop(N_ADD, r, x); }
static Node *mk_sub(Node *r, Node *x) { return binop(N_SUB, r, x); }
static Node *mk_mul(Node *r, Node *x) { return binop(N_MUL, r, x); }
static Node *mk_div(Node *r, Node *x) { return binop(N_DIV, r, x); }

/* st names the first receiver already. Make it a COMPUTE of mk(receiver, x)
 * -- or of x itself when mk is null, the GIVING case -- and do the same for
 * each further receiver in the series. The source expression is shared by
 * every statement, which is sound because generation only reads it. */
static void arith_series(Stmt *st, Node *(*mk)(Node *, Node *), Node *x)
{
    for (Stmt *cur = st; ; ) {
        Node *recv = node(N_SYM); recv->sym = cur->dst; recv->sub = cur->dsub;
        cur->op = ST_COMPUTE; cur->src = -1; cur->imm = 0;
        cur->expr = mk ? mk(recv, x) : x;
        if (at_arith_end()) break;
        cur = new_stmt(ST_COMPUTE);
        giving_target(cur);
    }
}

/* ON SIZE ERROR for the statements from stmts[first] to the last one made.
 * Each receiver whose result will not fit is left unchanged and sets a flag;
 * the last statement of the series tests the flag and either skips the
 * imperative statements or falls into them. II-51, general rule 6. */
static void parse_size_error(int first)
{
    if (is("ON")) next();
    if (!is("SIZE")) { eat_period(); return; }
    next(); expect("ERROR");
    int cont = ++nlabel;
    for (int i = first; i < nstmt; i++) { stmts[i].size_err = 1; stmts[i].lab2 = cont; }
    stmts[first].size_first = 1;
    stmts[nstmt - 1].size_last = 1;
    parse_stmt_list(1);
    new_stmt(ST_LABEL)->dst = cont;
}

/* Abbreviated combined relation conditions, II-47. In a sequence of relations
 * joined by AND or OR, a relation may leave out its subject, or its subject
 * and its operator, and take them from the nearest complete relation before
 * it: IF A = 1 OR 2 OR 3, IF A > 1 AND < 100, IF A = B OR C. The last complete
 * relation is remembered here and forgotten when a new condition starts. */
static Node *abbr_subj;
static int   abbr_op;
static int   cond_depth;

static int at_relop(void)
{
    return is("IS") || is("=") || is("EQUAL") || is("EQUALS") || is(">")
        || is("GREATER") || is("<") || is("LESS");
}

/* The condition a level-88 name stands for: its value, or its range, ORed
 * with the rest of its VALUE series. */
static Cond *cond_of_88(int ci)
{
    const Sym *cn = &syms[ci];
    Cond *c;
    Node *p = node(N_SYM); p->sym = cn->parent;
    Node *v;
    if (cn->cvalue_str) {
        v = node(N_STR);
        memcpy(v->lit, cn->cvalue, (size_t)cn->cvalue_len + 1);
        v->litlen = cn->cvalue_len;
        if (cn->cvalue_len == 1 && (unsigned char)cn->cvalue[0] == 0xFF) v->fig = FIG_HIGH;
        if (cn->cvalue_len == 1 && cn->cvalue[0] == 0) v->fig = FIG_LOW;
    } else {
        v = node(N_LIT);
        scale_literal(cn->cvalue, syms[cn->parent].scale, v->lit, sizeof v->lit);
        v->litscale = syms[cn->parent].scale;
    }
    if (!cn->has_hi) {
        c = cnode(C_REL); c->op = REL_EQ; c->l = p; c->r = v;
    } else {
        Node *h;
        if (cn->cvalue_str) {
            h = node(N_STR);
            memcpy(h->lit, cn->chi, (size_t)cn->chi_len + 1);
            h->litlen = cn->chi_len;
            if (cn->chi_len == 1 && (unsigned char)cn->chi[0] == 0xFF) h->fig = FIG_HIGH;
        } else {
            h = node(N_LIT);
            scale_literal(cn->chi, syms[cn->parent].scale, h->lit, sizeof h->lit);
            h->litscale = syms[cn->parent].scale;
        }
        Cond *lo = cnode(C_REL); lo->op = REL_NLT; lo->l = p; lo->r = v;
        Cond *hi = cnode(C_REL); hi->op = REL_NGT; hi->l = p; hi->r = h;
        c = cnode(C_AND); c->cl = lo; c->cr = hi;
    }
    if (cn->c88_next >= 0) {
        Cond *o = cnode(C_OR); o->cl = c; o->cr = cond_of_88(cn->c88_next);
        c = o;
    }
    return c;
}

static Cond *parse_relation(void)
{
    if (is("(")) { next(); Cond *c = parse_cond(); expect(")"); return c; }
    if (abbr_subj && at_relop()) {
        /* Subject omitted: IF A > 1 AND < 100. */
        int op;
        if (relop(&op) != 1) die("an abbreviated relation needs a relational operator");
        Cond *c = cnode(C_REL);
        c->op = op; c->l = abbr_subj; c->r = parse_expr();
        abbr_op = op;
        return c;
    }
    Node *l = parse_expr();
    int op;
    int rk = relop(&op);
    if (rk == 3 || rk == 4) {
        /* II-43: the operand's usage must be DISPLAY, and NUMERIC may not be
         * asked of an alphabetic item. */
        if (l->kind != N_SYM)
            die("a class condition tests an identifier, not an expression");
        const Sym *sy = &syms[l->sym];
        if (sy->usage != U_DISPLAY)
            die("a class condition needs a USAGE DISPLAY item -- II-43");
        if (!op && sy->is_alphabetic)
            die("IS NUMERIC may not be asked of an item whose category is "
                "alphabetic -- II-43");
        Cond *c = cnode(C_CLASS);
        c->cls_sym = l->sym; c->cls_sub = l->sub;
        c->cls_alpha = op; c->cls_not = (rk == 4);
        return c;
    }
    if (rk == 2) {                       /* sign condition: compare with zero */
        Cond *c = cnode(C_REL);
        Node *z = node(N_LIT);
        z->litscale = 0;
        scale_literal("0", 0, z->lit, sizeof z->lit);
        c->op = op; c->l = l; c->r = z;
        return c;
    }
    if (!rk) {
        /* No operator: a condition name, either a level 88 or a switch. */
        if (l->kind == N_SYM && syms[l->sym].is_switch) {
            Cond *c = cnode(C_SWITCH);
            c->sw_bit = syms[l->sym].sw_bit;
            c->sw_on  = syms[l->sym].sw_on;
            return c;
        }
        if (l->kind == N_SYM && syms[l->sym].is_88)
            return cond_of_88(l->sym);
        if (abbr_subj) {
            /* Subject and operator omitted: IF A = 1 OR 2, IF A = B OR C. */
            Cond *c = cnode(C_REL);
            c->op = abbr_op; c->l = abbr_subj; c->r = l;
            return c;
        }
        die("expected a relational operator, or a condition name");
    }
    Cond *c = cnode(C_REL);
    c->op = op; c->l = l; c->r = parse_expr();
    abbr_subj = l; abbr_op = op;
    return c;
}

static Cond *parse_not(void)
{
    if (is("NOT")) { next(); Cond *c = cnode(C_NOT); c->cl = parse_not(); return c; }
    return parse_relation();
}

static Cond *parse_and(void)
{
    Cond *l = parse_not();
    while (is("AND")) { next(); Cond *c = cnode(C_AND); c->cl = l; c->cr = parse_not(); l = c; }
    return l;
}

static Cond *parse_cond(void)
{
    if (cond_depth++ == 0) abbr_subj = NULL;   /* a fresh condition */
    Cond *l = parse_and();
    while (is("OR")) { next(); Cond *c = cnode(C_OR); c->cl = l; c->cr = parse_and(); l = c; }
    cond_depth--;
    return l;
}

/* ---- statements -------------------------------------------------------- */

static void eat_period(void) { if (is(".")) { next(); at_period = 1; } }

/* A subscript directly after an identifier, or NULL. */
/* The subscripts on a reference, as a list. A comma between them has already
 * been dropped by the scanner -- a comma followed by a space is a separator --
 * so ENTRY (A, B) and ENTRY (A B) arrive here identically. */
static Node *opt_subscript(void)
{
    if (!is("(")) return NULL;
    next();
    Node *head = NULL, **tail = &head;
    int n = 0;
    while (!tok.eof && !is(")")) {
        if (++n > 3) die("COBOL-74 allows at most three subscripts on a reference");
        *tail = parse_expr();
        tail = &(*tail)->next;
    }
    expect(")");
    if (!head) die("a subscript list may not be empty");
    return head;
}

static void parse_one_statement(void)
{
    if (is("DISPLAY")) {
        next();
        int first = nstmt;
        Stmt *st = new_stmt(ST_DISPLAY_LIT);
        int line = 0;                       /* characters on the line so far */
        /* COBOL concatenates the operands into one line -- and at level 2
         * the line may be longer than the device's, so what does not fit in
         * 120 characters continues on the next line: an operand is cut where
         * the line ends, and each further line is a further statement. */
        while (!tok.eof && !is(".") && !is("UPON") && !(st->ndop > 0 && starts_statement())) {
            int sym = -1; Node *sub = NULL; int n;
            char lit[MAXTOK]; int islit = tok.literal;
            if (islit) { memcpy(lit, tok.text, (size_t)tok.len + 1); n = tok.len; next(); }
            else {
                sym = consume_sym();
                if (syms[sym].is_88) die("DISPLAY of a condition name is meaningless");
                /* A group and a signed DISPLAY item are both just bytes as far
                 * as DISPLAY is concerned. A signed item shows its last digit
                 * overpunched -- 12345 in a PIC S9(5) prints as 1234E -- which
                 * is what IKFCBL00 does and what the oracle confirms. COMP and
                 * COMP-3 are still refused: their bytes are not characters. */
                if (!syms[sym].is_alpha && !syms[sym].is_group && !syms[sym].edited &&
                    syms[sym].usage != U_DISPLAY)
                    die("DISPLAY of a COMP or COMP-3 item needs a MOVE to a "
                        "DISPLAY item first");
                sub = opt_subscript();
                n = sub ? syms[sym].elem : syms[sym].bytes;
            }
            for (int off = 0; off < n; ) {
                if (line >= 120 || st->ndop >= 8) { st = new_stmt(ST_DISPLAY_LIT); line = 0; }
                int take = n - off < 120 - line ? n - off : 120 - line;
                if (islit) {
                    memcpy(st->dop[st->ndop].lit, lit + off, (size_t)take);
                    st->dop[st->ndop].lit[take] = 0;
                    st->dop[st->ndop].litlen = take;
                    st->dop[st->ndop].sym = -1;
                } else {
                    st->dop[st->ndop].sym = sym; st->dop[st->ndop].sub = sub;
                    st->dop[st->ndop].litlen = 0;
                    st->dop[st->ndop].part_off = off; st->dop[st->ndop].part_len = take;
                }
                st->ndop++;
                off += take; line += take;
            }
        }
        if (!st->ndop) die("DISPLAY with no operands");
        if (is("UPON")) {
            next();
            const char *dev = mnem_dev(tok.text);
            if (!dev) die("DISPLAY UPON needs a mnemonic-name from SPECIAL-NAMES");
            int console = !strcmp(dev, "CONSOLE");
            if (!console && strcmp(dev, "SYSOUT") && strcmp(dev, "SYSLST"))
                die("DISPLAY UPON: the mnemonic must stand for SYSOUT, SYSLST or CONSOLE");
            for (int k = first; k < nstmt; k++) stmts[k].upon_console = console;
            next();
        }
        eat_period();
        return;
    }

    if (is("IF")) {
        next();
        Cond *c = parse_cond();
        if (is("THEN")) next();
        int lelse = ++nlabel;
        Stmt *t = new_stmt(ST_IFTEST); t->cond = c; t->dst = lelse;
        parse_stmt_list(1);
        if (is("ELSE")) {
            next();
            int lend = ++nlabel;
            new_stmt(ST_BRANCH)->dst = lend;
            new_stmt(ST_LABEL)->dst = lelse;
            parse_stmt_list(1);
            new_stmt(ST_LABEL)->dst = lend;
        } else {
            new_stmt(ST_LABEL)->dst = lelse;
        }
        return;
    }

    if (is("MOVE") ) {
        next();
        if (is("CORRESPONDING") || is("CORR")) {
            next();
            int g1 = consume_sym(); Node *s1 = opt_subscript();
            expect("TO");
            int g2 = consume_sym(); Node *s2 = opt_subscript();
            if (!syms[g1].is_group || !syms[g2].is_group)
                die("MOVE CORRESPONDING needs two group items");
            int xs[128], ys[128];
            int n = corr_pairs(g1, g2, 0, xs, ys, 128);
            if (n == 0) die("MOVE CORRESPONDING: no item of the two groups corresponds");
            for (int k = 0; k < n; k++) {
                Stmt *m = new_stmt(ST_MOVE);
                m->src = xs[k]; m->ssub = s1;
                m->dst = ys[k]; m->dsub = s2;
                m->imm = 0; m->fig = 0;
            }
            eat_period();
            return;
        }
        Stmt *st = new_stmt(ST_MOVE);
        char save[MAXTOK]; int savelit = tok.literal, savelen = tok.len;
        memcpy(save, tok.text, (size_t)tok.len + 1);
        next();
        int all = 0; char unit[MAXTOK]; int unitlen = 0;
        if (!savelit && !strcmp(save, "ALL")) {
            if (!tok.literal) die("MOVE ALL must be followed by a nonnumeric literal");
            all = 1; unitlen = tok.len;
            memcpy(unit, tok.text, (size_t)tok.len + 1);
            next();
        }
        char squal[MAXQUAL][31]; int snq = (savelit || all) ? 0 : consume_quals(squal);
        Node *ssub = opt_subscript();
        expect("TO");
        /* One source, any number of receiving fields. The scaling of a numeric
         * literal depends on the item it lands in, so the source is classified
         * once per destination rather than once per statement. */
        for (Stmt *m = st; ; m = new_stmt(ST_MOVE)) {
            m->dst = consume_sym();
            m->dsub = opt_subscript();
            m->ssub = ssub;
            if (all) {
                const Sym *d = &syms[m->dst];
                if (!(d->is_alpha || d->is_group))
                    die("MOVE ALL literal to a numeric item is not implemented");
                m->fig = FIG_ALL;
                memcpy(m->lit, unit, (size_t)unitlen + 1);
                m->litlen = unitlen;
            } else if (savelit) {
                m->imm = 2;                       /* nonnumeric literal */
                memcpy(m->immdigits, save, (size_t)savelen + 1);
                m->immscale = savelen;
            } else if (is_numeric_literal(save)) {
                m->imm = 1; m->immscale = syms[m->dst].scale;
                scale_literal(save, syms[m->dst].scale, m->immdigits, sizeof m->immdigits);
            } else {
                int fg = fig_code(save);
                const Sym *d = &syms[m->dst];
                if (fg == FIG_ZERO && !(d->is_alpha || d->is_group)) {
                    /* ZERO into a numeric item is simply MOVE 0, so let the
                     * existing numeric path scale and store it. */
                    m->imm = 1; m->immscale = d->scale;
                    scale_literal("0", d->scale, m->immdigits, sizeof m->immdigits);
                } else if (fg != FIG_NONE) {
                    if (!(d->is_alpha || d->is_group))
                        die("only ZERO may be moved to a numeric item; SPACE, "
                            "HIGH-VALUE, LOW-VALUE and QUOTE are alphanumeric");
                    m->fig = fg;
                } else m->src = resolve_sym(save, squal, snq);
            }
            if (tok.eof || is(".") || starts_statement()) break;
        }
        eat_period();
        return;
    }

    if (is("ADD") || is("SUBTRACT")) {
        int sub = is("SUBTRACT");
        next();
        if (is("CORRESPONDING") || is("CORR")) {
            next();
            int g1 = consume_sym(); Node *s1 = opt_subscript();
            expect(sub ? "FROM" : "TO");
            int g2 = consume_sym(); Node *s2 = opt_subscript();
            if (!syms[g1].is_group || !syms[g2].is_group)
                die("ADD/SUBTRACT CORRESPONDING needs two group items");
            int rounded = 0;
            if (is("ROUNDED")) { rounded = 1; next(); }
            int xs[128], ys[128];
            int n = corr_pairs(g1, g2, 1, xs, ys, 128);
            if (n == 0) die("ADD/SUBTRACT CORRESPONDING: no elementary numeric item of the two groups corresponds");
            int first = nstmt;
            for (int k = 0; k < n; k++) {
                Stmt *m = new_stmt(ST_COMPUTE);
                m->src = -1; m->dst = ys[k]; m->dsub = s2; m->rounded = rounded;
                Node *r = node(N_SYM); r->sym = ys[k]; r->sub = s2;
                Node *x = node(N_SYM); x->sym = xs[k]; x->sub = s1;
                m->expr = binop(sub ? N_SUB : N_ADD, r, x);
            }
            parse_size_error(first);
            return;
        }
        Stmt *st = new_stmt(sub ? ST_SUB : ST_ADD);
        char save[MAXTOK];
        memcpy(save, tok.text, (size_t)tok.len + 1);
        next();
        char squal2[MAXQUAL][31];
        int snq2 = is_numeric_literal(save) ? 0 : consume_quals(squal2);
        Node *ssub2 = opt_subscript();
        if (!is(sub ? "FROM" : "TO")) {
            /* A series of sources. 1 NUC 1,2 has "identifier/literal series"
             * on both statements, so ADD a b TO c and SUBTRACT a b FROM c are
             * level 1 and not just the GIVING forms. Summing them and then
             * applying the sum is exactly what general rule 3 on II-51 says:
             * the operands are added together first. */
            Node *e = operand_node(save, squal2, snq2, ssub2);
            while (!tok.eof && !is("GIVING") && !is("TO") && !is("FROM") && !is("."))
                e = binop(N_ADD, e, parse_expr());
            int first = (int)(st - stmts);
            if (is("GIVING")) {
                next();
                giving_target(st);
                arith_series(st, NULL, sub ? binop(N_SUB, node_zero(), e) : e);
                parse_size_error(first);
                return;
            }
            expect(sub ? "FROM" : "TO");
            /* The receiver is also an operand: c = c +/- (a + b). */
            giving_target(st);
            Node *lhs = node(N_SYM);
            lhs->sym = st->dst; lhs->sub = st->dsub;
            if (is("GIVING")) {
                /* SUBTRACT a b FROM c GIVING d. */
                next();
                Node *rhs = binop(sub ? N_SUB : N_ADD, lhs, e);
                st->dsub = NULL;
                giving_target(st);
                arith_series(st, NULL, rhs);
            } else arith_series(st, sub ? mk_sub : mk_add, e);
            parse_size_error(first);
            return;
        }
        expect(sub ? "FROM" : "TO");
        st->dst = consume_sym();
        st->dsub = opt_subscript();
        st->ssub = ssub2;
        if (is("GIVING")) {
            /* ADD a TO b GIVING c  /  SUBTRACT a FROM b GIVING c: what was
             * parsed as the destination is really the second operand, and the
             * true destination follows GIVING. */
            next();
            Node *lhs = node(N_SYM);
            lhs->sym = st->dst; lhs->sub = st->dsub;
            Node *rhs = operand_node(save, squal2, snq2, ssub2);
            st->dsub = NULL;
            giving_target(st);
            int first = (int)(st - stmts);
            arith_series(st, NULL, sub ? binop(N_SUB, lhs, rhs) : binop(N_ADD, lhs, rhs));
            parse_size_error(first);
            return;
        }
        if (is("ROUNDED")) { st->rounded = 1; next(); }
        if (!at_arith_end() || is("ON") || is("SIZE") || st->rounded) {
            /* A series of receivers, ROUNDED, or ON SIZE ERROR: all of these
             * go through COMPUTE, which is where that machinery lives. The
             * plain single ADD keeps its shorter path below. */
            int first = (int)(st - stmts);
            arith_series(st, sub ? mk_sub : mk_add, operand_node(save, squal2, snq2, ssub2));
            parse_size_error(first);
            return;
        }
        if (is_numeric_literal(save)) {
            const char *dot = strchr(save, '.');
            st->imm = 1;
            st->immscale = dot ? (int)strlen(dot + 1) : 0;
            scale_literal(save, st->immscale, st->immdigits, sizeof st->immdigits);
        } else st->src = resolve_sym(save, squal2, snq2);
        eat_period();
        return;
    }

    if (is("STRING") || is("UNSTRING")) {
        int un = is("UNSTRING");
        next();
        Stmt *st = new_stmt(un ? ST_UNSTRING : ST_STRING);
        st->ptr_sym = st->tally_sym = -1;
        st->sop_first = nsops; st->sdl_first = nsops;
        if (!un) {
            /* STRING item ... DELIMITED BY x ... INTO recv [WITH POINTER p]
             * [ON OVERFLOW ...]. Several items may share one DELIMITED BY;
             * each item records its own. */
            int grp = nsops;
            while (!is("INTO")) {
                if (tok.eof || is(".")) die("STRING needs INTO");
                if (is("DELIMITED")) {
                    next(); if (is("BY")) next();
                    SOp d; memset(&d, 0, sizeof d); d.sym = -1;
                    if (is("SIZE")) { d.dsize = 1; next(); }
                    else parse_sop(&d, 1);
                    for (int k = grp; k < nsops; k++) {
                        sops[k].dsym = d.sym; sops[k].dsub = d.sub;
                        memcpy(sops[k].dlab, d.lab, sizeof d.lab);
                        sops[k].dlen = d.len; sops[k].dsize = d.dsize;
                    }
                    grp = nsops;
                    continue;
                }
                if (nsops >= MAXSOP) die("too many STRING/UNSTRING operands");
                SOp *o = &sops[nsops++];
                memset(o, 0, sizeof *o); o->sym = o->dsym = o->insym = o->cntsym = -1;
                parse_sop(o, 0);
                o->dsize = -1;                     /* no DELIMITED BY yet */
            }
            st->sop_n = nsops - st->sop_first;
            if (st->sop_n == 0) die("STRING has nothing to send");
            for (int k = st->sop_first; k < nsops; k++)
                if (sops[k].dsize < 0) die("every STRING sending item needs a DELIMITED BY phrase");
            next();
            st->dst = consume_sym(); st->dsub = opt_subscript();
            if (!(syms[st->dst].is_alpha || syms[st->dst].is_group))
                die("STRING INTO needs an alphanumeric item");
        } else {
            /* UNSTRING send [DELIMITED BY [ALL] d [OR [ALL] d]...]
             * INTO recv [DELIMITER IN x] [COUNT IN y] ... [WITH POINTER p]
             * [TALLYING IN t] [ON OVERFLOW ...]. */
            st->src = consume_sym(); st->ssub = opt_subscript();
            if (!(syms[st->src].is_alpha || syms[st->src].is_group))
                die("UNSTRING needs an alphanumeric sending item");
            if (is("DELIMITED")) {
                next(); if (is("BY")) next();
                for (;;) {
                    if (nsops >= MAXSOP) die("too many STRING/UNSTRING operands");
                    SOp *d = &sops[nsops++];
                    memset(d, 0, sizeof *d); d->sym = d->dsym = d->insym = d->cntsym = -1;
                    if (is("ALL")) { d->all = 1; next(); }
                    parse_sop(d, 1);
                    if (!is("OR")) break;
                    next();
                }
            }
            st->sdl_n = nsops - st->sdl_first;
            expect("INTO");
            st->sop_first = nsops;
            while (!tok.eof && !is(".") && !is("WITH") && !is("POINTER") && !is("TALLYING")
                   && !is("ON") && !is("OVERFLOW") && !starts_statement()) {
                if (nsops >= MAXSOP) die("too many STRING/UNSTRING operands");
                SOp *o = &sops[nsops++];
                memset(o, 0, sizeof *o); o->sym = o->dsym = o->insym = o->cntsym = -1;
                o->sym = consume_sym(); o->sub = opt_subscript();
                if (!(syms[o->sym].is_alpha || syms[o->sym].is_group))
                    die("an UNSTRING receiver that is numeric is not implemented; the receivers must be alphanumeric");
                if (is("DELIMITER")) {
                    next(); if (is("IN")) next();
                    o->insym = consume_sym(); o->insub = opt_subscript();
                    if (!(syms[o->insym].is_alpha || syms[o->insym].is_group))
                        die("DELIMITER IN needs an alphanumeric item");
                }
                if (is("COUNT")) {
                    next(); if (is("IN")) next();
                    o->cntsym = consume_sym(); o->cntsub = opt_subscript();
                    if (syms[o->cntsym].is_alpha || syms[o->cntsym].is_group || syms[o->cntsym].scale)
                        die("COUNT IN needs an integer item");
                }
            }
            st->sop_n = nsops - st->sop_first;
            if (st->sop_n == 0) die("UNSTRING INTO names no receiver");
        }
        if (is("WITH")) next();
        if (is("POINTER")) {
            next();
            st->ptr_sym = consume_sym(); st->ptr_sub = opt_subscript();
            if (syms[st->ptr_sym].is_alpha || syms[st->ptr_sym].is_group || syms[st->ptr_sym].scale)
                die("WITH POINTER needs an integer item");
        }
        if (un && is("TALLYING")) {
            next(); if (is("IN")) next();
            st->tally_sym = consume_sym(); st->tally_sub = opt_subscript();
            if (syms[st->tally_sym].is_alpha || syms[st->tally_sym].is_group || syms[st->tally_sym].scale)
                die("TALLYING IN needs an integer item");
        }
        if (is("ON")) next();
        if (is("OVERFLOW")) {
            next();
            st->ovf = 1; st->lab2 = ++nlabel;
            parse_stmt_list(1);
            new_stmt(ST_LABEL)->dst = st->lab2;
        } else eat_period();
        return;
    }

    if (is("SET")) {
        /* SET is table handling's own assignment. Because an index here holds
         * the occurrence number rather than a displacement, every valid
         * combination in the chart on III-12 is an integer move or an integer
         * add -- so this builds MOVE, ADD and SUBTRACT statements rather than
         * a codegen path of its own. */
        next();
        int recv[16], nrecv = 0;
        Node *rsub[16];
        while (!tok.eof && !is("TO") && !is("UP") && !is("DOWN")) {
            if (nrecv >= 16) die("too many receivers in one SET");
            recv[nrecv] = consume_sym();
            rsub[nrecv] = opt_subscript();
            nrecv++;
        }
        if (nrecv == 0) die("SET needs at least one receiving item");
        if (is("UP") || is("DOWN")) {
            int down = is("DOWN");
            next(); expect("BY");
            for (int k = 0; k < nrecv; k++)
                if (!syms[recv[k]].is_index)
                    die("SET ... UP BY and DOWN BY may only change an index-name");
            char save[MAXTOK];
            memcpy(save, tok.text, (size_t)tok.len + 1);
            int lit = is_numeric_literal(save);
            next();
            int ssym = lit ? -1 : lookup(save);
            Node *ssub = lit ? NULL : opt_subscript();
            if (!lit && ssym < 0) die("SET ... BY names something undeclared");
            if (!lit && (syms[ssym].is_index || syms[ssym].scale))
                die("SET ... BY needs an elementary integer item");
            for (int k = 0; k < nrecv; k++) {
                Stmt *m = new_stmt(down ? ST_SUB : ST_ADD);
                m->dst = recv[k]; m->dsub = rsub[k]; m->ssub = ssub;
                if (lit) {
                    m->imm = 1; m->immscale = 0;
                    scale_literal(save, 0, m->immdigits, sizeof m->immdigits);
                } else m->src = ssym;
            }
            eat_period();
            return;
        }
        expect("TO");
        char save[MAXTOK];
        memcpy(save, tok.text, (size_t)tok.len + 1);
        int lit = is_numeric_literal(save);
        next();
        int ssym = lit ? -1 : lookup(save);
        Node *ssub = lit ? NULL : opt_subscript();
        if (!lit && ssym < 0) die("SET ... TO names something undeclared");
        /* The validity chart on III-12. An index-name receives anything; an
         * integer item receives only an index-name; an index data item
         * receives only an index. */
        for (int k = 0; k < nrecv; k++) {
            const Sym *r = &syms[recv[k]];
            int src_is_index = !lit && syms[ssym].is_index;
            if (r->is_index) continue;                 /* receives anything */
            if (!src_is_index)
                die("SET may only put an index-name into an item that is not "
                    "itself an index -- see the chart on III-12");
            if (r->is_alpha || r->is_group || r->scale)
                die("SET ... TO an item that is not an index needs an "
                    "elementary integer");
        }
        for (int k = 0; k < nrecv; k++) {
            Stmt *m = new_stmt(ST_MOVE);
            m->dst = recv[k]; m->dsub = rsub[k]; m->ssub = ssub;
            if (lit) {
                m->imm = 1; m->immscale = 0;
                scale_literal(save, 0, m->immdigits, sizeof m->immdigits);
            } else m->src = ssym;
        }
        eat_period();
        return;
    }

    if (is("MULTIPLY") || is("DIVIDE")) {
        int div = is("DIVIDE");
        next();
        Node *a = parse_expr();
        int into = 0;
        if (div) {
            if (is("INTO")) { into = 1; next(); }
            else if (is("BY")) next();
            else die("DIVIDE wants INTO or BY");
        } else expect("BY");
        Node *b = parse_expr();
        Stmt *st = new_stmt(ST_COMPUTE);
        int first = (int)(st - stmts);
        /* DIVIDE a INTO b  is  b / a;  DIVIDE a BY b  is  a / b. */
        Node *quot = div ? (into ? binop(N_DIV, b, a) : binop(N_DIV, a, b))
                         : binop(N_MUL, a, b);
        if (is("GIVING")) {
            next();
            giving_target(st);
            int qscale = syms[st->dst].scale;
            arith_series(st, NULL, quot);
            if (is("REMAINDER")) {
                if (!div) die("REMAINDER belongs to DIVIDE");
                next();
                /* II-62: the remainder is the dividend less the product of the
                 * divisor and the quotient AS STORED -- truncated to the
                 * quotient item's scale, not rounded, whatever ROUNDED said.
                 * That is a second division, which is the honest price of not
                 * keeping a hidden copy of the quotient. */
                Stmt *r = new_stmt(ST_COMPUTE);
                r->src = -1;
                r->dst = consume_sym(); r->dsub = opt_subscript();
                Node *tq = node(N_TRUNC); tq->l = quot; tq->litscale = qscale;
                Node *dividend = into ? b : a, *divisor = into ? a : b;
                r->expr = binop(N_SUB, dividend, binop(N_MUL, tq, divisor));
            }
            parse_size_error(first);
            return;
        }
        if (is("REMAINDER")) die("REMAINDER needs GIVING");
        /* No GIVING: the second operand is the receiver, and there may be a
         * series of them. MULTIPLY a BY b is b = b * a; DIVIDE a INTO b is
         * b = b / a. DIVIDE a BY b has no meaning without GIVING. */
        if (div && !into) die("DIVIDE a BY b needs GIVING; DIVIDE a INTO b does not");
        if (b->kind != N_SYM)
            die(div ? "DIVIDE ... INTO needs an identifier to divide into"
                    : "MULTIPLY ... BY needs an identifier to multiply");
        st->dst = b->sym; st->dsub = b->sub;
        if (is("ROUNDED")) { st->rounded = 1; next(); }
        arith_series(st, div ? mk_div : mk_mul, a);
        parse_size_error(first);
        return;
    }

    if (is("COMPUTE")) {
        next();
        Stmt *st = new_stmt(ST_COMPUTE);
        int first = (int)(st - stmts);
        giving_target(st);
        while (!is("=") && !is("EQUAL")) {
            if (tok.eof || is(".")) die("COMPUTE has no =");
            giving_target(new_stmt(ST_COMPUTE));
        }
        if (is("EQUAL")) { next(); if (is("TO")) next(); } else expect("=");
        Node *e = parse_expr();
        for (int i = first; i < nstmt; i++) stmts[i].expr = e;
        parse_size_error(first);
        return;
    }

    if (is("PERFORM")) {
        next();
        Stmt *st = new_stmt(ST_PERFORM);
        snprintf(st->para, sizeof st->para, "%s", tok.text);
        next();
        if (is("THRU") || is("THROUGH")) {
            next(); snprintf(st->thru, sizeof st->thru, "%s", tok.text); next();
        } else snprintf(st->thru, sizeof st->thru, "%s", st->para);

        st->vary2_sym = st->vary3_sym = -1;
        if (is("VARYING")) {
            next();
            st->vary_sym = consume_sym();
            if (syms[st->vary_sym].is_alpha || syms[st->vary_sym].is_group)
                die("PERFORM VARYING needs a numeric identifier");
            expect("FROM"); st->vary_from = parse_expr();
            expect("BY");   st->vary_by = parse_expr();
            expect("UNTIL"); st->cond = parse_cond();
            /* AFTER: up to two more levels, each with its own FROM, BY and
             * UNTIL, nested inside the one before it. II-83. */
            for (int lv = 2; is("AFTER"); lv++) {
                if (lv > 3) die("PERFORM VARYING takes at most two AFTER phrases");
                next();
                int vs = consume_sym();
                if (syms[vs].is_alpha || syms[vs].is_group)
                    die("PERFORM VARYING ... AFTER needs a numeric identifier");
                expect("FROM"); Node *fr = parse_expr();
                expect("BY");   Node *by = parse_expr();
                expect("UNTIL"); Cond *c = parse_cond();
                if (lv == 2) { st->vary2_sym = vs; st->vary2_from = fr; st->vary2_by = by; st->acond2 = c; }
                else         { st->vary3_sym = vs; st->vary3_from = fr; st->vary3_by = by; st->acond3 = c; }
            }
        }
        else if (is("UNTIL")) { next(); st->cond = parse_cond(); }
        else if (!is(".") && (is_numeric_literal(tok.text) || lookup(tok.text) >= 0)) {
            /* the n TIMES form; guarded so a following statement's verb is
               never mistaken for a repeat count */
            st->times_expr = parse_expr();
            expect("TIMES");
        }
        eat_period();
        return;
    }

    if (is("GO")) {
        next();
        if (is("TO")) next();
        Stmt *st = new_stmt(ST_GOTO);
        if (is(".")) {
            /* GO TO with no procedure-name (II-65): legal only as the single
             * sentence of a paragraph some ALTER names, whose branch cell
             * then supplies the target. Until an ALTER runs the target is
             * undefined; the cell starts out zero, so an early GO TO takes a
             * program check that names its own line rather than going
             * somewhere plausible. */
            st->para[0] = 0;
            eat_period();
            return;
        }
        snprintf(st->para, sizeof st->para, "%s", tok.text);
        next();
        if (is("DEPENDING") || (!is(".") && !starts_statement())) {
            /* GO TO procedure-name series DEPENDING ON identifier: the value 1
             * selects the first name, 2 the second, and a value outside the
             * series falls through. The names ride in the DISPLAY operand
             * slots, which nothing else in a GO TO uses. */
            st->op = ST_GODEP;
            st->ndop = 0;
            snprintf(st->dop[st->ndop++].lit, MAXTOK, "%s", st->para);
            while (!is("DEPENDING")) {
                if (tok.eof || is(".")) die("GO TO names several procedures but has no DEPENDING ON");
                if (st->ndop >= 8) die("GO TO ... DEPENDING ON takes at most eight procedure-names here");
                snprintf(st->dop[st->ndop++].lit, MAXTOK, "%s", tok.text);
                next();
            }
            next();
            if (is("ON")) next();
            st->src = consume_sym();
            st->ssub = opt_subscript();
            const Sym *v = &syms[st->src];
            if (v->is_alpha || v->is_group || v->scale != 0)
                die("GO TO ... DEPENDING ON needs an elementary numeric integer");
        }
        eat_period();
        return;
    }

    if (is("OPEN")) {
        next();
        while (is("INPUT") || is("OUTPUT") || is("I-O") || is("EXTEND")) {
            int mode = is("INPUT") ? 1 : is("OUTPUT") ? 2 : is("I-O") ? 3 : 4;
            next();
            int any = 0;
            while (!tok.eof && !is(".") && !is("INPUT") && !is("OUTPUT")
                   && !is("I-O") && !is("EXTEND")) {
                int fi = file_index(tok.text);
                if (fi < 0) die("OPEN names something that is not a file");
                if (mode == 4 && files[fi].isam)
                    die("OPEN EXTEND on an ISAM file is not implemented");
                if (mode == 3 && !files[fi].vsam && files[fi].isam)
                    die("OPEN I-O on an ISAM file is not implemented");
                if (mode == 1) files[fi].opened_input = 1;
                else if (mode == 2) files[fi].opened_output = 1;
                else if (mode == 3) files[fi].opened_io = 1;
                else { files[fi].opened_extend = 1; if (!files[fi].vsam) files[fi].opened_output = 1; }
                Stmt *st = new_stmt(ST_OPEN); st->dst = fi; st->src = mode;
                any = 1; next();
                if (is("REVERSED")) {
                    if (mode != 1) die("REVERSED goes with OPEN INPUT");
                    files[fi].reversed = 1; next();
                }
                if (is("WITH")) next();
                if (is("NO")) { next(); expect("REWIND"); }
            }
            if (!any) die("OPEN with no file named");
        }
        eat_period();
        return;
    }

    if (is("CLOSE")) {
        next();
        int any = 0;
        while (!tok.eof && !is(".") && !starts_statement()) {
            int fi = file_index(tok.text);
            if (fi < 0) die("CLOSE names something that is not a file");
            Stmt *cs = new_stmt(ST_CLOSE); cs->dst = fi;
            any = 1; next();
            /* CLOSE REEL/UNIT is a forced end of volume (FEOV); WITH NO REWIND
             * leaves the tape where it is (CLOSE LEAVE); WITH LOCK and FOR
             * REMOVAL close as usual -- there is no operator here to tell. */
            while (is("REEL") || is("UNIT") || is("WITH") || is("NO") ||
                   is("REWIND") || is("LOCK") || is("FOR") || is("REMOVAL")) {
                if (is("REEL") || is("UNIT")) cs->close_opt = 2;
                if (is("REWIND")) cs->close_opt = 1;
                next();
            }
        }
        if (!any) die("CLOSE with no file named");
        eat_period();
        return;
    }

    if (is("READ")) {
        next();
        int fi = file_index(tok.text);
        if (fi < 0) die("READ names something that is not a file");
        next();
        /* READ f NEXT is the sequential half of ACCESS IS DYNAMIC. Plain
         * READ on the same file is the keyed half, which is why one carries
         * AT END and the other INVALID KEY. */
        int rnext = 0;
        if (is("NEXT")) { rnext = 1; next(); }
        if (is("RECORD")) next();
        if (rnext && files[fi].access != 2)
            die("READ NEXT needs ACCESS IS DYNAMIC");
        Stmt *st = new_stmt(ST_READ);
        st->dst = fi;
        st->read_next = rnext;
        if (is("INTO")) {
            /* READ f INTO x is READ f followed by MOVE record TO x, and the
             * move happens only when a record was actually read -- never on
             * the AT END path. */
            next();
            st->src = consume_sym();
        }
        st->lab1 = ++nlabel;                 /* AT END */
        st->lab2 = ++nlabel;                 /* continue */
        if (is("INVALID")) {
            st->had_invalid = 1;
            next(); expect("KEY");
            parse_stmt_list(1);
        } else if (is("AT") || is("END")) {
            if (is("AT")) next();
            expect("END");
            st->had_atend = 1;
            parse_stmt_list(1);
        } else {
            /* No phrase at all. The sentence still has to be closed, and the
             * AT END condition is then the I-O system's business: a USE
             * procedure, if the file has one. */
            eat_period();
        }
        new_stmt(ST_LABEL)->dst = st->lab2;
        return;
    }

    if (is("WRITE")) {
        next();
        int i = consume_sym();
        int fi = syms[i].fd_file;
        if (fi < 0) die("WRITE names something that is not a file's record");
        files[fi].has_write = 1;
        Stmt *st = new_stmt(ST_WRITE);
        st->dst = fi; st->rec = i; st->adv_sym = -1;
        if (is("FROM")) {
            /* WRITE r FROM x is MOVE x TO r followed by WRITE r. */
            next();
            st->src = consume_sym();
        }
        if (is("BEFORE") || is("AFTER")) {
            int after = is("AFTER");
            next();
            if (is("ADVANCING")) next();
            if (is("PAGE")) { next(); st->adv = -1; }
            else if (is_numeric_literal(tok.text)) {
                st->adv = atoi(tok.text);
                if (st->adv < 0 || st->adv > 60)
                    die("ADVANCING takes 0 to 60 lines");
                next();
                if (is("LINE") || is("LINES")) next();
            } else if (mnem_dev(tok.text)) {
                /* A mnemonic-name for a channel: C01 through C12 skip to that
                 * channel, CSP suppresses spacing. Encoded as 1000 + channel,
                 * 1013 for CSP; the runtime turns those into the ASA code. */
                const char *dev = mnem_dev(tok.text);
                if (!strcmp(dev, "CSP")) st->adv = 1013;
                else if (dev[0] == 'C' && isdigit((unsigned char)dev[1]) && isdigit((unsigned char)dev[2]) && !dev[3]
                         && atoi(dev + 1) >= 1 && atoi(dev + 1) <= 12) st->adv = 1000 + atoi(dev + 1);
                else die("ADVANCING mnemonic-name must stand for C01 through C12 or CSP");
                next();
            } else {
                /* ADVANCING identifier LINES: the count is read at run time,
                 * which the runtime was already built for. */
                st->adv_sym = consume_sym(); st->adv_sub = opt_subscript();
                if (syms[st->adv_sym].is_alpha || syms[st->adv_sym].is_group || syms[st->adv_sym].scale)
                    die("ADVANCING needs an integer item");
                st->adv = 0;
                if (is("LINE") || is("LINES")) next();
            }
            /* The runtime takes the count, or -1 for PAGE, negated when the
             * phrase was BEFORE -- which is what tells it to hold the advance
             * over rather than apply it. */
            st->adv_before = !after;
            files[fi].print = 1;
            snprintf(files[fi].pbuf, sizeof files[fi].pbuf, "FP%03d", fi);
        }
        if (is("AT") || is("END-OF-PAGE") || is("EOP")) {
            if (is("AT")) next();
            if (!is("END-OF-PAGE") && !is("EOP")) die("WRITE ... AT needs END-OF-PAGE");
            next();
            if (!files[fi].linage) die("END-OF-PAGE needs a file with a LINAGE clause -- IV-34");
            st->eop = 1; st->lab2 = ++nlabel;
            parse_stmt_list(1);
            new_stmt(ST_LABEL)->dst = st->lab2;
            return;
        }
        st->lab1 = ++nlabel;                 /* INVALID KEY */
        st->lab2 = ++nlabel;                 /* continue */
        if (is("INVALID")) {
            if (!files[fi].isam && !files[fi].vsam)
                die("INVALID KEY on WRITE needs an INDEXED file");
            st->had_invalid = 1;
            next(); expect("KEY");
            parse_stmt_list(1);
        } else {
            eat_period();
        }
        new_stmt(ST_LABEL)->dst = st->lab2;
        return;
    }

    /* REWRITE r [FROM x] [INVALID KEY ...] -- put back the record the last
     * READ is holding. DELETE f [RECORD] [INVALID KEY ...] -- erase it. Both
     * name what COBOL says they name: REWRITE a record, DELETE a file. */
    if (is("REWRITE")) {
        next();
        int i = consume_sym();
        int fi = syms[i].fd_file;
        if (fi < 0) die("REWRITE names something that is not a file's record");
        if (files[fi].isam) die("REWRITE on an ISAM file is not implemented");
        Stmt *st = new_stmt(ST_REWRITE);
        st->dst = fi; st->rec = i;
        if (is("FROM")) { next(); st->src = consume_sym(); }
        st->lab1 = ++nlabel;                 /* INVALID KEY */
        st->lab2 = ++nlabel;                 /* continue */
        if (is("INVALID")) {
            st->had_invalid = 1;
            /* A sequential REWRITE has no key to be invalid, and the standard
             * gives it no INVALID KEY phrase. Refusing it is kinder than
             * generating a branch that can never be taken. */
            if (!files[fi].vsam)
                die("INVALID KEY has no meaning on a sequential REWRITE");
            next(); expect("KEY"); parse_stmt_list(1);
        }
        else eat_period();
        new_stmt(ST_LABEL)->dst = st->lab2;
        return;
    }

    if (is("DELETE")) {
        next();
        int fi = file_index(tok.text);
        if (fi < 0) die("DELETE names something that is not a file");
        if (!files[fi].vsam) die("DELETE is implemented for VSAM files only");
        next();
        if (is("RECORD")) next();
        Stmt *st = new_stmt(ST_DELETE);
        st->dst = fi;
        st->lab1 = ++nlabel;
        st->lab2 = ++nlabel;
        if (is("INVALID")) { st->had_invalid = 1; next(); expect("KEY"); parse_stmt_list(1); }
        else eat_period();
        new_stmt(ST_LABEL)->dst = st->lab2;
        return;
    }

    /* START f [KEY IS {EQUAL TO | NOT LESS THAN | GREATER THAN} k]
     *          [INVALID KEY ...]
     * Positions the file so the next sequential READ returns the record the
     * key phrase describes. The key named must be the RECORD KEY: that is
     * where the RPL's search argument already points, so there is nothing to
     * move and nothing to choose. */
    if (is("START")) {
        next();
        int fi = file_index(tok.text);
        if (fi < 0) die("START names something that is not a file");
        if (!files[fi].vsam) die("START is implemented for VSAM files only");
        next();
        files[fi].has_start = 1;
        Stmt *st = new_stmt(ST_START);
        st->dst = fi;
        st->src = 0;                         /* 0 KEQ, 1 KGE */
        if (is("KEY")) {
            next(); if (is("IS")) next();
            if (is("=") || is("EQUAL")) {
                st->src = 0; next(); if (is("TO")) next();
            } else if (is(">")) {
                die("START KEY IS GREATER THAN is not implemented yet");
            } else if (is(">=")) {
                st->src = 1; next();
            } else if (is("NOT")) {
                next();
                if (!is("<") && !is("LESS")) die("START KEY IS NOT what?");
                next(); if (is("THAN")) next();
                st->src = 1;
            } else if (is("GREATER")) {
                next(); if (is("THAN")) next();
                if (!is("OR")) die("START KEY IS GREATER THAN is not implemented yet");
                next(); expect("EQUAL"); if (is("TO")) next();
                st->src = 1;
            } else die("START KEY IS what?");
            int k = consume_sym();
            if (k != files[fi].key_sym)
                die("START must name the RECORD KEY; generic keys are not "
                    "implemented yet");
        }
        st->lab1 = ++nlabel;                 /* INVALID KEY */
        st->lab2 = ++nlabel;                 /* continue */
        if (is("INVALID")) { st->had_invalid = 1; next(); expect("KEY"); parse_stmt_list(1); }
        else eat_period();
        new_stmt(ST_LABEL)->dst = st->lab2;
        return;
    }

    if (is("INITIATE") || is("TERMINATE")) {
        int init = is("INITIATE");
        next();
        int r = report_index(tok.text);
        if (r < 0) die("INITIATE/TERMINATE names something that is not a report");
        new_stmt(init ? ST_INITIATE : ST_TERMINATE)->dst = r;
        next();
        eat_period();
        return;
    }

    if (is("GENERATE")) {
        next();
        int g = rgroup_index(tok.text);
        if (g < 0) die("GENERATE names something that is not a report group");
        if (rgroups[g].type != RG_DETAIL)
            die("GENERATE of a non-detail group is not implemented");
        new_stmt(ST_GENERATE)->dst = g;
        next();
        eat_period();
        return;
    }

    if (is("SEARCH")) {
        next();
        if (!is("ALL")) {
            /* Serial SEARCH (III-7): from the index's current value up to the
             * last occurrence, the WHEN conditions are tried in order at
             * each; the first true one runs its statements and ends the
             * search, none true steps the index (and the VARYING item) and
             * goes round, and stepping past the table is AT END. */
            int t = consume_sym();
            const Sym *tb = &syms[t];
            if (tb->occurs < 1) die("SEARCH names something that is not a table");
            if (tb->index_sym < 0) die("SEARCH needs the table to have INDEXED BY");
            Stmt *st = new_stmt(ST_SEARCH);
            st->serial = 1;
            st->dst  = t;
            st->vary_sym = -1;
            st->lab1 = ++nlabel;             /* AT END */
            st->lab3 = ++nlabel;             /* past the whole statement */
            if (is("VARYING")) {
                next();
                st->vary_sym = consume_sym();
                if (syms[st->vary_sym].is_alpha || syms[st->vary_sym].is_group || syms[st->vary_sym].scale)
                    die("SEARCH VARYING needs an index-name or an integer item");
            }
            int have_atend = 0;
            if (is("AT") || is("END")) {
                if (is("AT")) next();
                expect("END");
                have_atend = 1;
            }
            new_stmt(ST_LABEL)->dst = st->lab1;
            if (have_atend) parse_stmt_list(1);
            new_stmt(ST_BRANCH)->dst = st->lab3;
            if (!is("WHEN")) die("SEARCH needs at least one WHEN");
            while (is("WHEN")) {
                next();
                if (st->nwhen >= 8) die("too many WHEN phrases on one SEARCH");
                st->whens[st->nwhen] = parse_cond();
                st->when_lab[st->nwhen] = ++nlabel;
                new_stmt(ST_LABEL)->dst = st->when_lab[st->nwhen];
                st->nwhen++;
                parse_stmt_list(1);
                new_stmt(ST_BRANCH)->dst = st->lab3;
            }
            new_stmt(ST_LABEL)->dst = st->lab3;
            eat_period();
            return;
        }
        next();
        int t = consume_sym();
        const Sym *tb = &syms[t];
        if (tb->occurs < 1) die("SEARCH ALL names something that is not a table");
        if (tb->index_sym < 0) die("SEARCH ALL needs the table to have INDEXED BY");
        if (tb->nkeys < 1) die("SEARCH ALL needs the table to have an ASCENDING or DESCENDING KEY");
        Stmt *st = new_stmt(ST_SEARCH);
        st->dst  = t;
        st->lab1 = ++nlabel;             /* AT END */
        st->lab2 = ++nlabel;             /* the WHEN body */
        st->lab3 = ++nlabel;             /* past the whole statement */
        st->src  = ++nlabel;             /* where "mid sorts before the target" lands */
        int have_atend = 0;
        if (is("AT") || is("END")) {
            if (is("AT")) next();
            expect("END");
            have_atend = 1;
        }
        /* The statement list order in the source is AT END first, then WHEN,
         * but both are reached by a branch out of the search loop. */
        new_stmt(ST_LABEL)->dst = st->lab1;
        if (have_atend) parse_stmt_list(1);
        new_stmt(ST_BRANCH)->dst = st->lab3;
        expect("WHEN");
        /* WHEN key-1 (index) = value-1 [AND key-2 (index) = value-2]...: the
         * keys named must be the table's, in order from the first. The binary
         * search needs "does the middle element sort before the target": for
         * one key that is key < value (or > for DESCENDING); for several it
         * is the first key deciding, or equal there and the next deciding. */
        Cond *c = parse_cond();
        Cond *eqs[8]; int neq = 0;
        for (Cond *w = c; ; w = w->cl) {
            Cond *leaf = (w->kind == C_AND) ? w->cr : w;
            if (leaf->kind != C_REL || leaf->op != REL_EQ)
                die("SEARCH ALL wants a WHEN of key (index) = value, joined by AND");
            if (neq >= 8) die("too many keys in a SEARCH ALL WHEN");
            eqs[neq++] = leaf;
            if (w->kind != C_AND) break;
        }
        /* they were collected right to left */
        for (int a = 0, z = neq - 1; a < z; a++, z--) { Cond *x = eqs[a]; eqs[a] = eqs[z]; eqs[z] = x; }
        if (neq > tb->nkeys) die("SEARCH ALL names more keys than the table has");
        for (int k = 0; k < neq; k++) {
            if (eqs[k]->l->kind != N_SYM || eqs[k]->l->sym != tb->keys[k])
                die("SEARCH ALL: the WHEN must name the table's keys in the order declared, each as key (index) = value");
        }
        st->cond = c;
        /* before = LESS1 or (EQ1 and (LESS2 or (EQ2 and ...))) */
        Cond *before = NULL;
        for (int k = neq - 1; k >= 0; k--) {
            Cond *less = cnode(C_REL);
            less->op = tb->keydesc[k] ? REL_GT : REL_LT;
            less->l = eqs[k]->l; less->r = eqs[k]->r;
            if (!before) before = less;
            else {
                Cond *eq = cnode(C_REL); eq->op = REL_EQ; eq->l = eqs[k]->l; eq->r = eqs[k]->r;
                Cond *and_ = cnode(C_AND); and_->cl = eq; and_->cr = before;
                Cond *or_ = cnode(C_OR); or_->cl = less; or_->cr = and_;
                before = or_;
            }
        }
        st->cond2 = before;
        new_stmt(ST_LABEL)->dst = st->lab2;
        parse_stmt_list(1);
        new_stmt(ST_LABEL)->dst = st->lab3;
        eat_period();
        return;
    }

    if (is("CANCEL")) {
        /* CANCEL identifier / 'literal' (XII-7): the program is released, and
         * the next CALL of it finds it in its initial state. A program not
         * loaded is left alone. */
        next();
        while (!tok.eof && !is(".") && !starts_statement()) {
            Stmt *st = new_stmt(ST_CANCEL);
            st->src = -1;
            if (tok.literal) {
                if (tok.len < 1 || tok.len > 8) die("a program name is 1 to 8 characters");
                memcpy(st->para, tok.text, (size_t)tok.len + 1);
                next();
            } else {
                st->src = consume_sym();
                if (!syms[st->src].is_alpha || syms[st->src].bytes > 8)
                    die("CANCEL identifier needs an alphanumeric item of at most 8 characters");
                st->para[0] = 0;
            }
        }
        eat_period();
        return;
    }

    if (is("CALL")) {
        /* ANS COBOL has only CALL 'literal', which the linkage editor resolves
         * -- there is no CALL identifier and so nothing to decide at compile
         * time. Dynamic loading is what the corpus's DYNALOAD routine provides
         * at run time, and DYNALOAD is itself reached by an ordinary static
         * call like any other subroutine. */
        next();
        Stmt *st = new_stmt(ST_CALL);
        st->src = -1;
        if (tok.literal) {
            if (tok.len < 1 || tok.len > 8)
                die("a called program name is 1 to 8 characters");
            memcpy(st->para, tok.text, (size_t)tok.len + 1);
            next();
        } else {
            /* CALL identifier, 2 IPC 0,2: the name is read at run time and the
             * program loaded by name -- what the DYNALOAD idiom did by hand.
             * IBM's ANS COBOL had no such thing; the standard does. */
            st->src = consume_sym();
            if (!syms[st->src].is_alpha || syms[st->src].bytes > 8)
                die("CALL identifier needs an alphanumeric item of at most 8 characters");
            st->para[0] = 0;
        }
        if (is("USING")) {
            next();
            while (!tok.eof && !is(".") && !starts_statement()) {
                if (st->ndop >= 8) die("too many CALL arguments");
                st->dop[st->ndop].sym = consume_sym();
                st->dop[st->ndop].litlen = 0;
                st->ndop++;
            }
        }
        eat_period();
        return;
    }

    if (is("ACCEPT")) {
        /* Format 1 at level 1: one transfer from the implementor's device,
         * which here is SYSIN. The FROM phrase -- both the mnemonic-name and
         * DATE/DAY/TIME -- is level 2. */
        next();
        Stmt *st = new_stmt(ST_ACCEPT);
        st->dst = consume_sym();
        st->dsub = opt_subscript();
        if (is("FROM")) {
            /* Level 2: a mnemonic-name, or DATE (YYMMDD), DAY (YYDDD) or
             * TIME (HHMMSSth), each moved to the item under the MOVE rules. */
            next();
            if (is("DATE")) st->acc_from = 1;
            else if (is("DAY")) st->acc_from = 2;
            else if (is("TIME")) st->acc_from = 3;
            else {
                const char *dev = mnem_dev(tok.text);
                if (!dev) die("ACCEPT FROM needs DATE, DAY, TIME or a mnemonic-name from SPECIAL-NAMES");
                if (!strcmp(dev, "CONSOLE")) st->acc_from = 4;
                else if (strcmp(dev, "SYSIN") && strcmp(dev, "SYSIPT"))
                    die("ACCEPT FROM: the mnemonic must stand for SYSIN, SYSIPT or CONSOLE");
            }
            next();
        }
        eat_period();
        return;
    }

    if (is("ALTER")) {
        /* ALTER para-1 TO [PROCEED TO] para-2, ... II-57. Syntax rule 1: the
         * altered paragraph holds a single sentence that is a GO TO without
         * DEPENDING, which is what lets the branch be compiled indirect. */
        next();
        do {
            Stmt *st = new_stmt(ST_ALTER);
            snprintf(st->para, sizeof st->para, "%s", tok.text);
            next();
            expect("TO");
            if (is("PROCEED")) { next(); expect("TO"); }
            snprintf(st->thru, sizeof st->thru, "%s", tok.text);
            next();
        } while (!tok.eof && !is(".") && !starts_statement());
        eat_period();
        return;
    }

    if (is("INSPECT")) {
        /* INSPECT id TALLYING ... [REPLACING ...] / INSPECT id REPLACING ...
         * At level 1 the compared and replacing operands are single character
         * items, which is what makes every clause a byte test. */
        next();
        Stmt *st = new_stmt(ST_INSPECT);
        st->dst = consume_sym();
        st->dsub = opt_subscript();
        if (syms[st->dst].usage != U_DISPLAY)
            die("INSPECT needs a USAGE DISPLAY item");
        st->ins_first = ninsop;
        int any = 0;
        while (is("TALLYING") || is("REPLACING")) {
            int repl = is("REPLACING");
            next();
            while (!tok.eof && !is(".") && !is("TALLYING") && !is("REPLACING")
                   && !starts_statement()) {
                if (ninsop >= MAXINSOP) die("too many INSPECT clauses");
                InsOp *o = &insops[ninsop];
                memset(o, 0, sizeof *o);
                o->c_sym = o->by_sym = o->bf_sym = -1;
                int tally = -1;
                /* After ALL, LEADING or FIRST a series of operands may follow
                 * under the same word -- REPLACING ALL 'A' BY 'B' 'C' BY 'D',
                 * TALLYING C FOR ALL 'A' 'B' -- so a bare operand here takes
                 * the kind (and counter) of the operation before it. For
                 * TALLYING an identifier followed by FOR is a new counter and
                 * anything else is such an operand. */
                int cont = 0, taken = -1;
                if (!repl) {
                    if (tok.literal || fig_code(tok.text) != FIG_NONE) cont = 1;
                    else {
                        taken = consume_sym();
                        if (is("FOR")) {
                            next();
                            tally = taken; taken = -1;
                            if (syms[tally].scale || syms[tally].is_alpha || syms[tally].is_group)
                                die("the TALLYING counter must be an elementary integer");
                        } else cont = 2;         /* an operand of the series */
                    }
                } else if (!is("CHARACTERS") && !is("ALL") && !is("LEADING") && !is("FIRST"))
                    cont = 1;
                if (cont) {
                    if (ninsop == st->ins_first || insops[ninsop-1].kind == INS_T_CHARS
                        || insops[ninsop-1].kind == INS_R_CHARS)
                        die("INSPECT wants ALL, LEADING, CHARACTERS or -- when replacing -- FIRST");
                    o->kind = insops[ninsop-1].kind;
                    if (!repl) tally = insops[ninsop-1].tally;
                    if (cont == 2) {
                        if (!(syms[taken].is_alpha || syms[taken].is_group) && syms[taken].usage != U_DISPLAY)
                            die("an INSPECT operand must be a DISPLAY item");
                        o->c_sym = taken; o->c_len = syms[taken].bytes;
                    } else ins_operand(&o->c_sym, o->c_lab, &o->c_len);
                } else if (is("CHARACTERS")) {
                    next();
                    o->kind = repl ? INS_R_CHARS : INS_T_CHARS;
                } else {
                    if (is("ALL"))          { next(); o->kind = repl ? INS_R_ALL   : INS_T_ALL; }
                    else if (is("LEADING")) { next(); o->kind = repl ? INS_R_LEAD  : INS_T_LEAD; }
                    else if (is("FIRST") && repl) { next(); o->kind = INS_R_FIRST; }
                    else die("INSPECT wants ALL, LEADING, CHARACTERS"
                             " or -- when replacing -- FIRST");
                    ins_operand(&o->c_sym, o->c_lab, &o->c_len);
                }
                if (repl) {
                    expect("BY");
                    ins_operand(&o->by_sym, o->by_lab, &o->by_len);
                    if (o->kind == INS_R_CHARS) {
                        if (o->by_len != 1) die("CHARACTERS BY takes a single character");
                    } else if (o->by_len != o->c_len)
                        die("the replacing string must be the same length as the one it replaces -- II-70");
                }
                if (is("BEFORE") || is("AFTER")) {
                    o->bf_after = is("AFTER");
                    next();
                    if (is("INITIAL")) next();
                    ins_operand(&o->bf_sym, o->bf_lab, &o->bf_len);
                }
                o->tally = tally;
                ninsop++; any = 1;
            }
        }
        if (!any) die("INSPECT needs a TALLYING or REPLACING phrase");
        st->ins_n = ninsop - st->ins_first;
        eat_period();
        return;
    }

    if (is("ENTER")) {
        /* ENTER language-name [routine-name]. II-63 -- it exists to let a
         * program change language mid-stream, and there is no other language
         * here to change to, so it is accepted and does nothing. That is what
         * "full capabilities for the ENTER statement" amounts to when the
         * implementor offers one language. */
        next();
        while (!tok.eof && !is(".")) next();
        eat_period();
        return;
    }

    if (is("GOBACK")) {                 /* what the corpus uses, not STOP RUN */
        next();
        new_stmt(ST_STOP);
        eat_period();
        return;
    }

    if (is("STOP")) {
        next();
        if (!is("RUN")) die("STOP literal is not implemented yet");
        next();
        new_stmt(ST_STOP);
        eat_period();
        return;
    }

    if (is("EXIT")) {
        next();
        if (is("PROGRAM")) {
            /* EXIT PROGRAM (XII-6): in a called program, return to the
             * caller; in a main program, nothing at all. Which one this is
             * cannot be known here beyond a PROCEDURE DIVISION USING, so a
             * called program without parameters gets the no-op -- and
             * GOBACK, which always returns, is the way out for it. */
            next();
            new_stmt(ST_EXITPGM);
            eat_period();
            return;
        }
        new_stmt(ST_EXIT);
        eat_period();
        return;
    }

    if (tok.text[0] && !tok.literal) {
        char nm[31];
        snprintf(nm, sizeof nm, "%s", tok.text);
        next();
        int a_section = 0;
        if (is("SECTION")) {
            next();
            /* A segment-number is accepted and ignored. Segmentation has a null
             * level in the standard, and the only thing a program can observe
             * of it -- an independent segment back in its initial state -- is
             * carried by ALTER, which this compiler does not implement. With
             * every section resident and no altered GO TO to reset, the number
             * says nothing about what the program does. */
            if (!is(".")) {
                if (!is_numeric_literal(tok.text))
                    die("a SECTION header takes only an optional segment-number");
                next();
            }
            a_section = 1;
        }
        if (is(".")) {
            next(); at_period = 1;
            if (para_index(nm) >= 0)
                die(a_section ? "duplicate section name" : "duplicate paragraph name");
            if (npara >= MAXPARA) die("too many paragraphs");
            snprintf(paras[npara].name, sizeof paras[npara].name, "%s", nm);
            paras[npara].is_range_end = 0;
            paras[npara].is_section = a_section;
            Stmt *st = new_stmt(ST_PARA);
            snprintf(st->para, sizeof st->para, "%s", nm);
            st->dst = npara++;
            return;
        }
        char m[160];
        snprintf(m, sizeof m,
                 "not implemented yet: '%s'. This slice supports MOVE, ADD, "
                 "SUBTRACT, COMPUTE, IF, DISPLAY, PERFORM, EXIT and STOP RUN.", nm);
        die(m);
    }
    die("unexpected token in PROCEDURE DIVISION");
}

static void parse_stmt_list(int allow_else)
{
    while (!tok.eof && !at_period) {
        if (allow_else && is("ELSE")) return;
        /* WHEN ends a SEARCH's AT END clause. Nothing else begins with it, so
         * stopping here unconditionally is safe. */
        if (is("WHEN")) return;
        parse_one_statement();
    }
}

static void parse_procedure(void)
{
    lex_parens = 1;
    expect("PROCEDURE"); expect("DIVISION");
    if (is("USING")) {
        next();
        is_subprogram = 1;
        while (!is(".")) {
            if (nusing >= MAXLINK) die("too many USING parameters");
            int i = consume_sym();
            if (!syms[i].linkage)
                die("PROCEDURE DIVISION USING names an item that is not in the "
                    "LINKAGE SECTION");
            if (syms[i].level != 1)
                die("PROCEDURE DIVISION USING needs an 01-level item");
            using_parm[nusing++] = i;
        }
    }
    expect(".");
    if (is("DECLARATIVES")) {
        next(); expect(".");
        while (!tok.eof && !is("END")) {
            /* Each declarative is a section whose header is followed at once
             * by USE -- syntax rule 1 -- so USE has to be recognised before
             * the statement parser sees it. */
            if (!is("USE")) { at_period = 0; parse_stmt_list(0); continue; }
            next();
            if (is("AFTER")) next();
            if (is("STANDARD")) next();
            if (is("EXCEPTION") || is("ERROR")) next();
            else die("USE ... EXCEPTION or ERROR PROCEDURE is the only form "
                     "this compiler implements");
            if (is("PROCEDURE")) next();
            if (is("ON")) next();
            if (ndecl >= MAXDECL) die("too many declarative sections");
            Decl *d = &decls[ndecl];
            memset(d, 0, sizeof *d);
            if (npara == 0 || !paras[npara-1].is_section)
                die("USE must follow a section header in the declaratives");
            d->sect = npara - 1;
            while (!tok.eof && !is(".")) {
                if      (is("INPUT"))  { d->mode = 1; next(); }
                else if (is("OUTPUT")) { d->mode = 2; next(); }
                else if (is("I-O"))    { d->mode = 3; next(); }
                else if (is("EXTEND")) { d->mode = 4; next(); }
                else {
                    int fi = file_index(tok.text);
                    if (fi < 0) die("USE ... ON names something that is not a file");
                    if (d->nfiles >= 8) die("too many files in one USE");
                    d->file[d->nfiles++] = fi;
                    next();
                }
            }
            expect(".");
            ndecl++;
        }
        expect("END"); expect("DECLARATIVES"); expect(".");
        decl_end_para = npara;
    }
    int stopped = 0;
    while (!tok.eof) {
        at_period = 0;
        parse_stmt_list(0);
    }
    for (int i = 0; i < nstmt; i++) if (stmts[i].op == ST_STOP) stopped = 1;
    if (!stopped) die("PROCEDURE DIVISION has no STOP RUN");
    for (int i = 0; i < nstmt; i++) {
        if (stmts[i].op == ST_ALTER) {
            int a = para_index(stmts[i].para), b = para_index(stmts[i].thru);
            if (a < 0) { char m[96]; snprintf(m, sizeof m, "ALTER names an unknown paragraph '%s'", stmts[i].para); die(m); }
            if (b < 0) { char m[96]; snprintf(m, sizeof m, "ALTER ... TO names an unknown procedure '%s'", stmts[i].thru); die(m); }
            paras[a].altered = 1;
            stmts[i].dst = a; stmts[i].src = b;
            continue;
        }
        if (stmts[i].op == ST_GOTO) {
            if (!stmts[i].para[0]) { stmts[i].dst = -1; continue; }   /* bare: an ALTER supplies it */
            int a = para_index(stmts[i].para);
            if (a < 0) { char m[96]; snprintf(m, sizeof m, "GO TO names an unknown paragraph '%s'", stmts[i].para); die(m); }
            stmts[i].dst = a;
            continue;
        }
        if (stmts[i].op == ST_GODEP) {
            for (int k = 0; k < stmts[i].ndop; k++) {
                int a = para_index(stmts[i].dop[k].lit);
                if (a < 0) { char m[96]; snprintf(m, sizeof m, "GO TO ... DEPENDING names an unknown paragraph '%s'", stmts[i].dop[k].lit); die(m); }
                stmts[i].dop[k].sym = a;
            }
            continue;
        }
        if (stmts[i].op != ST_PERFORM) continue;
        int a = para_index(stmts[i].para), b = para_index(stmts[i].thru);
        if (a < 0) { char m[96]; snprintf(m, sizeof m, "PERFORM names an unknown paragraph '%s'", stmts[i].para); die(m); }
        if (b < 0) { char m[96]; snprintf(m, sizeof m, "PERFORM THRU names an unknown paragraph '%s'", stmts[i].thru); die(m); }
        if (b < a) die("PERFORM THRU runs backwards");
        /* Naming a section means all of it. Without THRU the range end is the
         * same name, so this one line covers PERFORM SECT and
         * PERFORM PARA THRU SECT alike. */
        if (paras[b].is_section) b = section_end(b);
        stmts[i].dst = a; stmts[i].src = b;
        paras[b].is_range_end = 1;
    }
    /* Syntax rule 1 on II-57: an altered paragraph holds a single sentence that
     * is a GO TO without DEPENDING. Finding that GO TO is also how its original
     * target is learnt, which is what the branch cell starts out holding. */
    for (int a = 0; a < npara; a++) {
        if (!paras[a].altered) continue;
        int found = -1;
        for (int i = 0; i < nstmt; i++) {
            if (stmts[i].op != ST_PARA || stmts[i].dst != a) continue;
            for (int j = i + 1; j < nstmt; j++) {
                if (stmts[j].op == ST_LABEL) continue;   /* not a sentence */
                if (stmts[j].op == ST_GOTO) {
                    /* A bare GO TO has no target of its own: the cell starts
                     * out zero and the first ALTER fills it. */
                    found = stmts[j].para[0] ? stmts[j].dst : -2;
                } else found = -1;
                break;
            }
            break;
        }
        if (found == -2) { paras[a].alter_to = -1; continue; }
        if (found < 0) {
            char m[128];
            snprintf(m, sizeof m, "ALTER names '%s', which must hold a single "
                     "sentence that is a GO TO -- syntax rule 1 on II-57",
                     paras[a].name);
            die(m);
        }
        paras[a].alter_to = found;
    }

    /* A bare GO TO is legal only inside a paragraph some ALTER names. */
    {
        int cur = -1;
        for (int i = 0; i < nstmt; i++) {
            if (stmts[i].op == ST_PARA) cur = stmts[i].dst;
            if (stmts[i].op == ST_GOTO && !stmts[i].para[0] && (cur < 0 || !paras[cur].altered))
                die("GO TO without a procedure-name is allowed only in a paragraph that an ALTER names -- II-65");
        }
    }

    /* A USE procedure is entered and returned from exactly as a PERFORM range
     * is -- general rule 2 on IV-32 -- so its last paragraph needs the same
     * return through the range's exit cell. */
    for (int d = 0; d < ndecl; d++)
        paras[section_end(decls[d].sect)].is_range_end = 1;
}

/* Which declarative section covers this file, or -1. A file named outright
 * wins over one matched by open mode. */
static int decl_for(int fi)
{
    for (int d = 0; d < ndecl; d++)
        for (int k = 0; k < decls[d].nfiles; k++)
            if (decls[d].file[k] == fi) return d;
    for (int d = 0; d < ndecl; d++) {
        if (decls[d].nfiles || !decls[d].mode) continue;
        const File *f = &files[fi];
        if ((decls[d].mode == 1 && f->opened_input)  ||
            (decls[d].mode == 2 && f->opened_output) ||
            (decls[d].mode == 3 && f->opened_io)     ||
            (decls[d].mode == 4 && f->opened_extend)) return d;
    }
    return -1;
}

static int section_end(int i)
{
    int j = i;
    for (int k = i + 1; k < npara; k++) {
        if (paras[k].is_section) break;
        j = k;
    }
    return j;
}

/* ---- base locator cells -------------------------------------------------
 * WORKING-STORAGE and the file records live in their own CSECT, COBWS, cut
 * into 4096-byte chunks. Each chunk has a BL cell in the program CSECT
 * holding its address; a data base register is loaded from the cell and a
 * USING makes the chunk's symbols resolvable. This is the same idea the DMAP
 * showed IKFCBL00 using with its BL=1 cells, and it is what decouples the
 * size of WORKING-STORAGE from code addressability.
 *
 * A field may straddle a chunk boundary: only its first byte has to be within
 * 4095 of the base.
 *
 * The USING state has to match what the registers actually hold at run time,
 * so it is dropped at every label and after every PERFORM -- anywhere control
 * can arrive from somewhere that left different chunks loaded.
 */
#define CHUNK 4096
/* Two data bases, not three. R10 became a third CODE base when GL042's program
 * CSECT reached 9272 bytes and overran the 8192 that two code bases cover --
 * SAVEAREA sat past the end and every reference to it failed IFO209. No work
 * register was free to take the job, so the cheapest correct trade was to give
 * one back from the data side: a program with more than 8K of WORKING-STORAGE
 * now reloads its data bases more often, which costs instructions, not
 * correctness. */
#define NBASE 2
static const int base_reg[NBASE] = { 8, 9 };
static int base_chunk[NBASE] = { -9999, -9999 };
static int base_next;

static void reset_bases(void)
{
    char b[64]; int j = 0, any = 0;
    b[0] = 0;
    for (int i = 0; i < NBASE; i++) {
        if (base_chunk[i] == -9999) continue;
        j += snprintf(b + j, sizeof b - j, "%s%d", any ? "," : "", base_reg[i]);
        any = 1;
        base_chunk[i] = -9999;
    }
    if (any) asm_line("", "DROP", b, "");
    base_next = 0;
}

/* Areas are numbered: 0.. are WORKING-STORAGE chunks, and -(n+1) is LINKAGE
 * area n, whose base cell holds an address the caller supplied rather than one
 * of our own. Same mechanism either way -- load a cell, USING, DROP on reset. */
#define LINK_AREA(n) (-((n) + 1))

static void need_base(int area)
{
    char b[64];
    for (int i = 0; i < NBASE; i++) if (base_chunk[i] == area) return;
    int slot = base_next % NBASE;
    base_next++;
    if (base_chunk[slot] != -9999) {
        snprintf(b, sizeof b, "%d", base_reg[slot]);
        asm_line("", "DROP", b, "");
    }
    if (area < 0) {
        int a = -area - 1;
        snprintf(b, sizeof b, "%d,PBL%04d", base_reg[slot], a);
        asm_line("", "L", b, "parameter address");
        snprintf(b, sizeof b, "LS%04d,%d", a, base_reg[slot]);
        asm_line("", "USING", b, "");
    } else {
        snprintf(b, sizeof b, "%d,BL%04d", base_reg[slot], area);
        asm_line("", "L", b, "base locator");
        snprintf(b, sizeof b, "WSC%04d,%d", area, base_reg[slot]);
        asm_line("", "USING", b, "");
    }
    base_chunk[slot] = area;
}

static void need_sym_base(const Sym *sy)
{
    if (sy->linkage) need_base(LINK_AREA(sy->link_area));
    else             need_base(sy->offset / CHUNK);
}

/* ED patterns, emitted as hex constants. */
static struct { char label[16]; unsigned char b[PIC_MAXMASK]; int len; } mconsts[64];
static int nmconst;

static const char *intern_mask(const unsigned char *pat, int len)
{
    for (int i = 0; i < nmconst; i++)
        if (mconsts[i].len == len && !memcmp(mconsts[i].b, pat, (size_t)len))
            return mconsts[i].label;
    if (nmconst >= 64) die("too many ED patterns");
    snprintf(mconsts[nmconst].label, sizeof mconsts[nmconst].label, "M%04d", nmconst + 1);
    memcpy(mconsts[nmconst].b, pat, (size_t)len);
    mconsts[nmconst].len = len;
    return mconsts[nmconst++].label;
}

/* Halfword constants, for the element-size multiply. */
static struct { char label[16]; int v; } hconsts[64];
static int nhconst;

static const char *intern_half(int v)
{
    for (int i = 0; i < nhconst; i++) if (hconsts[i].v == v) return hconsts[i].label;
    if (nhconst >= 64) die("too many halfword constants");
    snprintf(hconsts[nhconst].label, sizeof hconsts[nhconst].label, "H%04d", nhconst + 1);
    hconsts[nhconst].v = v;
    return hconsts[nhconst++].label;
}

/* Leave (subscript - 1) in reg, as a binary integer. */
static void gen_subscript(Node *sub, int reg)
{
    char b[96];
    if (sub->sub) die("a subscript may not itself be subscripted");
    if (sub->kind == N_LIT) {
        long v = atol(sub->lit);
        if (v < 1) die("a subscript literal must be 1 or more");
        snprintf(b, sizeof b, "%d,%ld", reg, v - 1);
        asm_line("", "LA", b, "subscript-1");
        return;
    }
    if (sub->kind != N_SYM) die("a subscript must be a literal or a data name");
    const Sym *sy = &syms[sub->sym];
    if (sy->scale != 0 || sy->is_alpha || sy->is_group)
        die("a subscript must be an integer data item");
    need_sym_base(sy);
    switch (sy->usage) {
    case U_COMP:
        snprintf(b, sizeof b, "%d,%s", reg, sy->label);
        asm_line("", sy->bytes == 2 ? "LH" : "L", b, "subscript");
        break;
    case U_DISPLAY:
        snprintf(b, sizeof b, "DWK(8),%s(%d)", sy->label, sy->bytes);
        asm_line("", "PACK", b, "subscript");
        snprintf(b, sizeof b, "%d,DWK", reg);
        asm_line("", "CVB", b, "");
        break;
    default:
        snprintf(b, sizeof b, "DWK(8),%s(%d)", sy->label, sy->bytes);
        asm_line("", "ZAP", b, "subscript");
        snprintf(b, sizeof b, "%d,DWK", reg);
        asm_line("", "CVB", b, "");
        break;
    }
    snprintf(b, sizeof b, "%d,0", reg);
    asm_line("", "BCTR", b, "subscript-1");
}

/* Operand text for a field.
 *
 * The form depends on the instruction, which is the part that bites:
 *   FR_SS_LEN    SS operand carrying a length -- both operands of ZAP, PACK,
 *                UNPK, AP, CP; the FIRST operand of MVC and CLC
 *   FR_SS_NOLEN  the SECOND operand of MVC and CLC. SS-a format has one
 *                length only, and writing D(5) there means "base register 5",
 *                not "length 5" -- which assembles cleanly and then branches
 *                into hyperspace.
 *   FR_RX        RX operand: L, LH, ST, STH
 *
 * A subscripted reference cannot use an index register, because SS format has
 * none, so the element address is computed into reg instead. */
enum { FR_SS_LEN, FR_SS_NOLEN, FR_RX };

static void field_ref_m(const Sym *sy, Node *sub, int mode, int len, int reg,
                        char *out, size_t outn)
{
    char b[96];
    if (!sub) {
        need_sym_base(sy);
        if (mode == FR_SS_LEN) snprintf(out, outn, "%s(%d)", sy->label, len);
        else                   snprintf(out, outn, "%s", sy->label);
        return;
    }
    if (sy->occ_depth == 0)
        die("subscript on an item that is not inside an OCCURS table");
    int given = 0;
    for (Node *q = sub; q; q = q->next) given++;
    if (given != sy->occ_depth) {
        char m[128];
        snprintf(m, sizeof m, "%s needs %d subscript%s, not %d",
                 sy->name, sy->occ_depth, sy->occ_depth == 1 ? "" : "s", given);
        die(m);
    }
    /* address = label + sum over levels of (subscript - 1) * element size,
     * outermost level first. One term goes straight into reg; the rest are
     * built in R0 and added, which is free because nothing else is live
     * between the two instructions. */
    int lvl = 0;
    for (Node *q = sub; q; q = q->next, lvl++) {
        int r = (lvl == 0) ? reg : 0;
        gen_subscript(q, r);
        int elem = syms[sy->occ_chain[lvl]].elem;
        if (elem != 1) {
            snprintf(b, sizeof b, "%d,%s", r, intern_half(elem));
            asm_line("", "MH", b, "times element size");
        }
        if (lvl > 0) {
            snprintf(b, sizeof b, "%d,0", reg);
            asm_line("", "AR", b, "add this dimension");
        }
    }
    need_sym_base(sy);
    snprintf(b, sizeof b, "%d,%s(%d)", reg, sy->label, reg);
    asm_line("", "LA", b, "element address");
    switch (mode) {
    case FR_SS_LEN:   snprintf(out, outn, "0(%d,%d)", len, reg); break;
    case FR_SS_NOLEN: snprintf(out, outn, "0(%d)", reg);         break;
    default:          snprintf(out, outn, "0(,%d)", reg);        break;
    }
}

static void field_ref(const Sym *sy, Node *sub, int len, int reg, char *out, size_t outn)
{
    field_ref_m(sy, sub, len ? FR_SS_LEN : FR_RX, len, reg, out, outn);
}

/* ---- arithmetic ---------------------------------------------------------
 * Everything is computed in packed decimal, which is what COBOL semantics
 * want and what S/370 does in hardware. Binary (COMP) operands are converted
 * in and out with CVD/CVB; zoned (DISPLAY) with PACK/UNPK. Scale alignment is
 * SRP, whose shift count is the low six bits of the second operand address
 * read as a signed value, so a right shift of k is encoded as 64-k.
 */

/* Labels generated during code emission, past the ones the parser handed out. */
static int genlabel;

/* Translate-and-test tables for the class conditions, emitted only when used. */
enum { CLS_DIGIT, CLS_ALPHA, CLS_SIGN, CLS_N };
static int class_used[CLS_N];
static void need_class_table(int which) { class_used[which] = 1; }
static void gen_load(const Sym *sy, Node *sub, const char *wk);
static void gen_store(const Sym *sy, Node *sub, const char *wk);

/* VSAM addresses an RRDS by a fullword record number. A RELATIVE KEY that is
 * already one is used in place; any other elementary integer -- which is all
 * the standard asks for -- is converted through the file's own cell before a
 * request that reads the number, and back after one that sets it. */
static void rrn_cell_sym(const File *f, Sym *out)
{
    memset(out, 0, sizeof *out);
    out->usage = U_COMP; out->digits = 9; out->bytes = out->elem = 4;
    out->occ_parent = out->gparent = out->index_sym = out->askey_sym = -1;
    out->fd_file = out->redef_from = out->redef_cap = -1;
    snprintf(out->label, sizeof out->label, "%sK", f->label);
    snprintf(out->name, sizeof out->name, "RRN");
}
static void gen_rrn_to_cell(const File *f)
{
    if (!f->rrn_via_cell || f->key_sym < 0) return;
    Sym cell; rrn_cell_sym(f, &cell);
    asm_comment("  the RELATIVE KEY into VSAM's search argument");
    gen_load(&syms[f->key_sym], NULL, "PWK1");
    gen_store(&cell, NULL, "PWK1");
}
static void gen_rrn_from_cell(const File *f)
{
    if (!f->rrn_via_cell || f->key_sym < 0) return;
    Sym cell; rrn_cell_sym(f, &cell);
    asm_comment("  and the number VSAM used back into the RELATIVE KEY");
    gen_load(&cell, NULL, "PWK1");
    gen_store(&syms[f->key_sym], NULL, "PWK1");
}
static const char *intern_const(const char *digits);

/* A zoned item whose sign is not a trailing overpunch is copied to ZWK first
 * and taken apart there, so the same code serves a subscripted reference. */
static void gen_sign_load(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    int n = sy->digits;
    /* MVC carries one length, on its first operand -- so the source must come
     * back without one. */
    field_ref_m(sy, sub, FR_SS_NOLEN, sy->elem, 7, f, sizeof f);
    snprintf(b, sizeof b, "ZWK(%d),%s", sy->elem, f);
    asm_line("", "MVC", b, "the item, sign and all");
    if (!sy->sgn_sep) {
        /* Leading overpunch: give the trailing digit the leading zone, make
         * the leading digit plain, and PACK as usual. MVZ moves zones only. */
        snprintf(b, sizeof b, "ZWK+%d(1),ZWK", n - 1);
        asm_line("", "MVZ", b, "the sign moves to where PACK looks for it");
        asm_line("", "OI", "ZWK,X'F0'", "the leading digit is a digit again");
        snprintf(b, sizeof b, "%s(16),ZWK(%d)", wk, n);
        asm_line("", "PACK", b, "zoned -> packed");
        return;
    }
    /* SEPARATE: the digits pack on their own and the sign character decides. */
    snprintf(b, sizeof b, "%s(16),ZWK+%d(%d)", wk, sy->sgn_lead ? 1 : 0, n);
    asm_line("", "PACK", b, "the digits, without the sign character");
    snprintf(b, sizeof b, "ZWK+%d,C'-'", sy->sgn_lead ? 0 : n);
    asm_line("", "CLI", b, "negative?");
    int lab = ++genlabel;
    char l[16]; snprintf(l, sizeof l, "L%04d", lab);
    asm_line("", "BNE", l, "");
    snprintf(b, sizeof b, "%s+15,X'F0'", wk);
    asm_line("", "NI", b, "clear the sign nibble");
    snprintf(b, sizeof b, "%s+15,X'0D'", wk);
    asm_line("", "OI", b, "and make it negative");
    asm_line(l, "DS", "0H", "");
}

/* The reverse: build the zoned form in ZWK, sign where the clause asks for it,
 * then move the whole item into place. */
static void gen_sign_store(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    int n = sy->digits;
    if (!sy->sgn_sep) {
        snprintf(b, sizeof b, "ZWK(%d),%s(16)", n, wk);
        asm_line("", "UNPK", b, "packed -> zoned");
        snprintf(b, sizeof b, "ZWK(1),ZWK+%d", n - 1);
        asm_line("", "MVZ", b, "the sign moves to the leading digit");
        snprintf(b, sizeof b, "ZWK+%d,X'F0'", n - 1);
        asm_line("", "OI", b, "the trailing digit is a digit again");
    } else {
        snprintf(b, sizeof b, "ZWK+%d(%d),%s(16)", sy->sgn_lead ? 1 : 0, n, wk);
        asm_line("", "UNPK", b, "packed -> zoned");
        snprintf(b, sizeof b, "ZWK+%d,X'F0'", (sy->sgn_lead ? 1 : 0) + n - 1);
        asm_line("", "OI", b, "no overpunch: the sign has its own position");
        snprintf(b, sizeof b, "%s(16),%s(16)", wk, intern_const("0"));
        asm_line("", "CP", b, "negative?");
        int lneg = ++genlabel, ldone = ++genlabel;
        char ln[16], ld[16];
        snprintf(ln, sizeof ln, "L%04d", lneg);
        snprintf(ld, sizeof ld, "L%04d", ldone);
        asm_line("", "BM", ln, "");
        snprintf(b, sizeof b, "ZWK+%d,C'+'", sy->sgn_lead ? 0 : n);
        asm_line("", "MVI", b, "");
        asm_line("", "B", ld, "");
        asm_line(ln, "DS", "0H", "");
        snprintf(b, sizeof b, "ZWK+%d,C'-'", sy->sgn_lead ? 0 : n);
        asm_line("", "MVI", b, "");
        asm_line(ld, "DS", "0H", "");
    }
    field_ref(sy, sub, sy->elem, 6, f, sizeof f);
    snprintf(b, sizeof b, "%s,ZWK", f);
    asm_line("", "MVC", b, "the item, sign and all");
    (void)n;
}

/* PACK and UNPK hold each operand length in four bits, so 16 bytes is the
 * widest zoned field either can convert -- and the standard's ceiling is 18
 * digits. The extra one or two leading digits are converted separately and
 * moved into the packed field's free nibbles.
 *
 * With n digits and k = n - 16 left over, PACK of the low 16 digits lands
 * 17 nibbles right-justified in the 10-byte area, so nibbles 0, 1 and 2 are
 * free; the leading digits belong at nibbles 3-k .. 2. MVZ moves a high
 * nibble on its own, which is what makes the second one land without
 * disturbing the digit beside it. */
static void gen_wide_load(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    int k = sy->digits - 16;
    field_ref_m(sy, sub, FR_SS_NOLEN, sy->elem, 7, f, sizeof f);
    snprintf(b, sizeof b, "ZWK(%d),%s", sy->elem, f);
    asm_line("", "MVC", b, "the whole zoned item");
    /* The packed value has to end where a 16-byte field's sign lives, so the
     * 10 bytes go at the RIGHT of the work area and the six in front are
     * cleared. Within those ten, PACK leaves nibbles 0, 1 and 2 free. */
    snprintf(b, sizeof b, "%s(6),%s", wk, wk);
    asm_line("", "XC", b, "the high half of the work area is zero");
    snprintf(b, sizeof b, "%s+6(10),ZWK+%d(16)", wk, k);
    asm_line("", "PACK", b, "the low 16 digits, with the sign");
    snprintf(b, sizeof b, "DWK(2),ZWK(%d)", k);
    asm_line("", "PACK", b, "and the leading digits on their own");
    if (k == 2) {
        snprintf(b, sizeof b, "%s+6(1),DWK", wk);
        asm_line("", "MVC", b, "digit 1 into the free byte");
    }
    snprintf(b, sizeof b, "%s+7(1),DWK+1", wk);
    asm_line("", "MVZ", b, "and the next one into the free nibble");
}

/* The reverse. UNPK of the low 16 digits fills all but the leading ones;
 * those come out of the packed field's top byte separately. */
static void gen_wide_store(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    int k = sy->digits - 16;
    snprintf(b, sizeof b, "ZWK+%d(16),%s+7(9)", k, wk);
    asm_line("", "UNPK", b, "the low 16 digits, sign and all");
    snprintf(b, sizeof b, "DWK(3),%s+6(2)", wk);
    asm_line("", "UNPK", b, "the leading digits");
    snprintf(b, sizeof b, "ZWK(%d),DWK+%d", k, 3 - k);
    asm_line("", "MVC", b, "into the front of the item");
    asm_line("", "OI", "ZWK,X'F0'", "as plain digits");
    if (k == 2) asm_line("", "OI", "ZWK+1,X'F0'", "");
    field_ref_m(sy, sub, FR_SS_NOLEN, sy->elem, 6, f, sizeof f);
    snprintf(b, sizeof b, "%s(%d),ZWK", f, sy->elem);
    asm_line("", "MVC", b, "the whole zoned item");
    if (!sy->is_signed) {
        snprintf(b, sizeof b, "%s+%d,X'F0'", f, sy->elem - 1);
        asm_line("", "OI", b, "unsigned: force an F zone");
    }
}

static void gen_load(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    switch (sy->usage) {
    case U_DISPLAY:
        if (sy->digits > 16) { gen_wide_load(sy, sub, wk); break; }
        if (sy->sgn_lead || sy->sgn_sep) { gen_sign_load(sy, sub, wk); break; }
        field_ref(sy, sub, sy->elem, 7, f, sizeof f);
        snprintf(b, sizeof b, "%s(16),%s", wk, f);
        asm_line("", "PACK", b, "zoned -> packed");
        break;
    case U_COMP3:
        field_ref(sy, sub, sy->elem, 7, f, sizeof f);
        snprintf(b, sizeof b, "%s(16),%s", wk, f);
        asm_line("", "ZAP", b, "");
        break;
    case U_COMP:
        field_ref(sy, sub, 0, 7, f, sizeof f);
        snprintf(b, sizeof b, "2,%s", f);
        asm_line("", sy->elem == 2 ? "LH" : "L", b, "");
        asm_line("", "CVD", "2,DWK", "binary -> packed");
        snprintf(b, sizeof b, "%s(16),DWK(8)", wk);
        asm_line("", "ZAP", b, "");
        break;
    }
}

static void gen_load_imm(const char *label, const char *wk)
{
    char b[96];
    snprintf(b, sizeof b, "%s(16),%s(16)", wk, label);
    asm_line("", "ZAP", b, "literal");
}

static void gen_rescale(const char *wk, int from, int to)
{
    if (from == to) return;
    char b[96];
    int d = to - from;
    snprintf(b, sizeof b, "%s(16),%d,0", wk, d > 0 ? d : 64 + d);
    asm_line("", "SRP", b, d > 0 ? "align scale (left)" : "align scale (right)");
}

/* Store into an edited field.
 *
 * The pattern's digit selectors consume nibbles from the packed source, so the
 * selector count has to match the digits available. A packed field of n bytes
 * holds 2n-1 digits, so an even digit count leaves exactly one spare leading
 * nibble -- a zero that would eat a selector and shift the result right. One
 * extra selector is inserted for it, and the field takes the tail of the
 * result. IKFCBL00 always allowed two extra; computing it exactly is 0 or 1.
 */
/* Program-check reporting. A program that fails on bad packed data abends
 * S0C7 and says nothing about where -- true of ANS COBOL as well, which was
 * measured rather than assumed. A label per statement plus a table mapping
 * those labels to source lines lets a SPIE exit turn the interrupt address
 * into something a programmer can act on. */
#define MAXLINETAB 4096
static struct { int line; } linetab[MAXLINETAB];
static int nlinetab;
static int gen_lines = 1;      /* -s turns it off */


static int gen_edited_labels;

/* Set a file's FILE STATUS from R15 after a VSAM request.
 *
 * R15 says only that a request failed. Why it failed is in the RPL feedback
 * byte, and which feedback codes can occur depends on the request -- a GET can
 * reach end of data, a PUT cannot; a PUT can find a duplicate key, a GET
 * cannot. So each kind of request brings its own table of the codes it can
 * raise and the FILE STATUS each one means. Both halves of that mapping are
 * fixed, VSAM's by the access method and COBOL's by the standard; neither is
 * this compiler's to choose.
 *
 * Nothing is emitted when the program declared no FILE STATUS, which is legal
 * -- the AT END and INVALID KEY branches still work, the reason for taking one
 * just cannot be inspected afterwards.
 */
typedef struct { int fdbk; const char *status; const char *why; } VsFbk;

/* A request that cannot fail in any way worth telling apart: OPEN, CLOSE. */
static const VsFbk vs_simple[] = {{0, NULL, NULL}};

/* Sequential retrieval. Feedback 4 is end of data, which COBOL calls AT END. */
static const VsFbk vs_read[] = {
    { 4, "10", "end of data" },
    {0, NULL, NULL}
};

/* Storing a record. Feedback 8 is a key already present, 12 a key not greater
 * than the last one written, and 28 a cluster with no room left to extend
 * into. COBOL calls those 22, 21 and 24, and all three are INVALID KEY. */
static const VsFbk vs_write[] = {
    {  8, "22", "duplicate key" },
    { 12, "21", "key out of sequence" },
    { 28, "24", "cluster is full" },
    {0, NULL, NULL}
};

/* Rewriting or erasing the record a GET is holding. Feedback 8 is an attempt
 * to change the prime key, which COBOL folds into 21 along with the other
 * sequence violations; 16 is no such record; 28 is a cluster with no room to
 * put the longer record back. */
static const VsFbk vs_upd[] = {
    {  8, "21", "prime key changed" },
    { 16, "23", "no such record" },
    { 28, "24", "cluster is full" },
    {0, NULL, NULL}
};

/* Retrieving one record by key. Feedback 16 is no record with that key, which
 * COBOL calls INVALID KEY rather than AT END -- a direct read has no end to
 * reach. */
static const VsFbk vs_read_dir[] = {
    { 16, "23", "no such record" },
    {0, NULL, NULL}
};

/* A file needs two RPLs when it is opened I-O and the program also inserts.
 * Retrieval, rewrite and erase all want OPTCD=UPD so the record is held;
 * an insert is by definition not an update and wants NUP. VSAMIOS flips the
 * option with MODCB around each insert and flips it back. A compiler knows
 * which verb it is generating, so it can assemble both control blocks and
 * pick -- no runtime call, and no window in which a failed MODCB leaves the
 * RPL set wrong. */
/* Positioning. Feedback 16 is no record with that key; 4 is no record at or
 * after it, which is the same answer to the same question when the key phrase
 * was NOT LESS THAN. COBOL calls both INVALID KEY, status 23. */
static const VsFbk vs_start[] = {
    {  4, "23", "nothing at or after the key" },
    { 16, "23", "no such record" },
    {0, NULL, NULL}
};

static const char *rpl_name(const File *f, int inserting, char *buf, size_t n)
{
    snprintf(buf, n, "%s%c", f->label,
             (inserting && f->opened_io) ? 'N' : 'R');
    return buf;
}

static void gen_vsam_status(const File *f, const char *rpl, const VsFbk *tab);

/* The USE procedure that applies to the statement being generated, or -1:
 * set per statement, and only when the statement carries no AT END or
 * INVALID KEY of its own -- a phrase takes the error, a USE takes what no
 * phrase does (V-30, VI-32). */
static int gen_use_decl = -1;
static int *gen_use_nret;
static void gen_call_range(int a, int b, int *nret);

static void gen_use_call(void)
{
    if (gen_use_decl < 0) return;
    asm_comment("  no phrase for this: the USE procedure");
    gen_call_range(decls[gen_use_decl].sect, section_end(decls[gen_use_decl].sect), gen_use_nret);
}

static void gen_vsam_status(const File *f, const char *rpl, const VsFbk *tab)
{
    char b[128], fd[64];
    if (f->status_sym < 0) return;
    const Sym *st = &syms[f->status_sym];
    int lok = ++gen_edited_labels, ldone = ++gen_edited_labels;
    char aok[16], adone[16];
    snprintf(aok,   sizeof aok,   "G%04d", lok);
    snprintf(adone, sizeof adone, "G%04d", ldone);

    /* Setting the status is a store through a base register, and every label
     * below is a branch target, so the base tracker has to be told to forget
     * what it believes is loaded at each one. That is the whole of the S0C4
     * slice 1 cost: a base loaded on one path only, then used on both. */
    #define VS_SET(code, why) do {                                          \
        need_sym_base(st);                                                  \
        field_ref(st, NULL, 2, 6, fd, sizeof fd);                           \
        snprintf(b, sizeof b, "%s,=C'%s'", fd, (code));                     \
        asm_line("", "MVC", b, (why));                                      \
    } while (0)

    asm_line("", "LTR", "15,15", "VSAM request succeeded?");
    asm_line("", "BZ", aok, "");

    int ncase = 0;
    while (tab[ncase].status) ncase++;
    if (ncase > 8) die("too many VSAM feedback cases");
    if (ncase) {
        /* R15 alone does not say why, so ask the RPL. */
        snprintf(b, sizeof b, "RPL=%s,FIELDS=FDBK,AREA=VSFB,LENGTH=4", rpl);
        asm_line("", "SHOWCB", b, "why did it fail?");
    }
    char acase[8][16];
    for (int i = 0; i < ncase; i++) {
        snprintf(acase[i], sizeof acase[i], "G%04d", ++gen_edited_labels);
        snprintf(b, sizeof b, "VSFB+3,X'%02X'", tab[i].fdbk);
        asm_line("", "CLI", b, tab[i].why);
        asm_line("", "BE", acase[i], "");
    }
    char afail[16]; snprintf(afail, sizeof afail, "G%04d", ++gen_edited_labels);
    VS_SET("30", "permanent error");
    asm_line("", "B", afail, "");
    for (int i = 0; i < ncase; i++) {
        asm_line(acase[i], "DS", "0H", "");
        reset_bases();          /* arrived by branch: nothing is loaded */
        VS_SET(tab[i].status, tab[i].why);
        asm_line("", "B", afail, "");
    }
    asm_line(aok, "DS", "0H", "");
    reset_bases();
    VS_SET("00", "");
    asm_line("", "B", adone, "");
    /* Failed, status set: the USE procedure, when one applies and the
     * statement had no phrase of its own to take this. */
    asm_line(afail, "DS", "0H", "");
    reset_bases();
    gen_use_call();
    asm_line(adone, "DS", "0H", "");
    reset_bases();
    #undef VS_SET
}

static void gen_store_edited(const Sym *sy, Node *sub, const char *wk)
{
    char b[160], f[64];
    int n = sy->digits / 2 + 1;
    int spare = (2 * n - 1) - sy->digits;          /* 0 or 1 from parity */
    if (sy->need_lead_start && spare == 0) { n++; spare = (2 * n - 1) - sy->digits; }
    int patlen = 1 + spare + sy->bytes;
    if (patlen > PIC_MAXMASK) die("edited field too wide for the ED work area");

    unsigned char pat[PIC_MAXMASK];
    pat[0] = sy->mask[0];
    for (int i = 0; i < spare; i++) pat[1 + i] = 0x20;
    /* The last spare selector turns significance on when the field's own first
       digit position must always print. */
    if (sy->need_lead_start) pat[spare] = 0x21;
    memcpy(pat + 1 + spare, sy->mask + 1, (size_t)sy->bytes);

    snprintf(b, sizeof b, "EDSRC(%d),%s(16)", n, wk);
    asm_line("", "ZAP", b, "source, sized to the selector count");
    snprintf(b, sizeof b, "EDWK(%d),%s", patlen, intern_mask(pat, patlen));
    asm_line("", "MVC", b, "load the ED pattern");

    if (sy->floating) {
        /* EDMK reports where the first significant digit landed, and the
           floating sign goes one byte to its left.
           
           But EDMK loads R1 only when it meets a nonzero digit with the
           significance indicator still OFF. A value small enough that its first
           nonzero digit falls at or after the significance starter never
           satisfies that: the starter itself turns significance on, so every
           later digit prints with the indicator already set and R1 is never
           touched. -1.98 in ---,---,--9.99 is exactly that case, and the sign
           then landed wherever R1 happened to point.
           
           So the fallback must be one byte past the significance starter, which
           is where printing begins when EDMK stays silent. Scan the pattern
           rather than recomputing the arithmetic. */
        int fallback = sy->first_sel + spare;
        for (int i = 0; i < patlen; i++)
            if (pat[i] == 0x21) { fallback = i + 1; break; }
        snprintf(b, sizeof b, "1,EDWK+%d", fallback);
        asm_line("", "LA", b, "where printing starts if EDMK stays silent");
        snprintf(b, sizeof b, "EDWK(%d),EDSRC", patlen);
        asm_line("", "EDMK", b, "");
        asm_line("", "BCTR", "1,0", "one left of the first significant digit");
        if (sy->sign_char == '$') {
            /* A floating currency symbol is not a sign: it goes in whatever
             * the value. Written as hex so that the CURRENCY SIGN character
             * never has to survive the assembler's quoting rules. */
            snprintf(b, sizeof b, "0(1),X'%02X'", host_ebcdic(currency_sym));
            asm_line("", "MVI", b, "the floating currency symbol");
            goto floated;
        }
        int l1 = ++gen_edited_labels, l2 = ++gen_edited_labels;
        char la[16], lb[16];
        snprintf(la, sizeof la, "G%04d", l1);
        snprintf(lb, sizeof lb, "G%04d", l2);
        asm_line("", "BNM", la, "not negative?");
        asm_line("", "MVI", "0(1),C'-'", "");
        if (sy->sign_char == '+') {
            asm_line("", "B", lb, "");
            asm_line(la, "MVI", "0(1),C'+'", "");
            asm_line(lb, "DS", "0H", "");
        } else {
            asm_line(la, "DS", "0H", "");
        }
    floated: ;
    } else {
        snprintf(b, sizeof b, "EDWK(%d),EDSRC", patlen);
        asm_line("", "ED", b, "");
        if (sy->sign_pos >= 0) {
            int l1 = ++gen_edited_labels, l2 = ++gen_edited_labels;
            char la[16], lb[16], pos[24];
            snprintf(la, sizeof la, "G%04d", l1);
            snprintf(lb, sizeof lb, "G%04d", l2);
            snprintf(pos, sizeof pos, "EDWK+%d", sy->sign_pos + spare);
            asm_line("", "BNM", la, "not negative?");
            snprintf(b, sizeof b, "%s,C'-'", pos); asm_line("", "MVI", b, "");
            if (sy->sign_char == '+') {
                asm_line("", "B", lb, "");
                snprintf(b, sizeof b, "%s,C'+'", pos); asm_line(la, "MVI", b, "");
                asm_line(lb, "DS", "0H", "");
            } else {
                asm_line(la, "DS", "0H", "");
            }
        }
    }

    field_ref_m(sy, sub, FR_SS_LEN, sy->bytes, 6, f, sizeof f);
    snprintf(b, sizeof b, "%s,EDWK+%d", f, 1 + spare);
    asm_line("", "MVC", b, "the edited result");
}

static void gen_store(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    if (sy->edited) { gen_store_edited(sy, sub, wk); return; }
    switch (sy->usage) {
    case U_DISPLAY:
        if (sy->digits > 16) { gen_wide_store(sy, sub, wk); break; }
        if (sy->sgn_lead || sy->sgn_sep) { gen_sign_store(sy, sub, wk); break; }
        field_ref(sy, sub, sy->elem, 6, f, sizeof f);
        snprintf(b, sizeof b, "%s,%s(16)", f, wk);
        asm_line("", "UNPK", b, "packed -> zoned");
        if (!sy->is_signed) {
            if (sub) snprintf(b, sizeof b, "%d(6),X'F0'", sy->elem - 1);
            else     snprintf(b, sizeof b, "%s+%d,X'F0'", sy->label, sy->elem - 1);
            asm_line("", "OI", b, "unsigned: force an F zone");
        }
        break;
    case U_COMP3:
        field_ref(sy, sub, sy->elem, 6, f, sizeof f);
        snprintf(b, sizeof b, "%s,%s(16)", f, wk);
        asm_line("", "ZAP", b, "");
        if (!sy->is_signed) {
            /* ZAP leaves a C sign; an unsigned packed item carries F, which
               matters the moment the field is compared byte-wise -- an ISAM
               key, for instance. */
            if (sub) snprintf(b, sizeof b, "%d(6),X'0F'", sy->elem - 1);
            else     snprintf(b, sizeof b, "%s+%d,X'0F'", sy->label, sy->elem - 1);
            asm_line("", "OI", b, "unsigned: force an F sign");
        }
        break;
    case U_COMP:
        snprintf(b, sizeof b, "DWK(8),%s(16)", wk);
        asm_line("", "ZAP", b, "");
        asm_line("", "CVB", "2,DWK", "packed -> binary");
        field_ref(sy, sub, 0, 6, f, sizeof f);
        snprintf(b, sizeof b, "2,%s", f);
        asm_line("", sy->elem == 2 ? "STH" : "ST", b, "");
        break;
    }
}

/* Packed constants, interned so a value used twice is emitted once. */
static struct { char label[16]; char digits[34]; } consts[256];
static int nconst;

static const char *intern_const(const char *digits)
{
    for (int i = 0; i < nconst; i++)
        if (!strcmp(consts[i].digits, digits)) return consts[i].label;
    if (nconst >= 256) die("too many numeric constants");
    snprintf(consts[nconst].label, sizeof consts[nconst].label, "K%04d", nconst + 1);
    snprintf(consts[nconst].digits, sizeof consts[nconst].digits, "%s", digits);
    return consts[nconst++].label;
}

/* Nonnumeric constants, padded to the length the comparison or move needs. */
static struct { char label[16]; char text[MAXTOK]; int len; } sconsts[256];
static int nsconst;

/* Right-justify a digit string in WIDTH, zero filled. COBOL truncates on the
 * left, so an over-long value keeps its low-order digits. */
static void zero_pad(const char *v, int width, char *out, size_t n)
{
    int len = (int)strlen(v);
    if ((size_t)width + 1 > n) die("internal: VALUE too wide to pad");
    if (len >= width) { snprintf(out, n, "%s", v + (len - width)); return; }
    memset(out, '0', (size_t)(width - len));
    memcpy(out + (width - len), v, (size_t)len + 1);
}

static const char *intern_str(const char *text, int len, int pad)
{
    char buf[MAXTOK];
    int n = len > pad ? len : pad;
    if (n >= MAXTOK) die("literal too long");
    for (int i = 0; i < n; i++) buf[i] = i < len ? text[i] : ' ';
    buf[n] = 0;
    for (int i = 0; i < nsconst; i++)
        if (sconsts[i].len == n && !memcmp(sconsts[i].text, buf, (size_t)n))
            return sconsts[i].label;
    if (nsconst >= 256) die("too many nonnumeric constants");
    snprintf(sconsts[nsconst].label, sizeof sconsts[nsconst].label, "S%04d", nsconst + 1);
    memcpy(sconsts[nsconst].text, buf, (size_t)n + 1);
    sconsts[nsconst].len = n;
    return sconsts[nsconst++].label;
}

/* Load a field into a 16-byte work area. */
static void gen_load16(const Sym *sy, Node *sub, const char *wk)
{
    char b[128], f[64];
    switch (sy->usage) {
    case U_DISPLAY:
        field_ref(sy, sub, sy->elem, 7, f, sizeof f);
        snprintf(b, sizeof b, "%s(16),%s", wk, f);
        asm_line("", "PACK", b, "zoned -> packed");
        break;
    case U_COMP3:
        field_ref(sy, sub, sy->elem, 7, f, sizeof f);
        snprintf(b, sizeof b, "%s(16),%s", wk, f);
        asm_line("", "ZAP", b, "");
        break;
    case U_COMP:
        field_ref(sy, sub, 0, 7, f, sizeof f);
        snprintf(b, sizeof b, "2,%s", f);
        asm_line("", sy->elem == 2 ? "LH" : "L", b, "");
        asm_line("", "CVD", "2,DWK", "binary -> packed");
        snprintf(b, sizeof b, "%s(16),DWK(8)", wk);
        asm_line("", "ZAP", b, "");
        break;
    }
}

static void gen_rescale16(const char *wk, int from, int to, int round)
{
    if (from == to) return;
    char b[96];
    int d = to - from;
    snprintf(b, sizeof b, "%s(16),%d,%d", wk, d > 0 ? d : 64 + d,
             (d < 0 && round) ? 5 : 0);
    asm_line("", "SRP", b,
             d > 0 ? "align scale (left)"
                   : (round ? "align scale (right, ROUNDED)" : "align scale (right)"));
}

/* Evaluate n into WK<d>; returns the scale of the value left there.
 *
 * Intermediate-result rules. Addition and subtraction carry the wider scale;
 * multiplication carries the sum of the scales, exactly. Division is the one
 * the standard leaves to the implementation, and the one where GnuCOBOL's
 * unbounded intermediates can disagree with fixed-point: we carry the
 * destination's scale plus four guard digits, capped at twelve.
 */
#define DIVGUARD 4
#define DIVSCALEMAX 12

/* Constants a statement wants placed in the data area: parameter blocks for
 * STRING and UNSTRING. Collected while the code is generated and emitted with
 * the work areas, where the permanent base covers them. */
static struct { char lab[12], op[8], opd[80], cmt[48]; } pend_dc[MAXSOP * 4];
static int npend_dc;
static int use_str, use_uns, use_insprop, use_wto, use_wtor, use_adt, use_mvl, use_devtype, use_dcal;
static void pend(const char *lab, const char *op, const char *opd, const char *cmt)
{
    if (npend_dc >= MAXSOP * 4) die("too many STRING/UNSTRING blocks");
    snprintf(pend_dc[npend_dc].lab, 12, "%s", lab);
    snprintf(pend_dc[npend_dc].op, 8, "%s", op);
    snprintf(pend_dc[npend_dc].opd, 80, "%s", opd);
    snprintf(pend_dc[npend_dc].cmt, 48, "%s", cmt);
    npend_dc++;
}

/* The address of an operand into a parameter block slot -- at run time for an
 * identifier, whose subscript or base locator only exists then. */
static void sop_addr(const SOp *o, int use_delim, const char *slot)
{
    char b[128], f[64];
    int sy = use_delim ? o->dsym : o->sym;
    Node *sb = use_delim ? o->dsub : o->sub;
    if (sy < 0) return;                      /* a constant: assembled in */
    const Sym *y = &syms[sy];
    int n = sb ? y->elem : y->bytes;
    field_ref_m(y, sb, FR_RX, n, 6, f, sizeof f);
    snprintf(b, sizeof b, "1,%s", f);
    asm_line("", "LA", b, y->name);
    snprintf(b, sizeof b, "1,%s", slot);
    asm_line("", "ST", b, "");
}
static int sop_len(const SOp *o, int use_delim)
{
    int sy = use_delim ? o->dsym : o->sym;
    if (sy < 0) return use_delim ? o->dlen : o->len;
    Node *sb = use_delim ? o->dsub : o->sub;
    return sb ? syms[sy].elem : syms[sy].bytes;
}
/* An integer item to a fullword cell, and back. */
static void cell_in(int sy, Node *sb, const char *cell)
{
    char b[64];
    need_sym_base(&syms[sy]);
    gen_load(&syms[sy], sb, "PWK1");
    asm_line("", "ZAP", "DWK(8),PWK1(16)", "");
    asm_line("", "CVB", "2,DWK", "");
    snprintf(b, sizeof b, "2,%s", cell); asm_line("", "ST", b, syms[sy].name);
}
static void cell_out(int sy, Node *sb, const char *cell)
{
    char b[64];
    snprintf(b, sizeof b, "2,%s", cell); asm_line("", "L", b, "");
    asm_line("", "CVD", "2,DWK", "");
    asm_line("", "ZAP", "PWK1(16),DWK(8)", "");
    need_sym_base(&syms[sy]);
    gen_store(&syms[sy], sb, "PWK1");
}

/* While a statement under ON SIZE ERROR is being generated: where a detected
 * error branches to, past the store. Empty otherwise. */
static char gen_size_skip[16];
static int  use_szflg;

static int gen_expr(Node *n, int d, int tgtscale)
{
    char b[128], wk[8], wk2[8];
    if (d >= 5) die("expression nests too deeply for the work-area stack");
    snprintf(wk,  sizeof wk,  "WK%d", d);
    snprintf(wk2, sizeof wk2, "WK%d", d + 1);

    switch (n->kind) {
    case N_SYM:
        gen_load16(&syms[n->sym], n->sub, wk);
        return syms[n->sym].scale;

    case N_LIT: {
        const char *lab = intern_const(n->lit);
        snprintf(b, sizeof b, "%s(16),%s(16)", wk, lab);
        asm_line("", "ZAP", b, "literal");
        return n->litscale;
    }

    case N_NEG: {
        int s = gen_expr(n->l, d + 1, tgtscale);
        snprintf(b, sizeof b, "%s(16),%s(16)", wk, intern_const("0"));
        asm_line("", "ZAP", b, "unary minus");
        snprintf(b, sizeof b, "%s(16),%s(16)", wk, wk2);
        asm_line("", "SP", b, "");
        return s;
    }

    case N_ADD: case N_SUB: {
        int sl = gen_expr(n->l, d, tgtscale);
        int sr = gen_expr(n->r, d + 1, tgtscale);
        int ws = sl > sr ? sl : sr;
        gen_rescale16(wk,  sl, ws, 0);
        gen_rescale16(wk2, sr, ws, 0);
        snprintf(b, sizeof b, "%s(16),%s(16)", wk, wk2);
        asm_line("", n->kind == N_ADD ? "AP" : "SP", b, "");
        return ws;
    }

    case N_MUL: {
        int sl = gen_expr(n->l, d, tgtscale);
        int sr = gen_expr(n->r, d + 1, tgtscale);
        snprintf(b, sizeof b, "MULT8(8),%s(16)", wk2);
        asm_line("", "ZAP", b, "MP takes at most 8 bytes on the right");
        snprintf(b, sizeof b, "%s(16),MULT8(8)", wk);
        asm_line("", "MP", b, "scale becomes the sum of the scales");
        return sl + sr;
    }

    case N_DIV: {
        int sl = gen_expr(n->l, d, tgtscale);
        int sr = gen_expr(n->r, d + 1, tgtscale);
        if (gen_size_skip[0]) {
            /* Under ON SIZE ERROR a zero divisor is a size error rather than a
             * decimal-divide exception: flag it and leave the receiver alone.
             * Without the phrase the DP takes its S0CB, as the standard
             * leaves it undefined and IKFCBL00 does the same. */
            char lg[16]; snprintf(lg, sizeof lg, "L%04d", ++genlabel);
            snprintf(b, sizeof b, "%s(16),%s(16)", wk2, intern_const("0"));
            asm_line("", "CP", b, "dividing by zero?");
            asm_line("", "BNE", lg, "");
            asm_line("", "MVI", "SZFLG,X'01'", "size error");
            asm_line("", "B", gen_size_skip, "");
            asm_line(lg, "DS", "0H", "");
        }
        int sq = tgtscale + DIVGUARD;
        if (sq > DIVSCALEMAX) sq = DIVSCALEMAX;
        /* Quotient at scale sq needs the dividend pre-shifted by sq+sr-sl. */
        gen_rescale16(wk, sl, sq + sr, 0);
        snprintf(b, sizeof b, "DIVR8(8),%s(16)", wk2);
        asm_line("", "ZAP", b, "DP takes at most 8 bytes on the right");
        snprintf(b, sizeof b, "%s(16),DIVR8(8)", wk);
        asm_line("", "DP", b, "quotient in the leading 8 bytes");
        snprintf(b, sizeof b, "QTMP(8),%s(8)", wk);
        asm_line("", "ZAP", b, "");
        snprintf(b, sizeof b, "%s(16),QTMP(8)", wk);
        asm_line("", "ZAP", b, "drop the remainder");
        return sq;
    }
    case N_TRUNC: {
        int sl = gen_expr(n->l, d, tgtscale);
        gen_rescale16(wk, sl, n->litscale, 0);
        return n->litscale;
    }

    case N_POW: {
        /* Exponentiation in packed decimal is repeated multiplication, and
         * the result's scale is the base's times the exponent -- which has to
         * be known here. So a literal exponent is unrolled, for any base, and
         * an identifier exponent runs a loop, for an integer base only. A
         * negative or fractional exponent has no exact packed-decimal answer
         * and is refused. */
        Node *r = n->r;
        if (r->kind == N_LIT) {
            if (r->litscale != 0) die("a fractional exponent is not implemented -- it has no exact decimal value");
            int e = atoi(r->lit);
            int sl = gen_expr(n->l, d, tgtscale);
            if (sl * e > 30) die("the result of ** would carry more than thirty decimal places");
            if (e == 0) {
                snprintf(b, sizeof b, "%s(16),%s(16)", wk, intern_const("1"));
                asm_line("", "ZAP", b, "anything to the zero is one");
                return 0;
            }
            if (e > 1) {
                snprintf(b, sizeof b, "MULT8(8),%s(16)", wk);
                asm_line("", "ZAP", b, "the base");
                for (int k = 1; k < e; k++) {
                    snprintf(b, sizeof b, "%s(16),MULT8(8)", wk);
                    asm_line("", "MP", b, k == 1 ? "** unrolled" : "");
                }
            }
            return sl * e;
        }
        if (r->kind == N_NEG) die("a negative exponent is not implemented -- it has no exact decimal value");
        if (r->kind != N_SYM || syms[r->sym].scale != 0 || syms[r->sym].is_alpha || syms[r->sym].is_group)
            die("the exponent of ** must be an integer literal or an integer identifier");
        if (d + 2 >= 5) die("expression nests too deeply for the work-area stack");
        int sl = gen_expr(n->l, d + 1, tgtscale);
        if (sl != 0) die("** with an identifier exponent needs an integer base; the result's scale would not be known");
        char wk3[8]; snprintf(wk3, sizeof wk3, "WK%d", d + 2);
        gen_expr(r, d + 2, 0);
        snprintf(b, sizeof b, "DWK(8),%s(16)", wk3);
        asm_line("", "ZAP", b, "");
        asm_line("", "CVB", "3,DWK", "the exponent");
        snprintf(b, sizeof b, "%s(16),%s(16)", wk, intern_const("1"));
        asm_line("", "ZAP", b, "start from one");
        char lp[16], le[16];
        snprintf(lp, sizeof lp, "L%04d", ++genlabel);
        snprintf(le, sizeof le, "L%04d", ++genlabel);
        asm_line("", "LTR", "3,3", "");
        asm_line("", "BNP", le, "exponent of zero: one");
        snprintf(b, sizeof b, "MULT8(8),%s(16)", wk2);
        asm_line(lp, "ZAP", b, "the base");
        snprintf(b, sizeof b, "%s(16),MULT8(8)", wk);
        asm_line("", "MP", b, "");
        snprintf(b, sizeof b, "3,%s", lp);
        asm_line("", "BCT", b, "once per exponent");
        asm_line(le, "DS", "0H", "");
        return 0;
    }
    }
    die("internal: bad expression node");
    return 0;
}

/* An item whose length is only known at run time: a group containing an
 * OCCURS DEPENDING ON table, or that table itself. */
static int var_len(const Sym *y) { return y->odo_tab > 0 || (y->occurs > 0 && y->odo_dep >= 0); }

/* Load the current length of such an item into a register: the fixed part
 * plus the count times the element. */
static void gen_var_len(const Sym *y, int reg)
{
    char b[96];
    const Sym *t = y->odo_tab > 0 ? &syms[y->odo_tab] : y;
    const Sym *dep = &syms[t->odo_dep];
    int fixed = y->bytes - t->bytes;
    need_sym_base(dep);
    gen_load(dep, NULL, "PWK2");
    asm_line("", "ZAP", "DWK(8),PWK2(16)", "");
    snprintf(b, sizeof b, "%d,DWK", reg);
    asm_line("", "CVB", b, "DEPENDING ON count");
    snprintf(b, sizeof b, "%d,%s", reg, intern_half(t->elem));
    asm_line("", "MH", b, "times the element");
    if (fixed) { snprintf(b, sizeof b, "%d,%d(%d)", reg, fixed, reg); asm_line("", "LA", b, "plus the fixed part"); }
}

/* Alphanumeric move: left justified, space filled, truncated on the right.
 * Groups move as bytes, which is why a group MOVE is alphanumeric whatever
 * its subordinates are. */
static void gen_move_alpha(const Sym *d, Node *dsub, const Sym *sv, Node *ssub)
{
    char b[160], fd[64], fs[64];
    int dn = dsub ? d->elem : d->bytes;
    int sn = ssub ? sv->elem : sv->bytes;
    int n = dn < sn ? dn : sn;
    if ((var_len(d) && !dsub) || (var_len(sv) && !ssub) || n > 256) {
        /* A length only known at run time, or one too long for an MVC: the
         * runtime moves it, however long, and space fills the rest. */
        if (d->edited || d->just) die("a MOVE to an edited or JUSTIFIED item longer than 256 bytes, or of run-time length, is not implemented");
        use_mvl = 1;
        if (var_len(d) && !dsub) gen_var_len(d, 2); else { snprintf(b, sizeof b, "2,%d", dn); asm_line("", "LA", b, "receiver length"); }
        asm_line("", "STH", "2,MVLLEN", "");
        if (var_len(sv) && !ssub) gen_var_len(sv, 2); else { snprintf(b, sizeof b, "2,%d", sn); asm_line("", "LA", b, "sender length"); }
        asm_line("", "STH", "2,MVLLEN+2", "");
        need_sym_base(d);
        field_ref_m(d, dsub, FR_RX, dn, 6, fd, sizeof fd);
        snprintf(b, sizeof b, "1,%s", fd); asm_line("", "LA", b, "");
        asm_line("", "ST", "1,MVLPARM", "");
        need_sym_base(sv);
        field_ref_m(sv, ssub, FR_RX, sn, 7, fs, sizeof fs);
        snprintf(b, sizeof b, "1,%s", fs); asm_line("", "LA", b, "");
        asm_line("", "ST", "1,MVLPARM+4", "");
        asm_line("", "LA", "1,MVLPARM", "");
        asm_line("", "L", "15,VMVL", "");
        asm_line("", "BALR", "14,15", "COBMVL: any length, space filled");
        reset_bases();
        return;
    }
    if (d->edited && d->is_alpha) {
        /* Alphanumeric edited: lay down the template, then fill each run of
         * data positions from the sending item. Both are known at compile
         * time, so this is a handful of MVCs and no run-time decisions. */
        char fs2[64];
        field_ref_m(sv, ssub, FR_SS_NOLEN, sn, 7, fs2, sizeof fs2);
        snprintf(b, sizeof b, "%s(%d),%s", dsub ? "0(6)" : d->label, d->masklen,
                 intern_mask(d->mask, d->masklen));
        if (dsub) { field_ref_m(d, dsub, FR_SS_NOLEN, dn, 6, fd, sizeof fd);
                    snprintf(b, sizeof b, "0(%d,6),%s", d->masklen,
                             intern_mask(d->mask, d->masklen)); }
        asm_line("", "MVC", b, "the insertion characters");
        int soff = 0, i = 0;
        while (i < d->masklen) {
            if (d->mask[i]) { i++; continue; }
            int run = 0;
            while (i + run < d->masklen && !d->mask[i + run]) run++;
            if (soff < sn) {
                int take = sn - soff < run ? sn - soff : run;
                if (dsub) snprintf(b, sizeof b, "%d(%d,6),", i, take);
                else      snprintf(b, sizeof b, "%s+%d(%d),", d->label, i, take);
                if (ssub) snprintf(b + strlen(b), sizeof b - strlen(b), "%d(7)", soff);
                else snprintf(b + strlen(b), sizeof b - strlen(b), "%s+%d", sv->label, soff);
                asm_line("", "MVC", b, "a run of data positions");
                soff += take;
            }
            i += run;
        }
        (void)fs2;
        return;
    }
    if (d->just) {
        /* Right-aligned: the data lands at the end of the receiver, the pad
         * goes in front of it, and an over-long sender loses its left. */
        int doff = dn - n, soff = sn - n;
        field_ref_m(sv, ssub, FR_SS_NOLEN, n, 7, fs, sizeof fs);
        field_ref_m(d, dsub, FR_SS_NOLEN, n, 6, fd, sizeof fd);
        if (doff > 0) {
            if (dsub) snprintf(b, sizeof b, "0(6),C' '");
            else      snprintf(b, sizeof b, "%s,C' '", d->label);
            asm_line("", "MVI", b, "space fill on the left");
            if (doff > 1) {
                if (dsub) snprintf(b, sizeof b, "1(%d,6),0(6)", doff - 1);
                else snprintf(b, sizeof b, "%s+1(%d),%s", d->label, doff - 1, d->label);
                asm_line("", "MVC", b, "");
            }
        }
        if (dsub) snprintf(b, sizeof b, "%d(%d,6),", doff, n);
        else      snprintf(b, sizeof b, "%s+%d(%d),", d->label, doff, n);
        if (ssub) snprintf(b + strlen(b), sizeof b - strlen(b), "%d(7)", soff);
        else      snprintf(b + strlen(b), sizeof b - strlen(b), "%s+%d", sv->label, soff);
        asm_line("", "MVC", b, "JUSTIFIED: aligned right");
        return;
    }
    field_ref_m(sv, ssub, FR_SS_NOLEN, n, 7, fs, sizeof fs);
    field_ref(d, dsub, n, 6, fd, sizeof fd);
    snprintf(b, sizeof b, "%s,%s", fd, fs);
    asm_line("", "MVC", b, "alphanumeric move");
    if (dn > n) {
        int rest = dn - n;
        if (rest - 1 > 256) die("space fill longer than 256 bytes is not implemented yet");
        if (dsub) snprintf(b, sizeof b, "%d(6),C' '", n);
        else      snprintf(b, sizeof b, "%s+%d,C' '", d->label, n);
        asm_line("", "MVI", b, "space fill the remainder");
        if (rest > 1) {
            if (dsub) snprintf(b, sizeof b, "%d(%d,6),%d(6)", n + 1, rest - 1, n);
            else snprintf(b, sizeof b, "%s+%d(%d),%s+%d", d->label, n + 1, rest - 1, d->label, n);
            asm_line("", "MVC", b, "");
        }
    }
}

/* A continued statement: text padded so the continuation flag lands in
   column 72, and the continuation itself starting in column 16. */
/* A statement whose operand will not fit one card. Continuation cards carry the
 * operand in columns 16-71, with an X in column 72 of every card but the last.
 *
 * The operand must STOP by column 71: a character reaching column 72 is itself
 * read as the continuation indicator, which turns the following card into a
 * continuation of this one. That is how a DCB once swallowed the DCB defined
 * after it, leaving its label undefined -- so split at commas across as many
 * cards as it takes rather than assuming two will do. */
static void asm_cont(const char *first, const char *second)
{
    if (strlen(first) > 71) die("internal: continuation line too long");
    /* The operand lives at buffer indices 15..70, i.e. columns 16..71.
     * Index 71 is column 72 and must stay free for the continuation flag. */
    enum { WIDTH = 70 - 15 + 1 };
    char work[512];
    if (strlen(second) >= sizeof work) die("internal: operand too long");
    snprintf(work, sizeof work, "%s", second);

    char lines[8][WIDTH + 1];
    int nl = 0;
    lines[0][0] = 0;
    for (char *q = work; *q; ) {
        char *c = strchr(q, ',');
        size_t flen = c ? (size_t)(c - q) + 1 : strlen(q);
        if (flen > WIDTH) die("internal: continuation field will not fit a card");
        if (strlen(lines[nl]) + flen > WIDTH) {
            if (++nl >= 8) die("internal: too many continuation cards");
            lines[nl][0] = 0;
        }
        size_t at = strlen(lines[nl]);
        memcpy(lines[nl] + at, q, flen);
        lines[nl][at + flen] = 0;
        q += flen;
    }
    char b[128];
    memset(b, ' ', sizeof b);
    memcpy(b, first, strlen(first));
    b[71] = 'X'; b[72] = 0;
    fprintf(out, "%s\n", b);
    for (int i = 0; i <= nl; i++) {
        size_t n = strlen(lines[i]);
        memset(b, ' ', sizeof b);
        memcpy(b + 15, lines[i], n);
        if (i < nl) { b[71] = 'X'; b[72] = 0; }
        else b[15 + n] = 0;
        fprintf(out, "%s\n", b);
    }
}

/* Assembler C'...' needs both quotes and ampersands doubled. */
static void emit_literal(const char *label, const char *text, int len)
{
    char op[MAXTOK * 2 + 8];
    int j = 0;
    op[j++] = 'C'; op[j++] = '\'';
    for (int i = 0; i < len; i++) {
        if (text[i] == '\'' || text[i] == '&') op[j++] = text[i];
        op[j++] = text[i];
    }
    op[j++] = '\''; op[j] = 0;
    if (15 + (int)strlen(op) > 71)
        die("literal too long for one assembler statement");
    asm_line(label, "DC", op, "");
}

/* COBSTR and COBUNS. Both take R1 -> a parameter block laid out by the code
 * generator (see ST_STRING and ST_UNSTRING), work in bytes, and return the
 * overflow condition in R15. They know nothing about pictures: POINTER,
 * TALLYING and COUNT are fullword cells the caller converts. */
static void emit_string_runtime(void)
{
    asm_comment("");
    asm_comment(" COBSTR -- STRING. R1 -> AL4(receiver), AL2(len), AL2(n),");
    asm_comment("   AL4(pointer cell or 0); then per item AL4(addr), AL2(len),");
    asm_comment("   AL2(delimiter len, 0 for SIZE), AL4(delimiter addr).");
    asm_comment(" Each item goes byte by byte until its delimiter matches or");
    asm_comment(" it runs out; the receiver filling up is the overflow, and");
    asm_comment(" a pointer outside 1..len is overflow with nothing moved.");
    asm_comment(" R15 = 0 done, 1 overflow. Other bytes of the receiver are");
    asm_comment(" not touched, which is the rule: STRING does not space-fill.");
    asm_line("COBSTR", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE8+4", "");
    asm_line("", "LA", "11,RTSAVE8", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "LR", "2,1", "the block");
    asm_line("", "L", "3,0(2)", "receiver");
    asm_line("", "LH", "5,4(2)", "its length");
    asm_line("", "LH", "7,6(2)", "items");
    asm_line("", "L", "4,8(2)", "pointer cell");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BZ", "STR010", "none: start at 1");
    asm_line("", "L", "4,0(4)", "");
    asm_line("", "B", "STR020", "");
    asm_line("STR010", "LA", "4,1", "");
    asm_line("STR020", "BCTR", "4,0", "position, from 0");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BM", "STRBAD", "pointer below 1");
    asm_line("", "CR", "4,5", "");
    asm_line("", "BNL", "STRBAD", "pointer past the end");
    asm_line("", "LA", "6,12(2)", "the first item");
    asm_line("STR100", "LTR", "7,7", "");
    asm_line("", "BZ", "STRDONE", "all items sent");
    asm_line("", "L", "8,0(6)", "item");
    asm_line("", "LH", "9,4(6)", "its length");
    asm_line("", "LH", "11,6(6)", "delimiter length");
    asm_line("", "L", "10,8(6)", "delimiter");
    asm_line("STR110", "LTR", "9,9", "");
    asm_line("", "BZ", "STR190", "item exhausted");
    asm_line("", "LTR", "11,11", "");
    asm_line("", "BZ", "STR120", "SIZE: no delimiter to look for");
    asm_line("", "CR", "9,11", "");
    asm_line("", "BL", "STR120", "too few left to hold the delimiter");
    asm_line("", "LR", "15,11", "");
    asm_line("", "BCTR", "15,0", "");
    asm_line("", "EX", "15,STRCLC", "delimiter here?");
    asm_line("", "BE", "STR190", "yes: the item ends");
    asm_line("STR120", "CR", "4,5", "");
    asm_line("", "BNL", "STROVF", "receiver full");
    asm_line("", "LA", "15,0(3,4)", "");
    asm_line("", "MVC", "0(1,15),0(8)", "one byte");
    asm_line("", "LA", "8,1(8)", "");
    asm_line("", "BCTR", "9,0", "");
    asm_line("", "LA", "4,1(4)", "");
    asm_line("", "B", "STR110", "");
    asm_line("STR190", "LA", "6,12(6)", "next item");
    asm_line("", "BCTR", "7,0", "");
    asm_line("", "B", "STR100", "");
    asm_line("STRDONE", "SR", "15,15", "");
    asm_line("", "B", "STRRET", "");
    asm_line("STROVF", "LA", "15,1", "overflow");
    asm_line("STRRET", "L", "14,8(2)", "pointer cell");
    asm_line("", "LTR", "14,14", "");
    asm_line("", "BZ", "STRX", "");
    asm_line("", "LA", "4,1(4)", "back to counting from 1");
    asm_line("", "ST", "4,0(14)", "");
    asm_line("", "B", "STRX", "");
    asm_line("STRBAD", "LA", "15,1", "pointer out of range: overflow, nothing moved");
    asm_line("STRX", "L", "13,4(13)", "");
    asm_line("", "ST", "15,16(13)", "R15 survives the LM through its own slot");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "BR", "14", "");
    asm_line("STRCLC", "CLC", "0(0,8),0(10)", "executed with the delimiter length");
    asm_line("RTSAVE8", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBUNS -- UNSTRING. R1 -> AL4(sender), AL2(len), AL2(nd),");
    asm_comment("   AL4(pointer cell or 0), AL4(tally cell or 0), AL2(nr),");
    asm_comment("   AL2(0), AL4(0); then nd delimiters AL4(addr), AL2(len),");
    asm_comment("   AL2(ALL); then nr receivers AL4(addr), AL2(len),");
    asm_comment("   AL2(DELIMITER IN len), AL4(DELIMITER IN or 0),");
    asm_comment("   AL4(COUNT IN cell or 0).");
    asm_comment(" For each receiver the sender is scanned from the position");
    asm_comment(" for the first place any delimiter matches, delimiters tried");
    asm_comment(" in order; the bytes before it go to the receiver, left");
    asm_comment(" justified and space filled. No delimiters means a receiver");
    asm_comment(" takes its own length. Bytes left when the receivers are");
    asm_comment(" used up are the overflow. R15 = 0 done, 1 overflow.");
    asm_line("COBUNS", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE9+4", "");
    asm_line("", "LA", "11,RTSAVE9", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "LR", "2,1", "the block");
    asm_line("", "L", "3,0(2)", "sender");
    asm_line("", "LH", "5,4(2)", "its length");
    asm_line("", "LH", "10,6(2)", "delimiters");
    asm_line("", "L", "4,8(2)", "pointer cell");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BZ", "UNS010", "");
    asm_line("", "L", "4,0(4)", "");
    asm_line("", "B", "UNS020", "");
    asm_line("UNS010", "LA", "4,1", "");
    asm_line("UNS020", "BCTR", "4,0", "position, from 0");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BM", "UNSBAD", "");
    asm_line("", "CR", "4,5", "");
    asm_line("", "BNL", "UNSBAD", "pointer past the end");
    asm_line("", "LH", "7,16(2)", "receivers");
    asm_line("", "LR", "6,10", "");
    asm_line("", "SLL", "6,3", "eight bytes per delimiter");
    asm_line("", "LA", "6,24(6,2)", "the first receiver");
    asm_line("UNS100", "LTR", "7,7", "");
    asm_line("", "BZ", "UNSEND", "receivers used up");
    asm_line("", "CR", "4,5", "");
    asm_line("", "BNL", "UNSEND", "sender used up: the rest stay as they were");
    asm_line("", "LR", "8,4", "scan from the position");
    asm_line("", "SR", "9,9", "no delimiter matched yet");
    asm_line("", "LTR", "10,10", "");
    asm_line("", "BZ", "UNS140", "no delimiters: take the receiver's length");
    asm_line("UNS110", "CR", "8,5", "");
    asm_line("", "BNL", "UNS170", "end of the sender, no delimiter");
    asm_line("", "LA", "9,24(2)", "the first delimiter");
    asm_line("", "LR", "0,10", "");
    asm_line("UNS120", "L", "14,0(9)", "delimiter");
    asm_line("", "LH", "15,4(9)", "its length");
    asm_line("", "LR", "1,5", "");
    asm_line("", "SR", "1,8", "bytes left");
    asm_line("", "CR", "1,15", "");
    asm_line("", "BL", "UNS130", "not enough left for this one");
    asm_line("", "LA", "1,0(3,8)", "");
    asm_line("", "BCTR", "15,0", "");
    asm_line("", "EX", "15,UNSCLC", "matches here?");
    asm_line("", "BE", "UNS170", "yes, R9 says which");
    asm_line("UNS130", "LA", "9,8(9)", "next delimiter");
    asm_line("", "BCT", "0,UNS120", "");
    asm_line("", "SR", "9,9", "none matched at this byte");
    asm_line("", "LA", "8,1(8)", "");
    asm_line("", "B", "UNS110", "");
    asm_line("UNS140", "LH", "1,4(6)", "receiver length");
    asm_line("", "LA", "8,0(4,1)", "position plus that");
    asm_line("", "CR", "8,5", "");
    asm_line("", "BNH", "UNS170", "");
    asm_line("", "LR", "8,5", "but no further than the end");
    /* The bytes from 4 to 8 go to the receiver: space-fill it, then move. */
    asm_line("UNS170", "L", "14,0(6)", "receiver");
    asm_line("", "LH", "15,4(6)", "its length");
    asm_line("", "MVI", "0(14),C' '", "");
    asm_line("", "LR", "11,15", "");
    asm_line("", "BCTR", "11,0", "");
    asm_line("", "LTR", "11,11", "");
    asm_line("", "BZ", "UNS172", "a one-byte receiver is filled");
    asm_line("", "BCTR", "11,0", "");
    asm_line("", "EX", "11,UNSPAD", "space fill");
    asm_line("UNS172", "LR", "1,8", "");
    asm_line("", "SR", "1,4", "bytes to move");
    asm_line("", "L", "11,12(6)", "COUNT IN cell");
    asm_line("", "LTR", "11,11", "");
    asm_line("", "BZ", "UNS174", "");
    asm_line("", "ST", "1,0(11)", "the count, before truncation");
    asm_line("UNS174", "CR", "1,15", "");
    asm_line("", "BNH", "UNS176", "");
    asm_line("", "LR", "1,15", "truncated on the right");
    asm_line("UNS176", "LTR", "1,1", "");
    asm_line("", "BZ", "UNS180", "nothing to move");
    asm_line("", "BCTR", "1,0", "");
    asm_line("", "LA", "15,0(3,4)", "");
    asm_line("", "EX", "1,UNSMVC", "the bytes");
    /* DELIMITER IN: the delimiter that ended the receiver, or spaces. */
    asm_line("UNS180", "L", "14,8(6)", "DELIMITER IN");
    asm_line("", "LTR", "14,14", "");
    asm_line("", "BZ", "UNS186", "");
    asm_line("", "LH", "15,6(6)", "its length");
    asm_line("", "MVI", "0(14),C' '", "");
    asm_line("", "LR", "11,15", "");
    asm_line("", "BCTR", "11,0", "");
    asm_line("", "LTR", "11,11", "");
    asm_line("", "BZ", "UNS182", "");
    asm_line("", "BCTR", "11,0", "");
    asm_line("", "EX", "11,UNSPAD", "space fill");
    asm_line("UNS182", "LTR", "9,9", "");
    asm_line("", "BZ", "UNS186", "no delimiter: spaces");
    asm_line("", "LH", "1,4(9)", "delimiter length");
    asm_line("", "CR", "1,15", "");
    asm_line("", "BNH", "UNS184", "");
    asm_line("", "LR", "1,15", "");
    asm_line("UNS184", "BCTR", "1,0", "");
    asm_line("", "L", "15,0(9)", "");
    asm_line("", "EX", "1,UNSMVC", "the delimiter");
    /* Tally, then advance past the delimiter (and its repeats, for ALL). */
    asm_line("UNS186", "L", "14,12(2)", "TALLYING cell");
    asm_line("", "LTR", "14,14", "");
    asm_line("", "BZ", "UNS188", "");
    asm_line("", "L", "15,0(14)", "");
    asm_line("", "LA", "15,1(15)", "one more receiver acted on");
    asm_line("", "ST", "15,0(14)", "");
    asm_line("UNS188", "LTR", "9,9", "");
    asm_line("", "BZ", "UNS196", "no delimiter: position is where the scan stopped");
    asm_line("", "LH", "1,4(9)", "delimiter length");
    asm_line("", "LA", "4,0(1,8)", "past the delimiter");
    asm_line("", "LH", "0,6(9)", "ALL?");
    asm_line("", "LTR", "0,0", "");
    asm_line("", "BZ", "UNS198", "");
    asm_line("UNS190", "LR", "0,5", "");
    asm_line("", "SR", "0,4", "bytes left");
    asm_line("", "CR", "0,1", "");
    asm_line("", "BL", "UNS198", "not enough for another copy");
    asm_line("", "LA", "15,0(3,4)", "");
    asm_line("", "L", "14,0(9)", "");
    asm_line("", "LR", "11,1", "");
    asm_line("", "BCTR", "11,0", "");
    asm_line("", "EX", "11,UNSCLC2", "another copy of the delimiter?");
    asm_line("", "BNE", "UNS198", "");
    asm_line("", "LA", "4,0(1,4)", "skip it too");
    asm_line("", "B", "UNS190", "");
    asm_line("UNS196", "LR", "4,8", "");
    asm_line("UNS198", "LA", "6,16(6)", "next receiver");
    asm_line("", "BCTR", "7,0", "");
    asm_line("", "B", "UNS100", "");
    asm_line("UNSEND", "SR", "15,15", "");
    asm_line("", "CR", "4,5", "");
    asm_line("", "BNL", "UNS200", "");
    asm_line("", "LA", "15,1", "bytes left over: overflow");
    asm_line("UNS200", "L", "14,8(2)", "pointer cell");
    asm_line("", "LTR", "14,14", "");
    asm_line("", "BZ", "UNSX", "");
    asm_line("", "LA", "4,1(4)", "");
    asm_line("", "ST", "4,0(14)", "");
    asm_line("", "B", "UNSX", "");
    asm_line("UNSBAD", "LA", "15,1", "pointer out of range: overflow, nothing done");
    asm_line("UNSX", "L", "13,4(13)", "");
    asm_line("", "ST", "15,16(13)", "R15 survives the LM through its own slot");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "BR", "14", "");
    asm_line("UNSCLC", "CLC", "0(0,1),0(14)", "executed with the delimiter length");
    asm_line("UNSCLC2", "CLC", "0(0,15),0(14)", "");
    asm_line("UNSMVC", "MVC", "0(0,14),0(15)", "executed with the byte count");
    asm_line("UNSPAD", "MVC", "1(0,14),0(14)", "executed: propagate the space");
    asm_line("RTSAVE9", "DS", "18F", "");
}

static void emit_runtime(void)
{
    asm_comment("---------------------------------------------------------------");
    asm_comment(" COBRT -- our runtime. Nothing here is from SYS1.COBLIB.");
    asm_comment(" DISPLAY reaches SYSOUT through QSAM directly, which is the");
    asm_comment(" same access-method path IKFCBL00 uses for its own file I/O.");
    asm_comment(" Not reentrant: MVS 3.8j batch does not require it.");
    asm_comment("---------------------------------------------------------------");
    asm_line("COBRT", "CSECT", "", "");
    asm_line("", "ENTRY", "COBDISP,COBTERM,COBWRL,COBDATE,COBACC,COBUPSI", "");
    asm_line("", "ENTRY", "COBADV,COBSTR,COBUNS,COBWTO,COBWTOR,COBADT,COBMVL", "");
    asm_line("", "ENTRY", "COBDCAL,COBCANC", "");
    asm_comment("");
    asm_comment(" COBDISP -- write one line to SYSOUT.");
    asm_comment("   R1 -> A(text), A(halfword length).  Opens SYSOUT on demand.");
    asm_comment("");
    asm_line("COBDISP", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE1+4", "");
    asm_line("", "LA", "11,RTSAVE1", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(text)");
    asm_line("", "L", "3,4(0,1)", "A(length)");
    asm_line("", "LH", "4,0(0,3)", "length");
    asm_line("", "CLI", "RTOPEN,X'01'", "already open?");
    asm_line("", "BE", "COBD010", "");
    asm_line("", "OPEN", "(RTDCB,OUTPUT)", "");
    asm_line("", "MVI", "RTOPEN,X'01'", "");
    asm_line("COBD010", "MVI", "RTLINE,C' '", "ASA: single space");
    asm_line("", "MVC", "RTLINE+1(120),RTLINE", "blank the text area");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BNP", "COBD020", "empty: emit a blank line");
    asm_line("", "CH", "4,RTMAX", "");
    asm_line("", "BNH", "COBD015", "");
    asm_line("", "LH", "4,RTMAX", "truncate at line width");
    asm_line("COBD015", "BCTR", "4,0", "EX wants length-1");
    asm_line("", "EX", "4,COBDMVC", "");
    asm_line("COBD020", "PUT", "RTDCB,RTLINE", "");
    asm_line("", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("COBDMVC", "MVC", "RTLINE+1(0),0(2)", "executed, never fallen into");
    asm_comment("");
    asm_comment(" COBADV -- write one line with ASA carriage control.");
    asm_comment("");
    asm_comment("   R1 -> A(dcb), A(print buffer), A(halfword record length),");
    asm_comment("         A(halfword owed), A(halfword request)");
    asm_comment("");
    asm_comment(" ASA says what to do BEFORE a line prints, which is exactly");
    asm_comment(" what AFTER ADVANCING means. BEFORE has to be held over: the");
    asm_comment(" line goes out with whatever was owed from the last BEFORE,");
    asm_comment(" and its own count becomes what the next line owes. Once the");
    asm_comment(" two can add up the total is not known until run time, which");
    asm_comment(" is why this is a routine and not a few instructions inline.");
    asm_comment("");
    asm_comment(" The request is the line count, or -1 for PAGE, negated when");
    asm_comment(" the phrase was BEFORE.");
    asm_line("COBADV", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE7+4", "");
    asm_line("", "LA", "11,RTSAVE7", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(dcb)");
    asm_line("", "L", "3,4(0,1)", "A(buffer)");
    asm_line("", "L", "4,8(0,1)", "A(length)");
    asm_line("", "L", "5,12(0,1)", "A(owed)");
    asm_line("", "L", "6,16(0,1)", "A(request)");
    asm_line("", "L", "10,20(0,1)", "A(LINAGE cells), or 0");
    asm_line("", "L", "11,24(0,1)", "A(LINAGE-COUNTER), or 0");
    asm_line("", "LH", "7,0(0,4)", "the record length");
    asm_line("", "LTR", "7,7", "");
    asm_line("", "LH", "8,0(0,6)", "the request");
    asm_line("", "LH", "9,0(0,5)", "what the last BEFORE left owing");
    asm_line("", "LTR", "8,8", "BEFORE is the negative side");
    asm_line("", "BM", "ADV100", "");
    /* AFTER: this line's own count adds to what was owed, and nothing is left.
     * A page skip or a channel skip -- anything 999 or more -- swallows what
     * was owed, and one already owed stays whatever this asks. */
    asm_line("", "CH", "8,ADVPAGE", "AFTER PAGE, or a channel?");
    asm_line("", "BNL", "ADV020", "");
    asm_line("", "CH", "9,ADVPAGE", "was a skip already owed?");
    asm_line("", "BNL", "ADV030", "then it stays one, whatever this asks");
    asm_line("", "AR", "9,8", "owed plus this one");
    asm_line("", "B", "ADV030", "");
    asm_line("ADV020", "LR", "9,8", "a skip swallows what was owed");
    asm_line("ADV030", "XC", "0(2,5),0(5)", "nothing owed after an AFTER");
    asm_line("", "B", "ADV150", "");
    /* BEFORE: the line goes out on what was owed, and owes its own count. */
    asm_line("ADV100", "LCR", "8,8", "back to a positive request");
    asm_line("ADV110", "STH", "8,0(0,5)", "this is what the next line owes");
    asm_line("", "LTR", "9,9", "nothing owed?");
    asm_line("", "BNZ", "ADV150", "");
    asm_line("", "LH", "9,ADVONE", "then this line simply takes the next one");
    /* R9 now holds the advance to apply before printing.
     *
     * LINAGE: the logical page. A skip of any kind goes to the first line of
     * the next page's body; a count that would run past the body does the
     * same; otherwise the counter advances by the count. END-OF-PAGE is the
     * counter reaching FOOTING, or, with no FOOTING, a new page. */
    /* R8 carries the END-OF-PAGE answer from here: the PUTs below clobber
     * R15 (and R0, R1, R14), so it is moved into R15 only at the end. */
    asm_line("ADV150", "SR", "8,8", "no END-OF-PAGE yet");
    asm_line("", "LTR", "10,10", "a LINAGE file?");
    asm_line("", "BZ", "ADV200", "");
    asm_line("", "CH", "9,ADVPAGE", "a skip?");
    asm_line("", "BNL", "ADV160", "");
    asm_line("", "LH", "14,0(0,11)", "LINAGE-COUNTER");
    asm_line("", "AR", "14,9", "");
    asm_line("", "CH", "14,0(0,10)", "past the body?");
    asm_line("", "BH", "ADV160", "");
    asm_line("", "STH", "14,0(0,11)", "");
    asm_line("", "LH", "0,2(0,10)", "FOOTING");
    asm_line("", "LTR", "0,0", "");
    asm_line("", "BZ", "ADV200", "no FOOTING: no END-OF-PAGE short of the page");
    asm_line("", "CR", "14,0", "");
    asm_line("", "BL", "ADV200", "");
    asm_line("", "LA", "8,1", "END-OF-PAGE");
    asm_line("", "B", "ADV200", "");
    asm_line("ADV160", "LH", "14,ADVONE", "");
    asm_line("", "STH", "14,0(0,11)", "counter back to 1");
    asm_line("", "LA", "8,1", "a new page is END-OF-PAGE without FOOTING");
    asm_line("", "LH", "0,2(0,10)", "");
    asm_line("", "LTR", "0,0", "");
    asm_line("", "BZ", "ADV170", "");
    asm_line("", "SR", "8,8", "with FOOTING, only the footing is");
    asm_line("ADV170", "LH", "9,4(0,10)", "LINES AT TOP");
    asm_line("", "LTR", "9,9", "");
    asm_line("", "BNZ", "ADV175", "");
    asm_line("", "LH", "9,ADVPAGE", "no top margin: the line itself carries the eject");
    asm_line("", "B", "ADV200", "");
    asm_line("ADV175", "PUT", "(2),ADVB1", "eject on a blank line; R9 survives it");
    asm_line("ADV200", "CH", "9,ADVPAGE", "a page skip?");
    asm_line("", "BL", "ADV210", "");
    asm_line("", "BH", "ADV205", "a channel");
    asm_line("", "MVI", "0(3),C'1'", "skip to a new page");
    asm_line("", "B", "ADV300", "");
    asm_line("ADV205", "LA", "10,ADVCHAN", "");
    asm_line("", "AR", "10,9", "");
    asm_line("", "SH", "10,ADVCHOF", "request 1001 is the first code");
    asm_line("", "MVC", "0(1,3),0(10)", "the channel's ASA code");
    asm_line("", "B", "ADV300", "");
    asm_line("ADV210", "LTR", "9,9", "");
    asm_line("", "BNM", "ADV220", "");
    asm_line("", "SR", "9,9", "never negative here");
    /* More than three lines is blank lines first, three at a time. They come
     * from the runtime's own constant rather than from the caller's buffer:
     * the record to print is already sitting in that. */
    asm_line("ADV220", "CH", "9,ADVTHREE", "more than one code can carry?");
    asm_line("", "BNH", "ADV240", "");
    asm_line("", "PUT", "(2),ADVB3", "three blank lines at a time");
    asm_line("", "SH", "9,ADVTHREE", "");
    asm_line("", "B", "ADV220", "");
    asm_line("ADV240", "LA", "10,ADVCODE", "");
    asm_line("", "AR", "10,9", "");
    asm_line("", "MVC", "0(1,3),0(10)", "'+', ' ', '0' or '-'");
    asm_line("ADV300", "PUT", "(2),(3)", "the line itself");
    asm_line("", "L", "13,4(13)", "");
    asm_line("", "ST", "8,16(13)", "END-OF-PAGE, into R15's slot for the LM");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "BR", "14", "");
    asm_line("ADVB3", "DC", "C'-'", "a blank line that advances three");
    asm_line("", "DC", "CL132' '", "");
    asm_line("ADVB1", "DC", "C'1'", "a blank line that ejects");
    asm_line("", "DC", "CL132' '", "");
    asm_line("ADVCHAN", "DC", "C'123456789ABC+'", "channels 1-12, CSP");
    asm_line("ADVCHOF", "DC", "H'1001'", "");
    asm_line("ADVCODE", "DC", "C'+ 0-'", "0, 1, 2 or 3 lines");
    asm_line("ADVONE", "DC", "H'1'", "");
    asm_line("ADVPAGE", "DC", "H'999'", "the page-skip request");
    asm_line("ADVTHREE", "DC", "H'3'", "");
    asm_line("RTSAVE7", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBUPSI -- set the eight switches from the EXEC PARM.");
    asm_comment("");
    asm_comment(" PARM='/UPSI(10100000)' is the form IBM's later compilers");
    asm_comment(" take, and the one the runtime looks for: the literal UPSI");
    asm_comment(" anywhere in the parameter text, then the next eight 0 or 1");
    asm_comment(" characters, leftmost being UPSI-0. Anything else leaves all");
    asm_comment(" eight off, which is the documented default.");
    asm_line("COBUPSI", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE6+4", "");
    asm_line("", "LA", "11,RTSAVE6", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(parameter text)");
    asm_line("", "LA", "2,0(0,2)", "drop the end-of-list bit");
    asm_line("", "XC", "RTUPSI,RTUPSI", "all off unless the PARM says otherwise");
    asm_line("", "LH", "3,0(0,2)", "its length");
    asm_line("", "LA", "4,2(0,2)", "the text itself");
    asm_line("", "CH", "3,UPSIMIN", "room for UPSI and eight digits?");
    asm_line("", "BL", "UPSX", "");
    asm_line("", "SH", "3,UPSIMIN", "");
    asm_line("", "LA", "3,1(3)", "positions worth trying");
    asm_line("UPS10", "CLC", "0(4,4),UPSITAG", "");
    asm_line("", "BE", "UPS15", "");
    asm_line("", "LA", "4,1(4)", "");
    asm_line("", "BCT", "3,UPS10", "");
    asm_line("", "B", "UPSX", "no UPSI in the parameter");
    asm_line("UPS15", "LA", "4,4(4)", "past the tag");
    asm_line("", "CLI", "0(4),C'0'", "");
    asm_line("", "BE", "UPS20", "");
    asm_line("", "CLI", "0(4),C'1'", "");
    asm_line("", "BE", "UPS20", "");
    asm_line("", "LA", "4,1(4)", "skip a ( or an =");
    asm_line("UPS20", "SR", "5,5", "the byte being built");
    asm_line("", "LA", "6,8", "eight of them");
    asm_line("UPS30", "SLL", "5,1", "");
    asm_line("", "CLI", "0(4),C'1'", "");
    asm_line("", "BNE", "UPS40", "");
    asm_line("", "LA", "5,1(5)", "");
    asm_line("UPS40", "LA", "4,1(4)", "");
    asm_line("", "BCT", "6,UPS30", "");
    asm_line("", "ST", "5,RTUPSI", "the byte, for the caller to store");
    /* The byte goes back in R15, and it has to be put in the caller's save
     * area *before* the LM: the LM restores R12 too, and R12 is the base this
     * routine's own constants are addressed through. Loading it afterwards
     * reads RTUPSI through the caller's R12. */
    asm_line("UPSX", "L", "13,4(13)", "");
    asm_line("", "L", "15,RTUPSI", "the byte, while R12 is still ours");
    asm_line("", "ST", "15,16(13)", "into the save area's R15 slot");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "BR", "14", "");
    asm_comment("");
    asm_comment(" COBACC -- one transfer from SYSIN into the caller's item.");
    asm_comment("");
    asm_comment(" General rule 2 on II-53 leaves the size of a transfer to the");
    asm_comment(" implementor: here it is one 80-column record. The buffer is");
    asm_comment(" blanked first, so a receiver wider than a card is padded and");
    asm_comment(" a read past the last card returns spaces rather than the");
    asm_comment(" card before it.");
    asm_comment("");
    asm_comment(" Parameter list: A(item), A(halfword length).");
    asm_line("COBACC", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE5+4", "");
    asm_line("", "LA", "11,RTSAVE5", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(item)");
    asm_line("", "L", "3,4(0,1)", "A(length)");
    asm_line("", "LH", "4,0(0,3)", "length still to fill");
    /* Level 2 lifts the one-transfer rule: an item wider than a card is
     * filled from as many cards as it takes, eighty bytes at a time. */
    asm_line("COBA005", "LTR", "4,4", "");
    asm_line("", "BNP", "COBA040", "filled");
    asm_line("", "MVI", "ACCBUF,C' '", "");
    asm_line("", "MVC", "ACCBUF+1(255),ACCBUF", "blank the buffer");
    asm_line("", "CLI", "ACCEOFF,X'01'", "already at end of file?");
    asm_line("", "BE", "COBA020", "");
    asm_line("", "CLI", "ACCOPEN,X'01'", "already open?");
    asm_line("", "BE", "COBA010", "");
    asm_line("", "OPEN", "(ACCDCB,INPUT)", "");
    asm_line("", "MVI", "ACCOPEN,X'01'", "");
    asm_line("COBA010", "LA", "1,COBA030", "");
    asm_line("", "STCM", "1,7,ACCDCB+33", "into DCBEODAD");
    asm_line("", "GET", "ACCDCB,ACCBUF", "one card");
    asm_line("", "B", "COBA020", "");
    asm_line("COBA030", "MVI", "ACCEOFF,X'01'", "end of file: the buffer stays blank");
    asm_line("COBA020", "LR", "5,4", "");
    asm_line("", "CH", "5,ACCCARD", "");
    asm_line("", "BNH", "COBA035", "");
    asm_line("", "LH", "5,ACCCARD", "one card's worth");
    asm_line("COBA035", "BCTR", "5,0", "EX wants length-1");
    asm_line("", "EX", "5,COBAMVC", "");
    asm_line("", "LA", "2,1(5,2)", "past what was stored");
    asm_line("", "LA", "5,1(5)", "");
    asm_line("", "SR", "4,5", "");
    asm_line("", "B", "COBA005", "");
    asm_line("COBA040", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("COBAMVC", "MVC", "0(0,2),ACCBUF", "patched by EX");
    asm_line("ACCCARD", "DC", "H'80'", "a card");
    asm_comment("");
    asm_comment(" COBTERM -- close SYSOUT if COBDISP ever opened it.");
    asm_comment("");
    asm_line("COBTERM", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE2+4", "");
    asm_line("", "LA", "11,RTSAVE2", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "CLI", "ACCOPEN,X'01'", "");
    asm_line("", "BNE", "COBT005", "");
    asm_line("", "CLOSE", "(ACCDCB)", "");
    asm_line("", "MVI", "ACCOPEN,X'00'", "");
    asm_line("COBT005", "CLI", "RTOPEN,X'01'", "");
    asm_line("", "BNE", "COBT010", "");
    asm_line("", "CLOSE", "(RTDCB)", "");
    asm_line("", "MVI", "RTOPEN,X'00'", "");
    asm_line("COBT010", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_comment("");
    asm_comment(" COBDATE -- fill an 8-byte field with MM/DD/YY.");
    asm_comment("   R1 -> the field.  TIME DEC hands back the date as packed");
    asm_comment("   00YYDDDF, which is Julian, so the day of year has to be");
    asm_comment("   walked into a month and a day.");
    asm_comment("");
    asm_line("COBDATE", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE4+4", "");
    asm_line("", "LA", "11,RTSAVE4", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "LR", "2,1", "the field, before TIME takes R1");
    asm_line("", "TIME", "DEC", "R0 = HHMMSSth, R1 = 00YYDDDF");
    asm_line("", "ST", "1,DTPACK", "");
    asm_line("", "UNPK", "DTZONE(7),DTPACK(4)", "'00YYDDD'");
    asm_line("", "OI", "DTZONE+6,X'F0'", "");
    asm_line("", "PACK", "DTDW(8),DTZONE+4(3)", "");
    asm_line("", "CVB", "3,DTDW", "day of the year");
    asm_line("", "PACK", "DTDW(8),DTZONE+2(2)", "");
    asm_line("", "CVB", "4,DTDW", "the year");
    asm_line("", "N", "4,DTF3", "zero if a leap year -- 1901-2099, so");
    asm_comment("                             mod 4 is the whole rule");
    asm_line("", "LA", "5,1", "month");
    asm_line("", "LA", "6,DTMON", "");
    asm_line("DT010", "SR", "7,7", "");
    asm_line("", "IC", "7,0(0,6)", "days in this month");
    asm_line("", "CH", "5,DTH2", "February?");
    asm_line("", "BNE", "DT020", "");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BNZ", "DT020", "");
    asm_line("", "LA", "7,1(0,7)", "29 this year");
    asm_line("DT020", "CR", "3,7", "");
    asm_line("", "BNH", "DT030", "the day falls in this month");
    asm_line("", "SR", "3,7", "");
    asm_line("", "LA", "5,1(0,5)", "");
    asm_line("", "LA", "6,1(0,6)", "");
    asm_line("", "B", "DT010", "");
    asm_line("DT030", "CVD", "5,DTDW", "");
    asm_line("", "UNPK", "0(2,2),DTDW+6(2)", "MM");
    asm_line("", "OI", "1(2),X'F0'", "");
    asm_line("", "MVI", "2(2),C'/'", "");
    asm_line("", "CVD", "3,DTDW", "");
    asm_line("", "UNPK", "3(2,2),DTDW+6(2)", "DD");
    asm_line("", "OI", "4(2),X'F0'", "");
    asm_line("", "MVI", "5(2),C'/'", "");
    asm_line("", "MVC", "6(2,2),DTZONE+2", "YY, already zoned");
    asm_line("", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("DTMON", "DC", "AL1(31,28,31,30,31,30,31,31,30,31,30,31)", "");
    asm_line("DTF3", "DC", "F'3'", "");
    asm_line("DTH2", "DC", "H'2'", "");
    asm_line("DTPACK", "DS", "F", "");
    asm_line("DTZONE", "DS", "CL8", "");
    asm_line("DTDW", "DS", "D", "");
    asm_line("RTSAVE4", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBWRL -- advance the paper and write one report line.");
    asm_comment("   R1 -> A(dcb), A(current line), A(target line), A(buffer)");
    asm_comment("");
    asm_line("COBWRL", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE3+4", "");
    asm_line("", "LA", "11,RTSAVE3", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(dcb)");
    asm_line("", "L", "3,4(0,1)", "A(current line)");
    asm_line("", "L", "4,8(0,1)", "A(target line)");
    asm_line("", "L", "5,12(0,1)", "A(buffer)");
    asm_line("", "LH", "6,0(0,3)", "");
    asm_line("", "LH", "7,0(0,4)", "");
    asm_line("COBW010", "LA", "8,1(0,6)", "");
    asm_line("", "CR", "8,7", "already at the line before the target?");
    asm_line("", "BNL", "COBW020", "");
    asm_line("", "PUT", "(2),RTBLNK", "skip a line");
    asm_line("", "LA", "6,1(0,6)", "");
    asm_line("", "B", "COBW010", "");
    asm_line("COBW020", "PUT", "(2),(5)", "");
    asm_line("", "STH", "7,0(0,3)", "current line = target");
    asm_line("", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("RTBLNK", "DC", "CL133' '", "a blank line, ASA single space");
    asm_line("RTSAVE3", "DS", "18F", "");
    asm_line("RTOPEN", "DC", "X'00'", "");
    asm_line("RTMAX", "DC", "H'120'", "");
    asm_line("RTLINE", "DC", "CL121' '", "ASA byte + 120 columns");
    asm_line("RTSAVE1", "DS", "18F", "");
    asm_line("RTSAVE2", "DS", "18F", "");
    asm_line("RTSAVE5", "DS", "18F", "");
    asm_line("RTSAVE6", "DS", "18F", "");
    asm_line("RTUPSI", "DS", "F", "");
    asm_line("UPSITAG", "DC", "C'UPSI'", "");
    asm_line("UPSIMIN", "DC", "H'12'", "UPSI, a bracket, eight digits");
    asm_line("ACCOPEN", "DC", "X'00'", "");
    asm_line("ACCEOFF", "DC", "X'00'", "");
    asm_line("ACCBUF", "DC", "CL256' '", "one card from SYSIN, blank padded");
    asm_line("ACCMAX", "DC", "H'256'", "");
    asm_cont("ACCDCB   DCB   DDNAME=SYSIN,DSORG=PS,MACRF=(GM),",
             "EODAD=0");
    asm_cont("RTDCB    DCB   DDNAME=SYSOUT,DSORG=PS,MACRF=(PM),RECFM=FBA,",
             "LRECL=121,BLKSIZE=121");
    /* The routines added for level 2 -- STRING and UNSTRING, the console
     * forms, DATE/DAY/TIME, the any-length move -- come after the shared
     * data rather than before it. Each has its own base register and its
     * own data, so nothing they add pushes RTDCB or the buffers out of
     * reach of the routines that were here first, which is what happened
     * once the CSECT passed 4K. */
    emit_string_runtime();
    asm_comment("");
    asm_comment(" COBWTO -- DISPLAY UPON CONSOLE. R1 -> A(text), A(halfword length).");
    asm_comment(" The text goes into a WTO list whose length halfword is set");
    asm_comment(" from the parameter; MVS caps a line at what it will show.");
    asm_line("COBWTO", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE10+4", "");
    asm_line("", "LA", "11,RTSAVE10", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(text)");
    asm_line("", "L", "3,4(0,1)", "A(length)");
    asm_line("", "LH", "4,0(0,3)", "");
    asm_line("", "CH", "4,WTOMAX", "");
    asm_line("", "BNH", "WTO010", "");
    asm_line("", "LH", "4,WTOMAX", "");
    asm_line("WTO010", "MVI", "WTOTXT,C' '", "");
    asm_line("", "MVC", "WTOTXT+1(119),WTOTXT", "");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BNP", "WTO020", "");
    asm_line("", "BCTR", "4,0", "");
    asm_line("", "EX", "4,WTOMVC", "");
    asm_line("", "LA", "4,1(4)", "");
    asm_line("WTO020", "LA", "4,4(4)", "plus the header");
    asm_line("", "STH", "4,WTOLST", "");
    asm_line("", "WTO", "MF=(E,WTOLST)", "");
    asm_line("", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("WTOMVC", "MVC", "WTOTXT(0),0(2)", "executed");
    asm_line("WTOMAX", "DC", "H'120'", "");
    asm_line("WTOLST", "DC", "AL2(124),AL2(0)", "length, MCS flags");
    asm_line("WTOTXT", "DC", "CL120' '", "");
    asm_line("RTSAVE10", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBWTOR -- ACCEPT FROM CONSOLE. R1 -> A(item), A(halfword length).");
    asm_comment(" A WTOR asks the operator, the task waits on its ECB, and the");
    asm_comment(" reply is moved to the item space padded. One line only.");
    asm_line("COBWTOR", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE11+4", "");
    asm_line("", "LA", "11,RTSAVE11", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "A(item)");
    asm_line("", "L", "3,4(0,1)", "A(length)");
    asm_line("", "LH", "4,0(0,3)", "");
    asm_line("", "MVI", "WTORRPL,C' '", "");
    asm_line("", "MVC", "WTORRPL+1(119),WTORRPL", "");
    asm_line("", "XC", "WTORECB,WTORECB", "");
    asm_line("", "WTOR", "'COBC370: ACCEPT FROM CONSOLE',WTORRPL,120,WTORECB", "");
    asm_line("", "WAIT", "ECB=WTORECB", "");
    asm_line("", "LTR", "4,4", "");
    asm_line("", "BNP", "WTOR20", "");
    asm_line("", "CH", "4,WTORMAX", "");
    asm_line("", "BNH", "WTOR10", "");
    asm_line("", "LH", "4,WTORMAX", "");
    asm_line("WTOR10", "BCTR", "4,0", "");
    asm_line("", "EX", "4,WTORMVC", "");
    asm_line("WTOR20", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("WTORMVC", "MVC", "0(0,2),WTORRPL", "executed");
    asm_line("WTORMAX", "DC", "H'120'", "");
    asm_line("WTORECB", "DC", "F'0'", "");
    asm_line("WTORRPL", "DC", "CL120' '", "");
    asm_line("RTSAVE11", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBADT -- ACCEPT FROM DATE, DAY or TIME. R1 -> A(area), A(halfword");
    asm_comment(" kind: 1 DATE, 2 DAY, 3 TIME). Writes YYMMDD, YYDDD or HHMMSSth as");
    asm_comment(" zoned digits into the area. DATE borrows COBDATE's month walk.");
    asm_line("COBADT", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE12+4", "");
    asm_line("", "LA", "11,RTSAVE12", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "the area");
    asm_line("", "L", "3,4(0,1)", "");
    asm_line("", "LH", "3,0(0,3)", "the kind");
    asm_line("", "CH", "3,ADTH3", "");
    asm_line("", "BE", "ADT300", "TIME");
    asm_line("", "TIME", "DEC", "R1 = 00YYDDDF");
    asm_line("", "ST", "1,ADTPK", "");
    asm_line("", "UNPK", "ADTZ(7),ADTPK(4)", "'00YYDDD'");
    asm_line("", "OI", "ADTZ+6,X'F0'", "");
    asm_line("", "CH", "3,ADTH2", "");
    asm_line("", "BE", "ADT200", "DAY");
    asm_line("", "LA", "1,ADTB", "DATE: MM/DD/YY first");
    asm_line("", "L", "15,ADTVD", "");
    asm_line("", "BALR", "14,15", "");
    asm_line("", "MVC", "0(2,2),ADTB+6", "YY");
    asm_line("", "MVC", "2(2,2),ADTB", "MM");
    asm_line("", "MVC", "4(2,2),ADTB+3", "DD");
    asm_line("", "B", "ADTX", "");
    asm_line("ADT200", "MVC", "0(5,2),ADTZ+2", "YYDDD");
    asm_line("", "B", "ADTX", "");
    asm_line("ADT300", "TIME", "DEC", "R0 = HHMMSSth");
    asm_line("", "ST", "0,ADTPK", "");
    asm_line("", "MVI", "ADTPK+4,X'0F'", "a sign nibble to unpack against");
    asm_line("", "UNPK", "ADTZ(9),ADTPK(5)", "'HHMMSSth0'");
    asm_line("", "MVC", "0(8,2),ADTZ", "");
    asm_line("ADTX", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("ADTVD", "DC", "A(COBDATE)", "");
    asm_line("ADTH2", "DC", "H'2'", "");
    asm_line("ADTH3", "DC", "H'3'", "");
    asm_line("ADTPK", "DS", "2F", "");
    asm_line("ADTZ", "DS", "CL9", "");
    asm_line("ADTB", "DS", "CL8", "");
    asm_line("RTSAVE12", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBDCAL -- CALL identifier. R1 -> A(8-byte name), A(parameter list).");
    asm_comment(" A table of programs loaded so far: a name found there is called");
    asm_comment(" at the entry point remembered; one not found is LOADed and");
    asm_comment(" remembered, so each program is loaded once until CANCELled.");
    asm_comment(" The callee's return code comes back in R15.");
    asm_line("COBDCAL", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE14+4", "");
    asm_line("", "LA", "11,RTSAVE14", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "the name");
    asm_line("", "L", "3,4(0,1)", "the parameter list");
    asm_line("", "LA", "4,DCTAB", "");
    asm_line("", "LA", "5,16", "entries");
    asm_line("DCA010", "CLI", "0(4),X'00'", "an empty entry?");
    asm_line("", "BE", "DCA050", "then it is not loaded");
    asm_line("", "CLC", "0(8,4),0(2)", "this one?");
    asm_line("", "BE", "DCA030", "");
    asm_line("", "LA", "4,12(4)", "");
    asm_line("", "BCT", "5,DCA010", "");
    asm_line("", "LOAD", "EPLOC=(2)", "table full: load without remembering");
    asm_line("", "LR", "15,0", "");
    asm_line("", "B", "DCA040", "");
    asm_line("DCA050", "MVC", "0(8,4),0(2)", "remember the name");
    asm_line("", "LOAD", "EPLOC=(2)", "");
    asm_line("", "ST", "0,8(0,4)", "and the entry point");
    asm_line("DCA030", "L", "15,8(0,4)", "");
    asm_line("DCA040", "LR", "1,3", "R1 -> the callee's parameters");
    asm_line("", "BALR", "14,15", "");
    asm_line("", "L", "13,4(13)", "");
    asm_line("", "ST", "15,16(13)", "the callee's return code survives the LM");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "BR", "14", "");
    asm_line("RTSAVE14", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBCANC -- CANCEL. R1 -> A(8-byte name). A program in the table is");
    asm_comment(" DELETEd and forgotten; one that is not is left alone.");
    asm_line("COBCANC", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE15+4", "");
    asm_line("", "LA", "11,RTSAVE15", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "the name");
    asm_line("", "LA", "4,DCTAB", "");
    asm_line("", "LA", "5,16", "");
    asm_line("CAN010", "CLI", "0(4),X'00'", "");
    asm_line("", "BE", "CANX", "not loaded: nothing to do");
    asm_line("", "CLC", "0(8,4),0(2)", "");
    asm_line("", "BE", "CAN020", "");
    asm_line("", "LA", "4,12(4)", "");
    asm_line("", "BCT", "5,CAN010", "");
    asm_line("", "B", "CANX", "");
    asm_line("CAN020", "DELETE", "EPLOC=(2)", "release it");
    asm_line("", "XC", "0(12,4),0(4)", "and forget it");
    asm_line("CANX", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("DCTAB", "DC", "16XL12'00'", "loaded programs: CL8 name, A(entry)");
    asm_line("RTSAVE15", "DS", "18F", "");
    asm_comment("");
    asm_comment(" COBMVL -- an alphanumeric move of any length. R1 -> A(receiver),");
    asm_comment("   A(sender), A(halfword receiver length, halfword sender length).");
    asm_comment(" The shorter of the two is moved in 256-byte pieces; a receiver");
    asm_comment(" longer than the sender is space filled after it. Serves a group");
    asm_comment(" whose length depends on an OCCURS DEPENDING ON count, and any");
    asm_comment(" move too long for one MVC.");
    asm_line("COBMVL", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE13+4", "");
    asm_line("", "LA", "11,RTSAVE13", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "L", "2,0(0,1)", "receiver");
    asm_line("", "L", "3,4(0,1)", "sender");
    asm_line("", "L", "4,8(0,1)", "");
    asm_line("", "LH", "5,0(0,4)", "receiver length");
    asm_line("", "LH", "6,2(0,4)", "sender length");
    asm_line("", "LR", "7,5", "");
    asm_line("", "CR", "7,6", "");
    asm_line("", "BNH", "MVL010", "");
    asm_line("", "LR", "7,6", "bytes to move: the shorter");
    asm_line("MVL010", "SR", "5,7", "bytes to fill afterwards");
    asm_line("MVL020", "LTR", "7,7", "");
    asm_line("", "BNP", "MVL040", "moved");
    asm_line("", "LR", "8,7", "");
    asm_line("", "CH", "8,MVL256", "");
    asm_line("", "BNH", "MVL030", "");
    asm_line("", "LH", "8,MVL256", "a piece");
    asm_line("MVL030", "BCTR", "8,0", "");
    asm_line("", "EX", "8,MVLMVC", "");
    asm_line("", "LA", "8,1(8)", "");
    asm_line("", "AR", "2,8", "");
    asm_line("", "AR", "3,8", "");
    asm_line("", "SR", "7,8", "");
    asm_line("", "B", "MVL020", "");
    asm_line("MVL040", "LTR", "5,5", "");
    asm_line("", "BNP", "MVLX", "nothing to fill");
    asm_line("", "MVI", "0(2),C' '", "");
    asm_line("", "BCTR", "5,0", "");
    asm_line("MVL050", "LTR", "5,5", "");
    asm_line("", "BNP", "MVLX", "");
    asm_line("", "LR", "8,5", "");
    asm_line("", "CH", "8,MVL256", "");
    asm_line("", "BNH", "MVL060", "");
    asm_line("", "LH", "8,MVL256", "");
    asm_line("MVL060", "BCTR", "8,0", "");
    asm_line("", "EX", "8,MVLPAD", "propagate the space");
    asm_line("", "LA", "8,1(8)", "");
    asm_line("", "AR", "2,8", "");
    asm_line("", "SR", "5,8", "");
    asm_line("", "B", "MVL050", "");
    asm_line("MVLX", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("MVLMVC", "MVC", "0(0,2),0(3)", "executed");
    asm_line("MVLPAD", "MVC", "1(0,2),0(2)", "executed");
    asm_line("MVL256", "DC", "H'256'", "");
    asm_line("RTSAVE13", "DS", "18F", "");
}

/* Enter a range of procedures and come back, which is what a PERFORM does and
 * what a USE procedure needs: park the return address in the range's exit
 * cell, branch to its first paragraph, and put the fall-through back
 * afterwards so the range still runs straight through when nobody performed
 * it. */
static void gen_call_range(int a, int b, int *nret)
{
    char p1[16], x[16], f[16], r[16], t[64];
    snprintf(p1, sizeof p1, "P%04d", a);
    snprintf(x,  sizeof x,  "X%04d", b);
    snprintf(f,  sizeof f,  "F%04d", b);
    snprintf(r,  sizeof r,  "R%04d", ++*nret);
    snprintf(t, sizeof t, "15,%s", r);  asm_line("", "LA", t, "return here");
    snprintf(t, sizeof t, "15,%s", x);  asm_line("", "ST", t, "into the range's exit cell");
    asm_line("", "B", p1, "");
    asm_line(r, "DS", "0H", "");
    reset_bases();
    snprintf(t, sizeof t, "15,%s", f);  asm_line("", "LA", t, "restore fall-through");
    snprintf(t, sizeof t, "15,%s", x);  asm_line("", "ST", t, "");
}

/* Branch mnemonics after CP or CLC, by relation and by sense. */
static const char *br_true[]  = { "BE", "BL", "BH", "BNE", "BNH", "BNL" };
static const char *br_false[] = { "BNE", "BNL", "BNH", "BE", "BH", "BL" };


/* Can this numeric comparison be done as a byte compare? Only when both sides
 * are plain unsigned DISPLAY items of identical picture: same digit count,
 * same scale, and stored one digit per byte with no sign position, no P
 * scaling and no editing. Anything else -- a sign, COMP or COMP-3, a literal,
 * an expression, differing pictures -- takes the packed path. */
static int clc_operand(const Node *n)
{
    if (n->kind != N_SYM) return 0;
    const Sym *y = &syms[n->sym];
    return !y->is_group && !y->is_alpha && !y->is_88 && !y->is_index
        && y->usage == U_DISPLAY && !y->is_signed && !y->edited
        && !y->sgn_lead && !y->sgn_sep
        && y->digits > 0 && y->bytes == y->digits && y->bytes <= 256;
}

static int clc_comparable(const Node *l, const Node *r)
{
    if (!clc_operand(l) || !clc_operand(r)) return 0;
    const Sym *a = &syms[l->sym], *b = &syms[r->sym];
    /* Same digits and same scale means the two are aligned already; without
     * that the decimal points would have to be lined up first. */
    return a->digits == b->digits && a->scale == b->scale;
}

static int node_alpha(const Node *n)
{
    if (n->kind == N_STR) return 1;
    if (n->kind == N_SYM) return syms[n->sym].is_alpha || syms[n->sym].is_group;
    return 0;
}

static void gen_cond(Cond *c, int label, int jump_if_true)
{
    char b[128], l[16];
    switch (c->kind) {
    case C_NOT:
        gen_cond(c->cl, label, !jump_if_true);
        return;
    case C_AND:
        if (!jump_if_true) { gen_cond(c->cl, label, 0); gen_cond(c->cr, label, 0); }
        else {
            int skip = ++genlabel;
            gen_cond(c->cl, skip, 0);
            gen_cond(c->cr, label, 1);
            snprintf(l, sizeof l, "L%04d", skip); asm_line(l, "DS", "0H", "");
        }
        return;
    case C_OR:
        if (jump_if_true) { gen_cond(c->cl, label, 1); gen_cond(c->cr, label, 1); }
        else {
            int skip = ++genlabel;
            gen_cond(c->cl, skip, 1);
            gen_cond(c->cr, label, 0);
            snprintf(l, sizeof l, "L%04d", skip); asm_line(l, "DS", "0H", "");
        }
        return;
    case C_SWITCH: {
        /* One byte holds all eight switches, leftmost digit first, so UPSI-0
         * is X'80'. TM sets the condition code from the selected bits: all
         * zero, or -- with one bit selected -- all one. */
        snprintf(b, sizeof b, "UPSIB,X'%02X'", 0x80 >> c->sw_bit);
        asm_line("", "TM", b, "the switch");
        snprintf(l, sizeof l, "L%04d", label);
        asm_line("", (jump_if_true == c->sw_on) ? "BO" : "BZ", l, "");
        return;
    }
    case C_CLASS: {
        /* TRT scans a field against a 256-byte table and stops at the first
         * byte whose function code is not zero, so a table of zeros for the
         * acceptable characters and X'FF' everywhere else makes the whole test
         * one instruction and a branch: CC=0 means every byte was acceptable.
         * TRT sets R1 and R2 when it stops; nothing is live in them here. */
        const Sym *sy = &syms[c->cls_sym];
        int n = c->cls_sub ? sy->elem : sy->bytes;
        int want = jump_if_true ^ c->cls_not;   /* branch when the test holds */
        int fail = ++genlabel;
        char lf[16], f[64];
        snprintf(lf, sizeof lf, "L%04d", fail);
        snprintf(l, sizeof l, "L%04d", label);
        need_class_table(c->cls_alpha ? CLS_ALPHA : CLS_DIGIT);
        if (c->cls_alpha) {
            field_ref_m(sy, c->cls_sub, FR_SS_NOLEN, n, 6, f, sizeof f);
            snprintf(b, sizeof b, "%s(%d),CLSALF", f, n);
            asm_line("", "TRT", b, "every byte A-Z or space?");
            asm_line("", want ? "BZ" : "BNZ", l, "");
            asm_line(lf, "DS", "0H", "");
            reset_bases();
            return;
        }
        /* NUMERIC. An unsigned item is digits throughout; a signed one carries
         * its sign in one character position, which is the leading or trailing
         * digit for an overpunch and a '+' or '-' of its own for SEPARATE. */
        int sep = sy->sgn_sep, lead = sy->sgn_lead;
        int digits_off = (sep && lead) ? 1 : 0;
        int nd = sep ? n - 1 : n;
        if (sy->is_signed && sep) {
            int sgn_off = lead ? 0 : n - 1;
            field_ref_m(sy, c->cls_sub, FR_SS_NOLEN, n, 6, f, sizeof f);
            int ok = ++genlabel; char lo[16];
            snprintf(lo, sizeof lo, "L%04d", ok);
            snprintf(b, sizeof b, "%s+%d,C'+'", f, sgn_off);
            asm_line("", "CLI", b, "a separate sign is + or -");
            asm_line("", "BE", lo, "");
            snprintf(b, sizeof b, "%s+%d,C'-'", f, sgn_off);
            asm_line("", "CLI", b, "");
            asm_line("", "BNE", want ? lf : l, "");
            asm_line(lo, "DS", "0H", "");
            snprintf(b, sizeof b, "%s+%d(%d),CLSNUM", f, digits_off, nd);
            asm_line("", "TRT", b, "every other byte a digit?");
            asm_line("", want ? "BZ" : "BNZ", l, "");
            asm_line(lf, "DS", "0H", "");
            reset_bases();
            return;
        }
        field_ref_m(sy, c->cls_sub, FR_SS_NOLEN, n, 6, f, sizeof f);
        if (!sy->is_signed) {
            snprintf(b, sizeof b, "%s(%d),CLSNUM", f, n);
            asm_line("", "TRT", b, "every byte a digit?");
            asm_line("", want ? "BZ" : "BNZ", l, "");
            asm_line(lf, "DS", "0H", "");
            reset_bases();
            return;
        }
        /* Signed overpunch: the sign digit takes a table of its own, since a
         * C or D zone on it is valid there and nowhere else. */
        need_class_table(CLS_SIGN);
        int sgn_off = lead ? 0 : n - 1;
        int rest_off = lead ? 1 : 0;
        snprintf(b, sizeof b, "%s+%d(1),CLSSGN", f, sgn_off);
        asm_line("", "TRT", b, "a signed digit where the sign lives?");
        asm_line("", "BNZ", want ? lf : l, "");
        if (n > 1) {
            snprintf(b, sizeof b, "%s+%d(%d),CLSNUM", f, rest_off, n - 1);
            asm_line("", "TRT", b, "and digits everywhere else?");
            asm_line("", want ? "BZ" : "BNZ", l, "");
        } else if (want) asm_line("", "B", l, "");
        asm_line(lf, "DS", "0H", "");
        reset_bases();
        return;
    }
    case C_REL: {
        if (node_alpha(c->l) || node_alpha(c->r)) {
            /* Alphanumeric: compare over the longer operand, the shorter one
               space padded, which is what COBOL specifies. */
            const Node *L = c->l, *R = c->r;
            if (L->kind != N_SYM || node_alpha(L) == 0)
                { const Node *t = L; L = R; R = t; }
            if (L->kind != N_SYM || !node_alpha(L))
                die("alphanumeric comparison needs an identifier on one side");
            const Sym *ls = &syms[L->sym];
            int n = L->sub ? ls->elem : ls->bytes;
            char fl[64], fr[64];
            if (R->kind == N_STR && !R->fig && R->litlen > n) {
                /* A literal longer than the item: the item is space-padded to
                 * the literal's length. Compare the item's width, and only if
                 * that is equal compare spaces against the literal's tail. The
                 * condition code the branch below reads is then the whole
                 * comparison's. */
                const char *sl = intern_str(R->lit, R->litlen, R->litlen);
                char lx[16]; snprintf(lx, sizeof lx, "L%04d", ++genlabel);
                use_spcs = 1;
                field_ref(ls, L->sub, n, 6, fl, sizeof fl);
                snprintf(b, sizeof b, "%s,%s", fl, sl);
                asm_line("", "CLC", b, "the item's width");
                asm_line("", "BNE", lx, "");
                snprintf(b, sizeof b, "SPCS(%d),%s+%d", R->litlen - n, sl, n);
                asm_line("", "CLC", b, "spaces against the literal's tail");
                asm_line(lx, "DS", "0H", "");
                snprintf(l, sizeof l, "L%04d", label);
                asm_line("", jump_if_true ? br_true[c->op] : br_false[c->op], l, "");
                return;
            }
            if ((L->kind == N_SYM && var_len(&syms[L->sym]) && !L->sub) ||
                (R->kind == N_SYM && var_len(&syms[R->sym]) && !R->sub))
                die("comparison of an item whose length depends on an OCCURS DEPENDING ON count is not implemented; MOVE it to a fixed item first");
            if (R->kind == N_SYM && node_alpha(R)
                && (R->sub ? syms[R->sym].elem : syms[R->sym].bytes) != n) {
                /* Two items of different lengths: the shorter is space padded
                 * to the longer, II-42. Compare the common length; if equal,
                 * the longer one's tail against spaces -- on whichever side
                 * the longer one is, so the condition code reads the right
                 * way round. */
                const Sym *rs = &syms[R->sym];
                int rn = R->sub ? rs->elem : rs->bytes;
                int mn = n < rn ? n : rn;
                if (n > 256 || rn > 256) die("CLC is limited to 256 bytes");
                char lx[16]; snprintf(lx, sizeof lx, "L%04d", ++genlabel);
                use_spcs = 1;
                field_ref_m(ls, L->sub, FR_RX, n, 6, fl, sizeof fl);
                snprintf(b, sizeof b, "6,%s", fl); asm_line("", "LA", b, "the left item");
                field_ref_m(rs, R->sub, FR_RX, rn, 7, fr, sizeof fr);
                snprintf(b, sizeof b, "7,%s", fr); asm_line("", "LA", b, "the right item");
                snprintf(b, sizeof b, "0(%d,6),0(7)", mn);
                asm_line("", "CLC", b, "the common length");
                asm_line("", "BNE", lx, "");
                if (n > rn) snprintf(b, sizeof b, "%d(%d,6),SPCS", mn, n - mn);
                else        snprintf(b, sizeof b, "SPCS(%d),%d(7)", rn - mn, mn);
                asm_line("", "CLC", b, "the longer one's tail against spaces");
                asm_line(lx, "DS", "0H", "");
                snprintf(l, sizeof l, "L%04d", label);
                asm_line("", jump_if_true ? br_true[c->op] : br_false[c->op], l, "");
                return;
            }
            if (R->kind == N_STR) {
                const char *sl;
                if (R->fig == FIG_ALL) {
                    /* The unit repeated to the item's length -- a constant
                     * built here, since the length is known. */
                    char rep[MAXTOK];
                    if (n >= MAXTOK) die("ALL literal compared against an item wider than the token buffer");
                    for (int k = 0; k < n; k++) rep[k] = R->lit[k % R->litlen];
                    rep[n] = 0;
                    sl = intern_str(rep, n, n);
                } else if (R->fig) {
                    sl = fig_run(R->fig);
                } else {
                    sl = intern_str(R->lit, R->litlen, n);
                }
                field_ref(ls, L->sub, n, 6, fl, sizeof fl);
                snprintf(b, sizeof b, "%s,%s", fl, sl);
            } else if (R->kind == N_SYM && node_alpha(R)) {
                const Sym *rs = &syms[R->sym];
                int rn = R->sub ? rs->elem : rs->bytes;
                if (rn != n)
                    die("comparing alphanumeric items of different lengths is "
                        "not implemented yet");
                field_ref_m(rs, R->sub, FR_SS_NOLEN, n, 7, fr, sizeof fr);
                field_ref(ls, L->sub, n, 6, fl, sizeof fl);
                snprintf(b, sizeof b, "%s,%s", fl, fr);
            } else die("mixed alphanumeric and numeric comparison is not implemented yet");
            if (n > 256) die("CLC is limited to 256 bytes");
            asm_line("", "CLC", b, "alphanumeric compare");
        } else if (clc_comparable(c->l, c->r)) {
            /* Two unsigned DISPLAY items of the same picture are zoned F0
             * through F9, so a byte compare orders them exactly as a decimal
             * compare would -- one CLC instead of two PACKs and a CP, and the
             * PACKs were into a 16-byte work area whatever the field's real
             * width. IBM's own compilers do this, which is most of why they
             * are faster on comparison-heavy loops.
             *
             * The one behavioural difference: CP raises a data exception on a
             * field holding non-numeric bytes and CLC does not. Unsigned
             * DISPLAY receivers are stored with an F zone forced, so the only
             * way to get here with junk is to read it from a file -- where
             * IKFCBL00 is equally silent. */
            const Sym *ls = &syms[c->l->sym], *rs = &syms[c->r->sym];
            char fl[64], fr[64];
            field_ref(ls, c->l->sub, ls->bytes, 6, fl, sizeof fl);
            field_ref_m(rs, c->r->sub, FR_SS_NOLEN, rs->bytes, 7, fr, sizeof fr);
            snprintf(b, sizeof b, "%s,%s", fl, fr);
            asm_line("", "CLC", b, "unsigned zoned: a byte compare is exact");
        } else {
            int sl = gen_expr(c->l, 0, 0);
            int sr = gen_expr(c->r, 1, 0);
            int ws = sl > sr ? sl : sr;
            gen_rescale16("WK0", sl, ws, 0);
            gen_rescale16("WK1", sr, ws, 0);
            asm_line("", "CP", "WK0(16),WK1(16)", "numeric compare");
        }
        snprintf(l, sizeof l, "L%04d", label);
        asm_line("", jump_if_true ? br_true[c->op] : br_false[c->op], l, "");
        return;
    }
    }
}

/* One data item to another, numeric or alphanumeric. Factored out so report
 * SOURCE placement reuses it, editing included. */
static void emit_move(const Sym *d, Node *dsub, const Sym *sv, Node *ssub)
{
    if ((d->is_alpha || d->is_group) && (sv->is_alpha || sv->is_group)) {
        gen_move_alpha(d, dsub, sv, ssub);
        return;
    }
    if (d->is_alpha || d->is_group) {
        /* Numeric to alphanumeric. Rule 3c on II-75 allows it only for an
         * integer, and rule 4a says the receiving item is filled from the left
         * and space filled -- which is the ordinary alphanumeric move -- with
         * the operational sign NOT moved. */
        if (sv->scale != 0)
            die("MOVE of a non-integer numeric item to an alphanumeric item is "
                "not allowed -- rule 3c on II-75");
        if (sv->usage == U_DISPLAY) {
            /* Already a string of digits. */
            gen_move_alpha(d, dsub, sv, ssub);
            if (sv->is_signed) {
                /* The last byte carries the sign as an overpunch. It is moved
                 * as a character and then made a plain digit again, which is
                 * what "the operational sign will not be moved" means for a
                 * trailing sign. Only if it was moved at all: a sender wider
                 * than the receiver is truncated on the right first. */
                int dn = dsub ? d->elem : d->bytes;
                int sn = ssub ? sv->elem : sv->bytes;
                if (sn <= dn) {
                    char b[96];
                    if (dsub) snprintf(b, sizeof b, "%d(6),X'F0'", sn - 1);
                    else      snprintf(b, sizeof b, "%s+%d,X'F0'", d->label, sn - 1);
                    asm_line("", "OI", b, "the sign is not moved");
                }
            }
            return;
        }
        /* COMP or COMP-3: unpack into a zoned work area first, then move that.
         * ZWK is sized for the standard's widest numeric item. */
        {
            char b[96];
            int n = sv->digits;
            gen_load(sv, ssub, "PWK1");
            snprintf(b, sizeof b, "ZWK(%d),PWK1(16)", n);
            asm_line("", "UNPK", b, "packed -> zoned for an alphanumeric move");
            snprintf(b, sizeof b, "ZWK+%d,X'F0'", n - 1);
            asm_line("", "OI", b, "the sign is not moved");
            Sym tmp; memset(&tmp, 0, sizeof tmp);
            tmp.is_alpha = 1; tmp.bytes = tmp.elem = n;
            tmp.occ_parent = tmp.gparent = tmp.index_sym = tmp.askey_sym = -1;
            tmp.fd_file = tmp.redef_from = tmp.redef_cap = -1;
            snprintf(tmp.label, sizeof tmp.label, "ZWK");
            snprintf(tmp.name, sizeof tmp.name, "%s", sv->name);
            gen_move_alpha(d, dsub, &tmp, NULL);
            return;
        }
    }
    if (sv->is_alpha || sv->is_group)
        die("MOVE of an alphanumeric item to a numeric one is legal but not "
            "implemented yet -- the characters would have to be packed");
    if (sv->usage == U_COMP && d->usage == U_COMP && sv->scale == d->scale
        && !d->edited) {
        /* Binary to binary at the same scale is a copy. The general path spends
         * CVD, a ZAP into a 16-byte work area, a ZAP back out and CVB -- six
         * instructions and two decimal operations to move a fullword.
         *
         * The result is identical, not merely close. A narrowing move goes CVB
         * into a register and STH, which keeps the low halfword; a direct L and
         * STH keeps the same low halfword. Neither path forces a sign on an
         * unsigned receiver or truncates to the declared digit count, so the
         * stored bytes agree in every case. */
        char bb[128], fs[64], fd[64];
        field_ref(sv, ssub, 0, 7, fs, sizeof fs);
        snprintf(bb, sizeof bb, "2,%s", fs);
        asm_line("", sv->elem == 2 ? "LH" : "L", bb, "binary move, no decimal detour");
        field_ref(d, dsub, 0, 6, fd, sizeof fd);
        snprintf(bb, sizeof bb, "2,%s", fd);
        asm_line("", d->elem == 2 ? "STH" : "ST", bb, "");
        return;
    }
    gen_load(sv, ssub, "PWK1");
    gen_rescale("PWK1", sv->scale, d->scale);
    gen_store(d, dsub, "PWK1");
}

/* Render one report group: for each LINE, build the print buffer and hand it
 * to COBWRL, which advances the paper and writes. */
static void emit_report_group(int gi)
{
    char b[160], lab[16];
    RGroup *g = &rgroups[gi];
    Report *rp = &reports[g->report];
    snprintf(lab, sizeof lab, "RG%03d", gi);
    snprintf(b, sizeof b, " report group %s", g->name);
    asm_comment(b);
    asm_line(lab, "ST", "14,RGS", "save the return, COBWRL clobbers R14");

    for (int li = g->first_line; li < g->first_line + g->nline; li++) {
        RLine *l = &rlines[li];
        reset_bases();
        /* target line */
        if (l->absolute) {
            snprintf(b, sizeof b, "2,%d", l->n);
            asm_line("", "LA", b, "LINE n");
        } else {
            snprintf(b, sizeof b, "2,%s", rp->lbl_line);
            asm_line("", "LH", b, "");
            /* R2, not R0: register 0 as a base or index contributes nothing,
               so LA 0,n(0) loads n rather than adding to it. */
            snprintf(b, sizeof b, "2,%d(2)", l->n);
            asm_line("", "LA", b, "LINE PLUS n");
            if (li == g->first_line) {
                /* The first group printed on a fresh page goes at FIRST DETAIL,
                 * not wherever LINE PLUS n happens to land. The cell is set
                 * right after a page heading and consumed once. */
                char nf[16];
                snprintf(nf, sizeof nf, "L%04d", ++genlabel);
                snprintf(b, sizeof b, "3,%s", rp->lbl_first); asm_line("", "LH", b, "");
                asm_line("", "LTR", "3,3", "a page was just started?");
                asm_line("", "BZ", nf, "");
                asm_line("", "LR", "2,3", "then start at FIRST DETAIL");
                asm_line("", "SR", "3,3", "");
                snprintf(b, sizeof b, "3,%s", rp->lbl_first); asm_line("", "STH", b, "consume it");
                asm_line(nf, "DS", "0H", "");
            }
        }
        asm_line("", "STH", "2,RTGT", "");
        /* blank the buffer, carrying the pending carriage control */
        asm_line("", "MVC", "RBUF(1),RCTL", "carriage control");
        asm_line("", "MVI", "RCTL,C' '", "one eject only");
        asm_line("", "MVI", "RBUF+1,C' '", "");
        asm_line("", "MVC", "RBUF+2(131),RBUF+1", "blank the line");
        for (int fi = l->first_fld; fi < l->first_fld + l->nfld; fi++) {
            RField *f = &rfields[fi];
            const Sym *fs = &syms[f->sym];
            if (f->src >= 0) {
                emit_move(fs, NULL, &syms[f->src], NULL);
                reset_bases();
                need_sym_base(fs);
                snprintf(b, sizeof b, "RBUF+%d(%d),%s", f->column, fs->bytes, fs->label);
                asm_line("", "MVC", b, "COLUMN placement");
            } else {
                const char *sl = intern_str(f->lit, f->litlen, fs->bytes);
                snprintf(b, sizeof b, "RBUF+%d(%d),%s", f->column, fs->bytes, sl);
                asm_line("", "MVC", b, "COLUMN literal");
            }
        }
        snprintf(b, sizeof b, "1,RGP%03d", gi);
        asm_line("", "LA", b, "");
        asm_line("", "L", "15,VWRL", "");
        asm_line("", "BALR", "14,15", "");
    }
    asm_line("", "L", "14,RGS", "");
    asm_line("", "BR", "14", "");
}

/* Set or step a PERFORM VARYING identifier from an expression. */
static void emit_set_from_expr(const Sym *d, Node *e, int add)
{
    int rs = gen_expr(e, 0, d->scale);
    gen_rescale16("WK0", rs, d->scale, 0);
    if (add) {
        asm_line("", "ZAP", "PWK2(16),WK0(16)", "");
        gen_load(d, NULL, "PWK1");
        asm_line("", "AP", "PWK1(16),PWK2(16)", "");
    } else {
        asm_line("", "ZAP", "PWK1(16),WK0(16)", "");
    }
    gen_store(d, NULL, "PWK1");
}

/* Checks that need to know how a file is USED, not just how it is declared.
 * The OPEN modes are recorded as the PROCEDURE DIVISION is read, so none of
 * this can live in parse_data_division -- where an earlier version of the
 * DYNAMIC/I-O check sat, testing a flag that was still zero. */
static void resolve_file_use(void)
{
    for (int i = 0; i < nfile; i++) {
        File *f = &files[i];
        if (f->vsam) {
            if (f->access == 2 && f->opened_io)
                die("ACCESS IS DYNAMIC with OPEN I-O is not implemented yet -- "
                    "browsing wants OPTCD=NSP and updating wants UPD, and one "
                    "RPL cannot hold both");
            continue;
        }
        if (!f->opened_io || f->isam) continue;
        /* Updating a sequential file in place is QSAM's own update mode, not
         * BSAM: OPEN UPDAT, GET in locate mode, PUTX to write the block back.
         * BSAM would hand us raw blocks to deblock and rewrite by hand; QSAM
         * already does that, and blocked datasets and a short last block come
         * out right for free. */
        f->update = 1;
        if (f->opened_input || f->opened_output || f->opened_extend)
            die("a sequential file opened I-O cannot also be opened INPUT, "
                "OUTPUT or EXTEND -- update mode needs its own DCB");
        if (f->has_write)
            die("a sequential file opened I-O is read and rewritten, not "
                "written -- WRITE is for OUTPUT and EXTEND");
        if (f->reclen > 256)
            die("a record wider than 256 bytes needs a split MVC to be "
                "updated in place, which is not implemented");
    }
}

static void generate(void)
{
    char b[200], lab[24];
    int has_display = 0;
    resolve_file_use();
    for (int i = 0; i < nstmt; i++)
        if (stmts[i].op == ST_DISPLAY_LIT || stmts[i].op == ST_DISPLAY_ID)
            has_display = 1;

    asm_comment("---------------------------------------------------------------");
    {
        /* Always the base name. A comment may not reach column 72 and an
         * absolute path easily does -- but the better reason is that where
         * somebody keeps their source tree is not a property of the program,
         * and putting it here makes the generated code differ between two
         * checkouts of the same commit. */
        const char *sn = src.name, *slash = strrchr(sn, '/');
        if (slash) sn = slash + 1;
        snprintf(b, sizeof b, " Generated by cobc370 from %.40s", sn);
    }
    asm_comment(b);
    asm_comment(" Standard OS/360 entry linkage. SYS1.COBLIB is never referenced.");
    if (has_display || nreport)
        asm_comment(" DISPLAY and report output are served by our own COBRT runtime.");
    asm_comment("---------------------------------------------------------------");

    asm_line(progid, "CSECT", "", "");
    asm_line("", "STM", "14,12,12(13)", "save caller's registers");
    asm_line("", "BALR", "12,0", "first code base");
    /* BALR loads the address of the NEXT instruction, so the base label has to
       sit after it. Labelling the BALR itself puts every displacement two
       bytes out. */
    asm_line("COBBEG", "EQU", "*", "");
    asm_line("", "USING", "COBBEG,12", "");
    /* One base register covers 4096 bytes of code; a second doubles it. The
     * data no longer competes for this, since it lives in COBWS. */
    asm_line("", "LA", "11,2048(,12)", "second code base");
    asm_line("", "LA", "11,2048(,11)", "");
    asm_line("", "USING", "COBBEG+4096,11", "");
    asm_line("", "LA", "10,2048(,11)", "third code base");
    asm_line("", "LA", "10,2048(,10)", "");
    asm_line("", "USING", "COBBEG+8192,10", "");
    asm_line("", "ST", "13,SAVEAREA+4", "backward chain to caller");
    asm_line("", "LA", "0,SAVEAREA", "");
    asm_line("", "ST", "0,8(13)", "forward chain from caller");
    asm_line("", "LR", "13,0", "our save area is now current");

    if (nusing) {
        /* OS/360 linkage: R1 points at a list of fullword addresses, one per
         * argument, and R1 has survived the entry sequence untouched. Each is
         * stashed in that parameter's base cell; from then on a LINKAGE item is
         * addressed exactly like WORKING-STORAGE, just off a different cell. */
        asm_comment(" PROCEDURE DIVISION USING: the caller's parameter list");
        for (int k = 0; k < nusing; k++) {
            char b2[64];
            snprintf(b2, sizeof b2, "0,%d(0,1)", k * 4);
            asm_line("", "L", b2, "");
            snprintf(b2, sizeof b2, "0,PBL%04d", syms[using_parm[k]].link_area);
            asm_line("", "ST", b2, syms[using_parm[k]].name);
        }
    }
    if (curdate_sym >= 0) {
        /* Filled once, on entry. ANS COBOL refreshes the register at every
         * reference; this does not, which for a batch report is arguably the
         * better answer -- every page carries the same date even if the run
         * crosses midnight. Worth knowing rather than assuming. */
        const Sym *cd = &syms[curdate_sym];
        char fr[64];
        asm_comment(" CURRENT-DATE, from the system clock");
        need_sym_base(cd);
        field_ref_m(cd, NULL, FR_RX, cd->bytes, 6, fr, sizeof fr);
        snprintf(b, sizeof b, "1,%s", fr);
        asm_line("", "LA", b, "");
        asm_line("", "L", "15,VDATE", "");
        asm_line("", "BALR", "14,15", "");
        reset_bases();
    }
    if (uses_switches && !is_subprogram) {
        /* Before anything else touches R1: on entry to a main program it holds
         * the OS parameter list, which is where the UPSI string arrives. A
         * subprogram gets its caller's list instead, so its switches stay off
         * -- there is nowhere for them to come from. */
        asm_line("", "L", "15,VUPSI", "");
        asm_line("", "BALR", "14,15", "read the PARM");
        asm_line("", "STC", "15,UPSIB", "the eight switches");
    }
    /* The program mask. MVS dispatches a problem program with decimal overflow
     * enabled, so a ZAP of a value into a packed item too short for it took
     * an 0CA -- where COBOL, and IKFCBL00, truncate on the left. Clear the
     * whole mask: fixed-point overflow goes the same way, and the two
     * floating-point bits mean nothing here. */
    asm_line("", "SR", "0,0", "");
    asm_line("", "SPM", "0", "no overflow interrupts: high-order truncation");
    if (gen_lines) {
        /* Armed for every program interruption there is, and armed *here*:
         * the SPIE macro works through R1, which on entry to a subprogram is
         * the caller's parameter list. Arming before picking that up cost the
         * CALL roundtrip an S0C4 -- the linkage section had been filled from
         * whatever SPIE left behind. */
        /* Not codes 8, 10, 13 and 14: those are the maskable interruptions,
         * and naming them in a SPIE turns their program-mask bits ON. That is
         * how every packed result too wide for its item came to abend 0CA
         * instead of truncating -- the SPM above was being undone here. */
        asm_line("", "SPIE", "COBSPIE,((1,7),9,(11,12),15)", "report program checks by line");
    }

    int ndlit = 0, cur_para = -1, nret = 0;
    gen_use_nret = &nret;
    genlabel = nlabel;
    if (ndecl > 0 && decl_end_para >= 0) {
        /* Declaratives come first in the source and must not be fallen into --
         * syntax rule 3 on IV-32 keeps control from crossing either way. */
        char pd[16]; snprintf(pd, sizeof pd, "P%04d", decl_end_para);
        asm_comment(" branch around the declaratives");
        asm_line("", "B", pd, "");
    }
    for (int i = 0; i < nstmt; i++) {
        Stmt *st = &stmts[i];
        /* Which USE procedure could take an error on this statement. */
        gen_use_decl = -1;
        switch (st->op) {
        case ST_READ: case ST_WRITE: case ST_REWRITE: case ST_DELETE: case ST_START:
            if (!st->had_atend && !st->had_invalid) gen_use_decl = decl_for(st->dst);
            break;
        case ST_OPEN: case ST_CLOSE:
            gen_use_decl = decl_for(st->dst);
            break;
        }
        /* One label per statement, so a program check can be reported as a
         * line number rather than an address. The label costs nothing in the
         * object; the table it feeds is emitted at the end and is what the
         * SPIE exit walks. */
        if (gen_lines && st->line > 0 && nlinetab < MAXLINETAB &&
            st->op != ST_LABEL && st->op != ST_PARA) {
            char sl[16];
            snprintf(sl, sizeof sl, "T%04d", nlinetab);
            asm_line(sl, "DS", "0H", "");
            linetab[nlinetab].line = st->line;
            nlinetab++;
        }
        if (st->op == ST_PARA && cur_para >= 0 && paras[cur_para].is_range_end) {
            char x[16], f[16];
            snprintf(x, sizeof x, "X%04d", cur_para); snprintf(f, sizeof f, "F%04d", cur_para);
            asm_comment(" end of a PERFORM range: return through its cell");
            snprintf(b, sizeof b, "15,%s", x);  asm_line("", "L", b, "");
            asm_line("", "BR", "15", "");
            asm_line(f, "DS", "0H", "fall-through when not performed");
            reset_bases();
        }
        switch (st->op) {
        case ST_PARA: {
            reset_bases();
            char p[16];
            snprintf(p, sizeof p, "P%04d", st->dst);
            snprintf(b, sizeof b, " %s.", st->para);
            asm_comment(b);
            asm_line(p, "DS", "0H", "");
            cur_para = st->dst;
            break;
        }
        case ST_EXIT:
            asm_comment(" EXIT");
            break;
        case ST_LABEL: {
            reset_bases();
            char l[16]; snprintf(l, sizeof l, "L%04d", st->dst);
            asm_line(l, "DS", "0H", "");
            break;
        }
        case ST_BRANCH: {
            char l[16]; snprintf(l, sizeof l, "L%04d", st->dst);
            asm_line("", "B", l, "");
            break;
        }
        case ST_IFTEST:
            asm_comment(" IF");
            gen_cond(st->cond, st->dst, 0);
            break;
        case ST_OPEN: {
            File *f = &files[st->dst];
            snprintf(b, sizeof b, " OPEN %s %s",
                     st->src == 1 ? "INPUT" : st->src == 2 ? "OUTPUT"
                                  : st->src == 3 ? "I-O" : "EXTEND", f->name);
            asm_comment(b);
            if (f->vsam) {
                /* VSAM takes its mode from the ACB's MACRF, so OPEN names only
                 * the control block. */
                snprintf(b, sizeof b, "(%s)", f->label);
                asm_line("", "OPEN", b, "VSAM ACB");
                gen_vsam_status(f, NULL, vs_simple);
                reset_bases();
                break;
            }
            char lskip[16]; lskip[0] = 0;
            if (f->optional && st->src == 1) {
                /* SELECT OPTIONAL: the DD may be absent. DEVTYPE says so
                 * without opening anything; the file is then marked absent,
                 * READ takes AT END at once and CLOSE does nothing. */
                use_devtype = 1;
                snprintf(lskip, sizeof lskip, "L%04d", ++genlabel);
                snprintf(b, sizeof b, "%sN,DVAREA", f->label);
                asm_line("", "DEVTYPE", b, "is the DD there?");
                asm_line("", "LTR", "15,15", "");
                snprintf(b, sizeof b, "%sA,X'00'", f->label); asm_line("", "MVI", b, "present");
                asm_line("", "BZ", "*+12", "");
                snprintf(b, sizeof b, "%sA,X'01'", f->label); asm_line("", "MVI", b, "absent: the file is empty");
                asm_line("", "B", lskip, "");
            }
            snprintf(b, sizeof b, "(%s,%s)", f->label,
                     st->src == 3 ? "UPDAT" : st->src == 4 ? "EXTEND"
                     : st->src == 1 ? (f->reversed ? "RDBACK" : "INPUT") : "OUTPUT");
            asm_line("", "OPEN", b, st->src == 3 ? "QSAM update mode" : st->src == 4 ? "append" : "");
            if (lskip[0]) { asm_line(lskip, "DS", "0H", ""); reset_bases(); }
            if (f->isam == 2) {
                /* BISAM reads a whole BLOCK, not a record, and wants 16 bytes
                 * of working room in front of it. BLKSIZE is only known once
                 * OPEN has read the label, so the area is obtained here rather
                 * than assembled -- that way a BLOCK CONTAINS clause that is
                 * absent or wrong cannot corrupt storage. */
                snprintf(b, sizeof b, "0,%s+62", f->label);
                asm_line("", "LH", b, "DCBBLKSI, filled in by OPEN");
                asm_line("", "AH", "0,=H'16'", "ISAM's working prefix");
                asm_line("", "GETMAIN", "R,LV=(0)", "");
                snprintf(b, sizeof b, "1,DB%03d+12", st->dst);
                asm_line("", "ST", b, "area address into the DECB");
            }
            break;
        }
        case ST_CLOSE: {
            File *f = &files[st->dst];
            snprintf(b, sizeof b, " CLOSE %s", f->name);
            asm_comment(b);
            char lsk[16]; lsk[0] = 0;
            if (f->optional) {
                snprintf(lsk, sizeof lsk, "L%04d", ++genlabel);
                snprintf(b, sizeof b, "%sA,X'01'", f->label);
                asm_line("", "CLI", b, "absent?");
                asm_line("", "BE", lsk, "nothing to close");
            }
            if (st->close_opt == 2) {
                snprintf(b, sizeof b, "%s", f->label);
                asm_line("", "FEOV", b, "CLOSE REEL/UNIT: force end of volume");
            } else {
                snprintf(b, sizeof b, st->close_opt == 1 ? "(%s,LEAVE)" : "(%s)", f->label);
                asm_line("", "CLOSE", b, st->close_opt == 1 ? "WITH NO REWIND" : "");
            }
            if (lsk[0]) { asm_line(lsk, "DS", "0H", ""); reset_bases(); }
            if (f->vsam) { gen_vsam_status(f, NULL, vs_simple); reset_bases(); }
            break;
        }
        case ST_READ: {
            File *f = &files[st->dst];
            int decl = (!st->had_atend) ? decl_for(st->dst) : -1;
            char le[16], lc[16];
            snprintf(le, sizeof le, "L%04d", st->lab1);
            snprintf(lc, sizeof lc, "L%04d", st->lab2);
            snprintf(b, sizeof b, " READ %s", f->name);
            asm_comment(b);
            if (f->isam == 2) {
                /* BISAM, hand-built rather than via the READ macro so that the
                 * DECB lives in the data area and its area pointer can be the
                 * block obtained at OPEN. The two instructions below are what
                 * the macro itself generates: R1 -> DECB, then branch through
                 * DCBLRAN at DCB+88 ("address of read-write K module").
                 *
                 * Synchronisation is a plain WAIT on the DECB's own ECB. The
                 * documented macro is WAITF, which is not in TK5's SYS1.MACLIB;
                 * CHECK is NOT a substitute -- it loads a routine address from
                 * DCB+52, which in an ISAM DCB is DCBOPTCD, and branches into
                 * it (S0C1). */
                asm_line("", "MVI", "ISFLG,X'00'", "");
                snprintf(b, sizeof b, "DB%03d(4),DB%03d", st->dst, st->dst);
                asm_line("", "XC", b, "clear the ECB before each READ");
                snprintf(b, sizeof b, "DB%03d+24(2),DB%03d+24", st->dst, st->dst);
                asm_line("", "XC", b, "and the exception code");
                need_sym_base(&syms[f->nominal_sym]);
                snprintf(b, sizeof b, "1,%s", syms[f->nominal_sym].label);
                asm_line("", "LA", b, "NOMINAL KEY");
                snprintf(b, sizeof b, "1,DB%03d+20", st->dst);
                asm_line("", "ST", b, "into the DECB");
                snprintf(b, sizeof b, "1,DB%03d", st->dst);
                asm_line("", "LA", b, "R1 -> DECB, as the READ macro does");
                snprintf(b, sizeof b, "15,%s+88", f->label);
                asm_line("", "L", b, "DCBLRAN: read-write K module");
                asm_line("", "BALR", "14,15", "");
                snprintf(b, sizeof b, "ECB=DB%03d", st->dst);
                asm_line("", "WAIT", b, "not CHECK, and not WAITF");
                snprintf(b, sizeof b, "DB%03d+24(2),=X'0000'", st->dst);
                asm_line("", "CLC", b, "exception code set?");
                asm_line("", "BNE", le, "");
                asm_line("", "CLI", "ISFLG,X'00'", "or a permanent error?");
                asm_line("", "BNE", le, "");
                /* DECB+16 is the record pointer word: the logical record's
                 * address inside the block just read. */
                snprintf(b, sizeof b, "1,DB%03d+16", st->dst);
                asm_line("", "L", b, "record pointer word");
                need_sym_base(&syms[f->rec_sym]);
                snprintf(b, sizeof b, "%s(%d),0(1)", syms[f->rec_sym].label, f->reclen);
                asm_line("", "MVC", b, "block-relative record into the FD area");
                if (st->src >= 0) {
                    const Sym *t = &syms[st->src];
                    asm_comment("  INTO: only reached when a record was found");
                    need_sym_base(t); need_sym_base(&syms[f->rec_sym]);
                    gen_move_alpha(t, NULL, &syms[f->rec_sym], NULL);
                }
                asm_line("", "B", lc, "");
                asm_line(le, "DS", "0H", "INVALID KEY");
                reset_bases();
                gen_use_call();
                reset_bases();
                break;
            }
            if (f->vsam) {
                /* GET moves the record into the FD area because the RPL says
                 * MVE. R15 is zero when a record came back; anything else is
                 * end of data or a failure, and both take the AT END path. */
                char rn[10]; rpl_name(f, 0, rn, sizeof rn);
                /* On a DYNAMIC file the two kinds of READ share one RPL,
                 * because VSAM keeps position there and READ NEXT has to
                 * carry on from wherever the keyed READ landed. So the
                 * request type is set on it each time. Nothing to do for the
                 * other access modes: their RPL was assembled saying it. */
                int bykey = (f->access == 1) ||
                            (f->access == 2 && !st->read_next);
                char luser[16] = "";
                if (f->access == 2) {
                    /* NSP, not NUP. A direct request only leaves the RPL
                     * positioned for a following sequential one if it is
                     * asked to note where it got to -- without it READ NEXT
                     * returns end of data immediately, which is a quiet way
                     * to lose half of what DYNAMIC is for. */
                    snprintf(b, sizeof b, "RPL=%s,OPTCD=(%s)", rn,
                             bykey ? "DIR,NSP" : "SEQ");
                    asm_line("", "MODCB", b, bykey ? "by key" : "next");
                    /* A MODCB that fails must not look like end of data. It
                     * takes the same branch the program wrote, but with a
                     * status of its own and without decoding a feedback code
                     * that describes some earlier request. */
                    int lok = ++gen_edited_labels;
                    snprintf(luser, sizeof luser, "G%04d", ++gen_edited_labels);
                    char aok[16]; snprintf(aok, sizeof aok, "G%04d", lok);
                    asm_line("", "LTR", "15,15", "");
                    asm_line("", "BZ", aok, "");
                    if (f->status_sym >= 0) {
                        const Sym *st2 = &syms[f->status_sym];
                        char fd[64];
                        need_sym_base(st2);
                        field_ref(st2, NULL, 2, 6, fd, sizeof fd);
                        snprintf(b, sizeof b, "%s,=C'30'", fd);
                        asm_line("", "MVC", b, "could not set the request type");
                    }
                    asm_line("", "B", luser, "");
                    asm_line(aok, "DS", "0H", "");
                    reset_bases();
                }
                const VsFbk *rtab = bykey ? vs_read_dir : vs_read;
                if (bykey && f->org == 2) gen_rrn_to_cell(f);
                snprintf(b, sizeof b, "RPL=%s", rn);
                asm_line("", "GET", b, bykey
                         ? "VSAM retrieval by key" : "VSAM sequential retrieval");
                asm_line("", "LTR", "15,15", "got a record?");
                asm_line("", "BNZ", le, "");
                gen_vsam_status(f, rn, rtab);
                if (f->org == 2) gen_rrn_from_cell(f);
                if (st->src >= 0) {
                    const Sym *t = &syms[st->src];
                    asm_comment("  INTO: only reached when a record was read");
                    need_sym_base(t); need_sym_base(&syms[f->rec_sym]);
                    gen_move_alpha(t, NULL, &syms[f->rec_sym], NULL);
                }
                asm_line("", "B", lc, "");
                asm_line(le, "DS", "0H", bykey ? "INVALID KEY" : "AT END");
                reset_bases();
                gen_vsam_status(f, rn, rtab);
                if (luser[0]) { asm_line(luser, "DS", "0H", ""); reset_bases(); }
                reset_bases();
                break;
            }
            /* COBOL's AT END is per-READ but DCBEODAD is per-file, so patch it
             * before each GET. Offset 33 (X'21') holds the low three bytes of
             * the address -- exactly what IKFCBL00 does. */
            if (f->optional) {
                snprintf(b, sizeof b, "%sA,X'01'", f->label);
                asm_line("", "CLI", b, "absent OPTIONAL file?");
                asm_line("", "BE", le, "then it is at end");
            }
            snprintf(b, sizeof b, "1,%s", le);       asm_line("", "LA", b, "this READ's AT END");
            snprintf(b, sizeof b, "1,7,%s+33", f->label); asm_line("", "STCM", b, "into DCBEODAD");
            need_sym_base(&syms[f->rec_sym]);
            if (f->update) {
                /* Locate mode: GET returns R1 pointing at the record inside
                 * the access method's buffer. Keep that pointer -- a REWRITE
                 * has to put the new bytes back through it -- and copy the
                 * record out, because the program addresses its 01 at a fixed
                 * place and the buffer moves. */
                asm_line("", "GET", f->label, "QSAM locate mode");
                snprintf(b, sizeof b, "1,U%03d", st->dst);
                asm_line("", "ST", b, "remember where the record sits");
                snprintf(b, sizeof b, "%s(%d),0(1)", syms[f->rec_sym].label, f->reclen);
                asm_line("", "MVC", b, "into the record area");
            } else {
                snprintf(b, sizeof b, "%s,%s", f->label, syms[f->rec_sym].label);
                asm_line("", "GET", b, "QSAM move mode");
            }
            if (st->src >= 0) {
                const Sym *t = &syms[st->src];
                asm_comment("  INTO: only reached when a record was read");
                need_sym_base(t); need_sym_base(&syms[f->rec_sym]);
                gen_move_alpha(t, NULL, &syms[f->rec_sym], NULL);
            }
            asm_line("", "B", lc, "");
            asm_line(le, "DS", "0H", "AT END");
            reset_bases();
            if (decl >= 0) gen_call_range(decls[decl].sect,
                                          section_end(decls[decl].sect), &nret);
            break;
        }
        case ST_INITIATE: {
            Report *rp = &reports[st->dst];
            snprintf(b, sizeof b, " INITIATE %s", rp->name);
            asm_comment(b);
            asm_line("", "SR", "2,2", "");
            snprintf(b, sizeof b, "2,%s", rp->lbl_line); asm_line("", "STH", b, "");
            asm_line("", "MVI", "RCTL,C'1'", "eject before the first page");
            break;
        }
        case ST_TERMINATE:
            snprintf(b, sizeof b, " TERMINATE %s (no report footing in this subset)",
                     reports[st->dst].name);
            asm_comment(b);
            break;
        case ST_GENERATE: {
            RGroup *g = &rgroups[st->dst];
            Report *rp = &reports[g->report];
            int ph = -1;
            for (int k = 0; k < nrgroup; k++)
                if (rgroups[k].report == g->report && rgroups[k].type == RG_PAGE_HEADING) ph = k;
            snprintf(b, sizeof b, " GENERATE %s", g->name);
            asm_comment(b);
            reset_bases();
            /* A group fits on this page only if its LAST line is within LAST
             * DETAIL, not its first. PL-CLASS-END carries a second LINE PLUS 1
             * with no fields -- a blank spacer -- so measuring only the first
             * line under-counts by one and squeezes in a group ANS COBOL would
             * have pushed to the next page. */
            int span = 0, abs_at = -1;
            for (int k = g->first_line; k < g->first_line + g->nline; k++) {
                RLine *lk = &rlines[k];
                if (lk->absolute) { abs_at = lk->n; span = 0; }
                else span += lk->n;
            }
            if (abs_at >= 0) {
                snprintf(b, sizeof b, "2,%d", abs_at + span);
                asm_line("", "LA", b, "last line of the group");
            } else {
                snprintf(b, sizeof b, "2,%s", rp->lbl_line); asm_line("", "LH", b, "");
                snprintf(b, sizeof b, "2,%d(2)", span);
                asm_line("", "LA", b, "last line of the group");
            }
            int lnew = ++genlabel, lok = ++genlabel;
            char ln[16], lo[16];
            snprintf(ln, sizeof ln, "L%04d", lnew);
            snprintf(lo, sizeof lo, "L%04d", lok);
            snprintf(b, sizeof b, "3,%s", rp->lbl_line); asm_line("", "LH", b, "");
            asm_line("", "LTR", "3,3", "no page yet?");
            asm_line("", "BZ", ln, "");
            snprintf(b, sizeof b, "2,%s", intern_half(rp->last_detail));
            asm_line("", "CH", b, "past LAST DETAIL?");
            asm_line("", "BNH", lo, "");
            asm_line(ln, "MVI", "RCTL,C'1'", "new page");
            asm_line("", "SR", "2,2", "");
            snprintf(b, sizeof b, "2,%s", rp->lbl_line); asm_line("", "STH", b, "");
            if (ph >= 0) { snprintf(b, sizeof b, "14,RG%03d", ph); asm_line("", "BAL", b, "page heading"); }
            /* Do NOT fake the line counter here. Forcing it to FIRST DETAIL-1
             * without moving any paper leaves the logical line ahead of the
             * physical one, COBWRL then advances too few blanks, and every page
             * after the heading is short -- which is what split a transaction
             * across a page boundary in the journal. Leave the counter at what
             * the heading actually reached, and force the TARGET instead. */
            snprintf(b, sizeof b, "2,%d", rp->first_detail);
            asm_line("", "LA", b, "first group goes at FIRST DETAIL");
            snprintf(b, sizeof b, "2,%s", rp->lbl_first); asm_line("", "STH", b, "");
            asm_line(lo, "DS", "0H", "");
            snprintf(b, sizeof b, "14,RG%03d", st->dst);
            asm_line("", "BAL", b, "");
            reset_bases();
            break;
        }
        case ST_GOTO: {
            char p[16]; snprintf(p, sizeof p, "P%04d", st->dst);
            snprintf(b, sizeof b, " GO TO %s", st->para);
            asm_comment(b);
            if (cur_para >= 0 && paras[cur_para].altered) {
                /* This paragraph is ALTERed somewhere, so the branch goes
                 * through a cell the ALTER can write. */
                snprintf(b, sizeof b, "15,AL%04d", cur_para);
                asm_line("", "L", b, "the current target");
                asm_line("", "BR", "15", "");
            } else asm_line("", "B", p, "");
            break;
        }
        case ST_STRING: {
            const Sym *d = &syms[st->dst];
            int si = (int)(st - stmts);
            char blk[12], cell[12], slot[24];
            snprintf(blk, sizeof blk, "SB%04d", si);
            snprintf(cell, sizeof cell, "SP%04d", si);
            snprintf(b, sizeof b, " STRING ... INTO %s", d->name);
            asm_comment(b);
            use_str = 1;
            /* The block: receiver, its length, the item count, the pointer
             * cell; then per item its address, length, delimiter length and
             * delimiter address. Twelve bytes each, so every address is on a
             * fullword. */
            int rn = st->dsub ? d->elem : d->bytes;
            pend(blk, "DS", "0F", "STRING parameter block");
            pend("", "DC", "AL4(0)", "the receiver");
            snprintf(b, sizeof b, "AL2(%d),AL2(%d)", rn, st->sop_n); pend("", "DC", b, "its length, the item count");
            if (st->ptr_sym >= 0) { snprintf(b, sizeof b, "AL4(%s)", cell); pend("", "DC", b, "WITH POINTER cell"); }
            else pend("", "DC", "AL4(0)", "no POINTER");
            for (int k = 0; k < st->sop_n; k++) {
                const SOp *o = &sops[st->sop_first + k];
                if (o->sym >= 0) pend("", "DC", "AL4(0)", "item, stored at run time");
                else { snprintf(b, sizeof b, "AL4(%s)", o->lab); pend("", "DC", b, "item, a constant"); }
                snprintf(b, sizeof b, "AL2(%d),AL2(%d)", sop_len(o, 0), o->dsize ? 0 : sop_len(o, 1));
                pend("", "DC", b, o->dsize ? "length; DELIMITED BY SIZE" : "length, delimiter length");
                if (o->dsize) pend("", "DC", "AL4(0)", "");
                else if (o->dsym >= 0) pend("", "DC", "AL4(0)", "delimiter, stored at run time");
                else { snprintf(b, sizeof b, "AL4(%s)", o->dlab); pend("", "DC", b, "delimiter, a constant"); }
            }
            if (st->ptr_sym >= 0) pend(cell, "DS", "F", "the POINTER as a fullword");
            /* Fill in what only exists at run time, convert the pointer, call. */
            {
                char f[64];
                field_ref_m(d, st->dsub, FR_RX, rn, 6, f, sizeof f);
                snprintf(b, sizeof b, "1,%s", f); asm_line("", "LA", b, "the receiver");
                snprintf(b, sizeof b, "1,%s", blk); asm_line("", "ST", b, "");
            }
            for (int k = 0; k < st->sop_n; k++) {
                const SOp *o = &sops[st->sop_first + k];
                snprintf(slot, sizeof slot, "%s+%d", blk, 12 + 12 * k);
                sop_addr(o, 0, slot);
                snprintf(slot, sizeof slot, "%s+%d", blk, 12 + 12 * k + 8);
                if (!o->dsize) sop_addr(o, 1, slot);
            }
            if (st->ptr_sym >= 0) cell_in(st->ptr_sym, st->ptr_sub, cell);
            snprintf(b, sizeof b, "1,%s", blk); asm_line("", "LA", b, "");
            asm_line("", "L", "15,VSTR", "");
            asm_line("", "BALR", "14,15", "COBSTR");
            reset_bases();
            if (st->ptr_sym >= 0) {
                asm_line("", "LR", "3,15", "keep the overflow flag");
                cell_out(st->ptr_sym, st->ptr_sub, cell);
                asm_line("", "LTR", "3,3", "overflow?");
            } else asm_line("", "LTR", "15,15", "overflow?");
            if (st->ovf) {
                snprintf(b, sizeof b, "L%04d", st->lab2);
                asm_line("", "BZ", b, "no: past the ON OVERFLOW statements");
            }
            reset_bases();
            break;
        }
        case ST_UNSTRING: {
            const Sym *sv = &syms[st->src];
            int si = (int)(st - stmts);
            char blk[12], pcell[12], tcell[12], slot[24];
            snprintf(blk, sizeof blk, "UB%04d", si);
            snprintf(pcell, sizeof pcell, "SP%04d", si);
            snprintf(tcell, sizeof tcell, "ST%04d", si);
            snprintf(b, sizeof b, " UNSTRING %s ...", sv->name);
            asm_comment(b);
            use_uns = 1;
            int sn = st->ssub ? sv->elem : sv->bytes;
            /* The block: sending item and length, delimiter count, pointer
             * cell, tally cell, receiver count; then the delimiters at eight
             * bytes each and the receivers at sixteen. */
            pend(blk, "DS", "0F", "UNSTRING parameter block");
            pend("", "DC", "AL4(0)", "the sending item");
            snprintf(b, sizeof b, "AL2(%d),AL2(%d)", sn, st->sdl_n); pend("", "DC", b, "its length, the delimiter count");
            if (st->ptr_sym >= 0) { snprintf(b, sizeof b, "AL4(%s)", pcell); pend("", "DC", b, "WITH POINTER cell"); }
            else pend("", "DC", "AL4(0)", "no POINTER");
            if (st->tally_sym >= 0) { snprintf(b, sizeof b, "AL4(%s)", tcell); pend("", "DC", b, "TALLYING cell"); }
            else pend("", "DC", "AL4(0)", "no TALLYING");
            snprintf(b, sizeof b, "AL2(%d),AL2(0)", st->sop_n); pend("", "DC", b, "the receiver count");
            pend("", "DC", "AL4(0)", "");
            for (int k = 0; k < st->sdl_n; k++) {
                const SOp *o = &sops[st->sdl_first + k];
                if (o->sym >= 0) pend("", "DC", "AL4(0)", "delimiter, stored at run time");
                else { snprintf(b, sizeof b, "AL4(%s)", o->lab); pend("", "DC", b, "delimiter, a constant"); }
                snprintf(b, sizeof b, "AL2(%d),AL2(%d)", sop_len(o, 0), o->all);
                pend("", "DC", b, o->all ? "length; ALL" : "length");
            }
            for (int k = 0; k < st->sop_n; k++) {
                const SOp *o = &sops[st->sop_first + k];
                const Sym *r = &syms[o->sym];
                int rl = o->sub ? r->elem : r->bytes;
                int il = o->insym >= 0 ? (o->insub ? syms[o->insym].elem : syms[o->insym].bytes) : 0;
                pend("", "DC", "AL4(0)", r->name);
                snprintf(b, sizeof b, "AL2(%d),AL2(%d)", rl, il); pend("", "DC", b, "length, DELIMITER IN length");
                pend("", "DC", "AL4(0)", o->insym >= 0 ? "DELIMITER IN, stored at run time" : "no DELIMITER IN");
                if (o->cntsym >= 0) { snprintf(b, sizeof b, "AL4(SC%04d%c)", si, 'A' + k); pend("", "DC", b, "COUNT IN cell"); }
                else pend("", "DC", "AL4(0)", "no COUNT IN");
            }
            if (st->ptr_sym >= 0) pend(pcell, "DS", "F", "the POINTER as a fullword");
            if (st->tally_sym >= 0) pend(tcell, "DS", "F", "TALLYING as a fullword");
            for (int k = 0; k < st->sop_n; k++)
                if (sops[st->sop_first + k].cntsym >= 0) {
                    snprintf(b, sizeof b, "SC%04d%c", si, 'A' + k);
                    pend(b, "DS", "F", "COUNT IN as a fullword");
                }
            /* Run-time addresses, the cells in, the call, the cells out. */
            {
                char f[64];
                field_ref_m(sv, st->ssub, FR_RX, sn, 6, f, sizeof f);
                snprintf(b, sizeof b, "1,%s", f); asm_line("", "LA", b, "the sending item");
                snprintf(b, sizeof b, "1,%s", blk); asm_line("", "ST", b, "");
            }
            for (int k = 0; k < st->sdl_n; k++) {
                snprintf(slot, sizeof slot, "%s+%d", blk, 24 + 8 * k);
                sop_addr(&sops[st->sdl_first + k], 0, slot);
            }
            for (int k = 0; k < st->sop_n; k++) {
                const SOp *o = &sops[st->sop_first + k];
                int base = 24 + 8 * st->sdl_n + 16 * k;
                snprintf(slot, sizeof slot, "%s+%d", blk, base);
                sop_addr(o, 0, slot);
                if (o->insym >= 0) {
                    SOp t; memset(&t, 0, sizeof t); t.sym = o->insym; t.sub = o->insub;
                    snprintf(slot, sizeof slot, "%s+%d", blk, base + 8);
                    sop_addr(&t, 0, slot);
                }
            }
            if (st->ptr_sym >= 0) cell_in(st->ptr_sym, st->ptr_sub, pcell);
            if (st->tally_sym >= 0) cell_in(st->tally_sym, st->tally_sub, tcell);
            snprintf(b, sizeof b, "1,%s", blk); asm_line("", "LA", b, "");
            asm_line("", "L", "15,VUNS", "");
            asm_line("", "BALR", "14,15", "COBUNS");
            reset_bases();
            asm_line("", "LR", "3,15", "keep the overflow flag");
            if (st->ptr_sym >= 0) cell_out(st->ptr_sym, st->ptr_sub, pcell);
            if (st->tally_sym >= 0) cell_out(st->tally_sym, st->tally_sub, tcell);
            for (int k = 0; k < st->sop_n; k++) {
                const SOp *o = &sops[st->sop_first + k];
                if (o->cntsym < 0) continue;
                snprintf(b, sizeof b, "SC%04d%c", si, 'A' + k);
                cell_out(o->cntsym, o->cntsub, b);
            }
            asm_line("", "LTR", "3,3", "overflow?");
            if (st->ovf) {
                snprintf(b, sizeof b, "L%04d", st->lab2);
                asm_line("", "BZ", b, "no: past the ON OVERFLOW statements");
            }
            reset_bases();
            break;
        }
        case ST_GODEP: {
            const Sym *v = &syms[st->src];
            char lt[16], lx[16];
            snprintf(lt, sizeof lt, "L%04d", ++genlabel);
            snprintf(lx, sizeof lx, "L%04d", ++genlabel);
            snprintf(b, sizeof b, " GO TO ... DEPENDING ON %s", v->name);
            asm_comment(b);
            /* The value in a register, then a branch table indexed by it. A
             * value under 1 or past the last name falls through, which is
             * what the standard says happens. */
            need_sym_base(v);
            gen_load(v, st->ssub, "PWK1");
            asm_line("", "ZAP", "DWK(8),PWK1(16)", "");
            asm_line("", "CVB", "2,DWK", "the selector");
            asm_line("", "BCTR", "2,0", "1 selects the first entry");
            snprintf(b, sizeof b, "0,%s", intern_half(st->ndop));
            asm_line("", "LH", b, "the entry count");
            asm_line("", "CLR", "2,0", "past the last, or negative -- unsigned covers both");
            asm_line("", "BNL", lx, "out of range: fall through");
            asm_line("", "SLL", "2,2", "four bytes per entry");
            snprintf(b, sizeof b, "%s(2)", lt);
            asm_line("", "B", b, "into the table");
            asm_line(lt, "DS", "0H", "");
            for (int k = 0; k < st->ndop; k++) {
                snprintf(b, sizeof b, "P%04d", st->dop[k].sym);
                asm_line("", "B", b, st->dop[k].lit);
            }
            asm_line(lx, "DS", "0H", "");
            reset_bases();
            break;
        }
        case ST_ACCEPT: {
            const Sym *d = &syms[st->dst];
            int n = st->dsub ? d->elem : d->bytes;
            char fd[64];
            if (st->acc_from >= 1 && st->acc_from <= 3) {
                /* DATE, DAY or TIME: the runtime writes the digits into ZWK,
                 * and from there it is an ordinary MOVE from an unsigned
                 * DISPLAY integer of six, five or eight digits. */
                static const int dlen[4] = { 0, 6, 5, 8 };
                static const char *dnm[4] = { "", "DATE", "DAY", "TIME" };
                snprintf(b, sizeof b, " ACCEPT %s FROM %s", d->name, dnm[st->acc_from]);
                asm_comment(b);
                use_adt = 1;
                snprintf(b, sizeof b, "1,ADTP%d", st->acc_from); asm_line("", "LA", b, "");
                asm_line("", "L", "15,VADT", "");
                asm_line("", "BALR", "14,15", "the digits into ZWK");
                reset_bases();
                Sym tmp; memset(&tmp, 0, sizeof tmp);
                tmp.usage = U_DISPLAY; tmp.digits = dlen[st->acc_from];
                tmp.bytes = tmp.elem = dlen[st->acc_from];
                tmp.occ_parent = tmp.gparent = tmp.index_sym = tmp.askey_sym = -1;
                tmp.fd_file = tmp.redef_from = tmp.redef_cap = -1; tmp.parent = -1;
                snprintf(tmp.label, sizeof tmp.label, "ZWK");
                snprintf(tmp.name, sizeof tmp.name, "%s", dnm[st->acc_from]);
                need_sym_base(d);
                emit_move(d, st->dsub, &tmp, NULL);
                reset_bases();
                break;
            }
            snprintf(b, sizeof b, " ACCEPT %s%s", d->name, st->acc_from == 4 ? " FROM CONSOLE" : "");
            asm_comment(b);
            need_sym_base(d);
            field_ref_m(d, st->dsub, FR_SS_NOLEN, n, 6, fd, sizeof fd);
            /* The parameter list is built here rather than assembled as a
             * constant, because a subscripted receiver has no fixed address. */
            snprintf(b, sizeof b, "1,%s", fd);
            asm_line("", "LA", b, "A(item)");
            asm_line("", "ST", "1,ACCPARM", "");
            snprintf(b, sizeof b, "1,%d", n);
            asm_line("", "LA", b, "");
            asm_line("", "STH", "1,ACCLEN", "its length");
            asm_line("", "LA", "1,ACCLEN", "");
            asm_line("", "ST", "1,ACCPARM+4", "");
            asm_line("", "OI", "ACCPARM+4,X'80'", "last parameter");
            asm_line("", "LA", "1,ACCPARM", "");
            if (st->acc_from == 4) {
                use_wtor = 1;
                asm_line("", "L", "15,VWTOR", "");
                asm_line("", "BALR", "14,15", "a reply from the operator");
            } else {
                asm_line("", "L", "15,VACC", "");
                asm_line("", "BALR", "14,15", "from SYSIN, a card at a time");
            }
            reset_bases();
            break;
        }
        case ST_ALTER: {
            snprintf(b, sizeof b, " ALTER %s TO PROCEED TO %s", st->para, st->thru);
            asm_comment(b);
            snprintf(b, sizeof b, "15,P%04d", st->src);
            asm_line("", "LA", b, "the new target");
            snprintf(b, sizeof b, "15,AL%04d", st->dst);
            asm_line("", "ST", b, "into that paragraph's branch cell");
            break;
        }
        case ST_WRITE: {
            File *f = &files[st->dst];
            char wle[16], wlc[16];
            snprintf(wle, sizeof wle, "L%04d", st->lab1);
            snprintf(wlc, sizeof wlc, "L%04d", st->lab2);
            const Sym *wrec = &syms[st->rec >= 0 ? st->rec : f->rec_sym];
            snprintf(b, sizeof b, " WRITE %s", wrec->name);
            asm_comment(b);
            if (st->src >= 0) {
                const Sym *sv = &syms[st->src];
                asm_comment("  FROM: fill the record area first");
                need_sym_base(wrec); need_sym_base(sv);
                gen_move_alpha(wrec, NULL, sv, NULL);
            }
            if (f->vsam) {
                /* PUT stores from the FD area because the RPL says MVE. In
                 * load mode a key that is not greater than the last one
                 * written, or a cluster with no room to extend, come back as
                 * feedback codes rather than an abend -- which is exactly what
                 * COBOL's INVALID KEY is for. */
                need_sym_base(&syms[f->rec_sym]);
                char wn[10]; rpl_name(f, 1, wn, sizeof wn);
                if (f->org == 2 && f->access != 0) gen_rrn_to_cell(f);
                snprintf(b, sizeof b, "RPL=%s", wn);
                asm_line("", "PUT", b, f->access == 1
                         ? "VSAM insert by key" : "VSAM sequential store");
                asm_line("", "LTR", "15,15", "stored?");
                asm_line("", "BNZ", wle, "");
                gen_vsam_status(f, wn, vs_write);
                if (f->org == 2) gen_rrn_from_cell(f);
                asm_line("", "B", wlc, "");
                asm_line(wle, "DS", "0H", "INVALID KEY");
                reset_bases();
                gen_vsam_status(f, wn, vs_write);
                reset_bases();
                break;
            }
            if (f->isam) {
                /* QISAM load mode. PUT is the same macro as QSAM, but a key
                 * out of ascending order or a duplicate is reported by the
                 * access method through SYNAD -- that is what INVALID KEY
                 * tests. Records must be presented in ascending key order;
                 * ISAM has no way to insert during a load. */
                asm_line("", "MVI", "ISFLG,X'00'", "");
                need_sym_base(&syms[f->rec_sym]);
                snprintf(b, sizeof b, "%s,%s", f->label, syms[f->rec_sym].label);
                asm_line("", "PUT", b, "QISAM load");
                asm_line("", "CLI", "ISFLG,X'00'", "sequence or duplicate?");
                asm_line("", "BNE", wle, "");
                asm_line("", "B", wlc, "");
                asm_line(wle, "DS", "0H", "INVALID KEY");
                reset_bases();
                gen_use_call();
                break;
            }
            if (f->print) {
                /* The line goes out through the runtime, which owns the
                 * carriage control: AFTER applies its count now, BEFORE holds
                 * it over to the next line, and only the runtime can add the
                 * two together because only it knows what was owed. */
                if (f->reclen > 256)
                    die("a print record wider than 256 bytes needs a split MVC, "
                        "which is not implemented");
                /* The request is the line count, 999 for PAGE, negated when
                 * the phrase was BEFORE. BEFORE 0 and AFTER 0 are the same
                 * thing -- apply what is owed and owe nothing -- so the zero
                 * that cannot be negated needs no special case. */
                int adv = st->adv == -2 ? 1 : st->adv == -1 ? 999 : st->adv;
                if (st->adv_before) adv = -adv;
                need_sym_base(wrec);
                snprintf(b, sizeof b, "%s+1(%d),%s", f->pbuf, f->reclen, wrec->label);
                asm_line("", "MVC", b, "the record, behind its control byte");
                if (st->adv_sym >= 0) {
                    need_sym_base(&syms[st->adv_sym]);
                    gen_load(&syms[st->adv_sym], st->adv_sub, "PWK1");
                    asm_line("", "ZAP", "DWK(8),PWK1(16)", "");
                    asm_line("", "CVB", "1,DWK", "ADVANCING identifier LINES");
                    if (st->adv_before) asm_line("", "LCR", "1,1", "negative marks a BEFORE");
                } else {
                    snprintf(b, sizeof b, "1,%d", adv < 0 ? -adv : adv);
                    asm_line("", "LA", b, adv < 0 ? "BEFORE" : "AFTER");
                    if (adv < 0) asm_line("", "LCR", "1,1", "negative marks a BEFORE");
                }
                snprintf(b, sizeof b, "1,%sQ", f->pbuf);
                asm_line("", "STH", b, "this line's request");
                snprintf(b, sizeof b, "1,%sP", f->pbuf);
                asm_line("", "LA", b, "");
                asm_line("", "L", "15,VADV", "");
                asm_line("", "BALR", "14,15", "carriage control and PUT");
                reset_bases();
                if (st->eop) {
                    snprintf(b, sizeof b, "L%04d", st->lab2);
                    asm_line("", "LTR", "15,15", "END-OF-PAGE?");
                    asm_line("", "BZ", b, "no: past the imperative statements");
                }
                break;
            }
            need_sym_base(&syms[f->rec_sym]);
            snprintf(b, sizeof b, "%s,%s", f->label, syms[f->rec_sym].label);
            asm_line("", "PUT", b, "");
            break;
        }
        case ST_INSPECT: {
            const Sym *sy = &syms[st->dst];
            int n = st->dsub ? sy->elem : sy->bytes;
            char f[64], g[64];
            snprintf(b, sizeof b, " INSPECT %s", sy->name);
            asm_comment(b);
            for (int k = 0; k < st->ins_n; k++) {
                const InsOp *o = &insops[st->ins_first + k];
                int tallying = (o->kind <= INS_T_CHARS);
                /* R3 walks the field and R5 counts what is left of the range;
                 * R7 keeps the field's start, R4 tallies. Nothing else is
                 * live in them. */
                need_sym_base(sy);
                field_ref_m(sy, st->dsub, FR_SS_NOLEN, n, 6, f, sizeof f);
                snprintf(b, sizeof b, "3,%s", f);
                asm_line("", "LA", b, "the field");
                snprintf(b, sizeof b, "5,%d", n);
                asm_line("", "LA", b, "its length");
                if (o->bf_len) {
                    /* BEFORE/AFTER INITIAL: find the first occurrence of the
                     * bounding string. BEFORE scans up to it, or the whole
                     * field if absent; AFTER scans from just past it, or
                     * nothing if absent. */
                    int ls = ++genlabel, lf = ++genlabel, lnf = ++genlabel, lgo = ++genlabel;
                    char lls[16], llf[16], llnf[16], llgo[16];
                    snprintf(lls, sizeof lls, "L%04d", ls);  snprintf(llf, sizeof llf, "L%04d", lf);
                    snprintf(llnf, sizeof llnf, "L%04d", lnf); snprintf(llgo, sizeof llgo, "L%04d", lgo);
                    asm_line("", "LR", "7,3", "the field's start");
                    if (o->bf_sym >= 0) {
                        need_sym_base(&syms[o->bf_sym]);
                        field_ref_m(&syms[o->bf_sym], NULL, FR_SS_NOLEN, o->bf_len, 6, g, sizeof g);
                    } else snprintf(g, sizeof g, "%s", o->bf_lab);
                    snprintf(b, sizeof b, "5,%s", intern_half(o->bf_len));
                    asm_line(lls, "CH", b, "room for the bounding string?");
                    asm_line("", "BL", llnf, "");
                    snprintf(b, sizeof b, "0(%d,3),%s", o->bf_len, g);
                    asm_line("", "CLC", b, "INITIAL");
                    asm_line("", "BE", llf, "");
                    asm_line("", "LA", "3,1(3)", "");
                    snprintf(b, sizeof b, "5,%s", lls);
                    asm_line("", "BCT", b, "");
                    asm_line(llnf, "DS", "0H", "not found");
                    if (o->bf_after) asm_line("", "SR", "5,5", "AFTER: nothing to scan");
                    else { asm_line("", "LR", "3,7", ""); snprintf(b, sizeof b, "5,%d", n); asm_line("", "LA", b, "BEFORE: the whole field"); }
                    asm_line("", "B", llgo, "");
                    asm_line(llf, "DS", "0H", "found");
                    if (o->bf_after) {
                        snprintf(b, sizeof b, "3,%d(3)", o->bf_len);
                        asm_line("", "LA", b, "AFTER: from just past it");
                        snprintf(b, sizeof b, "5,%s", intern_half(o->bf_len));
                        asm_line("", "SH", b, "");
                    } else {
                        asm_line("", "LR", "5,3", "");
                        asm_line("", "SR", "5,7", "BEFORE: up to it");
                        asm_line("", "LR", "3,7", "");
                    }
                    asm_line(llgo, "DS", "0H", "");
                }
                if (o->kind == INS_T_CHARS) {
                    asm_line("", "LR", "2,5", "CHARACTERS: every position in range");
                    asm_line("", "CVD", "2,DWK", "");
                    asm_line("", "ZAP", "PWK1(16),DWK(8)", "");
                    gen_load(&syms[o->tally], NULL, "PWK2");
                    asm_line("", "AP", "PWK1(16),PWK2(16)", "TALLYING adds");
                    gen_store(&syms[o->tally], NULL, "PWK1");
                    reset_bases();
                    continue;
                }
                if (o->kind == INS_R_CHARS) {
                    /* Every position in range is replaced: set the first byte
                     * and let an overlapping MVC carry it, its length known
                     * only at run time when a range is in play. */
                    int ld = ++genlabel; char lld[16]; snprintf(lld, sizeof lld, "L%04d", ld);
                    if (o->by_sym >= 0) {
                        need_sym_base(&syms[o->by_sym]);
                        field_ref_m(&syms[o->by_sym], NULL, FR_SS_NOLEN, 1, 7, g, sizeof g);
                    } else snprintf(g, sizeof g, "%s", o->by_lab);
                    asm_line("", "LTR", "5,5", "");
                    asm_line("", "BZ", lld, "nothing in range");
                    snprintf(b, sizeof b, "0(1,3),%s", g);
                    asm_line("", "MVC", b, "CHARACTERS BY: the first");
                    asm_line("", "LR", "4,5", "");
                    asm_line("", "BCTR", "4,0", "");
                    asm_line("", "LTR", "4,4", "");
                    asm_line("", "BZ", lld, "");
                    asm_line("", "BCTR", "4,0", "");
                    use_insprop = 1;
                    asm_line("", "EX", "4,INSPROP", "and propagate");
                    asm_line(lld, "DS", "0H", "");
                    reset_bases();
                    continue;
                }
                /* A scan down the range: at each position, is the string
                 * here? A match tallies or replaces and steps past the whole
                 * string; a miss steps one byte, or ends a LEADING scan. */
                int lp = ++genlabel, nx = ++genlabel, dn = ++genlabel;
                char llp[16], lnx[16], ldn[16];
                snprintf(llp, sizeof llp, "L%04d", lp);
                snprintf(lnx, sizeof lnx, "L%04d", nx);
                snprintf(ldn, sizeof ldn, "L%04d", dn);
                if (tallying) asm_line("", "SR", "4,4", "the tally");
                if (o->c_sym >= 0) {
                    need_sym_base(&syms[o->c_sym]);
                    field_ref_m(&syms[o->c_sym], NULL, FR_SS_NOLEN, o->c_len, 7, g, sizeof g);
                } else snprintf(g, sizeof g, "%s", o->c_lab);
                snprintf(b, sizeof b, "5,%s", intern_half(o->c_len));
                asm_line(llp, "CH", b, "room for the string?");
                asm_line("", "BL", ldn, "");
                snprintf(b, sizeof b, "0(%d,3),%s", o->c_len, g);
                asm_line("", "CLC", b, "");
                int stop_on_miss = (o->kind == INS_T_LEAD || o->kind == INS_R_LEAD);
                asm_line("", "BNE", stop_on_miss ? ldn : lnx, "");
                if (tallying) asm_line("", "LA", "4,1(4)", "one more");
                else {
                    char h[64];
                    if (o->by_sym >= 0) {
                        need_sym_base(&syms[o->by_sym]);
                        field_ref_m(&syms[o->by_sym], NULL, FR_SS_NOLEN, o->by_len, 7, h, sizeof h);
                    } else snprintf(h, sizeof h, "%s", o->by_lab);
                    snprintf(b, sizeof b, "0(%d,3),%s", o->by_len, h);
                    asm_line("", "MVC", b, "replace");
                    if (o->kind == INS_R_FIRST) asm_line("", "B", ldn, "FIRST: done");
                }
                snprintf(b, sizeof b, "3,%d(3)", o->c_len);
                asm_line("", "LA", b, "past the string");
                snprintf(b, sizeof b, "5,%s", intern_half(o->c_len));
                asm_line("", "SH", b, "");
                asm_line("", "B", llp, "");
                asm_line(lnx, "DS", "0H", "");
                asm_line("", "LA", "3,1(3)", "");
                snprintf(b, sizeof b, "5,%s", llp);
                asm_line("", "BCT", b, "");
                asm_line(ldn, "DS", "0H", "");
                reset_bases();
                if (tallying) {
                    asm_line("", "CVD", "4,DWK", "");
                    asm_line("", "ZAP", "PWK1(16),DWK(8)", "");
                    gen_load(&syms[o->tally], NULL, "PWK2");
                    asm_line("", "AP", "PWK1(16),PWK2(16)", "TALLYING adds");
                    gen_store(&syms[o->tally], NULL, "PWK1");
                }
            }
            break;
        }
        case ST_START: {
            File *f = &files[st->dst];
            char sle[16], slc[16];
            snprintf(sle, sizeof sle, "L%04d", st->lab1);
            snprintf(slc, sizeof slc, "L%04d", st->lab2);
            snprintf(b, sizeof b, " START %s", f->name);
            asm_comment(b);
            char sn[10]; rpl_name(f, 0, sn, sizeof sn);
            if (f->org == 2) gen_rrn_to_cell(f);
            /* KEQ and KGE are the same field of the same RPL, and the RPL that
             * is positioned has to be the one the following READ uses -- VSAM
             * keeps position per RPL. So this is the one place a second
             * control block cannot stand in for a runtime change, and MODCB
             * earns its keep. It runs once per START, not once per record.
             *
             * A MODCB that fails takes the INVALID KEY path. The feedback code
             * decoded there will be whatever the RPL last held rather than a
             * reason for this failure, which is worth knowing but not worth
             * machinery: MODCB against a control block the assembler built
             * cannot fail for any reason a COBOL program can cause. */
            snprintf(b, sizeof b, "RPL=%s,OPTCD=(%s)", sn,
                     st->src ? "KGE" : "KEQ");
            asm_line("", "MODCB", b, st->src ? "not less than" : "equal to");
            asm_line("", "LTR", "15,15", "");
            asm_line("", "BNZ", sle, "");
            snprintf(b, sizeof b, "RPL=%s", sn);
            asm_line("", "POINT", b, "position by key");
            asm_line("", "LTR", "15,15", "positioned?");
            asm_line("", "BNZ", sle, "");
            gen_vsam_status(f, sn, vs_start);
            asm_line("", "B", slc, "");
            asm_line(sle, "DS", "0H", "INVALID KEY");
            reset_bases();
            gen_vsam_status(f, sn, vs_start);
            reset_bases();
            break;
        }
        case ST_REWRITE:
        case ST_DELETE: {
            File *f = &files[st->dst];
            int erase = (st->op == ST_DELETE);
            char wle[16], wlc[16];
            snprintf(wle, sizeof wle, "L%04d", st->lab1);
            snprintf(wlc, sizeof wlc, "L%04d", st->lab2);
            snprintf(b, sizeof b, " %s %s", erase ? "DELETE" : "REWRITE",
                     erase ? f->name : syms[f->rec_sym].name);
            asm_comment(b);
            if (!f->opened_io)
                die("REWRITE and DELETE need the file opened I-O");
            if (f->update) {
                /* The record goes back through the pointer the last GET
                 * returned, into the very buffer it came from, and PUTX with
                 * no output DCB tells QSAM to write that block back where it
                 * was read. Nothing here knows or cares whether the dataset is
                 * blocked. */
                if (st->src >= 0) {
                    const Sym *sv = &syms[st->src];
                    asm_comment("  FROM: fill the record area first");
                    need_sym_base(&syms[f->rec_sym]); need_sym_base(sv);
                    gen_move_alpha(&syms[f->rec_sym], NULL, sv, NULL);
                }
                need_sym_base(&syms[f->rec_sym]);
                snprintf(b, sizeof b, "1,U%03d", st->dst);
                asm_line("", "L", b, "where the last READ left the record");
                snprintf(b, sizeof b, "0(%d,1),%s", f->reclen, syms[f->rec_sym].label);
                asm_line("", "MVC", b, "back into the buffer");
                asm_line("", "PUTX", f->label, "write that block back");
                break;
            }
            if (f->org == 2 && f->access != 0) gen_rrn_to_cell(f);
            if (erase && f->org == 0)
                die("a record cannot be deleted from a VSAM ESDS -- entry "
                    "sequence is fixed once written");
            if (st->src >= 0) {
                const Sym *sv = &syms[st->src];
                asm_comment("  FROM: fill the record area first");
                need_sym_base(&syms[f->rec_sym]); need_sym_base(sv);
                gen_move_alpha(&syms[f->rec_sym], NULL, sv, NULL);
            }
            /* Neither macro takes a key. The RPL is holding the record the
             * last GET returned, and that is the one acted on -- which is why
             * changing the key first is an error rather than a move. */
            need_sym_base(&syms[f->rec_sym]);
            char un[10]; rpl_name(f, 0, un, sizeof un);
            snprintf(b, sizeof b, "RPL=%s", un);
            asm_line("", erase ? "ERASE" : "PUT", b,
                     erase ? "erase the held record" : "put the held record back");
            asm_line("", "LTR", "15,15", "done?");
            asm_line("", "BNZ", wle, "");
            gen_vsam_status(f, un, vs_upd);
            asm_line("", "B", wlc, "");
            asm_line(wle, "DS", "0H", "INVALID KEY");
            reset_bases();
            gen_vsam_status(f, un, vs_upd);
            reset_bases();
            break;
        }
        case ST_PERFORM: {
            char p1[16], x[16], f[16], r[16], lt[16], le[16], pt[16];
            snprintf(p1, sizeof p1, "P%04d", st->dst);
            snprintf(x,  sizeof x,  "X%04d", st->src);
            snprintf(f,  sizeof f,  "F%04d", st->src);
            snprintf(pt, sizeof pt, "PT%03d", i);
            if (!strcmp(st->para, st->thru)) snprintf(b, sizeof b, " PERFORM %s", st->para);
            else snprintf(b, sizeof b, " PERFORM %s THRU %s", st->para, st->thru);
            asm_comment(b);

            int looping = (st->cond != NULL) || (st->times_expr != NULL);
            if (st->vary_sym >= 0)
                emit_set_from_expr(&syms[st->vary_sym], st->vary_from, 0);
            if (st->vary2_sym >= 0) {
                /* VARYING ... AFTER: every level starts from its FROM. Then
                 * the outermost condition ends the whole thing; an inner
                 * condition true resets its own level, augments the level
                 * above, and retests from there; the body runs only when
                 * every condition is false, and augments the innermost. */
                emit_set_from_expr(&syms[st->vary2_sym], st->vary2_from, 0);
                if (st->vary3_sym >= 0) emit_set_from_expr(&syms[st->vary3_sym], st->vary3_from, 0);
                int l1 = ++genlabel, l2 = ++genlabel, l3 = ++genlabel, lr2 = ++genlabel, lr3 = ++genlabel, lx = ++genlabel;
                char s1[16], s2[16], s3[16], sr2[16], sr3[16], sx[16];
                snprintf(s1, sizeof s1, "L%04d", l1); snprintf(s2, sizeof s2, "L%04d", l2);
                snprintf(s3, sizeof s3, "L%04d", l3); snprintf(sr2, sizeof sr2, "L%04d", lr2);
                snprintf(sr3, sizeof sr3, "L%04d", lr3); snprintf(sx, sizeof sx, "L%04d", lx);
                asm_line(s1, "DS", "0H", "outer test"); reset_bases();
                gen_cond(st->cond, lx, 1);
                asm_line(s2, "DS", "0H", "AFTER test"); reset_bases();
                gen_cond(st->acond2, lr2, 1);
                if (st->vary3_sym >= 0) {
                    asm_line(s3, "DS", "0H", "second AFTER test"); reset_bases();
                    gen_cond(st->acond3, lr3, 1);
                }
                snprintf(r, sizeof r, "R%04d", ++nret);
                snprintf(b, sizeof b, "15,%s", r);  asm_line("", "LA", b, "return here");
                snprintf(b, sizeof b, "15,%s", x);  asm_line("", "ST", b, "into the range's exit cell");
                asm_line("", "B", p1, "");
                asm_line(r, "DS", "0H", "");
                reset_bases();
                snprintf(b, sizeof b, "15,%s", f);  asm_line("", "LA", b, "restore fall-through");
                snprintf(b, sizeof b, "15,%s", x);  asm_line("", "ST", b, "");
                if (st->vary3_sym >= 0) {
                    emit_set_from_expr(&syms[st->vary3_sym], st->vary3_by, 1);
                    asm_line("", "B", s3, "");
                    asm_line(sr3, "DS", "0H", "innermost done: reset it, step the middle"); reset_bases();
                    emit_set_from_expr(&syms[st->vary3_sym], st->vary3_from, 0);
                    emit_set_from_expr(&syms[st->vary2_sym], st->vary2_by, 1);
                    asm_line("", "B", s2, "");
                } else {
                    emit_set_from_expr(&syms[st->vary2_sym], st->vary2_by, 1);
                    asm_line("", "B", s2, "");
                }
                asm_line(sr2, "DS", "0H", "AFTER level done: reset it, step the outer"); reset_bases();
                emit_set_from_expr(&syms[st->vary2_sym], st->vary2_from, 0);
                emit_set_from_expr(&syms[st->vary_sym], st->vary_by, 1);
                asm_line("", "B", s1, "");
                asm_line(sx, "DS", "0H", ""); reset_bases();
                break;
            }
            if (st->times_expr) {
                gen_expr(st->times_expr, 0, 0);
                asm_line("", "ZAP", "DWK(8),WK0(16)", "");
                asm_line("", "CVB", "2,DWK", "repeat count");
                snprintf(b, sizeof b, "2,%s", pt); asm_line("", "STH", b, "");
            }

            int ltop = ++genlabel, lend = ++genlabel;
            snprintf(lt, sizeof lt, "L%04d", ltop);
            snprintf(le, sizeof le, "L%04d", lend);
            if (looping) { asm_line(lt, "DS", "0H", ""); reset_bases(); }
            /* UNTIL tests before each iteration, so the body may run zero
               times -- that is the COBOL rule, not an implementation choice. */
            if (st->cond) gen_cond(st->cond, lend, 1);
            if (st->times_expr) {
                snprintf(b, sizeof b, "2,%s", pt); asm_line("", "LH", b, "");
                asm_line("", "LTR", "2,2", "");
                asm_line("", "BNP", le, "");
            }

            snprintf(r, sizeof r, "R%04d", ++nret);
            snprintf(b, sizeof b, "15,%s", r);  asm_line("", "LA", b, "return here");
            snprintf(b, sizeof b, "15,%s", x);  asm_line("", "ST", b, "into the range's exit cell");
            asm_line("", "B", p1, "");
            asm_line(r, "DS", "0H", "");
            reset_bases();
            snprintf(b, sizeof b, "15,%s", f);  asm_line("", "LA", b, "restore fall-through");
            snprintf(b, sizeof b, "15,%s", x);  asm_line("", "ST", b, "");

            if (st->vary_sym >= 0)
                emit_set_from_expr(&syms[st->vary_sym], st->vary_by, 1);
            if (st->times_expr) {
                snprintf(b, sizeof b, "2,%s", pt); asm_line("", "LH", b, "");
                asm_line("", "BCTR", "2,0", "");
                snprintf(b, sizeof b, "2,%s", pt); asm_line("", "STH", b, "");
            }
            if (looping) {
                asm_line("", "B", lt, "");
                asm_line(le, "DS", "0H", "");
                reset_bases();
            }
            break;
        }
        case ST_SEARCH: {
            const Sym *tb = &syms[st->dst];
            const Sym *ix = &syms[tb->index_sym];
            if (st->serial) {
                char lp[16], le[16], fx[64];
                int ltop = ++genlabel;
                snprintf(lp, sizeof lp, "L%04d", ltop);
                snprintf(le, sizeof le, "L%04d", st->lab1);
                snprintf(b, sizeof b, " SEARCH %s", tb->name);
                asm_comment(b);
                asm_line(lp, "DS", "0H", "");
                reset_bases();
                need_sym_base(ix);
                field_ref_m(ix, NULL, FR_RX, ix->bytes, 6, fx, sizeof fx);
                snprintf(b, sizeof b, "1,%s", fx); asm_line("", "LH", b, "the index");
                snprintf(b, sizeof b, "1,%s", intern_half(tb->occurs));
                asm_line("", "CH", b, "past the last occurrence?");
                asm_line("", "BH", le, "AT END");
                for (int k = 0; k < st->nwhen; k++) {
                    reset_bases();
                    gen_cond(st->whens[k], st->when_lab[k], 1);
                }
                reset_bases();
                need_sym_base(ix);
                field_ref_m(ix, NULL, FR_RX, ix->bytes, 6, fx, sizeof fx);
                snprintf(b, sizeof b, "1,%s", fx); asm_line("", "LH", b, "");
                asm_line("", "LA", "1,1(1)", "next occurrence");
                snprintf(b, sizeof b, "1,%s", fx); asm_line("", "STH", b, "");
                if (st->vary_sym >= 0) {
                    /* VARYING: stepped in step with the index, whatever it is */
                    Node *one = node(N_LIT); strcpy(one->lit, "1"); one->litscale = 0;
                    emit_set_from_expr(&syms[st->vary_sym], one, 1);
                }
                asm_line("", "B", lp, "");
                reset_bases();
                break;
            }
            /* Binary search over a KEY table. Low and high live in storage
             * rather than registers because gen_cond is free to use any work
             * register, so nothing may stay live across it. */
            char lo[16], hi[16], lp[16], up[16];
            snprintf(lo, sizeof lo, "SL%03d", i);
            snprintf(hi, sizeof hi, "SH%03d", i);
            snprintf(lp, sizeof lp, "SP%03d", i);   /* loop top */
            snprintf(up, sizeof up, "SU%03d", i);   /* raise the low bound */
            snprintf(b, sizeof b, " SEARCH ALL %s", tb->name);
            asm_comment(b);
            asm_line("", "LA", "1,1", "");
            snprintf(b, sizeof b, "1,%s", lo);  asm_line("", "STH", b, "low = 1");
            snprintf(b, sizeof b, "1,%d", tb->occurs);
            asm_line("", "LA", b, "");
            snprintf(b, sizeof b, "1,%s", hi);  asm_line("", "STH", b, "high = OCCURS");
            asm_line(lp, "DS", "0H", "");
            reset_bases();
            snprintf(b, sizeof b, "1,%s", lo);  asm_line("", "LH", b, "");
            snprintf(b, sizeof b, "2,%s", hi);  asm_line("", "LH", b, "");
            asm_line("", "CR", "1,2", "low > high means it is not there");
            snprintf(b, sizeof b, "L%04d", st->lab1);
            asm_line("", "BH", b, "");
            asm_line("", "AR", "1,2", "");
            asm_line("", "SRA", "1,1", "mid = (low + high) / 2");
            need_sym_base(ix);
            field_ref_m(ix, NULL, FR_RX, ix->bytes, 6, b + 64, 64);
            snprintf(b, sizeof b, "1,%s", b + 64);
            asm_line("", "STH", b, "the index is the occurrence number");
            gen_cond(st->cond,  st->lab2, 1);      /* key = value: found */
            gen_cond(st->cond2, st->src, 1);       /* key < value: raise low */
            /* key > value: lower the high bound and go round again. */
            need_sym_base(ix);
            field_ref_m(ix, NULL, FR_RX, ix->bytes, 6, b + 64, 64);
            snprintf(b, sizeof b, "1,%s", b + 64);
            asm_line("", "LH", b, "");
            asm_line("", "BCTR", "1,0", "");
            snprintf(b, sizeof b, "1,%s", hi);  asm_line("", "STH", b, "high = mid - 1");
            asm_line("", "B", lp, "");
            snprintf(up, sizeof up, "L%04d", st->src);
            asm_line(up, "DS", "0H", "");
            reset_bases();
            need_sym_base(ix);
            field_ref_m(ix, NULL, FR_RX, ix->bytes, 6, b + 64, 64);
            snprintf(b, sizeof b, "1,%s", b + 64);
            asm_line("", "LH", b, "");
            asm_line("", "LA", "1,1(1)", "");
            snprintf(b, sizeof b, "1,%s", lo);  asm_line("", "STH", b, "low = mid + 1");
            asm_line("", "B", lp, "");
            reset_bases();
            break;
        }
        case ST_CALL: {
            char pl[16], vc[16];
            snprintf(pl, sizeof pl, "PL%03d", i);
            snprintf(vc, sizeof vc, "VC%03d", i);
            snprintf(b, sizeof b, " CALL '%s'", st->para);
            asm_comment(b);
            for (int k = 0; k < st->ndop; k++) {
                const Sym *a = &syms[st->dop[k].sym];
                char fa[64];
                need_sym_base(a);
                field_ref_m(a, NULL, FR_RX, a->bytes, 6, fa, sizeof fa);
                snprintf(b, sizeof b, "0,%s", fa);
                asm_line("", "LA", b, a->name);
                snprintf(b, sizeof b, "0,%s+%d", pl, k * 4);
                asm_line("", "ST", b, "");
            }
            if (st->ndop) {
                /* OS/360 convention: the high bit of the last address marks the
                 * end of the list, which is how the callee knows where to stop. */
                snprintf(b, sizeof b, "%s+%d,X'80'", pl, (st->ndop - 1) * 4);
                asm_line("", "OI", b, "high bit marks the last argument");
            }
            if (st->src >= 0) {
                /* By name: the name goes into an 8-byte field, space padded,
                 * and COBDCAL loads the program if it is not already loaded
                 * and calls it with R1 -> the parameter list. */
                const Sym *nm = &syms[st->src];
                char fn[64];
                use_dcal = 1;
                asm_line("", "MVC", "DYNNAME,=CL8' '", "");
                need_sym_base(nm);
                field_ref_m(nm, NULL, FR_SS_NOLEN, nm->bytes, 7, fn, sizeof fn);
                snprintf(b, sizeof b, "DYNNAME(%d),%s", nm->bytes, fn);
                asm_line("", "MVC", b, "the program name");
                snprintf(b, sizeof b, "1,%s", pl);
                asm_line("", "LA", b, "");
                asm_line("", "ST", "1,DYNPARM+4", "the callee's parameter list");
                asm_line("", "LA", "1,DYNPARM", "");
                asm_line("", "L", "15,VDCAL", "");
                asm_line("", "BALR", "14,15", "CALL identifier: load by name and call");
                reset_bases();
                break;
            }
            snprintf(b, sizeof b, "1,%s", pl);
            asm_line("", "LA", b, "R1 -> parameter list");
            snprintf(b, sizeof b, "15,%s", vc);
            asm_line("", "L", b, "");
            asm_line("", "BALR", "14,15", "static call, resolved by the linkage editor");
            break;
        }
        case ST_CANCEL: {
            use_dcal = 1;
            asm_comment(" CANCEL");
            asm_line("", "MVC", "DYNNAME,=CL8' '", "");
            if (st->src >= 0) {
                const Sym *nm = &syms[st->src];
                char fn[64];
                need_sym_base(nm);
                field_ref_m(nm, NULL, FR_SS_NOLEN, nm->bytes, 7, fn, sizeof fn);
                snprintf(b, sizeof b, "DYNNAME(%d),%s", nm->bytes, fn);
                asm_line("", "MVC", b, "the program name");
            } else {
                int n = (int)strlen(st->para);
                snprintf(b, sizeof b, "DYNNAME(%d),%s", n, intern_str(st->para, n, n));
                asm_line("", "MVC", b, "the program name");
            }
            asm_line("", "LA", "1,DYNPARM", "");
            asm_line("", "L", "15,VCANC", "");
            asm_line("", "BALR", "14,15", "release it, if it was loaded");
            reset_bases();
            break;
        }
        case ST_EXITPGM:
            if (!is_subprogram) { asm_comment(" EXIT PROGRAM in a main program: no effect"); break; }
            asm_comment(" EXIT PROGRAM: back to the caller");
            asm_line("", "L", "13,4(13)", "restore caller's save area");
            asm_line("", "LM", "14,12,12(13)", "restore caller's registers");
            asm_line("", "SR", "15,15", "return code 0");
            asm_line("", "BR", "14", "return to caller");
            break;
        case ST_STOP:
            asm_comment(is_subprogram ? " GOBACK to the caller" : " STOP RUN");
            /* A subprogram must not close the runtime's SYSOUT: the caller may
             * still be using it, and control is coming back here again. */
            if (has_display && !is_subprogram) {
                asm_line("", "L", "15,VTERM", "close anything the runtime opened");
                asm_line("", "BALR", "14,15", "");
            }
            asm_line("", "L", "13,4(13)", "restore caller's save area");
            asm_line("", "LM", "14,12,12(13)", "restore caller's registers");
            asm_line("", "SR", "15,15", "return code 0");
            asm_line("", "BR", "14", "return to caller");
            break;
        case ST_DISPLAY_LIT: {
            int off = 0;
            asm_comment(st->upon_console ? " DISPLAY UPON CONSOLE" : " DISPLAY");
            for (int k = 0; k < st->ndop; k++) {
                if (st->dop[k].sym < 0) {
                    const char *sl = intern_str(st->dop[k].lit, st->dop[k].litlen,
                                                st->dop[k].litlen);
                    snprintf(b, sizeof b, "DSPBUF+%d(%d),%s", off, st->dop[k].litlen, sl);
                    asm_line("", "MVC", b, "");
                    off += st->dop[k].litlen;
                } else {
                    const Sym *sy = &syms[st->dop[k].sym];
                    int n = st->dop[k].part_len;
                    char f[64];
                    need_sym_base(sy);
                    if (st->dop[k].sub) {
                        field_ref_m(sy, st->dop[k].sub, FR_RX, sy->elem, 6, f, sizeof f);
                        snprintf(b, sizeof b, "6,%s", f);
                        asm_line("", "LA", b, "the element");
                        snprintf(b, sizeof b, "DSPBUF+%d(%d),%d(6)", off, n, st->dop[k].part_off);
                    } else snprintf(b, sizeof b, "DSPBUF+%d(%d),%s+%d", off, n, sy->label, st->dop[k].part_off);
                    asm_line("", "MVC", b, "");
                    off += n;
                }
                if (off > 120) die("internal: DISPLAY line longer than 120 characters");
            }
            if (st->upon_console) {
                use_wto = 1;
                snprintf(b, sizeof b, "1,%d", off); asm_line("", "LA", b, "");
                asm_line("", "STH", "1,WTOLEN", "");
                asm_line("", "LA", "1,WTOPARM", "");
                asm_line("", "L", "15,VWTO", "");
                asm_line("", "BALR", "14,15", "to the operator");
                st->litlen = off;
                break;
            }
            snprintf(lab, sizeof lab, "PARM%04d", ++ndlit);
            snprintf(b, sizeof b, "1,%s", lab);
            asm_line("", "LA", b, "");
            asm_line("", "L", "15,VDISP", "");
            asm_line("", "BALR", "14,15", "");
            st->litlen = off;                 /* remembered for the parm list */
            break;
        }
        case ST_DISPLAY_ID:
            break;
        case ST_COMPUTE: {
            const Sym *d = &syms[st->dst];
            snprintf(b, sizeof b, " COMPUTE %s%s = ...%s", d->name,
                     st->rounded ? " ROUNDED" : "",
                     st->size_err ? " (ON SIZE ERROR)" : "");
            asm_comment(b);
            char lok[16], lskip[16];
            if (st->size_err) {
                use_szflg = 1;
                snprintf(lok, sizeof lok, "L%04d", ++genlabel);
                snprintf(lskip, sizeof lskip, "L%04d", ++genlabel);
                snprintf(gen_size_skip, sizeof gen_size_skip, "%s", lskip);
                if (st->size_first) asm_line("", "MVI", "SZFLG,X'00'", "no size error yet");
            }
            int rs = gen_expr(st->expr, 0, d->scale);
            gen_rescale16("WK0", rs, d->scale, st->rounded);
            asm_line("", "ZAP", "PWK1(16),WK0(16)", "");
            if (st->size_err) {
                /* A size error is a result whose magnitude has more integer
                 * digits than the item holds -- after ROUNDED, since rounding
                 * can carry. The magnitude is compared against 10 to the
                 * item's digit count at the item's scale; the sign nibble is
                 * forced positive so one compare covers both signs. */
                char lim[40];
                lim[0] = '1';
                memset(lim + 1, '0', (size_t)d->digits);
                lim[d->digits + 1] = 0;
                asm_line("", "ZAP", "WK1(16),PWK1(16)", "");
                asm_line("", "OI", "WK1+15,X'0F'", "magnitude");
                snprintf(b, sizeof b, "WK1(16),%s(16)", intern_const(lim));
                asm_line("", "CP", b, "against 10 ** digits");
                asm_line("", "BL", lok, "fits");
                asm_line("", "MVI", "SZFLG,X'01'", "size error: the item is left alone");
                asm_line("", "B", lskip, "");
                asm_line(lok, "DS", "0H", "");
                reset_bases();
                gen_store(d, st->dsub, "PWK1");
                asm_line(lskip, "DS", "0H", "");
                reset_bases();
                gen_size_skip[0] = 0;
                if (st->size_last) {
                    snprintf(b, sizeof b, "L%04d", st->lab2);
                    asm_line("", "CLI", "SZFLG,X'00'", "any size error in the series?");
                    asm_line("", "BE", b, "none: past the imperative statements");
                }
            } else gen_store(d, st->dsub, "PWK1");
            break;
        }
        case ST_MOVE:
        case ST_ADD:
        case ST_SUB: {
            const Sym *d = &syms[st->dst];
            const char *verb = st->op == ST_MOVE ? "MOVE" :
                               st->op == ST_ADD  ? "ADD"  : "SUBTRACT";
            const char *figname = fig_name(st->fig);
            if (st->fig)      snprintf(b, sizeof b, " %s %s -> %s", verb, figname, d->name);
            else if (st->imm) snprintf(b, sizeof b, " %s %s -> %s", verb, st->immdigits, d->name);
            else              snprintf(b, sizeof b, " %s %s -> %s", verb, syms[st->src].name, d->name);
            asm_comment(b);
            if (st->op == ST_MOVE) {
                if (st->fig) {
                    /* Set the first byte and let MVC propagate it across the
                     * rest, one byte at a time, which is what the overlapping
                     * operands of an SS instruction do. That needs no constant
                     * at all -- the earlier version built one the width of the
                     * receiving item, which capped MOVE SPACES at the token
                     * buffer rather than at anything the language cares about.
                     * Addressing goes through R1 so it works the same whether
                     * the item is subscripted or reached off a base locator. */
                    int dn = st->dsub ? d->elem : d->bytes;
                    if (dn > 256) die("a figurative MOVE is limited to 256 bytes");
                    char fd[64];
                    field_ref_m(d, st->dsub, FR_RX, dn, 6, fd, sizeof fd);
                    snprintf(b, sizeof b, "1,%s", fd);
                    asm_line("", "LA", b, figname);
                    int ul = 1;
                    if (st->fig == FIG_ALL) {
                        /* The first copy of the unit, then the overlapping MVC
                         * repeats it: each byte is copied from one unit-length
                         * behind, so the pattern carries across. A unit longer
                         * than the item is simply cut. */
                        ul = st->litlen < dn ? st->litlen : dn;
                        snprintf(b, sizeof b, "0(%d,1),%s", ul, intern_str(st->lit, ul, ul));
                        asm_line("", "MVC", b, "the unit");
                    } else {
                        snprintf(b, sizeof b, "0(1),%s", fig_byte(st->fig));
                        asm_line("", "MVI", b, "");
                    }
                    if (dn > ul) {
                        snprintf(b, sizeof b, "%d(%d,1),0(1)", ul, dn - ul);
                        asm_line("", "MVC", b, "propagate across the item");
                    }
                    break;
                }
                if (st->imm == 2 && !(d->is_alpha || d->is_group)) {
                    /* An alphanumeric literal into a numeric item. COBOL takes
                     * the characters as the digits and aligns on the decimal
                     * point, which for an integer means right-aligned, zero
                     * filled or truncated on the left -- the same rule the
                     * item-to-item path below implements. Being a literal, the
                     * whole thing can be worked out here and moved as one
                     * constant.
                     *
                     * This is legal ANS COBOL and real programs do it. It
                     * stopped compiling when quoted literals were correctly
                     * typed as alphanumeric rather than numeric, which is what
                     * MOVE '20' TO a PIC 99 had been relying on. */
                    if (d->usage != U_DISPLAY || d->is_signed || d->scale != 0 ||
                        d->edited)
                        die("MOVE of a nonnumeric literal to that numeric item "
                            "is not implemented yet -- only an unsigned display "
                            "integer");
                    for (int k = 0; k < st->immscale; k++)
                        if (!isdigit((unsigned char)st->immdigits[k]))
                            die("MOVE of a nonnumeric literal to a numeric item "
                                "needs the literal to be all digits");
                    int dn = st->dsub ? d->elem : d->bytes;
                    int sn = st->immscale;
                    if (dn > 256) die("MVC is limited to 256 bytes");
                    char pad[MAXTOK];
                    if (sn >= dn) memcpy(pad, st->immdigits + (sn - dn), dn);
                    else {
                        memset(pad, '0', dn - sn);
                        memcpy(pad + dn - sn, st->immdigits, sn);
                    }
                    const char *sl = intern_str(pad, dn, dn);
                    char fd[64];
                    need_sym_base(d);
                    field_ref(d, st->dsub, dn, 6, fd, sizeof fd);
                    snprintf(b, sizeof b, "%s,%s", fd, sl);
                    asm_line("", "MVC", b, "literal digits, right aligned");
                    break;
                }
                if (st->imm == 2) {
                    int dn = st->dsub ? d->elem : d->bytes;
                    if (dn > 256) die("MVC is limited to 256 bytes");
                    const char *sl;
                    if (d->just) {
                        /* A literal is a compile-time string, so JUSTIFIED
                         * costs nothing at run time: pad it on the left, and
                         * keep the right-hand characters when it is too long. */
                        char pad[260];
                        int sn = st->immscale;
                        if (sn >= dn) memcpy(pad, st->immdigits + (sn - dn), dn);
                        else {
                            memset(pad, ' ', dn - sn);
                            memcpy(pad + dn - sn, st->immdigits, sn);
                        }
                        sl = intern_str(pad, dn, dn);
                    } else sl = intern_str(st->immdigits, st->immscale, dn);
                    char fd[64];
                    field_ref(d, st->dsub, dn, 6, fd, sizeof fd);
                    snprintf(b, sizeof b, "%s,%s", fd, sl);
                    asm_line("", "MVC", b, "literal move, space padded");
                    break;
                }
                if (!st->imm && (d->is_alpha || d->is_group ||
                                 syms[st->src].is_alpha || syms[st->src].is_group)) {
                    const Sym *sv = &syms[st->src];
                    int s_alpha = sv->is_alpha || sv->is_group;
                    int d_alpha = d->is_alpha  || d->is_group;
                    if (s_alpha && !d_alpha && d->usage == U_DISPLAY &&
                        !d->is_signed && d->scale == 0 && !d->edited) {
                        /* Alphanumeric into an unsigned display integer. The
                         * characters are already the zoned digits, so this is a
                         * copy aligned on the right, zero filled or truncated
                         * on the left the way a decimal point alignment would
                         * leave it. */
                        int dn = st->dsub ? d->elem : d->bytes;
                        int sn = st->ssub ? sv->elem : sv->bytes;
                        int cp = dn < sn ? dn : sn;
                        char fs2[64], fdr[64];
                        if (dn > 256 || cp > 256) die("that move needs a loop");
                        need_sym_base(d); need_sym_base(sv);
                        /* Address the receiver through R1 so the offsets below
                         * are plain displacements. Doing the arithmetic on the
                         * label instead produces a relocatable displacement,
                         * which the assembler rejects (IFO228). */
                        field_ref_m(d, st->dsub, FR_RX, dn, 6, fdr, sizeof fdr);
                        snprintf(b, sizeof b, "1,%s", fdr);
                        asm_line("", "LA", b, "");
                        if (dn > cp) {
                            asm_line("", "MVI", "0(1),C'0'", "zero fill on the left");
                            if (dn - cp > 1) {
                                snprintf(b, sizeof b, "1(%d,1),0(1)", dn - cp - 1);
                                asm_line("", "MVC", b, "");
                            }
                        }
                        field_ref_m(sv, st->ssub, FR_SS_NOLEN, cp, 7, fs2, sizeof fs2);
                        snprintf(b, sizeof b, "%d(%d,1),%s", dn - cp, cp, fs2);
                        asm_line("", "MVC", b, "alphanumeric into a numeric item");
                        break;
                    }
                    /* One dispatcher for the category rules, shared with the
                     * Report Writer's SOURCE placement. */
                    emit_move(d, st->dsub, sv, st->ssub);
                    break;
                }
                if (st->imm) {
                    gen_load_imm(intern_const(st->immdigits), "PWK1");
                    gen_store(d, st->dsub, "PWK1");
                } else {
                    /* Item to item, through the same dispatcher the
                     * alphanumeric categories use. This was an inline copy of
                     * emit_move's tail -- load, rescale, store -- which meant a
                     * numeric MOVE never saw anything emit_move learned. That
                     * is the second time a duplicate of this path has gone
                     * stale; there is now only one. */
                    emit_move(d, st->dsub, &syms[st->src], st->ssub);
                }
            } else {
                /* Compute at the wider of the two scales, then truncate once on
                   store. Rescaling the source down first would lose digits that
                   the addition still needs: at scale 1, 0.05 + 0.05 must give
                   0.1, not 0.0. */
                int ss = st->imm ? st->immscale : syms[st->src].scale;
                int ws = d->scale > ss ? d->scale : ss;
                gen_load(d, st->dsub, "PWK1");
                gen_rescale("PWK1", d->scale, ws);
                if (st->imm) { gen_load_imm(intern_const(st->immdigits), "PWK2"); }
                else { gen_load(&syms[st->src], st->ssub, "PWK2"); }
                gen_rescale("PWK2", ss, ws);
                asm_line("", st->op == ST_ADD ? "AP" : "SP", "PWK1(16),PWK2(16)", "");
                gen_rescale("PWK1", ws, d->scale);
                gen_store(d, st->dsub, "PWK1");
            }
            break;
        }
        }
    }

    if (cur_para >= 0 && paras[cur_para].is_range_end) {
        char x[16], f[16];
        snprintf(x, sizeof x, "X%04d", cur_para); snprintf(f, sizeof f, "F%04d", cur_para);
        asm_comment(" end of a PERFORM range: return through its cell");
        snprintf(b, sizeof b, "15,%s", x);  asm_line("", "L", b, "");
        asm_line("", "BR", "15", "");
        asm_line(f, "DS", "0H", "fall-through when not performed");
    }

    {
        int any_isam = 0;
        for (int i = 0; i < nfile; i++)
            if (files[i].isam == 2 || (files[i].isam && files[i].opened_output))
                any_isam = 1;
        if (any_isam) {
            asm_comment(" BISAM permanent-error exit; a missing record instead");
            asm_comment(" shows up as a non-zero exception code in the DECB");
            asm_line("ISYNAD", "MVI", "ISFLG,X'01'", "");
            asm_line("", "BR", "14", "");
            asm_line("ISFLG", "DC", "X'00'", "");
            /* One DECB per BISAM READ statement. Layout is exactly what the
             * READ macro generates, confirmed against its expansion. */
            for (int k = 0; k < nstmt; k++) {
                if (stmts[k].op != ST_READ) continue;
                File *rf = &files[stmts[k].dst];
                if (rf->isam != 2) continue;
                char lab[16]; snprintf(lab, sizeof lab, "DB%03d", stmts[k].dst);
                static int done[MAXFILE];
                if (done[stmts[k].dst]) continue;
                done[stmts[k].dst] = 1;
                asm_line(lab, "DS", "0F", "BISAM DECB");
                asm_line("", "DC", "A(0)", "+0  ECB");
                asm_line("", "DC", "X'0280'", "+4  type");
                asm_line("", "DC", "AL2(0)", "+6  length");
                char ab[64];
                snprintf(ab, sizeof ab, "A(%s)", rf->label);
                asm_line("", "DC", ab, "+8  DCB");
                asm_line("", "DC", "A(0)", "+12 area, set at OPEN");
                asm_line("", "DC", "A(0)", "+16 record pointer word");
                asm_line("", "DC", "A(0)", "+20 key, set at READ");
                asm_line("", "DC", "AL2(0)", "+24 exception code");
            }
        }
    }
    for (int i = 0; i < nstmt; i++)
        if (stmts[i].op == ST_SEARCH) {
            char lab[16];
            snprintf(lab, sizeof lab, "SL%03d", i);
            asm_line(lab, "DC", "H'0'", "SEARCH ALL low bound");
            snprintf(lab, sizeof lab, "SH%03d", i);
            asm_line(lab, "DC", "H'0'", "high bound");
        }
    for (int i = 0; i < nstmt; i++)
        if (stmts[i].op == ST_PERFORM && stmts[i].times_expr) {
            char lab[16]; snprintf(lab, sizeof lab, "PT%03d", i);
            asm_line(lab, "DC", "H'0'", "PERFORM n TIMES counter");
        }

    /* ---- report group renderers, reached only by BAL ---- */
    for (int i = 0; i < nrgroup; i++) emit_report_group(i);
    if (nreport) {
        asm_line("VWRL", "DC", "V(COBWRL)", "");
        for (int i = 0; i < nrgroup; i++) {
            char lab[16];
            snprintf(lab, sizeof lab, "RGP%03d", i);
            Report *rp = &reports[rgroups[i].report];
            snprintf(b, sizeof b, "A(%s)", files[rp->file].label);
            asm_line(lab, "DC", b, rgroups[i].name);
            snprintf(b, sizeof b, "A(%s)", rp->lbl_line); asm_line("", "DC", b, "");
            asm_line("", "DC", "A(RTGT)", "");
            asm_line("", "DC", "X'80',AL3(RBUF)", "last parameter");
        }
        for (int i = 0; i < nreport; i++) {
            asm_line(reports[i].lbl_line, "DC", "H'0'", reports[i].name);
            asm_line(reports[i].lbl_page, "DC", "H'0'", "");
            asm_line(reports[i].lbl_first, "DC", "H'0'", "forced first-detail line");
        }
        asm_line("RTGT", "DS", "H", "target line");
        asm_line("RGS", "DS", "F", "renderer return address");
        asm_line("RCTL", "DC", "C' '", "pending carriage control");
        asm_line("RBUF", "DC", "CL133' '", "ASA byte + 132 columns");
    }

    /* ---- PERFORM exit cells ---- */
    for (int i = 0; i < npara; i++) {
        if (!paras[i].altered) continue;
        char x[16];
        snprintf(x, sizeof x, "AL%04d", i);
        if (paras[i].alter_to >= 0) snprintf(b, sizeof b, "A(P%04d)", paras[i].alter_to);
        else snprintf(b, sizeof b, "A(0)");
        asm_line(x, "DC", b, paras[i].alter_to >= 0 ? paras[i].name : "bare GO TO: undefined until ALTERed");
    }
    for (int i = 0; i < npara; i++) {
        if (!paras[i].is_range_end) continue;
        char x[16], f[16];
        snprintf(x, sizeof x, "X%04d", i); snprintf(f, sizeof f, "F%04d", i);
        snprintf(b, sizeof b, "A(%s)", f);
        asm_line(x, "DC", b, paras[i].name);
    }

    /* ---- constants and parameter lists ---- */
    if (has_display) {
        asm_line("VDISP", "DC", "V(COBDISP)", "");
        asm_line("VTERM", "DC", "V(COBTERM)", "");
    }
    /* Outside the DISPLAY guard: a program may want the date and print
     * nothing. */
    if (curdate_sym >= 0) asm_line("VDATE", "DC", "V(COBDATE)", "");
    for (int i = 0; i < nfile; i++)
        if (files[i].print) { asm_line("VADV", "DC", "V(COBADV)", ""); break; }
    if (uses_switches) {
        /* In the program's own CSECT, where the tests can reach it. */
        asm_line("UPSIB", "DC", "X'00'", "the eight switches, UPSI-0 leftmost");
        if (!is_subprogram) asm_line("VUPSI", "DC", "V(COBUPSI)", "");
    }
    for (int i = 0; i < nstmt; i++)
        if (stmts[i].op == ST_ACCEPT) {
            asm_line("VACC", "DC", "V(COBACC)", "");
            asm_line("ACCPARM", "DS", "2F", "ACCEPT parameter list");
            asm_line("ACCLEN", "DS", "H", "");
            break;
        }
    ndlit = 0;
    for (int i = 0; i < nstmt; i++) {
        Stmt *st = &stmts[i];
        if (st->op == ST_DISPLAY_LIT) {
            char plab[24], llab[24];
            snprintf(plab, sizeof plab, "PARM%04d", ++ndlit);
            snprintf(llab, sizeof llab, "LEN%04d", ndlit);
            asm_line(plab, "DC", "A(DSPBUF)", "");
            snprintf(b, sizeof b, "X'80',AL3(%s)", llab); asm_line("", "DC", b, "last parameter");
            snprintf(b, sizeof b, "H'%d'", st->litlen); asm_line(llab, "DC", b, "");
        }
    }

    if (nsym) {
        asm_comment(" work areas for decimal arithmetic");
        asm_line("DWK", "DS", "D", "CVD/CVB doubleword");
        if (use_hvals) asm_line("HVALS", "DC", "256X'FF'", "HIGH-VALUES, for comparison");
        if (use_lvals) asm_line("LVALS", "DC", "256X'00'", "LOW-VALUES, for comparison");
        if (use_qvals) asm_line("QVALS", "DC", "256X'7D'", "QUOTES, for comparison");
        if (use_spcs)  asm_line("SPCS", "DC", "256C' '", "space padding, for comparison");
        if (use_szflg) asm_line("SZFLG", "DS", "X", "ON SIZE ERROR: set by any receiver of a series");
        if (use_str) asm_line("VSTR", "DC", "V(COBSTR)", "");
        if (use_uns) asm_line("VUNS", "DC", "V(COBUNS)", "");
        if (use_insprop) asm_line("INSPROP", "MVC", "1(0,3),0(3)", "executed: INSPECT CHARACTERS propagation");
        if (use_wto) {
            asm_line("VWTO", "DC", "V(COBWTO)", "");
            asm_line("WTOPARM", "DC", "A(DSPBUF),X'80',AL3(WTOLEN)", "DISPLAY UPON CONSOLE");
            asm_line("WTOLEN", "DS", "H", "");
        }
        if (use_wtor) asm_line("VWTOR", "DC", "V(COBWTOR)", "");
        if (use_dcal) {
            asm_line("VDCAL", "DC", "V(COBDCAL)", "");
            asm_line("VCANC", "DC", "V(COBCANC)", "");
            asm_line("DYNPARM", "DC", "A(DYNNAME),A(0)", "CALL identifier: name, parameter list");
            asm_line("DYNNAME", "DS", "CL8", "");
        }
        if (use_mvl) {
            asm_line("VMVL", "DC", "V(COBMVL)", "");
            asm_line("MVLPARM", "DC", "A(0),A(0),X'80',AL3(MVLLEN)", "COBMVL: receiver, sender, lengths");
            asm_line("MVLLEN", "DS", "2H", "receiver length, sender length");
        }
        if (use_adt) {
            asm_line("VADT", "DC", "V(COBADT)", "");
            asm_line("ADTP1", "DC", "A(ZWK),X'80',AL3(ADTK1)", "ACCEPT FROM DATE");
            asm_line("ADTP2", "DC", "A(ZWK),X'80',AL3(ADTK2)", "ACCEPT FROM DAY");
            asm_line("ADTP3", "DC", "A(ZWK),X'80',AL3(ADTK3)", "ACCEPT FROM TIME");
            asm_line("ADTK1", "DC", "H'1'", "");
            asm_line("ADTK2", "DC", "H'2'", "");
            asm_line("ADTK3", "DC", "H'3'", "");
        }
        for (int k = 0; k < npend_dc; k++)
            asm_line(pend_dc[k].lab, pend_dc[k].op, pend_dc[k].opd, pend_dc[k].cmt);
        asm_line("PWK1", "DS", "PL16", "");
        asm_line("PWK2", "DS", "PL16", "");
        asm_line("EDSRC", "DS", "PL16", "ED source, sized to the selectors");
        asm_line("EDWK", "DS", "CL64", "ED pattern and result");
        asm_line("ZWK", "DS", "CL24", "zoned work area");
        asm_line("MULT8", "DS", "PL8", "MP right operand");
        asm_line("DIVR8", "DS", "PL8", "DP divisor");
        asm_line("QTMP", "DS", "PL8", "DP quotient");
        for (int i = 0; i < 6; i++) {
            char w[8]; snprintf(w, sizeof w, "WK%d", i);
            asm_line(w, "DS", "PL16", i == 0 ? "expression stack" : "");
        }
    }
    for (int i = 0; i < nfile; i++) {
        File *f = &files[i];
        char first[96];
        if (i == 0) asm_comment(" file control blocks");
        if (f->vsam) {
            /* VSAM wants an ACB, an RPL and an exit list where QSAM wants a
             * DCB. VSAMIOS builds these with MODCB because it serves any file
             * at run time; a compiler knows the organization, access and mode
             * when it emits them, so they can be assembled outright.
             *
             * MACRF/OPTCD for a KSDS: KEY addressing, SEQ sequence, NUP
             * because no record is being held for update, and MVE because the
             * record is moved to and from the FD area rather than the program
             * being handed a pointer into VSAM's buffer.
             *
             * IN or OUT comes from how the program opens the file. OUT in
             * SEQ is load mode, which is the only way records get into a KSDS
             * in the first place -- and which is why the keys have to arrive in
             * ascending order. OPEN I-O is OUT as well, since IN is retrieval
             * only, but without RST -- emptying a file the program means to
             * update would be a spectacular way to misread the verb.
             *
             * The RPL's NUP/UPD is the other half of I-O. Under UPD a GET does
             * not merely return the record, it holds it, and the PUT or ERASE
             * that follows acts on what is held rather than on a key. That is
             * exactly COBOL's rule that REWRITE and DELETE follow a READ.
             *
             * RST goes with it. OPEN OUTPUT in COBOL means this program
             * creates the file's contents, and RST is how VSAM says that: the
             * cluster is emptied at OPEN. It requires the cluster to be
             * defined REUSE, which is the right attribute for anything a
             * program loads. The alternative is to DELETE and DEFINE the
             * cluster before every load, which is catalog surgery, and this
             * system has demonstrated what that can cost. */
            /* No EXLST. An exit routine is entered on VSAM's terms, with
             * its own save area and return conventions, which suits a callable
             * routine like VSAMIOS but not code generated inline. Without an
             * EODAD, VSAM simply returns 8 in R15 with feedback 4 at end of
             * data, which is easier to test and impossible to get wrong. */
            /* OUT covers everything that writes. RST is what separates
             * creating a file from adding to one: OPEN OUTPUT means this
             * program supplies the contents, so the cluster is emptied at
             * OPEN; OPEN EXTEND and OPEN I-O mean it is already there. For an
             * ESDS that distinction is the whole of ESDSLOAD versus ESDSADDT,
             * which are otherwise the same program. */
            const char *macrf = f->opened_output ? "OUT,RST"
                              : (f->opened_io || f->opened_extend) ? "OUT"
                              : "IN";
            const char *optcd = f->opened_io ? "UPD" : "NUP";
            /* KEY finds a record by what is in it, ADR by where it is. An
             * entry-sequenced dataset has no key, so it can only be the
             * second. */
            const char *keyadr = f->org == 0 ? "ADR" : "KEY";
            /* SEQ walks the file in key order; DIR goes straight to one record
             * by key. That is the whole of ACCESS IS SEQUENTIAL versus RANDOM,
             * and it is why a random READ has an INVALID KEY rather than an AT
             * END: there is no end to reach when you asked for one record. */
            /* DYNAMIC is both, and the ACB has to say so. The RPL can only
             * be one at a time, so it is set per request -- see the READ. */
            const char *seqdir  = f->access == 1 ? "DIR" : "SEQ";
            const char *macrfsd = f->access == 2 ? "SEQ,DIR" : seqdir;
            snprintf(b, sizeof b, "DDNAME=%s,MACRF=(%s,%s,%s)",
                     f->ddname, keyadr, macrfsd, macrf);
            asm_line(f->label, "ACB", b, "VSAM access method control block");

            /* A direct request has to say which record it means. ARG is the
             * address of the search key and KEYLEN its length; the key lives
             * inside the record area, exactly where RECORD KEY says it does,
             * which is also where VSAMIOS points ARG. */
            char keyarg[80] = "";
            char rrncell[10] = "";
            if (f->org == 2) {
                /* An RRDS always needs an argument, even for a sequential
                 * write that does not use one: VSAM reads the field to report
                 * back which slot it assigned, and an RPL with none abends
                 * S0C4 on the first PUT. VSAMIOS sets ARG for every non-ESDS
                 * request for the same reason.
                 *
                 * COBOL only requires a RELATIVE KEY when the program reads by
                 * number, so when there is none the compiler supplies a
                 * fullword of its own to be written into. No KEYLEN either --
                 * a record number is always four bytes and VSAM knows it. */
                if (f->key_sym >= 0 && !f->rrn_via_cell)
                    snprintf(keyarg, sizeof keyarg, "ARG=%s,",
                             syms[f->key_sym].label);
                else {
                    snprintf(rrncell, sizeof rrncell, "%sK", f->label);
                    snprintf(keyarg, sizeof keyarg, "ARG=%s,", rrncell);
                }
            } else if ((f->access != 0 || f->has_start) && f->key_sym >= 0) {
                const Sym *k = &syms[f->key_sym];
                snprintf(keyarg, sizeof keyarg, "ARG=%s,KEYLEN=%d,",
                         k->label, k->bytes);
            }
            for (int pass = 0; pass < 2; pass++) {
                /* pass 1 is the insert RPL, and only an I-O file that also
                 * inserts needs one. */
                if (pass == 1 && !(f->opened_io && f->has_write)) continue;
                char rlab[10];
                snprintf(rlab, sizeof rlab, "%s%c", f->label, pass ? 'N' : 'R');
                snprintf(first, sizeof first, "%-8s RPL   ACB=%s,AREA=%s,",
                         rlab, f->label, syms[f->rec_sym].label);
                char second[100];
                /* KEQ is an OPTCD sub-option, not an RPL keyword of its own:
                 * find the record whose key equals the argument, rather than
                 * the first one greater than or equal to it. */
                snprintf(second, sizeof second,
                         "AREALEN=%d,RECLEN=%d,%sOPTCD=(%s,%s,%s%s,MVE)",
                         f->reclen, f->reclen, keyarg, keyadr, seqdir,
                         f->access != 0 ? "KEQ," : "",
                         pass ? "NUP" : optcd);
                asm_cont(first, second);
            }
            if (rrncell[0])
                asm_line(rrncell, "DS", "F", "relative record number");
            continue;
        }
        if (f->isam && f->opened_output) {
            /* QISAM load mode. Reading an ISAM file takes every attribute from
             * the label, but creating one means there is no label yet, so the
             * DCB has to carry the geometry: LRECL and BLKSIZE from the FD,
             * and KEYLEN and RKP worked out from where the RECORD KEY sits
             * inside the 01 record.
             *
             * OPTCD=L is the delete option: it reserves the first byte of each
             * record as a delete flag, which is why the corpus records all
             * begin with one and why the key starts at RKP=1. */
            if (f->key_sym < 0) die("an ISAM file opened OUTPUT needs a RECORD KEY");
            int keylen = syms[f->key_sym].bytes;
            int rkp    = syms[f->key_sym].offset - syms[f->rec_sym].offset;
            if (rkp < 1)
                die("RECORD KEY must follow the delete flag; put a PIC X first");
            int blksize = f->blk_chars > 0 ? f->blk_chars
                        : f->reclen * (f->blk_records > 0 ? f->blk_records : 1);
            snprintf(first, sizeof first,
                     "%-8s DCB   DDNAME=%s,DSORG=IS,MACRF=(PM),RECFM=%s,",
                     f->label, f->ddname,
                     (f->blk_records > 1 || (f->blk_chars > 0 && f->blk_chars > f->reclen))
                         ? "FB" : "F");
            char second[96];
            snprintf(second, sizeof second,
                     "LRECL=%d,BLKSIZE=%d,KEYLEN=%d,RKP=%d,OPTCD=L,SYNAD=ISYNAD",
                     f->reclen, blksize, keylen, rkp);
            asm_cont(first, second);
            continue;
        }
        if (f->isam) {
            /* Reading: RECFM, LRECL, BLKSIZE, KEYLEN and RKP all come from the
             * dataset label at OPEN, so the DCB only has to say which access
             * method. Random retrieval is BISAM; sequential is QISAM (GET),
             * which is why the MACRF differs. */
            /* State the record format when the FD states it. OPEN merges JCL
             * DCB= only into fields the program left zero, so a DCB that says
             * nothing lets the JCL win -- and BATCH codes RECFM=F on a DESCIDX
             * that GL039 blocked 257 to a block, which is S03B at OPEN. Where
             * the FD gives no BLOCK CONTAINS, stay silent and let the label
             * speak, which is what the sequential ISAM tests rely on. */
            char rf[16] = "";
            if (f->blk_records > 1 || (f->blk_chars > 0 && f->blk_chars > f->reclen))
                snprintf(rf, sizeof rf, ",RECFM=FB");
            else if (f->blk_records == 1 || f->blk_chars > 0)
                snprintf(rf, sizeof rf, ",RECFM=F");
            snprintf(b, sizeof b, "DDNAME=%s,DSORG=IS,MACRF=(%s)%s%s",
                     f->ddname, f->isam == 2 ? "R" : "GM", rf,
                     f->isam == 2 ? ",SYNAD=ISYNAD" : "");
            asm_line(f->label, "DCB", b, "");
            continue;
        }
        if (f->update) {
            /* Update mode. GL/PL is GET locate and PUTX: the access method
             * hands back a pointer into its own buffer and puts the block back
             * where it came from. The geometry comes from the label, as for
             * any file that already exists. */
            snprintf(b, sizeof b, "DDNAME=%s,DSORG=PS,MACRF=(GL,PL)", f->ddname);
            asm_line(f->label, "DCB", b, "");
            continue;
        }
        if (!f->opened_output) {
            /* Reading: RECFM, LRECL and BLKSIZE all come from the label, so
             * saying nothing is not laziness, it is the only correct thing.
             * Asserting BLKSIZE=LRECL here declared every input file
             * unblocked, and reading a real blocked dataset -- SVD001.COMPANY
             * is FB 50/23450 -- then abends S001-4 the moment OPEN compares
             * them. A DD * gets its attributes from the reader the same way. */
            snprintf(b, sizeof b, "DDNAME=%s,DSORG=PS,MACRF=(GM)", f->ddname);
            asm_line(f->label, "DCB", b, "");
            continue;
        }
        /* Writing: there is no label yet, so the geometry has to be stated.
         * BLOCK CONTAINS gives the blocking factor when the program declares
         * one; without it the records go out unblocked. */
        {
            /* A print file's records carry an ASA control byte the program
             * never sees, so the physical record is one longer than the 01. */
            const char *recfm = (f->report >= 0 || f->print) ? "FBA" : "FB";
            int lrecl = f->reclen + (f->print ? 1 : 0);
            int blk = f->blk_chars > 0 ? f->blk_chars
                    : lrecl * (f->blk_records > 0 ? f->blk_records : 1);
            if (f->blk_chars > 0 && f->blk_chars % lrecl)
                die("BLOCK CONTAINS n CHARACTERS must be a whole number of "
                    "records for a fixed-length file");
            /* A file the program both writes and reads needs both macros: a
             * DCB carrying only PM abends S013 at OPEN INPUT. The geometry is
             * still stated, because the file is created before it is read. */
            const char *macrf = f->opened_input ? "GM,PM" : "PM";
            snprintf(first, sizeof first,
                     "%-8s DCB   DDNAME=%s,DSORG=PS,MACRF=(%s),RECFM=%s,",
                     f->label, f->ddname, macrf, recfm);
            char second[80];
            if (f->reserve) snprintf(second, sizeof second, "LRECL=%d,BLKSIZE=%d,BUFNO=%d", lrecl, blk, f->reserve);
            else snprintf(second, sizeof second, "LRECL=%d,BLKSIZE=%d", lrecl, blk);
            asm_cont(first, second);
        }
    }
    for (int i = 0; i < nfile; i++) {
        if (!files[i].update) continue;
        char ul[12]; snprintf(ul, sizeof ul, "U%03d", i);
        asm_line(ul, "DS", "F", "the record the last GET located");
    }
    for (int i = 0; i < nfile; i++) {
        if (!files[i].print) continue;
        char pb[16]; snprintf(pb, sizeof pb, "%s", files[i].pbuf);
        snprintf(b, sizeof b, "CL%d", files[i].reclen + 1);
        asm_line(pb, "DS", b, "ASA byte + the record");
        char lab[20];
        snprintf(lab, sizeof lab, "%sP", pb);
        snprintf(b, sizeof b, "A(%s)", files[i].label);
        asm_line(lab, "DC", b, "COBADV parameter list");
        snprintf(b, sizeof b, "A(%s)", pb);          asm_line("", "DC", b, "");
        snprintf(b, sizeof b, "A(%sL)", pb);         asm_line("", "DC", b, "");
        snprintf(b, sizeof b, "A(%sO)", pb);         asm_line("", "DC", b, "");
        snprintf(b, sizeof b, "A(%sQ)", pb);         asm_line("", "DC", b, "");
        if (files[i].linage) {
            snprintf(b, sizeof b, "A(%sG)", pb);     asm_line("", "DC", b, "LINAGE cells");
            snprintf(b, sizeof b, "X'80',AL3(%s)", syms[files[i].lc_sym].label);
            asm_line("", "DC", b, "LINAGE-COUNTER");
        } else {
            asm_line("", "DC", "A(0)", "no LINAGE");
            asm_line("", "DC", "X'80',AL3(0)", "");
        }
        snprintf(lab, sizeof lab, "%sL", pb);
        snprintf(b, sizeof b, "H'%d'", files[i].reclen);
        asm_line(lab, "DC", b, "the record length");
        snprintf(lab, sizeof lab, "%sO", pb);
        asm_line(lab, "DC", "H'0'", "lines a BEFORE left owing");
        snprintf(lab, sizeof lab, "%sQ", pb);
        asm_line(lab, "DS", "H", "this line's request");
        if (files[i].linage) {
            snprintf(lab, sizeof lab, "%sG", pb);
            snprintf(b, sizeof b, "H'%d',H'%d',H'%d',H'%d'", files[i].linage,
                     files[i].footing, files[i].top, files[i].bottom);
            asm_line(lab, "DC", b, "LINAGE, FOOTING, TOP, BOTTOM");
        }
    }
    for (int i = 0; i < nfile; i++) {
        if (!files[i].optional) continue;
        snprintf(b, sizeof b, "CL8'%s'", files[i].ddname);
        char lab[20]; snprintf(lab, sizeof lab, "%sN", files[i].label);
        asm_line(lab, "DC", b, "the ddname, for DEVTYPE");
        snprintf(lab, sizeof lab, "%sA", files[i].label);
        asm_line(lab, "DC", "X'00'", "1 when the OPTIONAL file is absent");
    }
    if (use_devtype) asm_line("DVAREA", "DS", "6F", "DEVTYPE's answer");
    /* A TRT table: X'FF' everywhere, then zeros punched into the ranges the
     * test accepts. ORG is the compact way to say that, and it is how the
     * S/370 assembler manuals write it. */
    if (class_used[CLS_DIGIT]) {
        asm_comment(" IS NUMERIC: the digits");
        asm_line("CLSNUM", "DC", "256X'FF'", "");
        asm_line("", "ORG", "CLSNUM+X'F0'", "");
        asm_line("", "DC", "10X'00'", "0-9");
        asm_line("", "ORG", "CLSNUM+256", "");
    }
    if (class_used[CLS_ALPHA]) {
        asm_comment(" IS ALPHABETIC: A-Z and the space");
        asm_line("CLSALF", "DC", "256X'FF'", "");
        asm_line("", "ORG", "CLSALF+X'40'", "");
        asm_line("", "DC", "X'00'", "space");
        asm_line("", "ORG", "CLSALF+X'C1'", "");
        asm_line("", "DC", "9X'00'", "A-I");
        asm_line("", "ORG", "CLSALF+X'D1'", "");
        asm_line("", "DC", "9X'00'", "J-R");
        asm_line("", "ORG", "CLSALF+X'E2'", "");
        asm_line("", "DC", "8X'00'", "S-Z");
        asm_line("", "ORG", "CLSALF+256", "");
    }
    if (class_used[CLS_SIGN]) {
        asm_comment(" the overpunched digit: a C, D or F zone is all valid");
        asm_line("CLSSGN", "DC", "256X'FF'", "");
        asm_line("", "ORG", "CLSSGN+X'C0'", "");
        asm_line("", "DC", "10X'00'", "positive");
        asm_line("", "ORG", "CLSSGN+X'D0'", "");
        asm_line("", "DC", "10X'00'", "negative");
        asm_line("", "ORG", "CLSSGN+X'F0'", "");
        asm_line("", "DC", "10X'00'", "unsigned");
        asm_line("", "ORG", "CLSSGN+256", "");
    }

    for (int i = 0; i < nconst; i++) {
        snprintf(b, sizeof b, "PL16'%s'", consts[i].digits);
        asm_line(consts[i].label, "DC", b, i ? "" : "numeric constants");
    }
    for (int i = 0; i < nmconst; i++) {
        char hex[PIC_MAXMASK * 2 + 8]; int j = 0;
        for (int k = 0; k < mconsts[i].len; k++)
            j += snprintf(hex + j, sizeof hex - j, "%02X", mconsts[i].b[k]);
        snprintf(b, sizeof b, "XL%d'%s'", mconsts[i].len, hex);
        asm_line(mconsts[i].label, "DC", b, i ? "" : "ED patterns");
    }
    for (int i = 0; i < nhconst; i++) {
        snprintf(b, sizeof b, "H'%d'", hconsts[i].v);
        asm_line(hconsts[i].label, "DC", b, i ? "" : "element sizes");
    }
    for (int i = 0; i < nsconst; i++) {
        /* A literal padded out to a wide item makes a constant too long for
         * one statement -- MOVE 'HI' TO an X(80) is enough. Adjacent DCs lay
         * down the same bytes. */
        emit_split_dc(sconsts[i].label, sconsts[i].text, sconsts[i].len,
                      i ? "" : "nonnumeric constants");
    }
    /* One base locator per 4096-byte chunk of COBWS. */
    {
        int nchunk = (wslen + CHUNK - 1) / CHUNK;
        if (nchunk < 1) nchunk = 1;
        asm_comment(" base locator cells, one per 4096 bytes of COBWS");
        for (int i = 0; i < nchunk; i++) {
            char lab[16];
            snprintf(lab, sizeof lab, "BL%04d", i);
            snprintf(b, sizeof b, "A(WSC%04d)", i);
            asm_line(lab, "DC", b, "");
        }
    }
    {
        int any = 0;
        for (int i = 0; i < nstmt; i++) {
            if (stmts[i].op != ST_CALL) continue;
            char lab[16];
            if (!any) { asm_comment(" CALL parameter lists and entry points"); any = 1; }
            snprintf(lab, sizeof lab, "PL%03d", i);
            snprintf(b, sizeof b, "%dF", stmts[i].ndop ? stmts[i].ndop : 1);
            asm_line(lab, "DS", b, "");
            if (stmts[i].src >= 0) continue;         /* by name: no V-constant */
            snprintf(lab, sizeof lab, "VC%03d", i);
            snprintf(b, sizeof b, "V(%s)", stmts[i].para);
            asm_line(lab, "DC", b, "");
        }
    }
    if (nlinkarea) {
        asm_comment(" one cell per LINKAGE 01, filled in from the parameter list");
        for (int i = 0; i < nlinkarea; i++) {
            char lab[16];
            snprintf(lab, sizeof lab, "PBL%04d", i);
            asm_line(lab, "DC", "A(0)", syms[link_root[i]].name);
        }
    }
    /* Must sit in the program CSECT: it is addressed off the code base. */
    if (has_display) asm_line("DSPBUF", "DS", "CL121", "DISPLAY line");
    {
        int anyvsam = 0;
        for (int i = 0; i < nfile; i++) if (files[i].vsam) anyvsam = 1;
        if (anyvsam) asm_line("VSFB", "DS", "F", "VSAM SHOWCB feedback word");
    }
    asm_line("SAVEAREA", "DS", "18F", "");
    if (gen_lines && nlinetab) {
        /* A program check reports an address; a programmer wants a line. The
         * exit turns one into the other and then lets the abend happen for
         * real, so the completion code and the dump are exactly what they
         * would have been.
         *
         * R15 addresses the exit on entry, which is what makes this possible
         * without a base register of its own -- there is nowhere to save one
         * until you have one. */
        asm_comment(" program-check exit: report the source line, then let it abend");
        asm_line("COBSPIE", "DS", "0H", "");
        asm_line("", "USING", "COBSPIE,15", "");
        asm_line("", "STM", "14,12,SPIEREGS", "R15 is our base on entry");
        asm_line("", "LR", "9,15", "keep a base across the WTO");
        asm_line("", "DROP", "15", "");
        asm_line("", "USING", "COBSPIE,9", "");
        asm_line("", "LR", "10,1", "the PIE");
        asm_comment("  the interruption code, as the digit people know it");
        asm_line("", "SR", "7,7", "");
        asm_line("", "IC", "7,7(,10)", "low byte of the interruption code");
        asm_line("", "N", "7,SPIE15", "");
        asm_line("", "LA", "7,SPIEHEX(7)", "");
        asm_line("", "MVC", "SPIECODE(1),0(7)", "");
        asm_comment("  the interrupt address, as an offset into this module");
        asm_line("", "L", "2,8(,10)", "second word of the old PSW");
        asm_line("", "N", "2,SPIEADR", "leaves the instruction address");
        asm_line("", "S", "2,SPIEBEG", "relative to the entry point");
        asm_comment("  the last table entry at or before it names the statement");
        asm_line("", "L", "3,SPIETAB", "");
        asm_line("", "LH", "4,SPIENUM", "");
        asm_line("", "SR", "5,5", "no line yet");
        asm_line("SPIELOOP", "LTR", "4,4", "");
        asm_line("", "BZ", "SPIEFND", "");
        asm_line("", "LH", "6,0(,3)", "this statement's offset");
        asm_line("", "CR", "6,2", "");
        asm_line("", "BH", "SPIEFND", "past it: the previous one is the answer");
        asm_line("", "LH", "5,2(,3)", "");
        asm_line("", "LA", "3,4(,3)", "");
        asm_line("", "BCTR", "4,0", "");
        asm_line("", "B", "SPIELOOP", "");
        asm_line("SPIEFND", "CVD", "5,SPIEDW", "");
        asm_line("", "UNPK", "SPIELINE(5),SPIEDW+5(3)", "");
        asm_line("", "OI", "SPIELINE+4,X'F0'", "");
        /* And the offset itself, in hex: a fault in the runtime maps to the
         * last statement, which says nothing, and the offset is what the
         * assembler listing is indexed by. */
        asm_line("", "ST", "2,SPIEDW", "the offset, relative to COBBEG");
        asm_line("", "UNPK", "SPIEOFF(7),SPIEDW+1(4)", "low three bytes to zoned");
        asm_line("", "TR", "SPIEOFF(6),SPIEHEXT", "zoned to hex digits");
        asm_line("", "WTO", "MF=(E,SPIEWTO)", "into the job log, beside the abend");
        asm_comment("  cancel the exit and back up to the failing instruction,");
        asm_comment("  so the abend happens for real -- same code, same dump");
        /* Then end the task, with a code that names the interrupt: 3000
         * plus the program interruption code, so a data exception is U3007.
         *
         * The tidier ending would keep S0C7 itself -- back the resume
         * address up by the instruction length, cancel SPIE, and let the
         * instruction check again for real. That was tried both before and
         * after the cancel and neither worked: the program carried on and
         * returned a meaningless code instead of abending. Under 3.8j the
         * PIE's PSW does not appear to steer the resume. An explicit ABEND
         * is deterministic, and the message above names the real code, which
         * is the part a programmer needs. -s turns all of this off and
         * restores a bare S0C7. */
        asm_line("", "SR", "2,2", "");
        asm_line("", "IC", "2,7(,10)", "the interruption code");
        asm_line("", "A", "2,SPIE3000", "");
        asm_line("", "ABEND", "(2),DUMP", "");
        asm_line("SPIEHEX", "DC", "C'0123456789ABCDEF'", "");
        /* A full translate table: zoned X'F0'-X'FF' to the digit. Indexing
         * SPIEHEX-240 instead gives a negative displacement under the exit's
         * base, which assembles without complaint and reads garbage. */
        asm_line("SPIEHEXT", "DC", "240X'00',C'0123456789ABCDEF'", "");
        asm_line("SPIE15", "DC", "F'15'", "");
        asm_line("SPIE3000", "DC", "F'3000'", "");
        asm_line("SPIEADR", "DC", "X'00FFFFFF'", "");
        asm_line("SPIEBEG", "DC", "A(COBBEG)", "");
        asm_line("SPIETAB", "DC", "A(SPIELTB)", "");
        char nb[24]; snprintf(nb, sizeof nb, "H'%d'", nlinetab);
        asm_line("SPIENUM", "DC", nb, "statements in the table");
        asm_line("SPIEREGS", "DS", "15F", "");
        asm_line("SPIEDW", "DS", "D", "");
        asm_cont("SPIEWTO  WTO   'COBC370: PROGRAM CHECK 0C0 LINE 00000 OFFSET 000000',",
                 "MF=L");
        asm_line("SPIECODE", "EQU", "SPIEWTO+29,1", "the 0C? digit, patched above");
        asm_line("SPIELINE", "EQU", "SPIEWTO+36,5", "the line number, likewise");
        asm_line("SPIEOFF", "EQU", "SPIEWTO+49,7", "the offset from COBBEG, in hex");
        asm_comment(" statement offsets, ascending, paired with source lines");
        asm_line("SPIELTB", "DS", "0H", "");
        for (int i = 0; i < nlinetab; i++) {
            char t[64];
            snprintf(t, sizeof t, "AL2(T%04d-COBBEG),AL2(%d)", i, linetab[i].line);
            asm_line("", "DC", t, "");
        }
    }

    /* ---- COBWS: WORKING-STORAGE and the file records ----
     * A separate CSECT so its size never competes with code addressability.
     * Chunk origins are EQUs, so no padding is needed and a field may straddle
     * a boundary: only its first byte must be within 4095 of its base. */
    {
        int nchunk = (wslen + CHUNK - 1) / CHUNK;
        if (nchunk < 1) nchunk = 1;
        asm_line("COBWS", "CSECT", "", "");
        for (int i = 0; i < nchunk; i++) {
            char lab[16];
            snprintf(lab, sizeof lab, "WSC%04d", i);
            if (i == 0) snprintf(b, sizeof b, "COBWS");
            else snprintf(b, sizeof b, "COBWS+%d", i * CHUNK);
            asm_line(lab, "EQU", b, i ? "" : "chunk origins");
        }
        asm_comment(" WORKING-STORAGE");
        /* Emit one element's worth of definition per item and let the gap
         * filler reserve the rest of a table. A group's DS 0CLn occupies no
         * storage at all -- it is only a label -- so without this a table
         * reserves a single element and everything after it lands on top of
         * elements two onward. */
        int at = 0;
        for (int i = 0; i < nsym; i++) {
            Sym *sy = &syms[i];
            char cmt[96];
            if (sy->is_88 || sy->is_switch) continue;
            if (sy->linkage) continue;      /* described by a DSECT, not stored */
            if (sy->alias) {
                /* Shares storage with what it redefines, so it defines no bytes
                 * -- just a name for the same address. COBWS is the CSECT
                 * origin, and the assembler works out the displacement from
                 * whichever chunk base is in use. */
                snprintf(b, sizeof b, "COBWS+%d", sy->offset);
                snprintf(cmt, sizeof cmt, "%s REDEFINES", sy->name);
                asm_line(sy->label, "EQU", b, cmt);
                continue;
            }
            if (sy->offset > at) {
                snprintf(b, sizeof b, "XL%d", sy->offset - at);
                asm_line("", "DS", b, "reserve the rest of a table");
                at = sy->offset;
            }
            if (sy->is_group) {
                if (sy->elem <= 256) snprintf(b, sizeof b, "0CL%d", sy->elem);
                else                 snprintf(b, sizeof b, "0C");
                if (sy->occurs) snprintf(cmt, sizeof cmt, "%s (%02d group, OCCURS %d)",
                                         sy->name, sy->level, sy->occurs);
                else            snprintf(cmt, sizeof cmt, "%s (%02d group)", sy->name, sy->level);
                asm_line(sy->label, "DS", b, cmt);
                continue;                      /* a label only: no storage */
            }
            char dup[12] = "";
            if (sy->occurs > 1) snprintf(dup, sizeof dup, "%d", sy->occurs);
            if (sy->is_alpha && sy->has_value == 7) {
                /* VALUE ALL literal: the unit repeated to the item's width and
                 * laid down as a run of DCs, which is what a long plain
                 * literal becomes too. */
                char allrep[257];
                int ul = (int)strlen(sy->value), w = sy->elem;
                if (w > 256 || sy->occurs)
                    die("VALUE ALL on an item wider than 256 bytes, or on a table, is not implemented");
                for (int k = 0; k < w; k++) allrep[k] = sy->value[k % ul];
                allrep[w] = 0;
                snprintf(cmt, sizeof cmt, "%s PIC X(%d) VALUE ALL", sy->name, w);
                emit_split_dc(sy->label, allrep, w, cmt);
                at = sy->offset + sy->bytes;
                continue;
            }
            if (sy->is_alpha) {
                if (sy->has_value == 3 && sy->elem > 256)
                    die("a VALUE literal on an item longer than 256 bytes is "
                        "not implemented yet");
                if (sy->has_value == 3 && !dup[0]
                    && escaped_len(sy->value) + 8 > 71 - 15) {
                    /* Too long for one statement. Adjacent DCs lay the same
                     * bytes down, so the field is unchanged; only the source
                     * is split. The label goes on the first. */
                    snprintf(cmt, sizeof cmt, "%s PIC X(%d)", sy->name, sy->elem);
                    emit_split_dc(sy->label, sy->value, sy->elem, cmt);
                    at = sy->offset + sy->bytes;
                    continue;
                } else if (sy->has_value == 3) {
                    char op[MAXTOK * 2 + 24]; int j = 0;
                    j += snprintf(op + j, sizeof op - j, "%sCL%d'", dup, sy->elem);
                    for (const char *q = sy->value; *q; q++) {
                        if (*q == '\'' || *q == '&') op[j++] = *q;
                        op[j++] = *q;
                    }
                    op[j++] = '\''; op[j] = 0;
                    snprintf(b, sizeof b, "%s", op);
                } else if (sy->has_value >= 4) {
                    /* A repeated byte: X'00', X'FF', or the EBCDIC quote X'7D'.
                     * A duplication factor keeps this independent of length,
                     * so tables and long items need no special case. */
                    const char *by = sy->has_value == 4 ? "00"
                                   : sy->has_value == 5 ? "FF" : "7D";
                    snprintf(b, sizeof b, "%dX'%s'",
                             sy->elem * (sy->occurs ? sy->occurs : 1), by);
                } else if (sy->elem <= 256) {
                    snprintf(b, sizeof b, "%sCL%d' '", dup, sy->elem);
                } else {
                    snprintf(b, sizeof b, "%dC' '", sy->elem * (sy->occurs ? sy->occurs : 1));
                }
                snprintf(cmt, sizeof cmt, "%s PIC X(%d)%s", sy->name, sy->elem,
                         sy->occurs ? " table" : "");
                asm_line(sy->label, "DC", b, cmt);
                at = sy->offset + sy->bytes;
                continue;
            }
            if (sy->edited) {           /* an edited field holds characters */
                snprintf(b, sizeof b, "%sCL%d' '", dup, sy->elem);
                snprintf(cmt, sizeof cmt, "%s edited, %d chars", sy->name, sy->elem);
                asm_line(sy->label, "DC", b, cmt);
                at = sy->offset + sy->bytes;
                continue;
            }
            const char *v = (sy->has_value == 1) ? sy->value : "0";
            char digs[80], hex[96];
            switch (sy->usage) {
            case U_DISPLAY:
                /* A Z constant signs the last byte's zone C for a positive
                 * value. An UNSIGNED item must carry F there, or DISPLAY shows
                 * the final digit as a letter -- 12345 prints as 1234E -- and
                 * any byte-wise comparison against it fails. */
                if (sy->is_signed && sy->elem > 16) {
                    /* A Z constant is capped at 16 bytes like everything else
                     * that touches zoned data, so a wide one is written as the
                     * digits followed by the last one carrying its own sign
                     * zone -- which is all a Z constant would have produced. */
                    const char *p2 = v; int neg = 0;
                    if (*p2 == '+' || *p2 == '-') { neg = (*p2 == '-'); p2++; }
                    zero_pad(p2, sy->elem, digs, sizeof digs);
                    snprintf(b, sizeof b, "%sCL%d'%.*s',XL1'%c%c'", dup,
                             sy->elem - 1, sy->elem - 1, digs,
                             neg ? 'D' : 'C', digs[sy->elem - 1]);
                }
                else if (sy->is_signed) snprintf(b, sizeof b, "%sZL%d'%s'", dup, sy->elem, v);
                else {
                    zero_pad(v, sy->elem, digs, sizeof digs);
                    snprintf(b, sizeof b, "%sCL%d'%s'", dup, sy->elem, digs);
                }
                break;
            case U_COMP3:
                /* Same for packed: a P constant signs C, and an unsigned packed
                 * item carries F. Latent until such a field is compared as
                 * bytes -- which is exactly what an ISAM key is. */
                if (sy->is_signed) snprintf(b, sizeof b, "%sPL%d'%s'", dup, sy->elem, v);
                else {
                    zero_pad(v, sy->elem * 2 - 1, digs, sizeof digs);
                    snprintf(hex, sizeof hex, "%sF", digs);
                    snprintf(b, sizeof b, "%sXL%d'%s'", dup, sy->elem, hex);
                }
                break;
            default:
                /* The length modifier is not decoration: a bare DC F or DC H is
                 * ALIGNED by the assembler, which slides in slack bytes this
                 * compiler's no-padding layout knows nothing about. Label-based
                 * addressing hides that -- every internal access stays
                 * self-consistent -- until a GROUP address crosses a boundary
                 * the layout did not predict: CALL 'DYNALOAD' USING MB-FTL-IN
                 * handed FTL an address three slack bytes short of where
                 * FTL-YEAR had really been placed, FTL read a garbage date, and
                 * GL024 selected 0 of 24525 transactions with RC=0000. FL4/HL2
                 * are byte-aligned, which S/370's byte-oriented operands allow.
                 */
                snprintf(b, sizeof b, "%s%sL%d'%s'", dup,
                         sy->elem == 2 ? "H" : "F", sy->elem, v);
                break;
            }
            snprintf(cmt, sizeof cmt, "%s PIC %s9(%d)v%d %s%s",
                     sy->name, sy->is_signed ? "S" : "", sy->digits, sy->scale,
                     sy->usage == U_COMP ? "COMP" : sy->usage == U_COMP3 ? "COMP-3" : "DISP",
                     sy->occurs ? " table" : "");
            asm_line(sy->label, "DC", b, cmt);
            at = sy->offset + sy->bytes;
        }
        if (wslen > at) {
            snprintf(b, sizeof b, "XL%d", wslen - at);
            asm_line("", "DS", b, "reserve the rest of the last table");
        }
    }

    /* ---- LINKAGE SECTION: one DSECT per 01 ----
     * These describe storage the caller owns. A DSECT reserves nothing; it just
     * names the offsets, and the USING on the matching PBL cell supplies the
     * address that arrived in the parameter list. */
    for (int a = 0; a < nlinkarea; a++) {
        char lab[16], cmt[96];
        snprintf(lab, sizeof lab, "LS%04d", a);
        snprintf(cmt, sizeof cmt, "%s (caller's storage)", syms[link_root[a]].name);
        asm_line(lab, "DSECT", "", cmt);
        int at = 0;
        for (int i = 0; i < nsym; i++) {
            Sym *sy = &syms[i];
            if (!sy->linkage || sy->link_area != a || sy->is_88 || sy->is_switch) continue;
            if (sy->offset > at) {
                snprintf(b, sizeof b, "XL%d", sy->offset - at);
                asm_line("", "DS", b, "");
                at = sy->offset;
            }
            if (sy->is_group) {
                if (sy->elem <= 256) snprintf(b, sizeof b, "0CL%d", sy->elem);
                else                 snprintf(b, sizeof b, "0C");
                snprintf(cmt, sizeof cmt, "%s (%02d group)", sy->name, sy->level);
                asm_line(sy->label, "DS", b, cmt);
                continue;
            }
            snprintf(b, sizeof b, "CL%d", sy->elem * (sy->occurs ? sy->occurs : 1));
            snprintf(cmt, sizeof cmt, "%s", sy->name);
            asm_line(sy->label, "DS", b, cmt);
            at = sy->offset + sy->bytes;
        }
    }

    /* The runtime carries COBWRL as well as COBDISP, so a report program
       needs it even with no DISPLAY anywhere. */
    if (has_display || nreport) emit_runtime();
    asm_line("", "END", "", "");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outname = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outname = argv[++i];
        else if (!strcmp(argv[i], "-s")) gen_lines = 0;
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) { if (ncopy_dirs < 16) copy_dirs[ncopy_dirs++] = argv[++i]; }
        else if (!strncmp(argv[i], "-I", 2) && argv[i][2]) { if (ncopy_dirs < 16) copy_dirs[ncopy_dirs++] = argv[i] + 2; }
        else in = argv[i];
    }
    if (in) {
        const char *sl = strrchr(in, '/');
        if (sl) snprintf(src_dir, sizeof src_dir, "%.*s", (int)(sl - in), in);
    }
    /* One look at the source decides whether the CURRENT-DATE register is
     * worth creating. Cheaper than looking ahead in the parser, and a false
     * positive from a comment costs eight bytes. */
    if (in) {
        FILE *scan = fopen(in, "r");
        if (scan) {
            char ln[512];
            while (fgets(ln, sizeof ln, scan))
                if (strstr(ln, "CURRENT-DATE")) { uses_curdate = 1; break; }
            fclose(scan);
        }
    }
    if (!in) { fprintf(stderr, "usage: cobc370 prog.cbl [-o prog.asm] [-s] [-I dir]...\n"
                       "  -s      strip the line table and program-check exit\n"
                       "  -I dir  a directory to find COPY text in (repeatable)\n"); return 2; }

    src.fp = fopen(in, "r");
    if (!src.fp) { perror(in); return 2; }
    src.name = in; src.p = NULL; src.line = 0;

    out = outname ? fopen(outname, "w") : stdout;
    if (!out) { perror(outname); return 2; }

    next();
    parse_program_id();
    parse_environment();
    parse_data_division();
    parse_procedure();
    generate();

    if (out != stdout) fclose(out);
    return 0;
}
