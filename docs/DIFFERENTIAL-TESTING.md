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
