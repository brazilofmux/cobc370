#include <stdio.h>
#include <string.h>
#include <clibos.h>

static void *ld(int (**fn)(int))
{
    unsigned size = 0; char ac = 0;
    void *epa = __load(0, "PROBEF", &size, &ac);
    *fn = (int (*)(int))epa;
    return epa;
}

int main(void)
{
    int (*fn)(int); void *e1, *e2, *e3; int r;
    int i; void *blk[512];

    printf("PROBEF: 262288-byte section, 13312 bytes of TXT, 248976 with none.\n\n");

    /* A: the module dirties its own storage, is deleted, and reloaded.
     *    This is the resident-service case: LOAD / use / DELETE / LOAD. */
    e1 = ld(&fn);
    if (!e1) { printf("A: LOAD failed\n"); return 8; }
    printf("A1 load    epa=%08lX  first_nonzero=%d\n", (unsigned long)e1, fn(0));
    fn(1);
    r = fn(0);
    printf("A2 dirtied via the module; re-read WITHOUT reloading: first_nonzero=%d  %s\n",
           r, r ? "(control passes: the write is visible)"
                : "(CONTROL FAILED -- the write is not observable, test is vacuous)");
    if (r == 0) { printf("   aborting: a null control makes every later ZEROED meaningless\n"); return 12; }
    __delete("PROBEF");
    e2 = ld(&fn);
    r = fn(0);
    printf("A3 reload  epa=%08lX  first_nonzero=%d   %s\n", (unsigned long)e2, r,
           r == 0 ? "ZEROED" : "STALE DATA VISIBLE");
    printf("   same storage: %s\n\n", e1 == e2 ? "YES" : "no");
    __delete("PROBEF");

    /* B: dirty via many small getmains so the subpool retains the pages
     *    on its free chain rather than releasing them to the system. */
    for (i = 0; i < 512; i++) {
        blk[i] = getmain(1024, 0);
        if (blk[i]) memset(blk[i], 0xFF, 1024);
    }
    for (i = 0; i < 512; i++) if (blk[i]) freemain(blk[i]);
    printf("B1 dirtied 512 x 1KB in subpool 0, then freed them\n");
    e3 = ld(&fn);
    r = fn(0);
    printf("B2 load    epa=%08lX  first_nonzero=%d   %s\n", (unsigned long)e3, r,
           r == 0 ? "ZEROED" : "STALE DATA VISIBLE");
    __delete("PROBEF");
    return 0;
}
