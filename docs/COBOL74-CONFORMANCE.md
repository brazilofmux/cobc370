# Where cobc370 sits against COBOL-74

`LANGUAGE-SURVEY.md` is a map of *demand*: 21 verbs measured across the 30
DEFTLY programs, and an explicit list of what those programs never use. It is
the right map for reaching parity, and it is why parity came as fast as it did.

This is the other map. It says where the compiler sits against the standard
itself, so that going further is a set of bounded choices rather than an open
grind. It is deliberately not a backlog: nothing here is a commitment, and
several entries are things this project should probably never build.

Source: ANSI X3.23-1974, read from FIPS PUB 21-1, the federal adoption
carrying the full standard text.
<https://nvlpubs.nist.gov/nistpubs/Legacy/FIPS/fipspub21-1.pdf>

## The conformance space

The standard defines a Nucleus and eleven functional processing modules. Each
has two or three levels, and lower levels are proper subsets of higher ones
within a module. Nine modules have a **null** level, meaning you may implement
none of them and still conform.

    module                          levels     shorthand
    Nucleus                          1, 2      1 NUC 1,2   2 NUC 1,2
    Table Handling                   1, 2      1 TBL 1,2   2 TBL 1,2
    Sequential I-O                   1, 2      1 SEQ 1,2   2 SEQ 1,2
    Relative I-O                  0, 1, 2      1 REL 0,2   2 REL 0,2
    Indexed I-O                   0, 1, 2      1 INX 0,2   2 INX 0,2
    Sort-Merge                    0, 1, 2      1 SRT 0,2   2 SRT 0,2
    Report Writer                    0, 1      1 RPW 0,1
    Segmentation                  0, 1, 2      1 SEG 0,2   2 SEG 0,2
    Library                       0, 1, 2      1 LIB 0,2   2 LIB 0,2
    Debug                         0, 1, 2      1 DEB 0,2   2 DEB 0,2
    Inter-Program Communication   0, 1, 2      1 IPC 0,2   2 IPC 0,2
    Communication                 0, 1, 2      1 COM 0,2   2 COM 0,2

Two definitions bound the space:

- **The minimum standard** is the low level of the Nucleus plus the low level
  of Table Handling and Sequential I-O. Those are exactly the three modules
  without a null level.
- **Full American National Standard COBOL** is the highest level of the
  Nucleus and all eleven modules.

Report Writer is the odd one: a single real level, so there is no "Report
Writer Level 2" to aspire to. The whole module is `1 RPW 0,1` or nothing.

## Where cobc370 is

The short version, as of 2026-08-29: **Level 2 of the Nucleus, Table
Handling, Sequential I-O, Relative I-O, Inter-Program Communication and
Library and Indexed I-O; Segmentation at Level 1; the Report Writer at its
one level, complete since 2026-08-30; `SORT`, `RELEASE` and `RETURN` from
Sort-Merge since 2026-09-25; and the null level of Debug and Communication.** Each module's
section below says what is there and what is not, and the dates.

When this map was first drawn the compiler did not sit at a level at all. It
cut a diagonal, because it was built by demand -- several Level 2 elements
present while Level 1 elements of the same module were missing. The roadmap
(`COBOL74-ROADMAP.md`) is how that was closed, one element at a time against
the standard's own lists.

### Nucleus — Level 2, complete

Present, and enough to compile the corpus: `ADD SUBTRACT MULTIPLY DIVIDE
COMPUTE MOVE IF GO PERFORM STOP EXIT DISPLAY`, PICTURE with editing,
REDEFINES, SIGN, SYNCHRONIZED, USAGE COMP/COMP-3/DISPLAY, VALUE, level 77 and
88, relation and sign conditions, `AND`/`OR`/`NOT`.

`ACCEPT`, `ALTER`, `ENTER`, `INSPECT` (single-character), class conditions and
switch-status conditions were added on 2026-08-27 and 28.

`GO TO ... DEPENDING ON`, `CURRENCY SIGN` and `DECIMAL-POINT IS COMMA` --
the three Level 1 elements a check against the standard's own `1 NUC 1,2` list
found still refused on 2026-08-29 -- were added the same day. The claim made
on the 28th that the Nucleus was complete had been wrong; this one was made by
walking the element list. It was wrong once more: `BLANK WHEN ZERO`
(II-14, Level 1) had been missed on that walk and was found on 2026-08-30
while finishing the Report Writer; it is in now.

Already at **Level 2**, above a floor not yet reached: `COMPUTE` (Level 2, not
1), qualification with `OF`/`IN`, level-88 condition-names, `PERFORM UNTIL`,
`PERFORM VARYING`, nested `IF`, complex conditions, the full `01`-`49`
level-number range.

The rest of Level 2 -- `STRING`, `UNSTRING`, `CORRESPONDING` on
ADD/SUBTRACT/MOVE, level-66 `RENAMES`, `PERFORM VARYING ... AFTER`, the
figurative constants and abbreviated conditions -- was added on 2026-08-29.

### Table Handling — Level 2, complete

`OCCURS`, `INDEXED BY`, and three-level subscripting are there. `SEARCH` and
`SEARCH ALL` are there — and both are **Level 2** elements.

`SET` and `USAGE IS INDEX` were added on 2026-08-27; serial `SEARCH`, the
`KEY` series with `DESCENDING`, `SEARCH ALL` over several keys and
`OCCURS ... DEPENDING ON` on the 29th. `OCCURS DEPENDING ON` is supported in
`WORKING-STORAGE` and `LINKAGE`; inside an `FD` record it would mean
variable-length records and is refused with a message.

### Sequential I-O — Level 2, complete

`SELECT`/`ASSIGN`/`ORGANIZATION`/`ACCESS`/`FILE STATUS`, FD with its clauses
accepted, `OPEN INPUT/OUTPUT/I-O`, `CLOSE READ WRITE REWRITE`, `READ INTO`,
`WRITE FROM`, `REWRITE FROM`, `AT END`, `USE` declaratives, and
`WRITE ... BEFORE/AFTER ADVANCING` in both its integer and `PAGE` forms.

`RERUN`, `SAME AREA` and `CODE-SET` are accepted and ignored, which is the
right answer for a single-volume implementation with no alphabet-names.

Level 2 -- `LINAGE` with `LINAGE-COUNTER` and `END-OF-PAGE`, `ADVANCING` by
identifier and by channel mnemonic, `OPTIONAL`, `EXTEND`, `RESERVE`, `SAME
RECORD AREA`, `MULTIPLE FILE TAPE`, `REVERSED`, the `CLOSE` options -- was
added on 2026-08-29. That list was wrong about `SAME RECORD AREA`: the whole
`I-O-CONTROL` paragraph was being skipped, so the files' records were never
one area, and no test would have noticed. Vince Coen's COBXREF did -- it
reads one file and uses the other file's record -- and since 2026-09-25 the
records of the files named are laid over one another, as a file's own later
records are over its first (`samerec`). `REVERSED` and `CLOSE REEL`/`NO REWIND` are generated
and untested, being tape-only.

Variable-length records (`RECFM=V`/`VB`) were added on 2026-08-30: `RECORD
CONTAINS m TO n`, records of different lengths under one FD, and IBM's
`RECORDING MODE IS F/V` (`U` and `S` refused); `WRITE` writes the length of
the record named, `REWRITE` in update mode keeps the record's length, and
a dataset written by cobc370 reads back under IKFCBL00. `BLOCK CONTAINS 0`
leaves the block size to the DD or the label. The roadmap's closing section
sets out the DCB rules this rests on.

The probe matrix, run on the guest on 2026-08-30 and settling what the
online threads argue about. Each cell is one program -- `SELECT ... ASSIGN
TO UT-S-`, `RECORD CONTAINS 80 CHARACTERS`, and the FD stating `BLOCK
CONTAINS` as shown -- compiled by IBM's IKFCBL00 and by cobc370. The
twelve input cells read a 20-record dataset of the label shown:

