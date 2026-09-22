# Does program fetch zero what a module's text does not cover?

The question the BSS work turned on (mvslovers/cc370#443). If ld370 elides
an all-zero text record, the bytes it did not write have to come from
somewhere, and the only candidate is whatever storage program fetch was
given. In a fresh batch region that is zero. The case that matters is the
other one: a long-running address space doing LOAD / DELETE / LOAD, where
the storage has been used before.

## The probe

`probef.c` and `probeg.c` declare an array nothing ever initializes, and
read it through a `volatile unsigned char *` so the compiler cannot fold
the reads to zero and answer the question by itself.

    probe(0)  -> first non-zero offset + 1, or 0 if wholly zero
    probe(1)  -> fill it with X'FF', return -1

Linked with `--entry PROBE` and no crt0, so `driver.c` can `__load()` one
and call it through a function pointer. Built with the toolchain carrying
the elision, the member holds far less than it reserves:

    PROBEF   section 0x040090 = 262,288 B   TXT 13,312 B   248,976 B absent
    PROBEG   section 0x009D05 =  40,197 B   TXT 13,312 B    26,885 B absent,
                                                            ending mid-page

## The control

`driver.c` dirties the array through the module and re-reads it *without*
reloading, requiring a non-zero answer before it goes on. Without that,
every "zeroed" result below would read the same way if the write had
silently done nothing.

## Result, TK5 MVS 3.8j, 2026-09-22

Every reload landed at the address whose storage had just been dirtied --
reuse, not quiet relocation -- and the uncovered extent read as zero in
all of them:

| module | dirtied by | reload epa | uncovered reads |
|---|---|---|---|
| PROBEF | the module itself, then DELETE | same (0011FF38) | zero |
| PROBEF | 512 x 1KB subpool-0 churn | same | zero |
| PROBEF | one 384KB getmain over its extent | same (0011FF70) | zero |
| PROBEG | the module itself, then DELETE | same (001202F8) | zero |
| PROBEG | 512 x 1KB subpool-0 churn | same | zero |

## What it does not show

The mechanism is probably MVS releasing pages on FREEMAIN/DELETE and
zeroing on the next page fault, rather than fetch clearing the gap.
That is page-level behaviour: sub-page reuse, where a page is never
released, is not covered -- a 256KB module against 512KB of freed 1KB
blocks will still have been given fresh pages. And it is one system.

So this is what TK5 does, not what the architecture promises, which is
why the elision should still be opt-in.
