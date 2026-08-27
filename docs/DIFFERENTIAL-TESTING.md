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

## Second swap: GL023

Both GL022 and GL023 compiled by cobc370, both ahead of the shipped library:
**every non-blank line of the entire 37-step job is identical** — 2547 lines,
with GL023's own report matching at 302 lines. All steps RC=0000.

### The normaliser was hiding a whole report

GL023's page heading is `B Y   N A M E`. That line uses exactly six distinct
characters, is all capitals and spaces, and is over forty columns wide — which
is precisely how the banner heuristic identified JES2 separator art. It had been
deleting the heading, and with it any chance of noticing a difference there.

The raw capture had it four times; the normalised file had it zero times. Caught
only by looking for GL023's report by name after the diff came back clean.

Judging such a line in isolation cannot work: a block letter's thick row is
unmistakable (`JJJJJJJJJJ`) but its thin rows are not (`JJ      33        33`),
and they look exactly like a spaced-out heading. The detector now finds the
unmistakable rows and grows outward over adjacent lines that could belong to the
same block, so a heading standing on its own is never adjacent to one and never
touched.

Both earlier results were re-verified against the corrected normaliser before
being believed.

## Third swap: GL024 — and the bug of the project so far

GL024 selects the month's transactions (`PARM='202608'`, an FTL date window) and
feeds the entire journal pipeline. The first swap ran every step RC=0000 and
silently selected **0 of 24,525** transactions: three downstream sorts went
0-records, and the journal, activity and balance reports evaporated.

The chain that found it: the window computation was correct in an isolated
probe; an instrumented rebuild of the real program selected 172 (which is also
the right answer — the reference's 364 is *lines*, two per double-entry
transaction); the pristine build reproducibly selected 0. Diffing the two
assemblies exposed it:

**A bare `DC F` or `DC H` is aligned by the assembler.** My layout model assumed
no padding, so the assembler slid slack bytes under my offsets. Label-based
addressing hid it — every internal access stayed self-consistent — until a
*group* address crossed a boundary the layout never predicted:
`CALL 'DYNALOAD' USING … MB-FTL-IN` handed FTL an address three slack bytes
short of where `FTL-YEAR` had actually been placed. FTL read a garbage date,
returned a garbage window, and nothing matched it. The instrumented build only
worked because its extra counters happened to push the group onto a multiple of
four — alignment by luck, which is also why every earlier test and swap passed.

Two-part fix, both matching what IKFCBL00 itself does:

- COMP items are emitted `FL4'…'`/`HL2'…'` — the length modifier suppresses
  assembler alignment, so the emitted layout **is** the computed layout, always.
- 01/77 levels start on a doubleword (slack *between* areas, interiors tight),
  so control blocks handed to assembler routines have the layout those routines
  were written for, and COMP items land aligned in the common case. S/370's
  byte-oriented operands cover the rare mid-group unaligned field.

With the fix, GL022+GL023+GL024 swapped together reproduce the entire job:
every non-blank line identical, all 37 steps RC=0000.

The lesson for everything that follows: **RC=0000 with plausible-looking output
proves nothing about a program whose answers feed other programs.** Only the
end-to-end diff caught this.

## Swaps 4-7: GL025, GL026, GL029, GL030

**GL025, GL026 and GL029 verified**: with GL022-GL026 and GL029 all compiled by
cobc370, every non-blank line of the 37-step job is identical.

**GL030 is fully verified.** It was briefly reported as layout-differing; that
was a flaw in the comparison, not in the compiler — see OPEN-ITEMS item 1, now
closed. Use `bin/batch-compare`, which compares the ordered program-output lines
with blanks removed. Blank lines cannot be compared positionally, because ANS
COBOL and cobc370 encode identical vertical spacing differently in the ASA
carriage control.

### GL030 needed an ISAM DCB fix first

It abended **S03B** at OPEN. BATCH codes `DCB=(DSORG=IS,RECFM=F)` on DESCIDX,
but GL039 creates that dataset blocked 257 records to a block, so it is really
FB. OPEN merges JCL `DCB=` only into fields the program left zero, and our ISAM
input DCB stated no RECFM at all — so the JCL's wrong value won.

The DCB now states the record format when the FD states it (`BLOCK CONTAINS` >
1 gives FB), and stays silent when the FD does not, which is what the sequential
ISAM tests rely on. This is the mirror image of the sequential-input lesson from
GL022: say nothing where the label is authoritative, say it where the program
is.

