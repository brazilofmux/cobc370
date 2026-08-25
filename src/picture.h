/* COBOL PICTURE: scanner interface and the analysed result. */
#ifndef PICTURE_H
#define PICTURE_H
#include <ctype.h>

typedef struct { char sym; int rep; } PicItem;

#define PIC_MAXITEM 64
#define PIC_MAXMASK 64

typedef struct {
    int  digits;      /* digit positions */
    int  scale;       /* digits right of the decimal point */
    int  is_signed;
    int  is_alpha;    /* PIC X / A */
    int  edited;      /* needs an ED pattern rather than a plain move */
    int  bytes;       /* character positions in the field */
    int  floating;    /* a floating insertion string is present -> EDMK */
    unsigned char mask[PIC_MAXMASK];
    int  masklen;
    char err[96];     /* set when the picture cannot be handled */
} PicInfo;

int pic_scan(const char *s, PicItem *out, int max, int *errpos);
int pic_analyse(const char *s, PicInfo *info);   /* 0 ok, -1 with info->err */

#endif
