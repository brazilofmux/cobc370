# VSAM: plan and reference

## Why this is different from every slice so far

ANS COBOL has **no VSAM at all** — that is why `SVD001.VSAMIO` exists. So unlike
ISAM, LINKAGE, or Report Writer, there is no ANS COBOL implementation to diff
against. Native VSAM in cobc370 is not a reimplementation; it is a capability
the original compiler never had.

## The reference, and it is a good one

Jay Moseley's VSAMIO package, already on the system:

- `SVD001.VSAMIO.SOURCE(VSAMIOS)` — 670 lines of assembler, a working VSAM
  engine for MVS 3.8j. `SVD001.VSAMIO.OBJECT(VSAMIO)` is it pre-assembled.
- `VSAMIO` / `VSAMIOFB` — copybooks defining the parameter block.
- Seventeen COBOL programs driving it, and 28 JCL decks in `.CNTL`.

Those programs **are** the reference: they exercise VSAM through the assembler
routine, so whatever they print is what a natively-compiled equivalent must
print. `bin/vsam-test <member>` runs one, adapting only the job card.

Established so far: `VSTEST01` builds the input, `VSTESTK1` defines
`SVD001.VSTESTKS.CLUSTER`, `VSTESTK2` loads 100 records, `VSTESTK3` reads them
back. All clean.

## The ladder

The KSDS decks map exactly onto the order to build this in:

| Deck | Program | Exercises |
|---|---|---|
| K1 | IDCAMS | DEFINE CLUSTER |
| K2 | KSDSLOAD | sequential load — `OPEN OUTPUT`, `WRITE` |
| K3 | KSDSREAD | **sequential read — `OPEN INPUT`, `READ`** |
| K4 | KSDSUPDT | `OPEN I-O`, `REWRITE` |
| K5 | KSDSRAND | `ACCESS IS RANDOM`, `READ` by key |
| K6 | KSDSSSEQ | `START`, then sequential |

Then the same again for ESDS (`ORGANIZATION SEQUENTIAL`) and RRDS
(`ORGANIZATION RELATIVE`), which have their own decks.

## Mechanics, learned from VSAMIOS

    ACB   DDNAME=x,MACRF=(KEY,SEQ,IN),EXLST=exits
    RPL   ACB=acb,AREA=rec,AREALEN=n,OPTCD=(KEY,SEQ,NUP,MVE)
    EXLST EODAD=...,LERAD=...,SYNAD=...
    OPEN  (acb)        CLOSE (acb)
    GET   RPL=rpl      PUT RPL=rpl      ERASE RPL=rpl     POINT RPL=rpl
    SHOWCB RPL=rpl,FIELDS=(FDBK|RECLEN),AREA=,LENGTH=4

VSAMIOS builds its control blocks dynamically with `MODCB` because it serves any
file at run time. **A compiler does not need that**: organization, access and
mode are known at compile time, so the ACB and RPL can be assembled statically
with the right `MACRF`/`OPTCD`. That is a large simplification.

## Slice 1: KSDS sequential read

Target is `KSDSREAD` rewritten against native VSAM, producing byte-identical
output to the VSAMIO version.

Work list:

1. `FILE STATUS IS x` on SELECT — the first thing the compiler rejects.
2. `ORGANIZATION IS INDEXED` must distinguish VSAM from ISAM. The convention is
   the ASSIGN name: `DA-I-name` is ISAM, a bare or `AS-` name is VSAM.
3. Emit ACB, RPL and EXLST instead of a DCB.
4. `OPEN INPUT` / `CLOSE` against the ACB; `READ ... AT END` as `GET RPL=`.
5. Maintain FILE STATUS: `'00'` on success, `'10'` at end of file, and the
   VSAM feedback codes mapped for the error cases.

## Scale

This is bigger than the ISAM slice. SELECT grows several clauses, the verb set
grows `REWRITE`, `DELETE` and `START`, and FILE STATUS is a cross-cutting
concern ISAM never needed. Several sessions.
