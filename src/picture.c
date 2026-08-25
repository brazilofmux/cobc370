/* COBOL PICTURE analysis: turn the scanned symbols into a field description
 * and, for an edited picture, the byte pattern the S/370 ED instruction wants.
 *
 * The scanner (picture.rl) only tokenises. Everything intricate is here,
 * because floating insertion is the kind of rule that wants prose beside it:
 * a run of n sign symbols gives n-1 digit positions and one sign position.
 */
#include <stdio.h>
#include <string.h>
#include "picture.h"

/* EBCDIC, because the pattern is data on the target. */
static unsigned char ebcdic(char c)
{
    switch (c) {
    case ' ': return 0x40;  case '.': return 0x4B;  case '+': return 0x4E;
    case '$': return 0x5B;  case '*': return 0x5C;  case '-': return 0x60;
    case '/': return 0x61;  case ',': return 0x6B;  case '0': return 0xF0;
    case 'B': return 0xC2;  case 'C': return 0xC3;  case 'D': return 0xC4;
    case 'R': return 0xD9;
    default:  return 0x40;
    }
}

#define ED_FILL_BLANK 0x40
#define ED_DIGIT      0x20   /* digit selector */
#define ED_START      0x21   /* digit selector that also starts significance */

static int fail(PicInfo *in, const char *msg)
{
    snprintf(in->err, sizeof in->err, "%s", msg);
    return -1;
}

