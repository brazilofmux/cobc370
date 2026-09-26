/* The 64-bit millicode cc370 emits calls to and libc370 does not carry:
 * @@MULDI3 and @@NEGDI2 (the __-to-@@ mangling is the backend's).
 *
 * Every operation here is built from 32-bit halves, because a DImode
 * multiply or negate in this very code would call back into itself.
 * The 32x32->64 multiply goes through 16-bit halves for the same
 * reason: S/370 has no unsigned widening multiply for the backend to
 * expand inline.
 *
 * The halves are put together through a union, not "(UDW)hi << 32 | lo":
 * cc370 up to 2c40eb3 emitted SLDA, the arithmetic double shift, for a
 * 64-bit shift left, and SLDA keeps the sign bit where it is, so a high
 * word with its top bit set came out as X'7FFFFFFF...' -- every negative
 * product was 2**63 too big, found on TK5 on 2026-09-25. Reported as
 * mvslovers/cc370#468 and fixed upstream the next day (335e7d0, SLDL).
 * The union stays: it is right under either compiler, so this module does
 * not depend on which cc370 built it. S/370 is big-endian: the high word
 * is the first.
 */



typedef unsigned int UW;          /* 32 bits on the target and on the host */
typedef long long DW;
typedef unsigned long long UDW;

#if defined(MVS370) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
typedef union { unsigned long long d; struct { UW hi, lo; } w; } Halves;   /* S/370: big-endian */
#else
typedef union { unsigned long long d; struct { UW lo, hi; } w; } Halves;   /* a little-endian host, for testing */
#endif

static unsigned long long join(UW hi, UW lo)
{
    Halves h; h.w.hi = hi; h.w.lo = lo; return h.d;
}

static UW hiword(unsigned long long d) { Halves h; h.d = d; return h.w.hi; }
static UW loword(unsigned long long d) { Halves h; h.d = d; return h.w.lo; }

static UW umul32hi(UW x, UW y, UW *lo)
{
    UW xl = x & 0xffff, xh = x >> 16;
    UW yl = y & 0xffff, yh = y >> 16;
    UW ll = xl * yl;
    UW lh = xl * yh;
    UW hl = xh * yl;
    UW hh = xh * yh;
    UW mid = lh + hl;
    UW midc = mid < lh ? 0x10000UL : 0;
    UW l = ll + ((mid & 0xffff) << 16);
    UW lc = l < ll ? 1 : 0;
    *lo = l;
    return hh + (mid >> 16) + midc + lc;
}

DW __muldi3(DW u, DW v)
{
    UW al = loword((UDW)u), ah = hiword((UDW)u);
    UW bl = loword((UDW)v), bh = hiword((UDW)v);
    UW lo, hi;
    hi = umul32hi(al, bl, &lo);
    hi += al * bh + ah * bl;
    return (DW)join(hi, lo);
}

DW __negdi2(DW a)
{
    UW lo = loword((UDW)a), hi = hiword((UDW)a);
    UW nlo = (UW)0 - lo;
    UW nhi = ~hi + (lo == 0 ? 1 : 0);
    return (DW)join(nhi, nlo);
}
