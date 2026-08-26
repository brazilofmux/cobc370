# ISAM on MVS 3.8j: the working recipe

Both halves work: QISAM sequential (`GET`) and BISAM random (`READ` by key).
This records what it took, because almost none of it is guessable and the
documentation is famously bad.

The corpus is **read-only** on ISAM — nothing adds, modifies or deletes a record
— so retrieval is the whole requirement. Getting data *in and out* is a separate
JCL problem, solved by `SVD001.HELLO.CNTL(LDGLACCT)` and `(EXGLACCT)`.

## The dataset, from its own DSCB

`SVD001.GLACCT`, read straight out of `IEHLIST LISTVTOC FORMAT`:

| | |
|---|---|
| DSORG / RECFM | `IS` / `FB` |
| LRECL / BLKSIZE | 57 / 4161 — **73 records per block** |
| KEYLEN | **6** |
| OPTCD | `X'02'` = **L, the delete option** |

`OPTCD=L` is why every record carries a delete flag in byte 0, and why the key
starts at **RKP=1**. And `KEYLEN=6` against a key that displays as ten digits
means the key is `PIC 9(10) COMP-3`. A dump of record 1 confirms the whole
layout at once:

    00  000000 10202F  1C  C1  F1F0  000000 4348 ...
    ^   ^               ^   ^   ^     ^
    |   GLAC-KEY        |   |   SUBCL GLAC-BALANCE  S9(9)V99 COMP-3
    |   9(10) COMP-3    |   CLASS
    |   F sign          GLAC-CRDB S9 COMP-3, C sign
    GLAC-DELETE

Note the two sign nibbles: `F` on the unsigned key, `C` on the signed `CRDB`.
An ISAM key is compared **byte-wise**, so an unsigned packed field that carries
`C` (what `ZAP` leaves behind) never matches. That is the bug fixed in the
previous slice; without it random ISAM cannot work at all.

## QISAM sequential

`DSORG=IS,MACRF=(GM)` and an ordinary `GET`. `AT END` uses the same `DCBEODAD`
patch at offset 33 as QSAM.

## BISAM random — four separate bugs

The first attempt faulted S0C4 inside the `READ` macro. Four things were wrong;
each one alone is fatal.

**1. `CHECK` is not a BISAM macro.** BISAM synchronises with `WAITF`. `CHECK`
belongs to BSAM/BPAM/BDAM and expands to

    L  15,52(0,14)      load "check routine addr" from the DCB
    BALR 14,15

Offset 52 in an *ISAM* DCB is `DCBOPTCD`, so this branches to `X'02……'` → S0C1.

**2. `WAITF` is missing from TK5's `SYS1.MACLIB`.** It is the *only* absent ISAM
macro — `READ`, `WRITE`, `SETL`, `ESETL`, `FREEDBUF` and `RELEX` all assemble.
It is not needed: the DECB's first word is an ECB, so **`WAIT ECB=decb`**
synchronises correctly, and the result is read out of the DECB directly.

**3. The area must hold a BLOCK, not a record.** With blocked records BISAM
reads the whole 4161-byte block. Passing the 57-byte FD record area let ISAM
write 4161 bytes into it — that was the S0C4.

**4. ISAM wants 16 bytes of working room at the front of that area.** The block
lands at `area+16`, so the area must be `BLKSIZE+16`.

### The DECB, from the macro's own expansion

    +0   A(0)        ECB            -- clear before each READ
    +4   X'02'       type
    +5   X'80'       type
    +6   AL2(0)      length
    +8   A(dcb)
    +12  A(area)     block area, BLKSIZE+16
    +16  A(0)        RECORD POINTER WORD  <- the record, inside the block
    +20  A(key)      raw key (6 packed bytes here)
    +24  AL2(0)      exception code       <- non-zero is INVALID KEY

`cobc370` builds this by hand rather than calling the macro, so the DECB lives
in the data area and its area pointer can be the block obtained at OPEN. The
read itself is exactly what the macro generates:

    LA    1,DECB
    L     15,dcb+88          DCBLRAN, "address of read-write K module"
    BALR  14,15
    WAIT  ECB=DECB