| dataset label | `BLOCK CONTAINS` omitted | `10 RECORDS` | `0 RECORDS` |
|---|---|---|---|
| `F`, 80/80 | both read 20 | both 20 | both 20 |
| `FB`, 80/800 | **IBM: S001-4** (`IEC020I`, block longer than the DCB's `BLKSIZE=80`); cobc370: 20 | both 20 | both 20 |
| `V`, 84/88 | both 20 | both 20 | both 20 |
| `VB`, 84/844 | **IBM: S002-04** (`IEC036I`, same cause for V); cobc370: 20 | both 20 | both 20 |

The two abends are the trap of layer 2: an FD without `BLOCK CONTAINS`
makes IBM's compiler assert an unblocked DCB, which wins the merge against
the blocked label. cobc370 states nothing for input, so the label wins
every time; the price is that a program cannot override a label on input,
which no program of this corpus needs. With `BLOCK CONTAINS 0` both
compilers read anything, which is why the corpus JCL was always written
that way. A stated `10 RECORDS` against an unblocked or smaller-blocked
label is harmless under both: the buffer is merely bigger than the blocks.

The six output cells write 20 records and the label is what IEHLIST then
shows, `RECFM`/`BLKSIZE` (`LRECL` is 80 for F and 84 for V throughout):

| FD | IBM writes | cobc370 writes |
|---|---|---|
| F, `BLOCK CONTAINS` omitted | `F`/80 | `FB`/80 |
| F, `10 RECORDS` | `FB`/800 | `FB`/800 |
| F, `0 RECORDS`, DD `BLKSIZE=1600` | `FB`/1600 | `FB`/1600 |
| V, omitted | `V`/88 | `V`/88 |
| V, `10 RECORDS` | `VB`/844 | `VB`/844 |
| V, `0 RECORDS`, DD `BLKSIZE=1684` | `VB`/1684 | `VB`/1684 |

One difference, and it is a spelling: for an unblocked fixed file IBM's
label says `F` and cobc370's says `FB` with `BLKSIZE=LRECL`, which every
access method treats as the same thing (and which both compilers, and
IEBGENER, read back as 20 records). For V both write `RECFM=V`. Every
other cell is byte-identical between the two compilers.

### Relative I-O — Level 2, complete

RRDS through VSAM. `ORGANIZATION RELATIVE`, `ACCESS SEQUENTIAL/RANDOM`,
`READ WRITE REWRITE DELETE`, `INVALID KEY`, `RELATIVE KEY`.

`ACCESS DYNAMIC`, `READ NEXT` and `START` are Level 2 and were there first;
`USE` declaratives, the last Level 1 element, were added on 2026-08-29.

### Indexed I-O — Level 2, complete (with one VSAM-imposed split)

ISAM and VSAM KSDS. `ORGANIZATION INDEXED`, `RECORD KEY`, `ACCESS
SEQUENTIAL/RANDOM`, `READ WRITE REWRITE DELETE START`, `INVALID KEY`.

Above the floor: `ACCESS DYNAMIC`, `READ NEXT`, `READ ... KEY IS`, `START` —
Level 2.

`USE` declaratives were added on 2026-08-29; `ALTERNATE RECORD KEY` with
`DUPLICATES`, `READ ... KEY IS`, `START ... KEY IS` an alternate, the key of
reference, and the `02`/`22`/`23` statuses on 2026-08-30, on VSAM alternate
indexes and paths. This VSAM will not have a base and its paths open together
while the base is open for output, so a file opened `I-O` updates by the
prime key (VSAM maintaining the alternate indexes) and reads by alternate
keys only when opened `INPUT`; a program that opens a file `I-O` and reads it
by an alternate key is refused with that reason. The roadmap's closing
section records the probes.

### Report Writer — Level 1, complete

`1 RPW 0,1` is a single level; it was completed in five slices on
2026-08-30 -- the plan and the record are in `COBOL74-ROADMAP.md` under
"Report Writer: closing the module". Every element of the module's list
is present:

Present: `REPORT IS` on the FD; `RD` with the `PAGE` clause entire -- `LIMIT`,
`HEADING`, `FIRST DETAIL`, `LAST DETAIL`, `FOOTING`, with the implicit values
of 2.16.4(2) -- and without it, a single page of indefinite length;
`LINE-COUNTER` and `PAGE-COUNTER` as special registers, qualifiable by the
report-name, usable in `SOURCE` and in the Procedure Division; the `CONTROL`
clause with `FINAL` and a data-name series (2.10), breaks sensed by the
relation-condition rules of each item's category; report groups of every
`TYPE` -- `REPORT HEADING`, `PAGE HEADING`, `CONTROL HEADING`, `DETAIL`,
`CONTROL FOOTING`, `PAGE FOOTING`, `REPORT FOOTING` -- presented by Tables
1 to 5 of 2.5.5 (the fit tests, the first-line rules, the saved next group
integer, the final `LINE-COUNTER` settings, page advance processing) and
sequenced by 3.1.4 and 3.4.4 (footings minor to major up to the break,
headings major to minor from it; `TERMINATE` as a break at the most major
level); prior values of the controls for `CONTROL FOOTING` and `REPORT
FOOTING` `SOURCE`s (2.21.4(13)); `LINE NUMBER` absolute, `PLUS` and `NEXT
PAGE`; `NEXT GROUP` absolute, `PLUS` and `NEXT PAGE`, ignored on a footing
below the break level (2.15.4(3)); report entries with their clauses in any
order (2.5.3(2)); `COLUMN NUMBER`; `SOURCE` with subscripts, including a
sum counter defined later in the section; `VALUE`; `USAGE DISPLAY`; the
`SUM` clause with `UPON` and `RESET ON` (2.20) -- subtotalling on
`GENERATE`, crossfooting and rolling forward when a footing is processed,
counters zeroed by `INITIATE`, usable as `SOURCE` and in the Procedure
Division, qualified by the footing's name or the report's; `GENERATE
data-name` and `GENERATE report-name` (summary reports, 2.21.4(11)),
`INITIATE` and `TERMINATE`, of one report or a series; `USE BEFORE
REPORTING` with `SUPPRESS PRINTING` (3.3, 3.5); `GROUP INDICATE` (2.12);
`JUSTIFIED` and `BLANK WHEN ZERO` on printable items; `CODE` (2.7);
`REPORTS ARE` (several reports on one file); the FD clauses accepted.

Nothing in the module is refused. The oracles for the later slices are
the 1974 text itself, IKFCBL00 corroborating where its 1968 Report Writer
reaches.

**Measured against IKFCBL00's own report files, 2026-09-25** (issue #23),
by writing six of the `rpt*` programs to a data set under both compilers
and dumping the records:

- The file is `RECFM=FA`, 133 bytes -- the ASA byte and 132 columns --
  under both. cobc370 had briefly made it 134, the last byte a stray X'00'
  read from past the line buffer; `rptraw` now asks `LISTDS` what the file
  is, because a read-back through a shorter FD cannot tell.
- Four of the six print **the same pages**. The records differ only in
  IKFCBL00's habits: it ejects on a blank record (`1`) and prints the first
  line with `+`, and it spaces with the line's own `0` where cobc370 writes
  a blank line first. Same paper, different records.
- Two print differently, and there the 1974 text is followed rather than
  IKFCBL00's 1968 Report Writer: a summary report's `SUM` counters
  accumulate on `GENERATE report-name` as though a detail existed
  (2.21.4(11); IKFCBL00's print 0), and `NEXT GROUP` and a report heading
  sharing the first page are placed by the tables of 2.5.5.
- IKFCBL00 does not compile `rptnext`, `rptuse` or `rptcode` at all.

So the record layout matches IBM's; the encoding style does not, and is not
copied onto a Report Writer whose rules differ where they matter.

### Inter-Program Communication — Level 2, complete

`LINKAGE SECTION`, `PROCEDURE DIVISION USING`, `CALL 'literal' USING` all
work, and the call round trip is a regression test.

`EXIT PROGRAM`, `CALL identifier` and `CANCEL` were added on 2026-08-29.
`GOBACK`, an IBM extension not in the standard, stays as well. The source
comment that once said ANS COBOL has no `CALL identifier` was true of IBM's
compiler and false of the standard -- `2 IPC 0,2` lists it -- and that is the
kind of thing the standard-shaped map exists to catch.

Two things found on 2026-09-25 by running Jay Moseley's install check of
NCZ93205 -- the CBT PDS-reading routine Vince Coen's COBXREF calls -- under
both compilers:

- **Each CALL leaves the callee's R15 in `RETURN-CODE`**, as IKFCBL00's does,
  so a program that calls has the register whether or not it names it. A
  program that never mentions it still ends with its last callee's return
  code as the step's condition code (measured: a CALL returning 4, then
  `STOP RUN`, is `COND CODE 0004`). The check reads a member with
  `PERFORM ... UNTIL RETURN-CODE NOT = 0`; without this, it never stopped.
- **Each program's WORKING-STORAGE is its own.** It was a CSECT named
  `COBWS` in every program, and the linkage editor keeps only the first of
  two CSECTs with one name: a caller and a separately compiled subprogram,
  linked together, shared the caller's storage, and each wrote over the
  other's items. It is private code now, which is never merged. The call
  round trip had passed because its caller moved its input again before
  every CALL; it now checks that the caller's items survive, and that the
  return code comes back.

### Segmentation — Level 1

Segment-numbers on sections are accepted, and `ALTER` respects them. Level 2
adds `SEGMENT-LIMIT`, which is not.

### Library — Level 2, complete

`COPY text-name [OF library]` and `REPLACING` with pseudo-text, added on
2026-08-29, host side: the scanner stacks the copybook, found on the `-I`
directories or beside the program.

IBM's older form, `01 NAME COPY MEMBER.`, is accepted too: the COPY stands
for the rest of the entry, and the member's first entry header -- its level
number and data-name -- gives way to the copying program's, while its
clauses and every entry after it are copied as written. That is what
IKFCBL00 does on MVS 3.8j (checked there, with `LIB`); `tests/copyent`
records its output. A member that opens with clauses rather than a level
number is inserted as plain text, as before.

### Sort-Merge — SORT, RELEASE and RETURN

Added on 2026-09-25, because COBXREF sorts. `SD`; `SORT file ON ASCENDING/
DESCENDING KEY ...` with `INPUT PROCEDURE` or `USING` and `OUTPUT PROCEDURE`
or `GIVING`; `RELEASE [FROM]`; `RETURN [INTO] AT END`; IBM's `SORT-RETURN`
and `SORT-FILE-SIZE` registers. Every piece was measured on IKFCBL00 first,
and the five tests' expected output is its own (`sortug`, `sortproc`,
`sortret`, `sortsize`, and `samerec` for `SAME RECORD AREA`).