int pic_analyse(const char *s, PicInfo *info)
{
    PicItem it[PIC_MAXITEM];
    int errpos = 0;
    memset(info, 0, sizeof *info);

    int n = pic_scan(s, it, PIC_MAXITEM, &errpos);
    if (n < 0) {
        snprintf(info->err, sizeof info->err,
                 "PICTURE '%s' is not valid at character %d", s, errpos + 1);
        return -1;
    }

    if (n == 0) return fail(info, "empty PICTURE");

    /* Alphanumeric first, from the item list rather than a flattened one:
     * X(2100) is perfectly ordinary and must not be expanded character by
     * character just to be counted. */
    int nx = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].sym == 'X' || it[i].sym == 'A') nx++;
        total += it[i].rep;
    }
    if (nx) {
        if (nx != n) return fail(info, "mixed alphanumeric and numeric PICTURE "
                                       "characters are not implemented yet");
        info->is_alpha = 1;
        info->bytes = total;
        return 0;
    }

    /* Numeric pictures are short by construction -- at most 15 digit positions
     * plus insertions -- so flattening is safe here. */
    char f[256];
    int nf = 0;
    for (int i = 0; i < n; i++) {
        for (int r = 0; r < it[i].rep; r++) {
            if (nf >= (int)sizeof f - 1) return fail(info, "numeric PICTURE too long");
            f[nf++] = it[i].sym;
        }
    }
    f[nf] = 0;

    /* Numeric.
     *
     * A floating insertion string is the whole run of one sign or currency
     * symbol, and it is NOT broken by the insertion characters embedded in it:
     * ----,---,--9 is one floating string of nine '-', not three runs. Nine
     * symbols give eight digit positions and one sign position, so counting
     * per-run loses a digit at every comma. */
    char fl = 0;                       /* the floating symbol, if any */
    int  fl_first = -1;
    for (char c = 0, k = 0; k < 3; k++) {
        c = "+-$"[(int)k];
        int cnt = 0, first = -1;
        for (int i = 0; i < nf; i++)
            if (f[i] == c) { cnt++; if (first < 0) first = i; }
        if (cnt > 1) { fl = c; fl_first = first; break; }
    }

    int seen_point = 0;
    char fillch = ' ';

    for (int i = 0; i < nf; i++) {
        char c = f[i];
        if (fl && c == fl) {
            info->edited = 1;
            info->bytes++;
            if (c != '$') info->is_signed = 1;
            if (i != fl_first) {           /* every symbol but the first is a digit */
                info->digits++;
                if (seen_point) info->scale++;
            }
            continue;
        }
        switch (c) {
        case '9': info->digits++; if (seen_point) info->scale++; info->bytes++; break;
        case 'Z': info->digits++; if (seen_point) info->scale++; info->bytes++;
                  info->edited = 1; break;
        case '*': info->digits++; if (seen_point) info->scale++; info->bytes++;
                  info->edited = 1; fillch = '*'; break;
        case 'V': seen_point = 1; break;                  /* no character */
        case 'S': info->is_signed = 1; break;             /* no character */
        case '.': seen_point = 1; info->bytes++; info->edited = 1; break;
        case ',': case 'B': case '0': case '/':
                  info->bytes++; info->edited = 1; break;
        case '+': case '-':                                /* a fixed sign */
                  info->is_signed = 1; info->edited = 1; info->bytes++; break;
        case '$': info->edited = 1; info->bytes++; break;  /* fixed currency */
        case 'C': case 'D':                                /* CR / DB */
                  info->bytes += 2; info->edited = 1; info->is_signed = 1; break;
        default:  return fail(info, "unsupported PICTURE character");
        }
    }
    info->floating = (fl != 0);

    if (info->digits == 0) return fail(info, "PICTURE has no digit positions");
    if (info->digits > 15)
        return fail(info, "more than 15 digits needs a wider packed work area "
                          "than this compiler allocates");
    if (!info->edited) return 0;

    /* ---- the ED pattern -------------------------------------------------
     * One fill byte, then one byte per character position, so the pattern is
     * bytes+1 long. ED overwrites the pattern with the result; the field takes
     * the last `bytes` of it. The extra leading byte is also what gives EDMK
     * somewhere to put a floating sign when every digit position is
     * significant.
     *
     * The significance starter goes at the first '9'. If every digit position
     * suppresses, there is none, and a zero value prints entirely blank --
     * which is what COBOL specifies. */
    /* Where the significance starter goes.
     *
     * X'21' stores its digit using the significance state as it was BEFORE
     * setting it, so putting the starter on the first '9' makes a leading zero
     * there come out as fill -- +99999 would print '+ 0042'. The starter
     * belongs on the digit selector immediately PRECEDING the first '9', so
     * significance is already on when that '9' is examined.
     *
     * If the first '9' is itself the first digit position there is no
     * preceding selector, and every digit must print. That is what the spare
     * leading selector is for, and it is why IKFCBL00 always carried two extra
     * pattern bytes rather than computing the parity spare exactly. */
    int first9 = -1, nprev = -1, seen = 0;
    for (int i = 0; i < nf; i++) {
        int is_digit_pos = (f[i] == '9' || f[i] == 'Z' || f[i] == '*' ||
                            (fl && f[i] == fl && i != fl_first));
        if (f[i] == '9') { first9 = i; nprev = seen - 1; break; }
        if (is_digit_pos) seen++;
    }
    info->need_lead_start = (first9 >= 0 && nprev < 0);
    int start_at = -1;                  /* which digit position gets X'21' */
    if (first9 >= 0 && nprev >= 0) start_at = nprev;

    if (info->bytes + 1 > PIC_MAXMASK)
        return fail(info, "PICTURE too wide for an ED pattern");
    info->mask[0] = ebcdic(fillch);
    info->masklen = 1;
    info->sign_pos = -1;
    info->first_sel = -1;
    info->sign_char = fl ? fl : 0;

    int dseen = 0;
    for (int i = 0; i < nf; i++) {
        char c = f[i];
        if (fl && c == fl) {
            /* The first symbol is the sign position: a fill byte, which EDMK
               will overwrite if the sign has to go there. */
            if (i == fl_first) info->mask[info->masklen++] = ebcdic(fillch);
            else {
                if (info->first_sel < 0) info->first_sel = info->masklen;
                info->mask[info->masklen++] = (dseen++ == start_at) ? ED_START : ED_DIGIT;
            }
            continue;
        }
        switch (c) {
        case '9': case 'Z': case '*':
            if (info->first_sel < 0) info->first_sel = info->masklen;
            info->mask[info->masklen++] = (dseen++ == start_at) ? ED_START : ED_DIGIT;
            break;
        case 'V': case 'S': break;
        case '.': info->mask[info->masklen++] = ebcdic('.'); break;
        case ',': info->mask[info->masklen++] = ebcdic(','); break;
        case 'B': info->mask[info->masklen++] = ebcdic(' '); break;
        case '0': info->mask[info->masklen++] = ebcdic('0'); break;
        case '/': info->mask[info->masklen++] = ebcdic('/'); break;
        case '+': case '-':
            /* A fixed sign is NOT a pattern message byte. It sits left of the
               significance starter, so ED would replace it with fill. Leave a
               fill byte and let the code generator store the sign afterwards,
               where the condition code says which one it is. */
            info->sign_char = c; info->sign_pos = info->masklen;
            info->mask[info->masklen++] = ebcdic(fillch); break;
        case '$':
            info->mask[info->masklen++] = ebcdic(c); break;
        case 'C': info->mask[info->masklen++] = ebcdic('C');
                  info->mask[info->masklen++] = ebcdic('R'); break;
        case 'D': info->mask[info->masklen++] = ebcdic('D');
                  info->mask[info->masklen++] = ebcdic('B'); break;
        }
    }
    return 0;
}
