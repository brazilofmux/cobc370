# Differential testing: cobc370 against ANS COBOL

Compiling is not running. The regression suite proves each language feature
works in isolation; it says nothing about whether a complete report produces the
right numbers. The way to settle that is to run the real monthly job both ways
and diff.

## The job

`SVD001.DEFTLY.GO(BATCH)` — **37 EXEC steps**, of which 18 are COBOL programs
(GL022 through GL043) and the rest are SORT and IEFBR14. It reads
`SVD001.{ACCOUNT,COMPANY,DESCRIPT,LINE,TRANSACT}`, writes only temporary
datasets plus `SVD001.DESCIDX` which it creates and deletes itself, and sends
43 print files to SYSOUT.

**It is safe to re-run.** Nothing permanent is modified.

## Running it

    bin/batch-run <label> [steplib-dsn]

The stock deck sends everything to `SYSOUT=*`, which follows `MSGCLASS=A` and
becomes a PDF through virtual1403. The harness rewrites that to `SYSOUT=Z` so
the output is captured as text. Class only affects routing, not what the
programs print.

The job card also gains `USER=HERC01`. BATCH creates `SVD001.DESCIDX`, and
creating an SVD001 dataset needs authority the reader's default user PROD does
not have — as PROD it fails with an `IEF197I` allocation error whose real cause
is a `RAKF000A` line above it.

With a `steplib-dsn`, that library goes **ahead of** `SVD001.DEFTLY.LOADLIB`
on every step, so modules rebuilt by cobc370 are picked up in preference to the
ANS COBOL ones. That is how a single step is swapped for comparison.

## Normalising

    bin/batch-normalize <raw> > <text>

Two runs of the same job are not textually identical: times, job numbers, and
above all the devices and work volumes MVS happens to pick differ every time —
SORTWK01 landing on WORK02 rather than WORK03. None of that is program output.

The normaliser blanks volatile fields, and drops exactly one category of line:
the messages saying *where* datasets were placed (IEF236I/237I/285I/373I/374I/
375I/376I/142I), the accounting boxes, and JES2 block-letter separator pages.
Failure messages, step return codes and every line of program output are kept,
because those are what the comparison is for.

Banner art is identified by looking like banner art: forty or more columns of
nothing but capitals, digits and spaces, drawn from six or fewer distinct
characters. Report content never resembles that — it carries lowercase account
names, or punctuation in its dates and amounts, and even an all-capitals
heading like `G L   A C T I V I T Y   R E P O R T` uses a dozen distinct
letters.

## Verified reproducible

Two independent runs normalise to **byte-identical** text — 3359 lines, with 35
report headings, 154 balance lines and all 37 step return codes intact. Without
that, no diff would mean anything.

## Privacy

The captures contain a real general ledger: account names, balances,
transactions. They are **git-ignored** and reproducible by re-running. Do not
commit them, and do not send them anywhere.

## Method from here

1. Reference run, kept as `reference/reference.txt`. **Done.**
2. Rebuild one program with cobc370 into a separate loadlib.
3. Re-run with that library ahead of the shipped one; diff against the
   reference. Only that program's output should move — and ideally not at all.
4. Work outward from the leaf DYNALOAD modules (FTL, LTF, DIV — pure
   computation, easy to attribute) toward the report programs.

A caveat worth keeping in view: the ledger data itself changes as transactions
are posted, so a reference is only comparable to runs against the same data.
Re-take it if the input datasets change.

## First swap: GL022

**Result: every non-blank line of the report is identical — 1164 lines, 8 page
headings, matching the ANS COBOL build exactly, on real ledger data.** All 37
steps returned RC=0000.

`bin/cobc-build GL022` compiles the corpus source with cobc370 and links it into
`SVD001.COBC370.LOADLIB`, which `bin/cobc-lib-init`'s JCL
(`jcl/cobc370-lib-init.jcl`) allocates once. `bin/batch-run swap-gl022
SVD001.COBC370.LOADLIB` then puts that library ahead of the shipped one.

### What the swap caught

The first attempt abended **S001-4** on the very first read, and it was a real
bug: every sequential input DCB was emitted as `LRECL=n,BLKSIZE=n`, declaring
each file unblocked. `SVD001.COMPANY` is `FB 50/23450` — 469 records to a block,
exactly as its FD says — so OPEN rejected the mismatch immediately.

Input files now carry no geometry at all: `DDNAME=x,DSORG=PS,MACRF=(GM)`, with
RECFM, LRECL and BLKSIZE taken from the label. That is the same lesson ISAM
taught. Output files state their geometry, now using `BLOCK CONTAINS` for the
blocking factor.

This bug could not have been found by the regression suite: every test there
reads `DD *`, where the reader supplies 80-byte unblocked records and
`BLKSIZE=LRECL` happens to be true.

### The one remaining difference

36 blank lines appear only in the reference and 31 only in the swap. They fall
immediately **before each page heading** — that is, as a trailing blank at the
foot of the preceding page. Both builds then eject to top of form, so on paper
this should be invisible; the report body, the page breaks and the record on
every line are identical.

That last sentence is an inference from the text capture, not a verified fact:
the printer emulation has already interpreted the ASA carriage control by the
time we see it. Confirming it means comparing the raw carriage-control stream,
or simply comparing the two PDFs virtual1403 renders.
