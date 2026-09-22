/* 262,144 bytes of never-initialized storage; only the first 13,168 are
 * covered by a TXT record, the rest exists nowhere in the module.
 *   mode 0: return first_nonzero_offset + 1, or 0 if wholly zero
 *   mode 1: fill it with X'FF' and return -1
 * volatile so the compiler cannot fold the reads and answer by itself. */
char probe_sml[40001];

int probeg(int mode)
{
    volatile unsigned char *p = (volatile unsigned char *)probe_sml;
    int i;
    if (mode) { for (i = 0; i < 40001; i++) p[i] = 0xFF; return -1; }
    for (i = 0; i < 40001; i++) if (p[i]) return i + 1;
    return 0;
}