It is done the way IKFCBL00's `ILBOSRT0` does it: a `LINK` to the system
sort -- OS/360 Sort/Merge 1.05 on TK5 -- with the control statements in the
parameter list, and the records passing through E15 and E35 exits that live
in the program. The statements are IBM's to the byte: `SORT FIELDS=(pppp,lll,
ff,o,...)`, the format from the key's usage (`CH`, `ZD` for DISPLAY numerics,
`PD` for COMP-3, `FI` for COMP), `SIZE=E` from `SORT-FILE-SIZE` when it is
positive, and `RECORD TYPE=F,LENGTH=(n)`; `sortsize` checks the sort's own
echo of them against IKFCBL00's. The exits are coroutines with the program:
E15 resumes the input procedure and `RELEASE` hands a record back (return
code 12); E35 resumes the output procedure with a record and `RETURN` asks
for the next (4); the end of a procedure hands back 8. `PERFORM` returns
through cells in storage rather than registers, which is what makes it safe
to leave a procedure in the middle and come back. `USING` and `GIVING` are
the same machinery with procedures the compiler writes -- `OPEN`, `READ`,
`RELEASE`, `CLOSE`, or `RETURN`, `WRITE` -- so they read and write whatever
the compiler already can.

Measured and matched: `SORT-RETURN` is the sort's return code, stored when
the `SORT` ends, and a value the program moves there is ignored -- 16 set in
either procedure stops nothing (`sortret`). An output procedure of a sort
with no records runs, and its first `RETURN` takes `AT END`. The sort
refuses records too short for its work files (`IER059A`, reason 01, for a
14-byte record); that is the sort's limit, and a program meets it the same
way under either compiler.

Not implemented: `MERGE`, which IKFCBL00 does not have -- there is nothing
on this system to check it against -- and `COLLATING SEQUENCE`. A key with
`SIGN LEADING` or `SEPARATE` is refused by name.

### Null — nothing implemented

`Debug`, `Communication`. Both have a null level, so both are conforming
choices.

## The minimum standard

The minimum standard is `1 NUC` + `1 TBL` + `1 SEQ`, the three modules without
a null level. Table Handling closed on 2026-08-27, Sequential I-O on the 28th,
and the Nucleus on the 29th -- after a first claim on the 28th that turned out
to be three elements short. All three are complete, each verified against the
standard's element list.

It was eleven elements when this section was first written — `ACCEPT`, `ALTER`,
`ENTER`, `INSPECT`, class conditions and switch-status conditions in the
Nucleus; `SET` and `USAGE IS INDEX` in Table Handling; declaratives and
`WRITE ... ADVANCING` in Sequential I-O — and knowing it was eleven rather than
a hundred is what made it worth starting. Two more turned up on the way, both
because a list written from memory was checked against the standard's own
element list rather than trusted: switch-status conditions are Nucleus level 1,
and `BEFORE ADVANCING` and `OPEN I-O`/`REWRITE` are Sequential I-O level 1.
Both times the correction was found by testing each element one at a time.

That was the first claim the project could make, on 2026-08-29: **cobc370
implements the COBOL-74 minimum standard.** By the end of the same day the
roadmap's definition of done was reached as well -- Level 2 of those three
modules and of Relative I-O, Inter-Program Communication and Library, Indexed
I-O at Level 2 less `ALTERNATE RECORD KEY`, Segmentation at Level 1 -- and
on the 30th the Report Writer entire. None of it is a validated
claim — nobody has run the 1974 audit routines against it, and CCVS-85 tests
a later standard — but all of it is checkable, and the map above is where to
check it.

## Testing it

**In use here: CCVS-85.** The NIST COBOL-85 validation suite is public
domain — `newcob.val`, 512 test programs plus copy members and data files
delimited by `*HEADER`, extracted with `EXEC85`.
<https://github.com/Zaneham/nist-cobol85-test-suite>

It is the wrong standard year, but it is organised by the *same* map: test
names encode module and level (`NC211A` is Nucleus level 2, `SG102A` is
Segmentation level 1). So the modules claimed here can be run selectively, and
failures triaged into "genuinely missing" versus "COBOL-85 semantics that '74
did not have."

GnuCOBOL already carries the harness. In `~/gnucobol-svn/tests/cobol85`,
`make NC` (or `make modules`) downloads `newcob.val`, extracts `EXEC85` from
it, and splits the population file into per-module `.CBL` files -- 426
programs and 9748 assertions across twelve modules, all of which GnuCOBOL
passes. `bin/cobc-ccvs` runs them through this front end.

### What that measured

Almost nothing compiles yet, which was expected. The useful output is not a
score but a histogram -- which single missing thing blocks the most programs:

    126   more than 15 digits
     25   an unimplemented SELECT clause
     20   literal continuation
     20   an unrecognised Data Division entry
     13   BLOCK CONTAINS n CHARACTERS
      9   the SIGN clause
      6   the JUSTIFIED clause

Eleven slices in, the histogram has gone flat -- no single thing blocks more than
a tenth of the corpus any more:

     25   an unimplemented SELECT clause  (13 of them correctly refused)
     23   a USAGE DISPLAY item past 16 digits
      8   a RELATIVE KEY spelling
      5   nesting tables more than three deep (correctly refused)
      5   an alphanumeric PICTURE mixing X and 9
      5   a paragraph name reused in another section
      5   MAXSTMT, an internal limit rather than a feature

That flattening is the result worth noting. The first slices each cleared
something standing in front of a quarter to a half of the corpus; from here the
work is broad rather than deep.

## Where the edge is

After thirteen slices the corpus went from **2 of 265 programs compiling to 81**,
and the histogram no longer has a Level 1 gap at the top of it. What stops the
rest sorts into four kinds, and only the last is ordinary work:

**1. COBOL-85 spellings, correctly refused.** `STATUS IS` without `FILE`,
`PADDING CHARACTER`, `RECORD DELIMITER`, `NOT INVALID KEY`, `CALL ... BY
REFERENCE`, tables nested more than three deep, `ADVANCING` by an identifier.
These will sit near the top of the list forever. Implementing them would make
the compiler accept programs COBOL-74 does not have.

**2. Level 2 elements.** `SELECT OPTIONAL`, `ALTERNATE RECORD KEY`,
`OCCURS ... TO ... DEPENDING ON`, `LINAGE` and its counter, qualification of a
paragraph name by its section. Real COBOL-74, at the level above the one this
compiler is closing. `ALTERNATE RECORD KEY` is also the largest single piece of
infrastructure left: on MVS it means VSAM alternate indexes and paths, not a
compiler change.

**3. One machine limit that is still a limit.** `USAGE COMPUTATIONAL` past nine
digits needs a doubleword binary field, and S/370 has no 64-bit arithmetic.
It is not impossible -- IBM's own compilers convert through a 32-bit divide by
a power of ten and reassemble -- but everything else here is computed in packed
decimal, so it would add a multi-precision path used by nothing but the
representation tests. **18 programs.** This is the honest edge: the next thing
worth doing, and the first one whose cost is out of proportion to a COBOL-74
target on this machine.

**4. Ordinary remaining work, all small.**
continuation of a word or a numeric literal, and a `SIGN` clause on an item of
more than sixteen digits, a limitation this compiler introduced itself when the
zoned conversion was split.

### Reading the histogram: a blocker is not always a gap

The corpus is CCVS-**85**, and some of what it uses is COBOL-85 only. Refusing
those is the correct behaviour for a COBOL-74 compiler, so they will sit at the
top of the list forever and should not be worked on. Breaking the 25-program
`SELECT` bucket apart:

    11   STATUS IS without the word FILE   COBOL-85; '74 requires FILE STATUS
     2   PADDING CHARACTER IS             COBOL-85 only
    10   ALTERNATE RECORD KEY             Indexed I-O level 2, and on MVS it
                                          needs VSAM alternate indexes and paths
     2   qualification of the FILE STATUS name, and one ACCESS MODE spelling

So 13 of the 25 are conformant refusals and 10 need a large VSAM feature. The
histogram counts what stops a program, not what is missing from the compiler --
worth checking before treating the top line as the next slice.

Each fix uncovers the next thing, and the count that matters is the one at the
top of the list rather than the number that compile:

    blocker                      start   digits   REDEF   literals   SECT   FD   now
    more than 15 digits            126       22      22         22     22   22    22
    group REDEFINES                  -       94       0          0      0    0     0
    literal continuation            20       20     112          0      0    0     0
    a SECTION in the Proc Div        -        -       -         77      0    0     0
    WRITE of a non-first record      -        -       -          -     77    0     0
    WRITE ... AFTER ADVANCING        -        -       -          -      -   78     5

Two of those had been sitting near the bottom of the list the whole time, only
because most programs hit something else first.

Three of those are Nucleus **Level 1** requirements this map had missed:

- **18 digits.** `1 NUC 1,2` sets numeric literals at 1 through 18 digits and
  arithmetic operands at 18. **Done** -- see below.
- **Literal continuation.** Level 1 permits a nonnumeric literal to be
  continued on the next line with a hyphen in column 7; only *words and
  numeric* literals are held back to Level 2. cobc370 rejects all continuation.
- **Operand series.** `ADD identifier/literal series TO identifier` is Level 1,
  not 2. cobc370 takes one operand.

### 15 digits to 18

The ceiling was 15 because the packed scratch areas were 8 bytes. They are now
16, and the standard's 18 is reached for `COMP-3` and for literals: `PWK1`,
`PWK2` and `EDSRC` are `PL16`, and every `ZAP`/`AP`/`SP`/`SRP`/`CP` on them
carries a 16-byte length. `tests/bigdig.cbl` exercises 18-digit add, subtract,
`COMPUTE`, comparison, sign test and edited output, and agrees with GnuCOBOL.

**Zoned stops at 16, and that one is the machine.** `PACK` and `UNPK` hold each
operand length in four bits, so the widest zoned field they can convert is 16
bytes. A `USAGE DISPLAY` item of 17 or 18 digits now gets a diagnostic saying
so. Lifting it means splitting the conversion and shifting the top digits into
place with `MVO`; nothing has asked for that yet. Edited items are unaffected --
they are destinations, reached through `ED` rather than `UNPK`.

`MULTIPLY` and `DIVIDE` have their own machine ceiling: `MP` and `DP` take a
right-hand operand of at most 8 bytes, so a multiplier or divisor is limited to
15 digits whatever the scratch areas are. `MULT8`, `DIVR8` and `QTMP` stay
`PL8` for that reason.

In the corpus the 15-digit blocker went from **126 programs to 22**.

### An oracle that was wrong

`tests/bigdig.expected` records one value that GnuCOBOL does not produce.
GnuCOBOL trunk r5698 compares a signed `COMP-3` item against a literal wrongly
for some values -- 12, 15 and 18 significant digits among them -- while
comparing the same item against a same-width `COMP-3` item is right in the same
run. cobc370 gets all 36 values of the sweep right on the guest, so the test
records the right answer and says why.

Six bugs have come out of differential testing against GnuCOBOL. This is the
first one that was on the other side. `docs/DIFFERENTIAL-TESTING.md` has the
table.

### Group REDEFINES

Behind the digit ceiling sat a second bug, worth 94 programs, and it was a
false rejection rather than a missing feature:

```cobol
03 COMPUTED-A     PIC X(20).
03 CM-18V0 REDEFINES COMPUTED-A.
    04 COMPUTED-18V0  PIC -9(18).
    04 FILLER         PIC X.