BLKSIZE is only known once OPEN has read the label, so the area is `GETMAIN`ed
at OPEN from `DCBBLKSI` (DCB+62) rather than assembled from a `BLOCK CONTAINS`
clause that could be absent or wrong.

### Proof it is genuinely random

Reading the third record's key returns `PTROFFS = X'82'` = 130 = `16 + 2×57` —
the pointer resolves to the *third* record inside the block, not to its start —
and the bytes match a sequential read of the same record exactly. The `isamrnd`
test then fetches keys `10303` and `10301` in that order, descending, which
sequential access cannot produce, and takes INVALID KEY for a missing key.

Verified DCB offsets (from `DCBD DSORG=IS`): `DCBOPTCD` 52, `DCBBLKSI` 62,
`DCBLRECL` 82, `DCBLRAN` 88, `DCBLWKN` 92, `DCBRELEX` 104, `DCBFREED` 108.

## QISAM load mode — creating an ISAM dataset

`OPEN OUTPUT` on an indexed file is load mode: ordinary `PUT`, records presented
in **ascending key order**, and no way to insert. `GL039` in the corpus is the
worked example — it loads `SVD001.DESCIDX` from a flat file, BATCH uses it, and
then deletes it, so this path runs on every reporting run.

Reading takes every attribute from the label, but **creating has no label yet**,
so the DCB must carry the geometry itself:

    FD000    DCB   DDNAME=DESCIDX,DSORG=IS,MACRF=(PM),RECFM=FB,          X
                   LRECL=81,BLKSIZE=810,KEYLEN=10,RKP=1,OPTCD=L,         X
                   SYNAD=ISYNAD

`LRECL` and `BLKSIZE` come from `RECORD CONTAINS` and `BLOCK CONTAINS` — which
is why `BLOCK CONTAINS` finally has to be *parsed* rather than skipped. `KEYLEN`
and `RKP` are worked out from where the `RECORD KEY` sits inside the 01 record:
`RKP = key.offset - record.offset`, which is 1 because the delete flag comes
first. `OPTCD=L` selects the delete option and is what makes that first byte
meaningful.

A key out of order, or a duplicate, is reported through **SYNAD** — that is what
`INVALID KEY` on `WRITE` tests.

Verified end to end by `bin/cobc-isam-roundtrip`: a dataset created by
cobc370's output and read back by cobc370's output. The resulting DSCB is
structurally identical to the one ANS COBOL produced for GLACCT — `IS FB`,
`OPTCD=02`, Format-2 DSCB present, `PRCTR` equal to the record count.

### Four traps, all of which cost a run

**RAKF.** Reading `SVD001.*` as the reader default user `PROD` is allowed;
*creating* is not, and the failure surfaces as `IEF197I SYSTEM ERROR DURING
ALLOCATION` plus `JOB FAILED - JCL ERROR`, which reads like a JCL bug. The
actual cause is one line up: `RAKF000A PROD ,jobname ,DATASET ,SVD001.TESTIDX`.
Creating decks need `USER=HERC01,PASSWORD=@HERC01PW@`.

**Two `ASMFCLG` steps in one job collide.** The proc's `SYSLMOD` is
`&&GOSET(GO)` with `DISP=(MOD,PASS)`, so the second link-edit cannot store
another member called `GO`: it warns `IEW0421 ... WILL TRY TO STORE UNDER
'TEMPNAME'` with RC=4, and `PGM=*.LKED.SYSLMOD` then silently runs the *first*
program again. Give the second step its own `//LKED.SYSLMOD DD DSN=&&GOSET2(GO)`.

**Do not code `DCB=(DSORG=IS,RECFM=F)` on the DD when reading.** A loader that
honours `BLOCK CONTAINS` writes **FB**, and the JCL DCB overrides the label, so
`RECFM=F` contradicts the data and OPEN abends **S03B**. Supply no DCB at all
and let OPEN read the label.

