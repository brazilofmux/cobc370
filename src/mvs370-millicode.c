/* The 64-bit millicode cc370 emits calls to and libc370 does not carry:
 * @@MULDI3 and @@NEGDI2 (the __-to-@@ mangling is the backend's).
 *
 * Every operation here is built from 32-bit halves, because a DImode
 * multiply or negate in this very code would call back into itself.
 * The 32x32->64 multiply goes through 16-bit halves for the same
 * reason: S/370 has no unsigned widening multiply for the backend to
 * expand inline.
 */

typedef unsigned long UW;
typedef long long DW;
typedef unsigned long long UDW;

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
    UW al = (UW)(UDW)u, ah = (UW)((UDW)u >> 32);
    UW bl = (UW)(UDW)v, bh = (UW)((UDW)v >> 32);
    UW lo, hi;
    hi = umul32hi(al, bl, &lo);
    hi += al * bh + ah * bl;
    return (DW)(((UDW)hi << 32) | lo);
}

DW __negdi2(DW a)
{
    UW lo = (UW)(UDW)a, hi = (UW)((UDW)a >> 32);
    UW nlo = (UW)0 - lo;
    UW nhi = ~hi + (lo == 0 ? 1 : 0);
    return (DW)(((UDW)nhi << 32) | nlo);
}