03 FILLER PIC X(50).
```

The subordinates fill exactly 20 bytes, but the compiler said *"a REDEFINES may
not be longer than the item it redefines"* -- and said it at the *following*
`01`, several entries later.

The REDEFINES state lived in two parse-local variables. An elementary
redefinition retired them itself; a group redefinition never did, because it
returns to the parser through the group path instead. So the bound stayed armed
after the group closed, and the first sibling past it tripped a limit that
should have been gone.

The state now belongs to the item -- `redef_from` and `redef_cap` on the
symbol -- and a group hands the cursor back when it closes, in one shared
`close_group`. `enclosing_cap` recovers the bound of an outer redefinition when
an inner one ends, so redefinitions nest.

That also fixed something no test had reached: a group redefinition *shorter*
than the item it covers. The cursor used to resume wherever the subordinates
stopped, so the next sibling would have been laid down inside the redefined
item. `tests/grpredef.cbl` covers the exact fit, the short one, a redefinition
nested inside a redefining group, and the item after all of them.

### The SIGN clause

`[SIGN IS] {LEADING|TRAILING} [SEPARATE CHARACTER]`, II-31. Without `SEPARATE`
the sign is an overpunch on the leading or trailing digit and the `S` costs
nothing; with it the sign is its own character position, `+` or `-`, and the
`S` is counted in the size. Trailing overpunch is what this compiler already
did with no clause at all, which is exactly the choice general rule 2 leaves to
the implementor -- so of the four combinations only three needed code.

A zoned item whose sign is not a trailing overpunch is copied into `ZWK` and
taken apart there, which makes a subscripted reference no harder than a plain
one. `MVZ` does the overpunch cases in one instruction each: on the way in the
leading zone is moved to where `PACK` looks for it and the leading digit is
made plain again; on the way out, the reverse.

**Only the SEPARATE layouts are compared byte for byte.** An overpunch is
implementor-defined *and* character-set dependent -- a negative 5 is `X'D5'` in
EBCDIC and something else entirely in ASCII -- so `tests/signclau.cbl` checks
those forms through their value and reserves the byte comparison, via
`REDEFINES ... PIC X(6)`, for `+12345` and `12345-`, which both compilers must
spell the same way.

One assembler error was worth the trip: `MVC` carries one length, on its first
operand, and asking `field_ref` for a source operand *with* a length produced
`MVC ZWK(5),D0001(5)` -- two lengths, which IFOX00 reports as a relocatable
displacement rather than as the obvious thing.

### COMP-1 and COMP-2: IBM's floating point

Not in the 1974 standard at all; IBM's, in every compiler of the line,
and what a scientific program of the era uses. John Pratt's report of
COBCAL74 (manyone/cobcal74) brought it here (#40): a calculator whose value
stack is COMP-2 and whose square root is `x ** 0.5`.

Implemented since 2026-09-26: COMP-1 (4 bytes, short) and COMP-2 (8 bytes,
long) hexadecimal float, with VALUE (the floating literal `1.5E+00`, which
IKFCBL00 requires for a non-integer, and plain decimals too); MOVE to and
from any numeric item; COMPUTE, ADD, SUBTRACT, MULTIPLY, DIVIDE with
floating operands or receivers; `**` with any exponent; comparisons; DISPLAY
in IKFCBL00's layout (a sign or a blank, the point, 17 digits for COMP-2
and 8 for COMP-1, E, the exponent's sign or a blank, two digits). The
conversions, the power and the DISPLAY text are four runtime routines;
`ln` and `exp` are computed in the runtime, in hexadecimal float, to about
sixteen digits.

Measured against IKFCBL00 first, in three probes of over a hundred cases,
and matched wherever IBM's answer was a rule:

- Floating to fixed converts to the nearest value, an exact half going
  toward zero: 2.5 gives 2, 2.50001 gives 3, 6.6 gives 7, 0.995 gives 0.99.
  ROUNDED changes nothing. A value too wide for the receiver truncates on
  the left as any MOVE does, where IBM prints digits that mean nothing.
- COMP-1 keeps 24 bits: 0.1 is 0.099999964237, as IBM's is.
- `0 ** 0` is 1.

And departed from, each measured, where IBM's answer was not one:

- An expression with a floating operand, or a floating receiver, is
  evaluated in floating point throughout -- the rule IBM's later compilers
  state. IKFCBL00 does a fixed-point subexpression in fixed point at the
  receiver's decimals, zero for a float, so `COMPUTE F = 15 / 10` gives 1
  and its own compile of COBCAL74 answers `1.5+1` with 2.00000 and `2^0.5`
  with 1.00000. Under cobc370 they are 2.50000 and 1.41421.
- `**` with an integral exponent multiplies, so `(-8) ** 3` is -512, and
  `2 ** 10` is 1024; IKFCBL00 gave +512 through logarithms of |x| for a
  floating exponent, and 24 and 0 for `2 ** 10` and `10 ** 3` into a
  COMP-2, its integer power truncated somewhere.
- A negative base to a fractional exponent, zero to a negative one, and
  division by zero are SIZE ERRORs, and leave the receiver alone, where
  IBM used |x| or gave nothing.
- Zero displays with exponent 00; IBM prints E 81 for a long zero and E 72
  for a short one. The digits of an inexact value come from one scaling
  multiply and a rounded conversion, so 0.001 shows as .10000000000000000E-02
  where IBM's shows .99999999999999920E-03; neither is the stored value's
  own 17 digits, and IBM's last two are wrong.

`DIVIDE ... REMAINDER` has no floating form, a floating item is not a
subscript, a count or a key, and ON SIZE ERROR on a floating ADD or
SUBTRACT says to use COMPUTE. Tests: `float1`, `float2`, `float3`, and
COBCAL74 itself over a batch of expressions (`cobcal74`).

### BLOCK CONTAINS n CHARACTERS

General rule 3 on IV-11: `CHARACTERS` states the physical record size
outright, where `RECORDS` states how many logical records a block holds. The
compiler had only the `RECORDS` form. Both now reach `BLKSIZE`, and a
`CHARACTERS` figure that is not a whole number of records is refused rather
than rounded, since these files are fixed-length.

**What the test found.** `tests/blkchar.cbl` writes seven 80-byte records with
`BLOCK CONTAINS 240 CHARACTERS` -- three to a block, so two full blocks and a
short one -- then closes the file, reopens it for input and reads them back. It
abended **S013** at the second `OPEN`, and the reason had nothing to do with
blocking: a file opened both `OUTPUT` and `INPUT` in one program was getting a
DCB with `MACRF=(PM)` alone, because the DCB was written as an either/or. It
now carries `MACRF=(GM,PM)` when both modes appear.

That is a bug no earlier test could have found. Every file test until this one
either wrote a file or read one; none did both through the same `SELECT`.

### MOVE from numeric to alphanumeric

The largest single blocker the corpus ever showed -- 53 programs at its peak,
because the CCVS harness reports its own results with
`MOVE PASS-COUNTER TO CCVS-E-4-1`, a `PIC 999` into a `PIC XXX`.

Rule 3c on II-75 allows the move only for an integer; rule 4a says the
receiver is filled from the left and space filled, truncated on the right, and
**the operational sign is not moved**. A `USAGE DISPLAY` sender is already a
string of digits, so it is an ordinary alphanumeric move followed by forcing an
`F` zone over the trailing overpunch -- and only when that byte was moved at
all, since a sender wider than the receiver loses its tail first. `COMP` and
`COMP-3` senders are unpacked into a zoned work area and moved from there.

Finding it took longer than fixing it. The category rules existed in two
places: `emit_move`, shared with the Report Writer's `SOURCE` placement, and an
inline copy in the `MOVE` codegen. Implementing it in `emit_move` changed
nothing, because the copy was what ran. The copy is gone and both paths now go
through the one dispatcher.

The reverse direction -- an alphanumeric item into a numeric one -- is legal
under the same rules and is still refused, but the message now says that it is
legal and what it would take, rather than implying the combination is invalid.

### ACCEPT

Format 1 at level 1, II-53: one transfer from the implementor's device, which
here is SYSIN. The `FROM` phrase -- the mnemonic-name form and `DATE`/`DAY`/
`TIME` alike -- is level 2 and is refused by name.

General rule 2 leaves the size of a transfer to the implementor. Here it is one
80-column record, into a buffer blanked before each read, so a receiver wider
than a card is space-padded and a read past the last card returns spaces rather
than the card before it. `COBACC` joins `COBDISP` in the runtime and takes the
same shape of parameter list; the list is built at run time rather than
assembled as a constant, because a subscripted receiver has no fixed address.

`COBTERM` now closes SYSIN as well as SYSOUT, and only if something opened it.

### ALTER, and what it does to segmentation

`ALTER para-1 TO [PROCEED TO] para-2`, II-57. Syntax rule 1 is what makes it
implementable: the altered paragraph holds a single sentence that is a `GO TO`
without `DEPENDING`. So that branch is compiled indirect -- a load from a cell
and a register branch -- and `ALTER` stores a new address in the cell. The cell
starts out holding the target the `GO TO` was written with, which the compiler
learns by finding that `GO TO` while checking rule 1 holds.

**This closes a loop opened by the segmentation note above.** That note said an
independent segment has no observable state without `ALTER`, so a `SECTION`
segment-number could be accepted and ignored. `ALTER` now exists, so the
statement is no longer vacuous: general rule 1 says a modified `GO TO` in an
independent segment may be returned to its initial state. This compiler does
not do that -- every section is resident and nothing is reset -- so a program
that both alters a `GO TO` in a segment numbered 50 or above *and* depends on
it reverting would be wrong here. Nothing in the corpus does, and general rule
2 forbids the cross-segment case outright, but it is no longer true that the
segment-number says nothing at all.

### INSPECT

`1 NUC 1,2` restricts `INSPECT` to a **single character data item**, which is
what makes the level 1 form tractable: every clause becomes a byte test down
the field rather than a substring search. `TALLYING` with `ALL`, `LEADING` and
`CHARACTERS`; `REPLACING` with `ALL`, `LEADING`, `FIRST` and `CHARACTERS`; and
both phrases on one statement. `BEFORE`/`AFTER INITIAL` are level 2 and are
refused by name.

One scan shape serves all of them: R3 walks the field, R5 counts it down, R4
tallies. `LEADING` branches out of the loop on the first mismatch instead of
around the body, and `FIRST` branches out after replacing once.
`CHARACTERS` needs no scan at all -- for `TALLYING` it is the length, and for
`REPLACING` it is one `MVI` and an overlapping `MVC` to carry the byte down.

`TALLYING` **adds to** the counter rather than setting it, which the test
relies on: two `ALL` clauses in a row leave 6 and then 8.

The bug worth recording is mine and it was a one-line assumption. I took the
field address with `LA 3,0(6)`, expecting `field_ref` to have loaded R6 --
which it does for a *subscripted* reference and does not for a plain one, where
it hands back the label instead. The scans read whatever R6 held, found
nothing, and the run ended in a protection exception. Asking for the operand
text and writing `LA 3,<that>` works for both shapes.

### Operand series, and ENTER

`1 NUC 1,2` lists "identifier/literal series" under both `ADD` and `SUBTRACT`,
so `ADD A B TO C` is level 1 and not only the `GIVING` forms. General rule 3 on
II-51 says the operands are added together first and the result then applied,
which is exactly what summing them into one expression does -- so the series
forms became `COMPUTE C = C + (A + B)` and needed no new code generation. The
parser previously read one operand and then insisted on `TO`, which is why
`ADD 1 2 TO N` failed on the word `TO` itself.

`ENTER language-name [routine-name]` is level 1 and is accepted and ignored.
It exists to let a program change language mid-stream; there is no other
language here to change to, so "full capabilities for the ENTER statement"
amounts to taking the sentence. GnuCOBOL rejects `ENTER LINKAGE` outright, so
that one is covered by compiling rather than by comparison.

### Switch-status conditions, and Nucleus level 1

`SPECIAL-NAMES` binds an implementor-name to a mnemonic and, for a switch, an
`ON STATUS` and `OFF STATUS` condition-name; `IF SW-ON` then tests it. II-8 and
II-44.

**The 1974 standard leaves the implementor-names open, and the compiler this
one replaces gives no help.** IKFCBL00, asked directly rather than remembered,
accepts `SYSIN`/`SYSIPT`, `SYSOUT`/`SYSLST`, `SYSPUNCH`/`SYSPCH`, `CONSOLE`,
`C01` through `C12` (`C13` is refused), `CSP`, and `S01`/`S02` -- and rejects
`UPSI-n` and `SWITCH-n` outright. OS/360 ANS COBOL has no external switches at
all; `UPSI` belongs to the DOS and OS/VS lines.

So the spellings here are a deliberate extension past IKFCBL00, taken from the
IBM systems that do have switches, and **both `UPSI-0`..`UPSI-7` and
`SWITCH-0`..`SWITCH-7` are accepted** so that source from either lineage
compiles. They reach one byte with `UPSI-0` as its leftmost bit, which is how
the string is written, and a test is one `TM`.

The bits arrive as `PARM='/UPSI(10100000)'` on the EXEC card -- the form IBM's
later compilers take. The runtime looks for the literal `UPSI` anywhere in the
parameter text and reads the next eight `0`/`1` characters; anything else
leaves all eight off, which is the documented default. A subprogram's R1 is its
caller's parameter list rather than a PARM, so its switches stay off and the
code to read them is not generated.

One bug in that routine is worth keeping, because it is a class of mistake
rather than a typo: it returned the byte in R15 with `L 15,RTUPSI` placed
*after* `LM 14,12,12(13)`. The LM restores R12, which is the base register the
routine's own constants are addressed through -- so the load read `RTUPSI`
through the caller's R12 and returned whatever was there. The value has to be
put in the save area's R15 slot before the LM, and let the LM deliver it.

**With this, the Nucleus is complete at level 1.** Every element of
`1 NUC 1,2` compiles and runs on the guest.

### Class conditions

`IS [NOT] NUMERIC` and `IS [NOT] ALPHABETIC`, II-43. The operand must be
`USAGE DISPLAY`; `NUMERIC` may not be asked of an item whose *category* is
alphabetic, which is `PIC A` and not `PIC X` -- a distinction the compiler did
not draw before, since one `is_alpha` covered both.

`TRT` does each test in one instruction. Its table gives a function byte per
character; the instruction stops at the first non-zero one and sets the
condition code, so a table of zeros for the acceptable characters and `X'FF'`
everywhere else makes `CC=0` mean "every byte was acceptable". The tables are
written the way the assembler manuals write them -- `DC 256X'FF'` and then
`ORG` back to punch zeros into each accepted range -- and are emitted only for
the tests a program actually uses.

The signed cases are where the rule has teeth. An unsigned item is numeric only
if it holds digits *and no sign*; a signed one only if it holds digits *and a
valid sign*. So an overpunched item gets two tests: the sign position against a
table that accepts a C, D or F zone, and everything else against the digits.
`SIGN IS SEPARATE` compares its own character against `+` and `-` instead.

63 tests pass on the guest.

### Table Handling level 1 is complete

Against the element list for `1 TBL 1,2`:

    index-name                                    INDEXED BY, and as a series
    subscripting and indexing, three levels       done here
    OCCURS integer TIMES                          already had it
    USAGE IS INDEX                                done
    relation conditions on indexes                falls out of the representation
    SET, both formats, receiver series            done

That is the whole module at level 1, and Table Handling is one of the three
modules the minimum standard is made of.

### Table Handling: USAGE IS INDEX and SET

Table Handling is one of the three modules in the minimum standard, and its
level 1 floor was the clearest example of the diagonal this map describes:
`SEARCH` and `SEARCH ALL` -- both **level 2** -- already worked, while `SET`
and `USAGE IS INDEX`, both **level 1**, did not exist.

The representation made this cheap. An index-name in this compiler holds the
**occurrence number** rather than a displacement, which the standard permits --
the form is the implementor's choice. An index data item is given the same
representation, a signed fullword. Every valid combination in the chart on
III-12 is then an integer move or an integer add, so `SET` builds `MOVE`, `ADD`
and `SUBTRACT` statements rather than a code path of its own:

    Sending item        Receiving item
                        integer item   index-name   index data item
    integer literal     no             yes          no
    integer data item   no             yes          no
    index-name          yes            yes          yes
    index data item     no             yes          yes

Those refusals are enforced, and each one names the chart. Operands on both
sides may be subscripted, which the corpus needs -- `SET INDEX1 TO TABLE2-REC
(INDEX2)` is the shape that found it.

### Subscripting to three levels

This was the structural piece. `opt_subscript` returned one `Node *` and died
on a comma; `occ_parent` held one table. A subscript is now a list -- `Node`
gained a `next` -- and every item carries `occ_chain`, the enclosing `OCCURS`
tables outermost first, with `occ_depth` saying how many subscripts a reference
to it needs. The address stopped being one multiply and became a sum:

    address = label + sum over levels of (subscript - 1) x element size

One term goes into the addressing register and the rest are built in R0 and
added, which is free because nothing else is live between those instructions.
A reference with the wrong number of subscripts is now a diagnostic naming the
item and both counts -- something the one-dimensional model could not check.

Two things fell out. A **group** that carries `OCCURS` can now be subscripted:
it appends itself to its own chain, and `MOVE ROW (3) TO X` moves the whole
row. And `INDEXED BY` accepts a series, with the first index-name being the
one `SEARCH` uses.

Five corpus programs nest tables more than three deep. COBOL-85 raised the
limit to seven; COBOL-74 stops at three, so refusing them is correct and the
message says which standard is speaking.

An unrelated gap surfaced while writing the test and is worth recording:
`DISPLAY` of a subscripted item is not implemented, so the test moves elements
to a work field first.

### Declaratives

`USE AFTER STANDARD ERROR PROCEDURE` is level 1 in all three I-O modules, and
it is one of the eleven elements between this compiler and the minimum
standard. The DEFTLY corpus has no declaratives; 58 CCVS programs do.

The shape falls out of machinery that was already there. General rule 2 on
IV-32 says control returns to the invoking routine after a USE procedure --
which is precisely a `PERFORM` range, entered by parking a return address in
the range's exit cell and left by branching through it. The `PERFORM` codegen
became `gen_call_range`, and a declarative section's last paragraph is marked a
range end exactly as a performed one is. Syntax rule 3 keeps control from
crossing into or out of the declaratives, so they are branched around.

**What invokes one, and what does not.** General rule 1 says the procedure runs
after the standard error routine, or on the AT END condition when the statement
carried no AT END phrase. cobc370 invokes it on **AT END without the phrase**,
which is the condition it detects. It does **not** invoke it on an OPEN failure
or on a physical I-O error: the QSAM DCBs carry no SYNAD exit, so there is
nothing to call from. GnuCOBOL does invoke it on an OPEN failure -- that
difference is visible in `tests/declar.cbl` if the input file is missing, where
GnuCOBOL runs the procedure twice and this compiler would not run it at all.

A `READ` with no phrase at all also now closes its own sentence; it previously
left the period behind, because every test until this one carried `AT END`.

### OPEN I-O and REWRITE on a sequential file — QSAM, not BSAM

The last element of `1 SEQ`. Updating a record in place is the one sequential
operation that is not read-forward or write-forward, and the obvious way to do
it on MVS is BSAM: `OPEN UPDAT`, `READ`/`CHECK`, `WRITE` the block back. That
is also the wrong way. BSAM hands back a *block*, so a blocked dataset means
deblocking by hand, tracking which record within the block the program is
looking at, computing the length of a short last block from the residual count
in the IOB, and holding a dirty block until the moment before the next read.
Several hundred lines of runtime, and every one of them a place to be wrong.

QSAM already does all of that. Its update mode is `OPEN UPDAT` with
`MACRF=(GL,PL)`: `GET` in locate mode returns R1 pointing at the record inside
the access method's own buffer, and `PUTX` with no output DCB writes the block
that record came from back where it was read. Blocking, the short last block
and the write-back ordering are the access method's problem.

So the compiled code is four instructions on each side. `READ` keeps the
pointer `GET` returned and copies the record out to the 01 — the program
addresses its record area at a fixed place and the buffer does not stay put.
`REWRITE` copies it back through that pointer and issues `PUTX`.

The DCB says nothing about geometry, as for any file that already exists: the
label is the authority. A file opened I-O may not also be opened INPUT, OUTPUT
or EXTEND, because the MACRF is settled at assembly time and one DCB cannot be
both; and `WRITE` on such a file is refused, which is what the standard says
anyway — a sequential file opened I-O is read and rewritten, not written.

`tests/sequpd.cbl` writes six records `BLOCK CONTAINS 3 RECORDS`, so its
rewrites straddle a block boundary: records 2 and 4 are in different blocks,
and record 5 is rewritten `FROM` working storage in the second block after
record 4 has forced the first one out. If `PUTX` were putting back the wrong
block, that is where it would show.

**Found on the way:** the check that refuses `ACCESS IS DYNAMIC` with
`OPEN I-O` had been sitting in `parse_data_division`, testing a flag that is
not set until the PROCEDURE DIVISION is read. It had never once fired. Both
that check and the new update-mode ones now live in `resolve_file_use`, called
at the top of code generation, where the OPEN modes are known.

### WRITE ... AFTER ADVANCING

`BEFORE/AFTER integer LINES` and `BEFORE/AFTER PAGE` are Sequential I-O level
1, and IBM's mnemonic-name for a channel rides with them. Since 2026-09-25
they are written exactly as IKFCBL00 writes them, measured byte for byte on
TK5 by dumping what it put in a data set:

- The carriage control is the record's **own first byte**. The program
  reserves it -- `03 FILLER PIC X` at the head of a print record is what
  programs of this system all do -- and the byte is overwritten on every
  WRITE. The file is `RECFM=FM` (`FBM`, `VM`, `VBM` as it is blocked or
  variable) with `LRECL` the record itself.
- The codes are **machine codes**, not ASA. `AFTER n` is immediate spacing
  records -- X'1B' for three lines at a time, then X'13' or X'0B' -- followed
  by the line written with X'01', write without spacing. `AFTER 0` puts the
  no-op X'03' first. `BEFORE n` is the line written with X'09', X'11' or
  X'19', write and space, and immediate spacing for anything past three.
  A channel is X'8B' (skip to channel 1, immediate) before the line for
  `AFTER`, X'89' (write, then skip) for `BEFORE`; channels 2-12 follow the
  same pattern. `BEFORE 0` is the line with X'01' alone.
- A `WRITE` with no `ADVANCING` on such a file is `BEFORE 1`, X'09'. IKFCBL00
  warns about it (IKF4093I) and writes that.

So nothing is held over from one `WRITE` to the next: write-and-space does
in one record what an ASA byte could only promise for the following one.

`tests/advance.cbl` writes every case, reads the file back through a second
FD with the control byte as data, and shows each code as a number; its
expected output is IKFCBL00's own run of the same program. COBXREF's
37-page listing is byte-identical to IBM's.

**Before this**, the clause followed the ANS text alone: an ASA byte the
compiler put *in front of* the record, and `BEFORE` held over to the next line
because ASA can only say what to do before a line prints. Programs written
for this system reserve the first byte themselves, so under that scheme they
printed one column to the right with a stray last character -- which is how
COBXREF found it -- and a `BEFORE` with nothing owed moved to the next line
where IBM's overprints.

**LINAGE** has no IKFCBL00 to be measured against. A `LINAGE` file keeps the
page accounting it had -- `LINAGE-COUNTER`, the body, `FOOTING` and
`END-OF-PAGE`, with `BEFORE` held over as the standard's counting has it --
and writes the lines it arrives at with the same record layout and codes:
the spacing or skip, immediate, then the line with X'01'. `seqlvl2` checks
the codes against that model.

`ADVANCING` by an identifier or a mnemonic-name is level 2, and both are
implemented: the count, or the channel, is read at run time and handled the
same way.

### Several record descriptions per FD

An FD may describe its record more than one way, and the 01s are not separate
areas: each describes the same buffer, implicitly redefining the first. The
compiler recorded only the first, so `WRITE` naming any other one was rejected
as "not a file's record" -- 77 programs, because the CCVS harness describes its
print line twice.

Each 01 under an FD now records which file it belongs to, later ones overlay
the first through the same machinery a `REDEFINES` uses, the file's record
length is the longest of the descriptions, and `WRITE ... FROM` fills the
record that was actually named rather than the first.

**Known gap, found while testing this.** An FD whose record descriptions differ
in length describes a *variable-length* file. GnuCOBOL writes one that way --
each record with its own length prefix. cobc370 emits `RECFM=FB` with
`LRECL` set to the longest description, so a shorter record is written padded
to the full length. `RECFM=V` is not implemented. `tests/fdrecs.cbl` therefore
describes its record three ways at one length, which is the case the corpus
actually needs; the differing-length case is recorded here rather than papered
over.

### Procedure Division sections

Section-names and section headers are Nucleus level 1. The DEFTLY corpus is
written entirely in paragraphs, so nothing had ever asked for them.

They cost little, because the existing machinery already had the shape. A
`PERFORM` range ends by returning through a cell just before the next
procedure's label, so a section only needs to say where its range ends: at the
last paragraph before the next section header. One line in the resolution pass
covers both `PERFORM SECT` and `PERFORM PARA THRU SECT`, since a `PERFORM`
without `THRU` already resolves its range end to its own name.

A segment-number on the header is accepted and ignored, which is a conforming
choice: Segmentation has a null level, and the only thing a program can observe
of it -- an independent segment back in its initial state -- is carried by
`ALTER`, which this compiler does not implement. With every section resident
and no altered `GO TO` to reset, the number says nothing about what the program
does. `tests/sections.cbl` carries a `SECTION 50` header for that reason.

### Literal continuation

`1 NUC 1,2` allows a nonnumeric literal to be broken across lines; words and
numeric literals are held back to Level 2. I-106, 5.8.2.2 gives the rule: a
hyphen in the indicator area, area A blank, and -- because the literal has no
closing quotation mark yet -- the first nonblank character in area B must be a
quotation mark, with the literal resuming at the character after it.

The clause that costs something is **"all spaces at the end of the continued
line are considered part of the literal."** A file whose line stops at column
38 still contributes 34 spaces, because the reference-format line runs to
column 72 whether or not the bytes are in the file. That was verified against
GnuCOBOL at six different stopping columns before implementing it: the spaces
contributed are always `72 - column of the last character`.

A hyphen outside a literal now gets a diagnostic naming what it is, instead of
being silently discarded as it was before.

### Three silent truncations behind it

Making the literal reach the compiler was one thing; getting it out the other
side turned up three places that cut data without saying so.

- **`asm_line` clamped a statement at column 71.** A comment being trimmed is
  fine, and that is what the code was written for -- but the clamp applied to
  the operand too. A 51-character literal became
  `DC CL51'...33 characters...'`, which assembles clean at RC=0000 and holds
  the wrong bytes. This is now fatal.
- **`Sym.value` was 34 bytes**, sized for a scaled numeric and shared with
  alphanumeric `VALUE`s, so any `VALUE` literal past 33 characters had been
  quietly cut -- with or without continuation.
- **`MAXTOK` was 64**, and the scanner dropped characters past it rather than
  complaining. It is 132 now, enough for the standard's 120-character literal,
  and overflow is diagnosed.

A literal too long for one statement is emitted as adjacent `DC`s. The
assembler lays them down contiguously, so the field is the same bytes; only
the source is split.

### A bug rather than an absence

The first CCVS run also found a PICTURE
containing a repetition count before its decimal point -- `PIC 9(2).99`, or
`-9(9).9(9)` -- was split at the period and the rest of the entry read as a
new one. It blocked 98 of the 336 programs, more than any missing feature.
Fixed: the scanner now applies the standard's own rule inside a picture, where
a period is a separator only when a space follows it. Every existing test's
generated assembler is byte-identical across the change.

**CCVS-74 itself: identified, not obtained.** Reported catalogue identifiers
are NTIS `AD/A-036 173` / DTIC `ADA036173`, "COBOL Compiler Validation System,
1974. Version 3.0" — audit routines plus an executive routine that resolved
implementor-defined names and generated the JCL, distributed on 9-track tape.
*These identifiers are second-hand and not verified here:* DTIC returns 403 to
scripted fetches and the search results do not surface the record directly.
What is confirmed is the shape — NTIS catalogues sibling items such as
`ADA024914`, "HYPO-COBOL Compiler Validation System (HCCVS) - Population File
(Tape)", as tape products rather than reports.

If it is ever wanted, the realistic routes are the vintage mainframe
preservation community and a direct NTIS media request, not a download.

### IBM spellings of the era

The charter is COBOL-74 plus the IBM spellings programs of that era used.
Each of these came from real programs Ed Liss ran through the compiler, and
each was measured against IBM ANS COBOL (IKFCBL00) on TK5 before it went in;
the tests' expected output is IKFCBL00's own.

- `ID DIVISION` for `IDENTIFICATION DIVISION`; `EJECT`, `SKIP1`, `SKIP2` and
  `SKIP3` as listing control, consumed wherever they stand (`iddiv`,
  `listctl`).
- `RETURN-CODE`, a halfword special register whose value becomes the step's
  condition code at `STOP RUN` or `GOBACK` (`retcode`).
- `01 NAME COPY MEMBER.`, under Library above (`copyent`).
- `EXAMINE`, the verb `INSPECT` replaced, with the `TALLY` register it counts
  into (`examine`). `TALLYING ALL`, `LEADING` and `UNTIL FIRST`, with or
  without `REPLACING BY`; `REPLACING ALL`, `LEADING`, `FIRST` and
  `UNTIL FIRST`. It is lowered onto `INSPECT`'s operations: `UNTIL FIRST x`
  is `CHARACTERS BEFORE INITIAL x`, and `TALLY` is reset first because
  `INSPECT` only adds. Matching is on bytes, as IKFCBL00's is: the last digit
  of a signed item carries its sign and matches no digit. `TALLY` is an
  ordinary `9(5) COMP` item otherwise -- moved, added to, displayed.
- `DISPLAY` of a `COMP` or `COMP-3` item (`dispcomp`). IKFCBL00 converts it:
  one digit per PICTURE position, no decimal point, a negative sign
  overpunched on the last digit, and a positive value shown as plain digits
  -- not with the C zone a signed `DISPLAY` item prints as. The earlier
  refusal asked for a MOVE first; real programs do not.
- `GO TO ... DEPENDING ON` with any number of procedure-names (`godep10`).
  Eight was this compiler's own limit, not IBM's or the standard's.

### What COBXREF found

Vince Coen's COBXREF, a cross-referencer he wrote for IBM ANS COBOL on this
system, is the largest real program yet: 1,622 lines. Besides Sort-Merge and
`SAME RECORD AREA`, above:

- `TIME-OF-DAY`, IBM's HHMMSS register. Unlike `CURRENT-DATE`, which is
  filled once at entry, it is refreshed before every statement that names it,
  as IKFCBL00's is: a program timing itself gets the time it asked for.
- `VALUE ZERO` on an alphanumeric item, which is the character 0 in every
  position. It was refused as a numeric VALUE on a `PIC X` item.
- A quoted literal could be taken for a keyword or operator. `IF C = '-'`
  followed by a statement read `- MOVE` as a subtraction. A keyword is never
  quoted, and the test that recognises one now says so.
- Conditions came from a pool of 256 for the whole program; COBXREF has more
  `IF X = 'A' OR 'B' ...` than that. The pool is 4,096, like the statements.
- A `DISPLAY` of more than eight operands printed two lines. Operands have
  lived in a side table for a long time; the split at eight was left over
  from the fixed array before it.

And the largest one: COBXREF's code did not fit. Three code base registers
covered 12K of program, constants included, and COBXREF is 16K. Since
2026-09-25 code is addressed a block at a time. R12 is the base of the block
the program is in, and every paragraph starts one with `BALR`, so it is right
whether control falls in or branches in. A paragraph longer than one base
reaches is split at a sentence boundary, where nothing branches across,
using an estimate of the code's size that errs large. Constants, work areas
and the out-of-line routines are one region after the code on R11 and R10,
which never change. A branch to a paragraph in another block goes through its
address; a return into a block -- a `PERFORM` coming back, a sort exit
resuming -- reloads R12. A program's code has no size limit now: a block has
4K and the constants 8K (`bigpara` is one 12K paragraph, split ten ways).

With it: numeric constants were 256 at 16 bytes each, a third of the new
constants region between them; each is now only as long as its longest
reference, and there may be 2,048, as there may be of nonnumeric ones. The
program-check line table widened to fullword offsets and moved past the
literal pool, where it needs no base. And a program that printed but never
displayed anything linked without the runtime and branched to zero -- the
runtime is now there for every caller of it, not only `DISPLAY`.

### The 2026-09-26 audit

A read of the whole compiler for what no program had yet tripped over, each
finding first run under IKFCBL00 on TK5 where IBM has the feature, and filed
as issues #26 to #39 so that the fixes could be watched. What changed, with
the tests that hold it:

- **Arithmetic.** A subscripted `COMP-3` right operand of `*` or `/` was
  read as element one (`subsmul`). `**` sized its multiplier at eight bytes,
  which capped the running product at 15 digits: `1.05 ** 9` took a data
  exception on the ninth multiply (`powfix`). A `COMP` receiver was not
  truncated to its PICTURE and an unsigned one kept a sign (`compunsg`).
  `DIVIDE ... REMAINDER` formed the quotient at the remainder's scale
  (`remaindr`).
- **Editing and MOVE.** Fixed and floating `$`, all-`Z` pictures, `*` fill
  beside a sign, and high-order truncation into an edited field, each
  measured against IBM's (`editfix`); numeric-edited senders,
  alphanumeric-edited receivers, and `SIGN SEPARATE` senders (`signmove`).
- **Conditions.** A literal on the left of an ordering compare was compared
  the wrong way round; `IF X = ZERO` on an alphanumeric item; `-.5`; a
  condition-name under a subscripted or group item (`condlit`, `lvl88grp`).
  `COPY ... REPLACING X BY Y` renamed the `X` inside `PIC X(5)` and inside
  literals (`cpyrepx`) -- and its word protector then stepped backward on a
  bare period and never returned, which is the hang the first regression
  run after it found.
- **PERFORM.** Nested ranges sharing an exit paragraph lost the outer
  return; `n TIMES` with n over 32767 ran zero times (`perfnest`).
- **Data Division.** `66` over an open group, `88` under a group, `VALUE`
  on `SIGN` items (`signval`), `SAME RECORD AREA`, a special register first
  named in the REPORT SECTION making file 0 variable-length (`rptdate`).
- **Code size.** A sentence too long for one 4K block is now refused with
  its line, and the constants region is checked before its `LTORG`; a
  program with WORKING-STORAGE in one chunk and no LINKAGE gets a 12K
  constants region on a third base.
- **VSAM.** One static ACB carried `OUT,RST` into every OPEN, so a program
  that loaded a cluster and then opened it `INPUT` emptied it again: the mode
  is now set by `MODCB` at each OPEN when a file is opened more than one way.
  `REWRITE` and `DELETE` in `RANDOM` and `DYNAMIC` access name the record by
  key, with no READ first, as VI-13 and VI-27 have it -- a `GET` for update
  around a copy of the record area, feedback 16 becoming status 23. And a
  `DYNAMIC` file's insert string, which is `DIR` so that a keyed READ after a
  WRITE meets no hold, is `SEQ` while the file is being created: a cluster in
  load mode takes only sequential `PUT`s (`ksdsboth`, `ksdsrnd2`).
- **QSAM and ISAM.** An OPEN that MVS could not do -- no DD -- stored status
  00 and left the USE procedure unentered; the first READ then went through
  the unopened DCB. `DCBOFLGS` is tested: status 30 and the USE procedure, or
  a message naming the DD and a U0035 when the program has neither
  (`opennodd`). A BISAM READ took the DECB's "overflow record" bit for an
  exception, so any record added after the load read as 30; and it moved the
  record with one `MVC`, which a record over 256 bytes cannot assemble
  (`cobc-isam-roundtrip`, which now adds a record with a hand-written BISAM
  step and loads a 301-byte file). Making that record took two things worth
  writing down: a file loaded with `OPTCD=L` alone has no overflow area, so
  IBM's `APPLY CYL-OVERFLOW OF n TRACKS ON file` (I-O-CONTROL) is now
  honoured, as `CYLOFL=n,OPTCD=LY` on the load DCB; and the area a `WRITE
  KN` names begins with 16 bytes for the control program, which GC26-3873
  says and nothing else does. `LINAGE-COUNTER` and the lines a `BEFORE`
  left owing start over at every OPEN, IV-15 (`linreopn`). `MOVE SPACES` to
  an item longer than 256 bytes was refused; it propagates in further `MVC`s.

Found by the port sweep after all that: on TK5 the compiler refused
`-1 ** 1` as "over eighteen digits" while the host folded it. A C probe
run on the guest put it on cc370 emitting `SLDA`, the sign-preserving
arithmetic shift, for a 64-bit shift left (mvslovers/cc370#468): the
compiler's own 64-bit millicode recombined a product's halves with such a
shift and lost bit 63 of every negative product. The millicode now joins
the halves through a union, and the literal fold works in decimal digits
with no 64-bit arithmetic at all; cc370's runtime has no 64-bit divide
either, so none is used.

`VALUE` in an entry with `OCCURS`, or under one, is refused: the 1974
VALUE clause forbids it (COBOL-85 is where it became legal) and IKFCBL00
refuses it too (IKF2149I), which was measured before deciding. The compiler
had been laying the value into the first occurrence only, so a table that
worked when storage happened to be zero would not elsewhere. `OCCURS` at
level 01 or 77 is refused for the same reasons (IKF2043I). Four early
tests, oracled against GnuCOBOL before IKFCBL00 was the oracle, set their
tables in the Procedure Division now.

From the notes in #39, three that protect a program: falling off the end
of the last paragraph is an implicit `STOP RUN` (IKFCBL00 ends the run
there too, measured), and a program no longer has to contain one; `CALL
... USING` takes 64 arguments and `PROCEDURE DIVISION USING` 64
parameters, where the caps were 8 and 16 (`sub12` in the CALL round trip
passes twelve); and a nonnumeric literal is refused as an arithmetic
operand, a repeat count or an ADVANCING count, where `ADD '1' TO A` once
compiled. The rest of #39 stays as recorded there: parity with IKFCBL00
(STOP RUN inside a sort exit), better than it (the 18-digit ADD), or
harmless on a legal program.

`INSPECT` with several operands in a phrase (#32) is now one pass over the
field per phrase, as II-68 to II-70 describe it: the operands are tried in
the order written at each position, the first that matches tallies or
replaces and the scan steps past its string, a LEADING operand stays live
only while its matches are contiguous from the start of its range, a FIRST
operand until its one replacement, and every BEFORE/AFTER range is found
before anything is replaced. The operations used to run one after another
over the whole field. IKFCBL00 has EXAMINE and no INSPECT, so `inspect3`
is checked against the standard's text. `OCCURS DEPENDING ON` in a file record is still
refused, so the variable-length `WRITE` in #38 cannot arise; the RDW code is
right for it when it is allowed.

## What this map is not

It is not a plan. Reading it, the honest conclusions are that Debug and
Communication should probably stay at null forever -- Sort-Merge was on that
list until a real program needed it; that Segmentation
is nearly free if it is ever wanted (see below); that `COPY` and declaratives
are the two absences most likely to be *pulled* by a real program; and that
the eleven-element gap to the minimum standard is worth closing mostly because
it is small enough to close.

### A note on Segmentation and W^X

Segmentation is described as specifying "object program overlay requirements,"
which sounds like it needs writable code. It does not. Every rule in Section
IX is written in terms of *state*: an independent segment is in its initial
state on first entry and under three enumerated conditions, and in its
last-used state otherwise; a fixed overlayable segment is "always made
available in its last used state"; and §2.4 leaves the mechanism entirely to
the implementor.

So a conforming implementation may keep everything resident and satisfy the
standard by re-initializing an independent segment's state at the qualifying
entries. In COBOL-74 the carrier of that state is `ALTER` — an altered `GO TO`
reverting to its original target is what "initial state" observably means for
a procedure. Without `ALTER`, an independent segment has no observable state,
and Segmentation reduces to accepting `SECTION` segment-numbers and honouring
the Level 1 rule that sections sharing a number be contiguous in the source.

This matters for a hypothetical second backend on a W^X target, where code
cannot be overwritten at all: the inability to overlay does not, by itself,
put Segmentation out of reach.