## Swap 8: GL033, and closing the pagination question

GL033 verified. All eight swapped programs — GL022, GL023, GL024, GL025, GL026,
GL029, GL030, GL033 — now reproduce the entire 37-step job exactly:
**2545 program-output lines, ordered, identical.**

The pagination question that had been open since GL022 is closed, and the answer
is that cobc370 was right all along. See OPEN-ITEMS item 1 for the ASA evidence.
The lesson is about the harness rather than the compiler: **a positional diff of
printer output is not a valid comparison** when two compilers encode the same
spacing differently. `bin/batch-compare` replaces it.

## Swaps 9-11: GL034, GL035, GL036

All verified. GL034 went clean first try — 1617 lines of assembler, DYNALOAD
calls to both LTF and FTL, an `OCCURS 10` table of COMP-3 subtotals, a PARM.

GL035/GL036 exposed a real edited-field bug: a floating minus printed at the far
left of the field instead of against the digits, for values small enough that
the first nonzero digit falls at or after the significance starter. `EDMK` never
loads R1 in that case, and the fallback was wrong. See the compiler README.

Worth noting why nothing caught it earlier: GL030 uses the identical picture,
and verified clean, because every amount in the journal is positive. It took a
report with negative balances.

## Complete: all 18 programs

**Every COBOL program in BATCH — all 18, GL022 through GL043 — compiled by
cobc370 and run ahead of the ANS COBOL library, reproduces the entire 37-step
monthly job exactly: 2545 program-output lines, ordered, identical.**

Confirmed by listing `SVD001.COBC370.LOADLIB`: 18 members, all picked up in
preference to the shipped ones.

### The last three bugs

**GL042 would not assemble.** Its program CSECT reached 9272 bytes and overran
the 8192 that two code base registers cover, so `SAVEAREA` sat past the end and
every reference failed IFO209. No work register was free, so R10 was taken back
from the data side to serve as a third code base. A program with more than 8K of
WORKING-STORAGE now reloads its data bases more often — instructions, not
correctness.

**GL043 broke a page one group too late.** `PL-CLASS-END` carries a second
`LINE PLUS 1` with no fields, a blank spacer, so the group occupies two lines.
The page-fit test measured only a group's *first* line, under-counted, and kept
a group ANS COBOL would have pushed to the next page — visible as a missing
`P R O F I T   L O S S` heading near the end of the report. A group fits only if
its **last** line is within LAST DETAIL.

**GL035/GL036 exposed the floating sign** (see the compiler README).

### What the reference run was worth

Six real bugs, none of which the 28-test regression suite could find:

| Bug | Why the tests missed it |
|---|---|
| Input DCB claimed unblocked | every test reads `DD *`, where `BLKSIZE=LRECL` is true |
| COMP items aligned by the assembler | alignment by luck; only an external routine reading a group exposed it |
| ISAM input DCB silent on RECFM | only BATCH's JCL codes a conflicting `RECFM=F` |
| Floating sign misplaced | every earlier amount was positive |
| Code addressability capped at 8K | no test program was large enough |
| Page fit measured the first line | needed a multi-line group at a page boundary |

Every one required real data, real JCL, or real program size.

## When the oracle is the one that is wrong

An oracle is a second opinion, not an authority, and the seventh finding went
the other way.

`tests/bigdig.cbl` compares a signed `COMP-3` item against zero. GnuCOBOL
4.0-early-dev answers wrongly, and only in one narrow place:

    PIC S9(16) COMP-3 VALUE -1234567890123456      < 0  ->  true    correct
    PIC S9(17) COMP-3 VALUE -12345678901234567     < 0  ->  true    correct
    PIC S9(18) COMP-3 VALUE -123456789012345678    < 0  ->  false   WRONG
    PIC S9(18)        VALUE -123456789012345678    < 0  ->  true    correct

The value itself is fine: the same item compares equal to its own 18-digit
literal and moves to an edited field correctly. It is the comparison against
zero, at exactly 18 packed digits.

cobc370 gets it right, so `bigdig.expected` records `NEG` and the test carries
a comment saying that this line is deliberately not the oracle's answer.

The practice that matters here is the one that made it visible: when a
comparison fails, find out *which* side is wrong before believing either. This
one was found by narrowing until a single construct differed, then checking the
neighbouring widths -- 16 and 17 digits packed, and 18 digits zoned -- until
the wrong answer was surrounded by right ones.
