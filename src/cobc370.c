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

#define MAXLINE 256
#define MAXTOK  64

/* ---- source reader: fixed format ------------------------------------- */
/* cols 1-6 sequence, 7 indicator, 8-72 code, 73-80 sequence. A '*' or '/'
 * in column 7 makes the line a comment. */

typedef struct {
    FILE *fp;
    char  buf[MAXLINE];
    char *p;            /* cursor within the current line's code area */
    int   line;
    const char *name;
} Src;

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
        s->p = s->buf + 7;                         /* code starts col 8 */
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

static void die(const char *msg)
{
    fprintf(stderr, "%s:%d: %s\n", src.name, tok.line ? tok.line : src.line, msg);
    exit(1);
}

static void next(void)
{
    tok.eof = 0;
    for (;;) {
        if (!src.p || !*src.p) { if (!src_fill(&src)) { tok.eof = 1; tok.text[0]=0; return; } }
        while (*src.p && isspace((unsigned char)*src.p)) src.p++;
        if (*src.p) break;
    }
    tok.line = src.line;
    tok.literal = 0;
    if (*src.p == '.') { src.p++; strcpy(tok.text, "."); tok.len = 1; return; }
    if (*src.p == '(' || *src.p == ')') {
        tok.text[0] = *src.p++; tok.text[1] = 0; tok.len = 1; return;
    }
    if (*src.p == '\'' || *src.p == '"') {
        /* Nonnumeric literal. A doubled quote is one quote character; a period
           inside a literal does not end the sentence. */
        char q = *src.p++;
        int i = 0;
        for (;;) {
            if (!*src.p) die("unterminated literal (literals may not span lines)");
            if (*src.p == q) {
                if (*(src.p + 1) == q) { src.p += 2; }
                else { src.p++; break; }
            } else { src.p++; }
            if (i < MAXTOK - 1) tok.text[i++] = *(src.p - 1);
        }
        tok.text[i] = 0; tok.len = i; tok.literal = 1;
        return;
    }
    int i = 0;
    while (*src.p && !isspace((unsigned char)*src.p)) {
        if (*src.p == '.') {
            /* A period is a decimal point only when it sits between digits;
               otherwise it ends the sentence. */
            if (!(i > 0 && isdigit((unsigned char)tok.text[i-1])
                       && isdigit((unsigned char)*(src.p + 1)))) break;
        }
        if (i < MAXTOK-1) tok.text[i++] = (char)toupper((unsigned char)*src.p);
        src.p++;
    }
    tok.text[i] = 0; tok.len = i;
}

static int is(const char *w) { return strcmp(tok.text, w) == 0; }

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
    if (op      && *op)      { n = strlen(op);      memcpy(b + 9,  op, n); }
    if (operand && *operand) { n = strlen(operand); memcpy(b + 15, operand, n); }
    int end = 15 + (operand ? (int)strlen(operand) : 0);
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

/* ---- parser ----------------------------------------------------------- */

static char progid[9];

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
    char value[34];   /* the VALUE literal, already scaled to an integer */
    int  is_88;       /* condition name: no storage, tests its parent */
    int  parent;
    char cvalue[MAXTOK];
    int  cvalue_len, cvalue_str;
} Sym;

#define MAXSYM 256
static Sym syms[MAXSYM];
static int nsym, wslen;

/* ---- expressions ------------------------------------------------------
 * A small AST, evaluated onto a stack of packed work areas. Scales are
 * tracked at compile time; see gen_expr for the intermediate-result rules,
 * which are where this will diverge from GnuCOBOL's unbounded intermediates
 * if it diverges anywhere.
 */
enum { N_SYM, N_LIT, N_STR, N_ADD, N_SUB, N_MUL, N_DIV, N_NEG };

typedef struct Node {
    int kind;
    int sym;
    char lit[MAXTOK];   /* N_LIT: scaled digits.  N_STR: the text */
    int litscale;
    int litlen;         /* N_STR */
    struct Node *l, *r;
} Node;

/* Conditions. Relations compare two expressions; AND/OR short-circuit. */
enum { REL_EQ, REL_LT, REL_GT, REL_NE, REL_NGT, REL_NLT };
enum { C_REL, C_AND, C_OR, C_NOT };

typedef struct Cond {
    int kind, op;
    Node *l, *r;
    struct Cond *cl, *cr;
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

#define MAXNODE 512
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
       ST_PARA, ST_PERFORM, ST_STOP, ST_EXIT,
       ST_LABEL, ST_BRANCH, ST_IFTEST };

typedef struct {
    int  op;
    int  dst, src;          /* symbol indices, -1 when unused */
    char lit[MAXTOK];       /* DISPLAY literal */
    int  litlen;
    int  imm;               /* source is a numeric literal */
    char immdigits[34];     /* that literal, scaled to an integer */
    int  immscale;
    Node *expr;             /* COMPUTE */
    int  rounded;
    char para[31];          /* ST_PARA name, or PERFORM's first paragraph */
    char thru[31];          /* PERFORM ... THRU */
    Cond *cond;             /* ST_IFTEST */
} Stmt;

/* Paragraphs, resolved after parsing so a PERFORM may name one that has not
 * been seen yet. */
typedef struct { char name[31]; int is_range_end; } Para;
#define MAXPARA 256
static Para paras[MAXPARA];
static int npara;

static int para_index(const char *n)
{
    for (int i = 0; i < npara; i++) if (!strcmp(paras[i].name, n)) return i;
    return -1;
}

#define MAXSTMT 512
static Stmt stmts[MAXSTMT];
static int nstmt;

static int lookup(const char *n)
{
    for (int i = 0; i < nsym; i++) if (!strcmp(syms[i].name, n)) return i;
    return -1;
}

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