**Continuation cards must stop by column 71.** A character in column 72 *is* the
continuation flag, so an operand that reaches it turns the next card into a
continuation — one DCB silently swallowed the DCB defined after it, and the only
symptom was `IFO188 ... IS AN UNDEFINED SYMBOL` for a label that was plainly
there.

## Still worth moving to VSAM

None of this makes ISAM a good idea. The escape hatch is the **ISAM Interface
Program**: a VSAM KSDS with `AMP='AMORG'` on the DD, against a DCB that still
says `DSORG=IS`, and OPEN routes the ISAM macros into VSAM. That means the ISAM
support here is not wasted after VSAM lands — the *JCL* can redirect a program
without recompiling it. (IIP was withdrawn from z/OS around V1R7; whether 3.8j's
VSAM has it is untested here.)

## Recovery gap — narrower than it first looks

`FW.ACCOUNTS`, the flat input `LDGLACCT` loads from, no longer exists; it is an
older schema. So `SVD001.GLACCT` specifically **cannot be rebuilt from source**
and must be preserved as data. See `doc/DISASTER-RECOVERY.md`.

That is a gap in one dataset's provenance, **not** in the ISAM load path. BATCH
still creates an ISAM file of descriptions, uses it, and deletes it on every
run, so the three-DD create JCL above is exercised routinely and is known good.

It also means retrieval is not the whole story for the compiler: replacing that
BATCH program eventually needs ISAM **load mode** — QISAM sequential output in
ascending key order — and not just the random and sequential reads implemented
so far.

## The JCL, which is worse than the access method

This is the part the historical record is thinnest on, so the working decks are
kept verbatim in `jcl/isam/LDGLACCT.jcl` and `jcl/isam/EXGLACCT.jcl`. The shapes
for creating and for reading are not the same, which is the trap.

**Creating** an ISAM dataset takes **three concatenated DD statements under one
ddname**, in this order, with the area named by an element suffix on the DSNAME:

    //GO.GLACCT DD DSNAME=SVD001.GLACCT(INDEX),DISP=(,KEEP),UNIT=3380,
    //             VOL=SER=SVD003,SPACE=(CYL,2),
    //             DCB=(DSORG=IS,RECFM=F)
    //          DD DSNAME=SVD001.GLACCT(PRIME),DISP=(,KEEP),UNIT=3380,
    //             VOL=SER=SVD003,SPACE=(CYL,20),DCB=*.GLACCT
    //          DD DSNAME=SVD001.GLACCT(OVFLOW),DISP=(,KEEP),UNIT=3380,
    //             VOL=SER=SVD003,SPACE=(CYL,5),DCB=*.GLACCT

Each area gets its own `SPACE`. Only the first carries a real `DCB`; the other
two back-reference it with `DCB=*.GLACCT`. `(INDEX)` and `(OVFLOW)` are
optional in principle and painful in practice — supply all three.

**Reading** an existing one is a single ordinary DD with no element suffix:

    //GO.GLACCT DD DSNAME=SVD001.GLACCT,DISP=(OLD,KEEP),
    //             DCB=DSORG=IS,VOL=SER=SVD003,UNIT=3380

`DCB=DSORG=IS` is not actually required — OPEN takes DSORG, RECFM, LRECL,
BLKSIZE, KEYLEN and RKP from the label. The regression harness allocates it as
plain `DSN=SVD001.GLACCT,DISP=OLD,UNIT=3380,VOL=SER=SVD003` and that is enough.

**Deleting** one, so a load can be rerun, uses `IEFBR14` with

    //DSN2DEL DD DSN=SVD001.GLACCT,DISP=(MOD,DELETE,DELETE),
    //           UNIT=3380,VOL=SER=SVD003

`MOD` rather than `OLD` so the step works whether or not the dataset is there.

Two constraints that are properties of ISAM, not of the JCL: the dataset must be
**loaded in ascending key order** in a single pass, and the load program writes
it sequentially — there is no incremental create.
