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
Library; Indexed I-O at Level 2 but for `ALTERNATE RECORD KEY`; Segmentation
at Level 1; the Report Writer's page-manager subset without the module claim;
and the null level of Sort-Merge, Debug and Communication.** Each module's
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
walking the element list.

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
added on 2026-08-29. `REVERSED` and `CLOSE REEL`/`NO REWIND` are generated
and untested, being tape-only.

### Relative I-O — Level 2, complete

RRDS through VSAM. `ORGANIZATION RELATIVE`, `ACCESS SEQUENTIAL/RANDOM`,
`READ WRITE REWRITE DELETE`, `INVALID KEY`, `RELATIVE KEY`.

`ACCESS DYNAMIC`, `READ NEXT` and `START` are Level 2 and were there first;
`USE` declaratives, the last Level 1 element, were added on 2026-08-29.

### Indexed I-O — Level 2 but for ALTERNATE RECORD KEY

ISAM and VSAM KSDS. `ORGANIZATION INDEXED`, `RECORD KEY`, `ACCESS
SEQUENTIAL/RANDOM`, `READ WRITE REWRITE DELETE START`, `INVALID KEY`.

Above the floor: `ACCESS DYNAMIC`, `READ NEXT`, `READ ... KEY IS`, `START` —
Level 2.

`USE` declaratives were added on 2026-08-29. Missing from Level 2:
`ALTERNATE RECORD KEY` with `DUPLICATES`, which on MVS is VSAM alternate
indexes and paths before it is a compiler change.

### Report Writer — a fraction of its only level

Being completed, slice by slice, since 2026-08-30 -- the plan is in
`COBOL74-ROADMAP.md` under "Report Writer: closing the module".

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
`INITIATE`, `TERMINATE`.

Missing, each refused with a message: `GROUP INDICATE`; `JUSTIFIED` and
`BLANK WHEN ZERO` on report items; `CODE`; `REPORTS ARE`; `SUPPRESS`; `USE
BEFORE REPORTING`; `INITIATE`/`TERMINATE` of several reports in one
statement.

`1 RPW 0,1` is a single level, so the module is not claimed until the list
above is empty.

### Inter-Program Communication — Level 2, complete

`LINKAGE SECTION`, `PROCEDURE DIVISION USING`, `CALL 'literal' USING` all
work, and the call round trip is a regression test.

`EXIT PROGRAM`, `CALL identifier` and `CANCEL` were added on 2026-08-29.
`GOBACK`, an IBM extension not in the standard, stays as well. The source
comment that once said ANS COBOL has no `CALL identifier` was true of IBM's
compiler and false of the standard -- `2 IPC 0,2` lists it -- and that is the
kind of thing the standard-shaped map exists to catch.

### Segmentation — Level 1

Segment-numbers on sections are accepted, and `ALTER` respects them. Level 2
adds `SEGMENT-LIMIT`, which is not.

### Library — Level 2, complete

`COPY text-name [OF library]` and `REPLACING` with pseudo-text, added on
2026-08-29, host side: the scanner stacks the copybook, found on the `-I`
directories or beside the program.

### Null — nothing implemented

`Sort-Merge`, `Debug`, `Communication`. All three have a null level, so all
three are conforming choices.

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
I-O at Level 2 less `ALTERNATE RECORD KEY`, Segmentation at Level 1 -- with
the Report Writer's page-manager subset besides. None of it is a validated
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
1. The DEFTLY corpus carries its vertical spacing as an ASA character inside
the record and never used the clause; the CCVS corpus uses it 380 times.

On S/370 the clause *is* ASA carriage control: a byte in front of the record
saying what to do **before** the line prints -- which is exactly what AFTER
means. `' '` is one line, `'0'` two, `'-'` three, `'+'` none, `'1'` a new page;
more than three lines is written as blank lines first, three at a time. A file
that any `WRITE ... ADVANCING` names becomes `RECFM=FBA` with `LRECL` one
greater than the record, and the line goes out through a per-file buffer whose
first byte is the control character. A plain `WRITE` on such a file gets one
line, which is what general rule 9 on IV-35 requires.

`BEFORE` is level 1 too -- the element list says `BEFORE/AFTER integer LINES`
and `BEFORE/AFTER PAGE` -- and it works now. It costs a runtime routine rather
than a few inline instructions, because the deferral is real state: the line
goes out with whatever the last `BEFORE` left owing, and its own count becomes
what the next line owes. Once the two can add up, the total is not known until
run time.

`COBADV` takes the request as a line count, 999 for `PAGE`, negated when the
phrase was `BEFORE`. It applies what is owed, writes the line, and stores what
the statement defers. `BEFORE 0` and `AFTER 0` encode the same way because they
*are* the same thing: apply what is owed and owe nothing.

Three things that had to be decided rather than looked up, since the standard
fixes none of them: a `BEFORE` with nothing owed prints on the next line rather
than overprinting, an owed page skip stays a page skip however many lines the
next statement asks for, and the blank lines an advance of more than three
needs come from the runtime's own constant -- the caller's buffer already holds
the record to print, which the first version of this cheerfully blanked.

`ADVANCING` by an identifier or a mnemonic-name is level 2 and is refused as
such.

**GnuCOBOL cannot be the oracle for this one** -- it writes a text file with
newlines rather than control bytes. `tests/advance.cbl` is therefore its own:
it writes the file, closes it, reads it back through a second FD with the
control byte as ordinary data, and displays what it finds. The expected values
come from the standard and IBM's ASA encoding, not from a second compiler.

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

## What this map is not

It is not a plan. Reading it, the honest conclusions are that Sort-Merge,
Debug and Communication should probably stay at null forever; that Segmentation
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