/* ---- PICTURE ----------------------------------------------------------- */
/* Numeric pictures only: optional S, then 9s and one V, with (n) repetition.
 * Editing characters are a later slice and are rejected by name here. */

static void parse_picture(const char *pic, Sym *sy)
{
    int seen_v = 0;
    sy->digits = 0; sy->scale = 0; sy->is_signed = 0;
    const char *p = pic;
    if (*p == 'S') { sy->is_signed = 1; p++; }
    while (*p) {
        char c = *p++;
        int rep = 1;
        if (*p == '(') {
            rep = 0; p++;
            while (isdigit((unsigned char)*p)) rep = rep * 10 + (*p++ - '0');
            if (*p != ')') die("malformed PICTURE repetition count");
            p++;
        }
        if (c == '9') { sy->digits += rep; if (seen_v) sy->scale += rep; }
        else if (c == 'V') { if (seen_v) die("more than one V in a PICTURE"); seen_v = 1; }
        else {
            char m[96];
            snprintf(m, sizeof m,
                     "PICTURE character '%c' is not implemented yet "
                     "(numeric pictures only: S, 9, V)", c);
            die(m);
        }
    }
    if (sy->digits == 0) die("PICTURE has no digit positions");
    if (sy->digits > 15)
        die("more than 15 digits needs a wider packed work area than this "
            "slice allocates");
}

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
    int dot = 0;
    for (; *p; p++) {
        if (*p == '.') { if (dot) return 0; dot = 1; continue; }
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

/* ---- DATA DIVISION ----------------------------------------------------- */

static void parse_data_division(void)
{
    if (!is("DATA")) return;
    next(); expect("DIVISION"); expect(".");
    if (is("FILE"))
        die("FILE SECTION is not implemented yet");
    if (!is("WORKING-STORAGE")) { while (!tok.eof && !is("PROCEDURE")) next(); return; }
    next(); expect("SECTION"); expect(".");

    /* Open groups, innermost last. A group's size is not known until an item
     * at the same or a lower level closes it. */
    int stack[32], sp = 0;
    int cursor = 0;

    while (!tok.eof && !is("PROCEDURE")) {
        if (!isdigit((unsigned char)tok.text[0]))
            die("expected a level number in WORKING-STORAGE");
        int level = atoi(tok.text);
        if (level != 77 && level != 88 && (level < 1 || level > 49))
            die("level number out of range (01-49, 77, or 88)");
        if (level == 66) die("level 66 RENAMES is not implemented yet");
        next();

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
            next();
            expect("VALUE"); if (is("IS")) next();
            if (is("ZERO") || is("ZEROS") || is("ZEROES")) { strcpy(cn->cvalue, "0"); next(); }
            else if (is("SPACE") || is("SPACES")) { cn->cvalue_str = 1; strcpy(cn->cvalue, " "); cn->cvalue_len = 1; next(); }
            else if (tok.literal) { cn->cvalue_str = 1; memcpy(cn->cvalue, tok.text, (size_t)tok.len + 1); cn->cvalue_len = tok.len; next(); }
            else if (is_numeric_literal(tok.text)) { snprintf(cn->cvalue, sizeof cn->cvalue, "%s", tok.text); next(); }
            else die("level 88 VALUE must be a literal");
            expect(".");
            nsym++;
            continue;
        }

        while (sp > 0 && syms[stack[sp-1]].level >= level) {
            Sym *g = &syms[stack[--sp]];
            g->bytes = cursor - g->offset;
        }
        if (level == 1 || level == 77) {
            while (sp > 0) { Sym *g = &syms[stack[--sp]]; g->bytes = cursor - g->offset; }
            cursor = wslen;                 /* 01 items start a fresh area */
        }

        if (nsym >= MAXSYM) die("too many data items");
        Sym *sy = &syms[nsym];
        memset(sy, 0, sizeof *sy);
        snprintf(sy->label, sizeof sy->label, "D%04d", nsym);
        sy->level = level;
        if (strlen(tok.text) > 30) die("data name too long");
        if (!strcmp(tok.text, "FILLER")) snprintf(sy->name, sizeof sy->name, "FILL%04d", nsym);
        else {
            strcpy(sy->name, tok.text);
            if (lookup(sy->name) >= 0)
                die("duplicate data name (qualification with OF/IN is not "
                    "implemented yet)");
        }
        next();

        char pic[64] = "";
        sy->usage = U_DISPLAY;
        while (!tok.eof && !is(".")) {
            if (is("PIC") || is("PICTURE")) {
                next(); if (is("IS")) next();
                if (strlen(tok.text) >= sizeof pic) die("PICTURE too long");
                strcpy(pic, tok.text); next();
            } else if (is("USAGE")) { next(); if (is("IS")) next(); }
            else if (is("COMP") || is("COMPUTATIONAL")) { sy->usage = U_COMP; next(); }
            else if (is("COMP-3") || is("COMPUTATIONAL-3")) { sy->usage = U_COMP3; next(); }
            else if (is("DISPLAY")) { sy->usage = U_DISPLAY; next(); }
            else if (is("VALUE")) {
                next(); if (is("IS")) next();
                if (is("ZERO") || is("ZEROS") || is("ZEROES")) { strcpy(sy->value, "0"); sy->has_value = 1; next(); }
                else if (is("SPACE") || is("SPACES")) { sy->has_value = 2; next(); }
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

        if (!pic[0]) {
            /* No PICTURE: this is a group, and the items that follow are its
             * subordinates. Its size is filled in when it closes. */
            sy->is_group = 1;
            sy->offset = cursor;
            if (sy->has_value) die("a group item may not carry VALUE here");
            if (sp >= 32) die("group nesting too deep");
            stack[sp++] = nsym;
            nsym++;
            if (level == 1 || level == 77) { /* wslen advances when it closes */ }
            continue;
        }

        if (pic[0] == 'X') {
            sy->is_alpha = 1;
            const char *p = pic + 1;
            int rep = 1;
            if (*p == '(') { rep = 0; p++;
                while (isdigit((unsigned char)*p)) rep = rep * 10 + (*p++ - '0');
                if (*p != ')') die("malformed PICTURE repetition count"); p++; }
            while (*p == 'X') { rep++; p++; }
            if (*p) die("mixed PICTURE characters are not implemented yet");
            sy->bytes = rep;
            if (sy->has_value == 1) die("a numeric VALUE on a PIC X item is not implemented yet");
        } else {
            parse_picture(pic, sy);
            if (sy->has_value == 3) die("a nonnumeric VALUE on a numeric item is not implemented yet");
            if (sy->has_value == 1) {
                char scaled[34];
                scale_literal(sy->value, sy->scale, scaled, sizeof scaled);
                strcpy(sy->value, scaled);
            }
            switch (sy->usage) {
            case U_DISPLAY: sy->bytes = sy->digits; break;
            case U_COMP3:   sy->bytes = sy->digits / 2 + 1; break;
            case U_COMP:
                if (sy->digits <= 4)      sy->bytes = 2;
                else if (sy->digits <= 9) sy->bytes = 4;
                else die("COMP wider than 9 digits needs doubleword arithmetic, "
                         "which S/370 does not have and this slice does not emulate");
                /* Alignment inside a group would move subordinate offsets, so
                 * COMP items are left unaligned here; S/370 decimal and RX
                 * instructions tolerate it. */
                break;
            }
        }
        sy->offset = cursor;
        cursor += sy->bytes;
        if (level == 1 || level == 77) wslen = cursor;
        nsym++;
    }
    while (sp > 0) { Sym *g = &syms[stack[--sp]]; g->bytes = cursor - g->offset; }
    if (cursor > wslen) wslen = cursor;
    if (wslen > 3800)
        die("WORKING-STORAGE exceeds one base-register displacement; "
            "base locator cells are not implemented yet");
}

static void skip_division(const char *kw)
{
    if (!is(kw)) return;
    next(); expect("DIVISION"); expect(".");
    while (!tok.eof && !is("DATA") && !is("PROCEDURE")) next();
}

/* ---- expression parser -------------------------------------------------
 * COBOL requires spaces around the arithmetic operators, which is what makes
 * WS-TOTAL one word and A - B three. Parentheses are separators and tokenize
 * on their own.
 */
static Node *parse_expr(void);

static Node *parse_primary(void)
{
    if (is("(")) { next(); Node *n = parse_expr(); expect(")"); return n; }
    if (is_numeric_literal(tok.text)) {
        Node *n = node(N_LIT);
        const char *dot = strchr(tok.text, '.');
        n->litscale = dot ? (int)strlen(dot + 1) : 0;
        scale_literal(tok.text, n->litscale, n->lit, sizeof n->lit);
        next();
        return n;
    }
    if (tok.literal) {
        Node *n = node(N_STR);
        memcpy(n->lit, tok.text, (size_t)tok.len + 1);
        n->litlen = tok.len;
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
    n->sym = need_sym(tok.text);
    next();
    return n;
}

static Node *parse_unary(void)
{
    if (is("-")) { next(); Node *n = node(N_NEG); n->l = parse_unary(); return n; }
    if (is("+")) { next(); return parse_unary(); }
    return parse_primary();
}

static Node *parse_term(void)
{
    Node *l = parse_unary();
    for (;;) {
        if (is("**")) die("exponentiation is not implemented yet");
        if (is("*")) { next(); Node *n = node(N_MUL); n->l = l; n->r = parse_unary(); l = n; }
        else if (is("/")) { next(); Node *n = node(N_DIV); n->l = l; n->r = parse_unary(); l = n; }
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

static Stmt *new_stmt(int op)
{
    if (nstmt >= MAXSTMT) die("too many statements");
    Stmt *st = &stmts[nstmt++];
    memset(st, 0, sizeof *st);
    st->op = op; st->dst = st->src = -1;
    return st;
}

/* COBOL-74 has no END-IF: a period ends the whole sentence, unwinding every
 * open IF. at_period carries that up through the nested statement lists. */
static int at_period;
static int nlabel;

static void parse_stmt_list(int allow_else);

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
    if (neg) die("NOT must be followed by a relational operator");
    return 0;
}

static Cond *parse_relation(void)
{
    if (is("(")) { next(); Cond *c = parse_cond(); expect(")"); return c; }
    Node *l = parse_expr();
    int op;
    if (!relop(&op)) {
        /* No operator: this must be a level 88 condition name. */
        if (l->kind != N_SYM || !syms[l->sym].is_88)
            die("expected a relational operator, or a level 88 condition name");
        const Sym *cn = &syms[l->sym];
        Cond *c = cnode(C_REL);
        c->op = REL_EQ;
        Node *p = node(N_SYM); p->sym = cn->parent;
        Node *v;
        if (cn->cvalue_str) {
            v = node(N_STR);
            memcpy(v->lit, cn->cvalue, (size_t)cn->cvalue_len + 1);
            v->litlen = cn->cvalue_len;
        } else {
            v = node(N_LIT);
            const char *dot = strchr(cn->cvalue, '.');
            v->litscale = dot ? (int)strlen(dot + 1) : 0;
            scale_literal(cn->cvalue, syms[cn->parent].scale, v->lit, sizeof v->lit);
            v->litscale = syms[cn->parent].scale;
        }
        c->l = p; c->r = v;
        return c;
    }
    Cond *c = cnode(C_REL);
    c->op = op; c->l = l; c->r = parse_expr();
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
    Cond *l = parse_and();
    while (is("OR")) { next(); Cond *c = cnode(C_OR); c->cl = l; c->cr = parse_and(); l = c; }
    return l;
}

/* ---- statements -------------------------------------------------------- */

static void eat_period(void) { if (is(".")) { next(); at_period = 1; } }

static void parse_one_statement(void)
{
    if (is("DISPLAY")) {
        next();
        if (tok.literal) {
            Stmt *st = new_stmt(ST_DISPLAY_LIT);
            memcpy(st->lit, tok.text, (size_t)tok.len + 1);
            st->litlen = tok.len;
            next();
            if (tok.literal) die("DISPLAY of several operands is not implemented yet");
        } else {
            int i = need_sym(tok.text);
            if (syms[i].is_group) die("DISPLAY of a group item is not implemented yet");
            if (syms[i].is_88) die("DISPLAY of a condition name is meaningless");
            if (!syms[i].is_alpha && (syms[i].usage != U_DISPLAY || syms[i].is_signed))
                die("DISPLAY takes an alphanumeric or unsigned USAGE DISPLAY "
                    "item in this slice; MOVE to one first.");
            Stmt *st = new_stmt(ST_DISPLAY_ID);
            st->src = i;
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

    if (is("MOVE")) {
        next();
        Stmt *st = new_stmt(ST_MOVE);
        char save[MAXTOK]; int savelit = tok.literal, savelen = tok.len;
        memcpy(save, tok.text, (size_t)tok.len + 1);
        next();
        expect("TO");
        st->dst = need_sym(tok.text);
        next();
        if (savelit) {
            st->imm = 2;                       /* nonnumeric literal */
            memcpy(st->immdigits, save, (size_t)savelen + 1);
            st->immscale = savelen;
        } else if (is_numeric_literal(save)) {
            st->imm = 1; st->immscale = syms[st->dst].scale;
            scale_literal(save, syms[st->dst].scale, st->immdigits, sizeof st->immdigits);
        } else st->src = need_sym(save);
        eat_period();
        return;
    }

    if (is("ADD") || is("SUBTRACT")) {
        int sub = is("SUBTRACT");
        next();
        Stmt *st = new_stmt(sub ? ST_SUB : ST_ADD);
        char save[MAXTOK];
        memcpy(save, tok.text, (size_t)tok.len + 1);
        next();
        expect(sub ? "FROM" : "TO");
        st->dst = need_sym(tok.text);
        next();
        if (is("GIVING")) die("GIVING is not implemented yet");
        if (is_numeric_literal(save)) {
            const char *dot = strchr(save, '.');
            st->imm = 1;
            st->immscale = dot ? (int)strlen(dot + 1) : 0;
            scale_literal(save, st->immscale, st->immdigits, sizeof st->immdigits);
        } else st->src = need_sym(save);
        eat_period();
        return;
    }

    if (is("COMPUTE")) {
        next();
        Stmt *st = new_stmt(ST_COMPUTE);
        st->dst = need_sym(tok.text);
        next();
        if (is("ROUNDED")) { st->rounded = 1; next(); }
        if (is("EQUAL")) { next(); if (is("TO")) next(); } else expect("=");
        st->expr = parse_expr();
        eat_period();
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
        if (is("UNTIL") || is("VARYING") || is("TIMES"))
            die("PERFORM UNTIL / VARYING / TIMES are not implemented yet");
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
        if (is("PROGRAM")) die("EXIT PROGRAM is not implemented yet");
        new_stmt(ST_EXIT);
        eat_period();
        return;
    }

    if (tok.text[0] && !tok.literal) {
        char nm[31];
        snprintf(nm, sizeof nm, "%s", tok.text);
        next();
        if (is(".")) {
            next(); at_period = 1;
            if (para_index(nm) >= 0) die("duplicate paragraph name");
            if (npara >= MAXPARA) die("too many paragraphs");
            snprintf(paras[npara].name, sizeof paras[npara].name, "%s", nm);
            paras[npara].is_range_end = 0;
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
        parse_one_statement();
    }
}

static void parse_procedure(void)
{
    expect("PROCEDURE"); expect("DIVISION"); expect(".");
    int stopped = 0;
    while (!tok.eof) {
        at_period = 0;
        parse_stmt_list(0);
    }
    for (int i = 0; i < nstmt; i++) if (stmts[i].op == ST_STOP) stopped = 1;
    if (!stopped) die("PROCEDURE DIVISION has no STOP RUN");
    for (int i = 0; i < nstmt; i++) {
        if (stmts[i].op != ST_PERFORM) continue;
        int a = para_index(stmts[i].para), b = para_index(stmts[i].thru);
        if (a < 0) { char m[96]; snprintf(m, sizeof m, "PERFORM names an unknown paragraph '%s'", stmts[i].para); die(m); }
        if (b < 0) { char m[96]; snprintf(m, sizeof m, "PERFORM THRU names an unknown paragraph '%s'", stmts[i].thru); die(m); }
        if (b < a) die("PERFORM THRU runs backwards");
        stmts[i].dst = a; stmts[i].src = b;
        paras[b].is_range_end = 1;
    }
}

/* ---- arithmetic ---------------------------------------------------------
 * Everything is computed in packed decimal, which is what COBOL semantics
 * want and what S/370 does in hardware. Binary (COMP) operands are converted
 * in and out with CVD/CVB; zoned (DISPLAY) with PACK/UNPK. Scale alignment is
 * SRP, whose shift count is the low six bits of the second operand address
 * read as a signed value, so a right shift of k is encoded as 64-k.
 */

static void gen_load(const Sym *sy, const char *wk)
{
    char b[96];
    switch (sy->usage) {
    case U_DISPLAY:
        snprintf(b, sizeof b, "%s(8),%s(%d)", wk, sy->label, sy->bytes);
        asm_line("", "PACK", b, "zoned -> packed");
        break;
    case U_COMP3:
        snprintf(b, sizeof b, "%s(8),%s(%d)", wk, sy->label, sy->bytes);
        asm_line("", "ZAP", b, "");
        break;
    case U_COMP:
        snprintf(b, sizeof b, "2,%s", sy->label);
        asm_line("", sy->bytes == 2 ? "LH" : "L", b, "");
        asm_line("", "CVD", "2,DWK", "binary -> packed");
        snprintf(b, sizeof b, "%s(8),DWK(8)", wk);
        asm_line("", "ZAP", b, "");
        break;
    }
}

static void gen_load_imm(const char *label, const char *wk)
{
    char b[96];
    snprintf(b, sizeof b, "%s(8),%s(8)", wk, label);
    asm_line("", "ZAP", b, "literal");
}

static void gen_rescale(const char *wk, int from, int to)
{
    if (from == to) return;
    char b[96];
    int d = to - from;
    snprintf(b, sizeof b, "%s(8),%d,0", wk, d > 0 ? d : 64 + d);
    asm_line("", "SRP", b, d > 0 ? "align scale (left)" : "align scale (right)");
}

static void gen_store(const Sym *sy, const char *wk)
{
    char b[96];
    switch (sy->usage) {
    case U_DISPLAY:
        snprintf(b, sizeof b, "%s(%d),%s(8)", sy->label, sy->bytes, wk);
        asm_line("", "UNPK", b, "packed -> zoned");
        if (!sy->is_signed) {
            snprintf(b, sizeof b, "%s+%d,X'F0'", sy->label, sy->bytes - 1);
            asm_line("", "OI", b, "unsigned: force an F zone");
        }
        break;
    case U_COMP3:
        snprintf(b, sizeof b, "%s(%d),%s(8)", sy->label, sy->bytes, wk);
        asm_line("", "ZAP", b, "");
        break;
    case U_COMP:
        snprintf(b, sizeof b, "DWK(8),%s(8)", wk);
        asm_line("", "ZAP", b, "");
        asm_line("", "CVB", "2,DWK", "packed -> binary");
        snprintf(b, sizeof b, "2,%s", sy->label);
        asm_line("", sy->bytes == 2 ? "STH" : "ST", b, "");
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
static void gen_load16(const Sym *sy, const char *wk)
{
    char b[96];
    switch (sy->usage) {
    case U_DISPLAY:
        snprintf(b, sizeof b, "%s(16),%s(%d)", wk, sy->label, sy->bytes);
        asm_line("", "PACK", b, "zoned -> packed");
        break;
    case U_COMP3:
        snprintf(b, sizeof b, "%s(16),%s(%d)", wk, sy->label, sy->bytes);
        asm_line("", "ZAP", b, "");
        break;
    case U_COMP:
        snprintf(b, sizeof b, "2,%s", sy->label);
        asm_line("", sy->bytes == 2 ? "LH" : "L", b, "");
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

static int gen_expr(Node *n, int d, int tgtscale)
{
    char b[128], wk[8], wk2[8];
    if (d >= 5) die("expression nests too deeply for the work-area stack");
    snprintf(wk,  sizeof wk,  "WK%d", d);
    snprintf(wk2, sizeof wk2, "WK%d", d + 1);

    switch (n->kind) {
    case N_SYM:
        gen_load16(&syms[n->sym], wk);
        return syms[n->sym].scale;

    case N_LIT: {
        const char *lab = intern_const(n->lit);
        snprintf(b, sizeof b, "%s(16),%s(8)", wk, lab);
        asm_line("", "ZAP", b, "literal");
        return n->litscale;
    }

    case N_NEG: {
        int s = gen_expr(n->l, d + 1, tgtscale);
        snprintf(b, sizeof b, "%s(16),%s(8)", wk, intern_const("0"));
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
    }
    die("internal: bad expression node");
    return 0;
}

/* Alphanumeric move: left justified, space filled, truncated on the right.
 * Groups move as bytes, which is why a group MOVE is alphanumeric whatever
 * its subordinates are. */
static void gen_move_alpha(const Sym *d, const Sym *sv)
{
    char b[128];
    int n = d->bytes < sv->bytes ? d->bytes : sv->bytes;
    if (n > 256) die("MVC is limited to 256 bytes; long moves need a loop, "
                     "which is not implemented yet");
    snprintf(b, sizeof b, "%s(%d),%s", d->label, n, sv->label);
    asm_line("", "MVC", b, "alphanumeric move");
    if (d->bytes > n) {
        int rest = d->bytes - n;
        snprintf(b, sizeof b, "%s+%d,C' '", d->label, n);
        asm_line("", "MVI", b, "space fill the remainder");
        if (rest > 1) {
            if (rest - 1 > 256) die("space fill longer than 256 bytes is not implemented yet");
            snprintf(b, sizeof b, "%s+%d(%d),%s+%d", d->label, n + 1, rest - 1, d->label, n);
            asm_line("", "MVC", b, "");
        }
    }
}

/* A continued statement: text padded so the continuation flag lands in
   column 72, and the continuation itself starting in column 16. */
static void asm_cont(const char *first, const char *second)
{
    char b[80];
    if (strlen(first) > 71) die("internal: continuation line too long");
    memset(b, ' ', sizeof b);
    memcpy(b, first, strlen(first));
    b[71] = 'X'; b[72] = 0;
    fprintf(out, "%s\n", b);
    fprintf(out, "               %s\n", second);
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

static void emit_runtime(void)
{
    asm_comment("---------------------------------------------------------------");
    asm_comment(" COBRT -- our runtime. Nothing here is from SYS1.COBLIB.");
    asm_comment(" DISPLAY reaches SYSOUT through QSAM directly, which is the");
    asm_comment(" same access-method path IKFCBL00 uses for its own file I/O.");
    asm_comment(" Not reentrant: MVS 3.8j batch does not require it.");
    asm_comment("---------------------------------------------------------------");
    asm_line("COBRT", "CSECT", "", "");
    asm_line("", "ENTRY", "COBDISP,COBTERM", "");
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
    asm_comment(" COBTERM -- close SYSOUT if COBDISP ever opened it.");
    asm_comment("");
    asm_line("COBTERM", "STM", "14,12,12(13)", "");
    asm_line("", "BALR", "12,0", "");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,RTSAVE2+4", "");
    asm_line("", "LA", "11,RTSAVE2", "");
    asm_line("", "ST", "11,8(13)", "");
    asm_line("", "LR", "13,11", "");
    asm_line("", "CLI", "RTOPEN,X'01'", "");
    asm_line("", "BNE", "COBT010", "");
    asm_line("", "CLOSE", "(RTDCB)", "");
    asm_line("", "MVI", "RTOPEN,X'00'", "");
    asm_line("COBT010", "L", "13,4(13)", "");
    asm_line("", "LM", "14,12,12(13)", "");
    asm_line("", "SR", "15,15", "");
    asm_line("", "BR", "14", "");
    asm_line("RTOPEN", "DC", "X'00'", "");
    asm_line("RTMAX", "DC", "H'120'", "");
    asm_line("RTLINE", "DC", "CL121' '", "ASA byte + 120 columns");
    asm_line("RTSAVE1", "DS", "18F", "");
    asm_line("RTSAVE2", "DS", "18F", "");
    asm_cont("RTDCB    DCB   DDNAME=SYSOUT,DSORG=PS,MACRF=(PM),RECFM=FBA,",
             "LRECL=121,BLKSIZE=121");
}

/* Branch mnemonics after CP or CLC, by relation and by sense. */
static const char *br_true[]  = { "BE", "BL", "BH", "BNE", "BNH", "BNL" };
static const char *br_false[] = { "BNE", "BNL", "BNH", "BE", "BH", "BL" };

static int genlabel;

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
            int n = ls->bytes;
            if (R->kind == N_STR) {
                const char *sl = intern_str(R->lit, R->litlen, n);
                snprintf(b, sizeof b, "%s(%d),%s", ls->label, n, sl);
            } else if (R->kind == N_SYM && node_alpha(R)) {
                if (syms[R->sym].bytes != n)
                    die("comparing alphanumeric items of different lengths is "
                        "not implemented yet");
                snprintf(b, sizeof b, "%s(%d),%s", ls->label, n, syms[R->sym].label);
            } else die("mixed alphanumeric and numeric comparison is not implemented yet");
            if (n > 256) die("CLC is limited to 256 bytes");
            asm_line("", "CLC", b, "alphanumeric compare");
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

static void generate(void)
{
    char b[200], lab[24];
    int has_display = 0;
    for (int i = 0; i < nstmt; i++)
        if (stmts[i].op == ST_DISPLAY_LIT || stmts[i].op == ST_DISPLAY_ID)
            has_display = 1;

    asm_comment("---------------------------------------------------------------");
    snprintf(b, sizeof b, " Generated by cobc370 from %s", src.name);
    asm_comment(b);
    asm_comment(" Standard OS/360 entry linkage. SYS1.COBLIB is never referenced.");
    if (has_display)
        asm_comment(" DISPLAY is served by our own COBRT runtime, below.");
    asm_comment("---------------------------------------------------------------");

    asm_line(progid, "CSECT", "", "");
    asm_line("", "STM", "14,12,12(13)", "save caller's registers");
    asm_line("", "BALR", "12,0", "establish addressability");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,SAVEAREA+4", "backward chain to caller");
    asm_line("", "LA", "11,SAVEAREA", "");
    asm_line("", "ST", "11,8(13)", "forward chain from caller");
    asm_line("", "LR", "13,11", "our save area is now current");

    int ndlit = 0, cur_para = -1, nret = 0;
    genlabel = nlabel;
    for (int i = 0; i < nstmt; i++) {
        Stmt *st = &stmts[i];
        if (st->op == ST_PARA && cur_para >= 0 && paras[cur_para].is_range_end) {
            char x[16], f[16];
            snprintf(x, sizeof x, "X%04d", cur_para); snprintf(f, sizeof f, "F%04d", cur_para);
            asm_comment(" end of a PERFORM range: return through its cell");
            snprintf(b, sizeof b, "15,%s", x);  asm_line("", "L", b, "");
            asm_line("", "BR", "15", "");
            asm_line(f, "DS", "0H", "fall-through when not performed");
        }
        switch (st->op) {
        case ST_PARA: {
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
        case ST_PERFORM: {
            char p1[16], x[16], f[16], r[16];
            snprintf(p1, sizeof p1, "P%04d", st->dst);
            snprintf(x,  sizeof x,  "X%04d", st->src);
            snprintf(f,  sizeof f,  "F%04d", st->src);
            snprintf(r,  sizeof r,  "R%04d", ++nret);
            if (!strcmp(st->para, st->thru)) snprintf(b, sizeof b, " PERFORM %s", st->para);
            else snprintf(b, sizeof b, " PERFORM %s THRU %s", st->para, st->thru);
            asm_comment(b);
            snprintf(b, sizeof b, "15,%s", r);  asm_line("", "LA", b, "return here");
            snprintf(b, sizeof b, "15,%s", x);  asm_line("", "ST", b, "into the range's exit cell");
            asm_line("", "B", p1, "");
            asm_line(r, "DS", "0H", "");
            /* restore the cell so a later fall-through is not diverted */
            snprintf(b, sizeof b, "15,%s", f);  asm_line("", "LA", b, "restore fall-through");
            snprintf(b, sizeof b, "15,%s", x);  asm_line("", "ST", b, "");
            break;
        }
        case ST_STOP:
            asm_comment(" STOP RUN");
            if (has_display) {
                asm_line("", "L", "15,VTERM", "close anything the runtime opened");
                asm_line("", "BALR", "14,15", "");
            }
            asm_line("", "L", "13,4(13)", "restore caller's save area");
            asm_line("", "LM", "14,12,12(13)", "restore caller's registers");
            asm_line("", "SR", "15,15", "return code 0");
            asm_line("", "BR", "14", "return to caller");
            break;
        case ST_DISPLAY_LIT:
            snprintf(b, sizeof b, " DISPLAY '%s'", st->lit);
            asm_comment(b);
            snprintf(lab, sizeof lab, "PARM%04d", ++ndlit);
            snprintf(b, sizeof b, "1,%s", lab);
            asm_line("", "LA", b, "");
            asm_line("", "L", "15,VDISP", "");
            asm_line("", "BALR", "14,15", "");
            break;
        case ST_DISPLAY_ID:
            snprintf(b, sizeof b, " DISPLAY %s", syms[st->src].name);
            asm_comment(b);
            snprintf(lab, sizeof lab, "PARM%04d", ++ndlit);
            snprintf(b, sizeof b, "1,%s", lab);
            asm_line("", "LA", b, "");
            asm_line("", "L", "15,VDISP", "");
            asm_line("", "BALR", "14,15", "");
            break;
        case ST_COMPUTE: {
            const Sym *d = &syms[st->dst];
            snprintf(b, sizeof b, " COMPUTE %s%s = ...", d->name,
                     st->rounded ? " ROUNDED" : "");
            asm_comment(b);
            int rs = gen_expr(st->expr, 0, d->scale);
            gen_rescale16("WK0", rs, d->scale, st->rounded);
            asm_line("", "ZAP", "PWK1(8),WK0(16)", "");
            gen_store(d, "PWK1");
            break;
        }
        case ST_MOVE:
        case ST_ADD:
        case ST_SUB: {
            const Sym *d = &syms[st->dst];
            const char *verb = st->op == ST_MOVE ? "MOVE" :
                               st->op == ST_ADD  ? "ADD"  : "SUBTRACT";
            if (st->imm) snprintf(b, sizeof b, " %s %s -> %s", verb, st->immdigits, d->name);
            else         snprintf(b, sizeof b, " %s %s -> %s", verb, syms[st->src].name, d->name);
            asm_comment(b);
            if (st->op == ST_MOVE) {
                if (st->imm == 2) {
                    if (!(d->is_alpha || d->is_group))
                        die("MOVE of a nonnumeric literal to a numeric item is "
                            "not implemented yet");
                    const char *sl = intern_str(st->immdigits, st->immscale, d->bytes);
                    if (d->bytes > 256) die("MVC is limited to 256 bytes");
                    snprintf(b, sizeof b, "%s(%d),%s", d->label, d->bytes, sl);
                    asm_line("", "MVC", b, "literal move, space padded");
                    break;
                }
                if (!st->imm && (d->is_alpha || d->is_group ||
                                 syms[st->src].is_alpha || syms[st->src].is_group)) {
                    const Sym *sv = &syms[st->src];
                    if (!(d->is_alpha || d->is_group) || !(sv->is_alpha || sv->is_group))
                        die("MOVE between a numeric and an alphanumeric item is "
                            "not implemented yet");
                    gen_move_alpha(d, sv);
                    break;
                }
                if (st->imm) { gen_load_imm(intern_const(st->immdigits), "PWK1"); }
                else { gen_load(&syms[st->src], "PWK1"); gen_rescale("PWK1", syms[st->src].scale, d->scale); }
                gen_store(d, "PWK1");
            } else {
                /* Compute at the wider of the two scales, then truncate once on
                   store. Rescaling the source down first would lose digits that
                   the addition still needs: at scale 1, 0.05 + 0.05 must give
                   0.1, not 0.0. */
                int ss = st->imm ? st->immscale : syms[st->src].scale;
                int ws = d->scale > ss ? d->scale : ss;
                gen_load(d, "PWK1");
                gen_rescale("PWK1", d->scale, ws);
                if (st->imm) { gen_load_imm(intern_const(st->immdigits), "PWK2"); }
                else { gen_load(&syms[st->src], "PWK2"); }
                gen_rescale("PWK2", ss, ws);
                asm_line("", st->op == ST_ADD ? "AP" : "SP", "PWK1(8),PWK2(8)", "");
                gen_rescale("PWK1", ws, d->scale);
                gen_store(d, "PWK1");
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

    /* ---- PERFORM exit cells ---- */
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
    ndlit = 0;
    for (int i = 0; i < nstmt; i++) {
        Stmt *st = &stmts[i];
        if (st->op == ST_DISPLAY_LIT) {
            char plab[24], tlab[24], llab[24];
            snprintf(plab, sizeof plab, "PARM%04d", ++ndlit);
            snprintf(tlab, sizeof tlab, "LIT%04d", ndlit);
            snprintf(llab, sizeof llab, "LEN%04d", ndlit);
            snprintf(b, sizeof b, "A(%s)", tlab);      asm_line(plab, "DC", b, "");
            snprintf(b, sizeof b, "X'80',AL3(%s)", llab); asm_line("", "DC", b, "last parameter");
            emit_literal(tlab, st->lit, st->litlen);
            snprintf(b, sizeof b, "H'%d'", st->litlen); asm_line(llab, "DC", b, "");
        } else if (st->op == ST_DISPLAY_ID) {
            char plab[24], llab[24];
            snprintf(plab, sizeof plab, "PARM%04d", ++ndlit);
            snprintf(llab, sizeof llab, "LEN%04d", ndlit);
            snprintf(b, sizeof b, "A(%s)", syms[st->src].label); asm_line(plab, "DC", b, "");
            snprintf(b, sizeof b, "X'80',AL3(%s)", llab);       asm_line("", "DC", b, "last parameter");
            snprintf(b, sizeof b, "H'%d'", syms[st->src].is_alpha ? syms[st->src].bytes
                                                                  : syms[st->src].digits);
            asm_line(llab, "DC", b, "");
        }
    }

    /* ---- WORKING-STORAGE ---- */
    if (nsym) {
        asm_comment(" WORKING-STORAGE");
        for (int i = 0; i < nsym; i++) {
            Sym *sy = &syms[i];
            char cmt[96];
            if (sy->is_88) continue;      /* a condition name has no storage */
            if (sy->is_group) {
                snprintf(b, sizeof b, "0CL%d", sy->bytes);
                snprintf(cmt, sizeof cmt, "%s (%02d group)", sy->name, sy->level);
                asm_line(sy->label, "DS", b, cmt);
                continue;
            }
            if (sy->is_alpha) {
                if (sy->has_value == 3) {
                    char op[MAXTOK * 2 + 16]; int j = 0;
                    j += snprintf(op + j, sizeof op - j, "CL%d'", sy->bytes);
                    for (const char *q = sy->value; *q; q++) {
                        if (*q == '\'' || *q == '&') op[j++] = *q;
                        op[j++] = *q;
                    }
                    op[j++] = '\''; op[j] = 0;
                    snprintf(b, sizeof b, "%s", op);
                } else snprintf(b, sizeof b, "CL%d' '", sy->bytes);
                snprintf(cmt, sizeof cmt, "%s PIC X(%d)", sy->name, sy->bytes);
                asm_line(sy->label, "DC", b, cmt);
                continue;
            }
            const char *v = (sy->has_value == 1) ? sy->value : "0";
            switch (sy->usage) {
            case U_DISPLAY: snprintf(b, sizeof b, "ZL%d'%s'", sy->bytes, v); break;
            case U_COMP3:   snprintf(b, sizeof b, "PL%d'%s'", sy->bytes, v); break;
            default:        snprintf(b, sizeof b, "%s'%s'", sy->bytes == 2 ? "H" : "F", v); break;
            }
            snprintf(cmt, sizeof cmt, "%s PIC %s9(%d)v%d %s",
                     sy->name, sy->is_signed ? "S" : "", sy->digits, sy->scale,
                     sy->usage == U_COMP ? "COMP" : sy->usage == U_COMP3 ? "COMP-3" : "DISP");
            asm_line(sy->label, "DC", b, cmt);
        }
        asm_comment(" work areas for decimal arithmetic");
        asm_line("DWK", "DS", "D", "CVD/CVB doubleword");
        asm_line("PWK1", "DS", "PL8", "");
        asm_line("PWK2", "DS", "PL8", "");
        asm_line("MULT8", "DS", "PL8", "MP right operand");
        asm_line("DIVR8", "DS", "PL8", "DP divisor");
        asm_line("QTMP", "DS", "PL8", "DP quotient");
        for (int i = 0; i < 6; i++) {
            char w[8]; snprintf(w, sizeof w, "WK%d", i);
            asm_line(w, "DS", "PL16", i == 0 ? "expression stack" : "");
        }
    }
    for (int i = 0; i < nconst; i++) {
        snprintf(b, sizeof b, "PL8'%s'", consts[i].digits);
        asm_line(consts[i].label, "DC", b, i ? "" : "numeric constants");
    }
    for (int i = 0; i < nsconst; i++) {
        char op[MAXTOK * 2 + 16]; int j = 0;
        j += snprintf(op + j, sizeof op - j, "CL%d'", sconsts[i].len);
        for (const char *q = sconsts[i].text; *q; q++) {
            if (*q == '\'' || *q == '&') op[j++] = *q;
            op[j++] = *q;
        }
        op[j++] = '\''; op[j] = 0;
        asm_line(sconsts[i].label, "DC", op, i ? "" : "nonnumeric constants");
    }
    asm_line("SAVEAREA", "DS", "18F", "");

    if (has_display) emit_runtime();
    asm_line("", "END", "", "");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outname = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outname = argv[++i];
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "usage: cobc370 prog.cbl [-o prog.asm]\n"); return 2; }

    src.fp = fopen(in, "r");
    if (!src.fp) { perror(in); return 2; }
    src.name = in; src.p = NULL; src.line = 0;

    out = outname ? fopen(outname, "w") : stdout;
    if (!out) { perror(outname); return 2; }

    next();
    parse_program_id();
    skip_division("ENVIRONMENT");
    parse_data_division();
    parse_procedure();
    generate();

    if (out != stdout) fclose(out);
    return 0;
}
