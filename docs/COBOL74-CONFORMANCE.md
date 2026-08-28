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

The short version: **the compiler does not sit at a level.** It cuts a
diagonal across the map, because it was built by demand. It implements several
Level 2 elements while missing Level 1 elements of the same module. That is
not a defect — it is what "pulled, not pushed" produces — but it does mean no
conformance claim is currently available, not even the minimum standard.

### Nucleus — below Level 1

Present, and enough to compile the corpus: `ADD SUBTRACT MULTIPLY DIVIDE
COMPUTE MOVE IF GO PERFORM STOP EXIT DISPLAY`, PICTURE with editing,
REDEFINES, SIGN, SYNCHRONIZED, USAGE COMP/COMP-3/DISPLAY, VALUE, level 77 and
88, relation and sign conditions, `AND`/`OR`/`NOT`.

Missing from **Level 1**:

    ACCEPT              never used by the corpus
    ALTER               never used by the corpus
    ENTER               never used by the corpus
    INSPECT             never used by the corpus
    class condition     IS NUMERIC / IS ALPHABETIC -- dies with a message
    switch-status       SPECIAL-NAMES ... ON/OFF STATUS

Six elements. The class condition is the only one that feels like an omission
rather than a decision: `IS NUMERIC` is the natural way a program guards
itself against the S0C7 that `pgmchk` exists to report.

Already at **Level 2**, above a floor not yet reached: `COMPUTE` (Level 2, not
1), qualification with `OF`/`IN`, level-88 condition-names, `PERFORM UNTIL`,
`PERFORM VARYING`, nested `IF`, complex conditions, the full `01`-`49`
level-number range.

Missing from Level 2, beyond the Level 1 gaps: `STRING`, `UNSTRING`,
`CORRESPONDING` on ADD/SUBTRACT/MOVE, level-66 `RENAMES` (which dies with a
message), `PERFORM VARYING ... AFTER`.

### Table Handling — below Level 1, with Level 2 features

`OCCURS`, `INDEXED BY`, and three-level subscripting are there. `SEARCH` and
`SEARCH ALL` are there — and both are **Level 2** elements.

Missing from Level 1: the `SET` statement, and `USAGE IS INDEX`. This is the
sharpest illustration of the diagonal: the module's Level 2 search facility
works, while the Level 1 statement for moving an index does not exist.

### Sequential I-O — Level 1 but for declaratives

`SELECT`/`ASSIGN`/`ORGANIZATION`/`ACCESS`/`FILE STATUS`, FD with its clauses
accepted, `OPEN CLOSE READ WRITE REWRITE`, `READ INTO`, `WRITE FROM`, `AT END`.

Missing from Level 1: the `USE` statement — that is, declaratives — and
`WRITE ... BEFORE/AFTER ADVANCING`. Vertical spacing is carried in the record
as an ASA control character instead, which is what the corpus and the Report
Writer path both do, but it is not the standard's spelling.

Level 2 adds `LINAGE`, `OPTIONAL`, `RESERVE`, `SAME RECORD AREA`, `EXTEND`,
`MULTIPLE FILE TAPE` — none present.

### Relative I-O — most of Level 1, two Level 2 features, no declaratives

RRDS through VSAM. `ORGANIZATION RELATIVE`, `ACCESS SEQUENTIAL/RANDOM`,
`READ WRITE REWRITE DELETE`, `INVALID KEY`, `RELATIVE KEY`.

Above the floor: `ACCESS DYNAMIC`, `READ NEXT` and `START` are all **Level 2**.

Missing from Level 1: `USE`.

### Indexed I-O — same shape

ISAM and VSAM KSDS. `ORGANIZATION INDEXED`, `RECORD KEY`, `ACCESS
SEQUENTIAL/RANDOM`, `READ WRITE REWRITE DELETE START`, `INVALID KEY`.

Above the floor: `ACCESS DYNAMIC`, `READ NEXT`, `READ ... KEY IS`, `START` —
Level 2.

Missing from Level 1: `USE`. Missing from Level 2: `ALTERNATE RECORD KEY` with
`DUPLICATES`.

### Report Writer — a fraction of its only level

Present: `REPORT IS` on the FD, `RD` with `PAGE LIMIT`, `HEADING`, `FIRST
DETAIL`, `LAST DETAIL`, `CONTROL`; report groups of `TYPE DETAIL` and `TYPE
PAGE HEADING`; `LINE NUMBER` absolute and `PLUS`; `COLUMN NUMBER`; `SOURCE`;
`VALUE`; `GENERATE`, `INITIATE`, `TERMINATE`.

Missing: `TYPE` for `REPORT HEADING`, `CONTROL HEADING`, `CONTROL FOOTING`,
`PAGE FOOTING`, `REPORT FOOTING`; the `SUM` clause and sum counters, with
subtotalling, crossfooting and rolling forward; `NEXT GROUP`; `GROUP
INDICATE`; `CODE`; `SUPPRESS`; `USE BEFORE REPORTING`; `LINE-COUNTER` and
`PAGE-COUNTER` as referenceable special registers.

The absent half is the half that computes. What exists is a page manager.

### Inter-Program Communication — Level 1 but for EXIT PROGRAM

`LINKAGE SECTION`, `PROCEDURE DIVISION USING`, `CALL 'literal' USING` all
work, and the call round trip is a regression test.

Missing from Level 1: `EXIT PROGRAM`, which dies with a message. Subprograms
return via `GOBACK`, which is an IBM extension and **not in the 1974 standard
at all**.

Missing from Level 2: `CALL identifier` and `CANCEL`. Worth flagging, because
the source comment at the `CALL` site says ANS COBOL has no `CALL identifier`.
That is true of IBM's ANS COBOL, and false of the standard: `2 IPC 0,2` lists
`The CALL statement ... identifier` explicitly. A demand-shaped map could not
have caught that; this is the kind of thing the standard-shaped one is for.

### Null — nothing implemented

`Sort-Merge`, `Segmentation`, `Library` (no `COPY`), `Debug`, `Communication`.

All five have a null level, so all five are conforming choices. `COPY` is the
only one with an obvious pull behind it.

## What would reach the minimum standard

The minimum standard is `1 NUC` + `1 TBL` + `1 SEQ`. Against today:

    Nucleus         ACCEPT, ALTER, ENTER, INSPECT, class conditions,
                    switch-status conditions
    Table Handling  SET, USAGE IS INDEX
    Sequential I-O  USE (declaratives), WRITE ... ADVANCING

Eleven elements. Several are small; `INSPECT` and declaratives are not. That
is the whole distance to a claim the project can actually make, and it is
worth knowing that it is eleven and not a hundred.

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

**4. Ordinary remaining work, all small.** `ACCEPT`, `ALTER`, `ENTER`,
`INSPECT` and operand series on `ADD`/`SUBTRACT` -- the last five elements
between this compiler and Nucleus level 1 -- plus continuation of a word or a
numeric literal, and a `SIGN` clause on an item of more than sixteen digits, a
limitation this compiler introduced itself when the zoned conversion was split.

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

`BEFORE ADVANCING` is refused, and the message says why: ASA acts before the
line prints, so BEFORE needs the advance held over to the next `WRITE`, which
means carrying state per file. Six of the 248 corpus programs that use
`ADVANCING` need it. `ADVANCING` by an identifier or a mnemonic-name is level 2
and is refused as such.

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
