/* cobc -- a COBOL cross-compiler for MVS 3.8j, slice 0.
 *
 * Reads fixed-format COBOL on the Mac and emits S/370 assembler source, which
 * is assembled and link-edited on the guest. The compiler never runs on MVS.
 *
 * Slice 0 accepts exactly one program shape: the divisions, and a PROCEDURE
 * DIVISION whose only statement is STOP RUN. Anything else is a diagnostic
 * rather than silence -- the point of a first slice is to prove the toolchain
 * end to end, not to pretend at coverage.
 *
 * Emitted code is the standard OS/360 entry linkage. It deliberately does NOT
 * call ILBOSTP1 or anything else in SYS1.COBLIB: this compiler brings its own
 * runtime, and for STOP RUN the runtime is nothing at all.
 *
 *   cc -O2 -o cobc cobc.c
 *   ./cobc prog.cbl -o prog.asm
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
    if (*src.p == '.') { src.p++; strcpy(tok.text, "."); return; }
    int i = 0;
    while (*src.p && !isspace((unsigned char)*src.p) && *src.p != '.') {
        if (i < MAXTOK-1) tok.text[i++] = (char)toupper((unsigned char)*src.p);
        src.p++;
    }
    tok.text[i] = 0;
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
    if (name    && *name)    { n = strlen(name);    memcpy(b + 0,  name, n); }
    if (op      && *op)      { n = strlen(op);      memcpy(b + 9,  op, n); }
    if (operand && *operand) { n = strlen(operand); memcpy(b + 15, operand, n); }
    int end = 15 + (operand ? (int)strlen(operand) : 0);
    if (end < 15) end = 15;
    if (comment && *comment) {
        int c = end + 2; if (c < 35) c = 35;
        memcpy(b + c, comment, strlen(comment));
        end = c + (int)strlen(comment);
    }
    b[end] = 0;
    for (int i = end - 1; i >= 0 && b[i] == ' '; i--) b[i] = 0;
    fprintf(out, "%s\n", b);
}

static void asm_comment(const char *text) { fprintf(out, "*%s\n", text); }

/* ---- parser ----------------------------------------------------------- */

static char progid[9];

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

static void skip_division(const char *kw)
{
    if (!is(kw)) return;
    next(); expect("DIVISION"); expect(".");
    while (!tok.eof && !is("DATA") && !is("PROCEDURE")) next();
}

static void parse_procedure(void)
{
    expect("PROCEDURE"); expect("DIVISION"); expect(".");
    int stopped = 0;
    while (!tok.eof) {
        if (is("STOP")) {
            next();
            if (!is("RUN")) die("slice 0 supports STOP RUN only "
                                "(STOP literal is not implemented)");
            next();
            if (is(".")) next();
            stopped = 1;
            continue;
        }
        if (is(".")) { next(); continue; }
        {
            char m[160];
            snprintf(m, sizeof m,
                     "slice 0 supports only STOP RUN; found '%s'. "
                     "This is a real limit, not a parse failure.", tok.text);
            die(m);
        }
    }
    if (!stopped) die("PROCEDURE DIVISION has no STOP RUN");
}

/* ---- code generation -------------------------------------------------- */

static void generate(void)
{
    char b[128];
    asm_comment("---------------------------------------------------------------");
    snprintf(b, sizeof b, " Generated by cobc slice 0 from %s", src.name);
    asm_comment(b);
    asm_comment(" Standard OS/360 entry linkage. No SYS1.COBLIB reference:");
    asm_comment(" this compiler brings its own runtime, and STOP RUN needs none.");
    asm_comment("---------------------------------------------------------------");

    asm_line(progid, "CSECT", "", "");
    asm_line("", "STM", "14,12,12(13)", "save caller's registers");
    asm_line("", "BALR", "12,0", "establish addressability");
    asm_line("", "USING", "*,12", "");
    asm_line("", "ST", "13,SAVEAREA+4", "backward chain to caller");
    asm_line("", "LA", "11,SAVEAREA", "");
    asm_line("", "ST", "11,8(13)", "forward chain from caller");
    asm_line("", "LR", "13,11", "our save area is now current");
    asm_comment(" PROCEDURE DIVISION");
    asm_comment(" STOP RUN");
    asm_line("", "L", "13,4(13)", "restore caller's save area");
    asm_line("", "LM", "14,12,12(13)", "restore caller's registers");
    asm_line("", "SR", "15,15", "return code 0");
    asm_line("", "BR", "14", "return to caller");
    asm_line("SAVEAREA", "DS", "18F", "");
    asm_line("", "END", "", "");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outname = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outname = argv[++i];
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "usage: cobc prog.cbl [-o prog.asm]\n"); return 2; }

    src.fp = fopen(in, "r");
    if (!src.fp) { perror(in); return 2; }
    src.name = in; src.p = NULL; src.line = 0;

    out = outname ? fopen(outname, "w") : stdout;
    if (!out) { perror(outname); return 2; }

    next();
    parse_program_id();
    skip_division("ENVIRONMENT");
    skip_division("DATA");
    parse_procedure();
    generate();

    if (out != stdout) fclose(out);
    return 0;
}
