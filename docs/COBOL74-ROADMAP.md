# COBOL-74: the remaining feature work

This is the plan for finishing the language. The conformance map
(`COBOL74-CONFORMANCE.md`) says where the compiler *is*; this says what is
left, in what order, and what is deliberately never going to be done. When the
work here is complete the feature side of cobc370 is closed, and what remains
is optimization.

Every item below was checked against the compiler on 2026-08-29 by compiling a
minimal program that uses it -- not taken from memory or from the map. The map
had been wrong twice before, both times in the direction of claiming more than
was there, and the check found a third instance (see Tier 0).

## Definition of done

**The high level of the three required modules, the low level of the modules
the compiler already carries, and COPY:**

    2 NUC 1,2   Nucleus, Level 2
    2 TBL 1,2   Table Handling, Level 2
    2 SEQ 1,2   Sequential I-O, Level 2
    1 REL 0,2   Relative I-O, Level 1
    1 INX 0,2   Indexed I-O, Level 1
    1 IPC 0,2   Inter-Program Communication, Level 1  (Level 2 is cheap; take it)
    1 SEG 0,2   Segmentation, Level 1                  (already there)
    1 LIB 0,2   Library, Level 1 -- COPY               (Level 2, REPLACING, is small)

That is a real, nameable conformance level, well above the minimum standard,
and every module in it is one the compiler already has most of.

**Deliberately out of scope**, each with a reason:

- **Communication** (`COM`). The teleprocessing module -- message control
  systems, queues, terminals. Nothing on MVS 3.8j to bind it to, and no
  implementation of the era shipped it as more than a stub.
- **Debug** (`DEB`). `WITH DEBUGGING MODE` and `USE FOR DEBUGGING`. A null
  level is conforming, the program-check exit already reports the source line,
  and nothing pulls for it.
- **Sort-Merge** (`SRT`). On MVS this means driving the system sort through
  E15/E35 exits from `RELEASE`/`RETURN`. Real work, and the corpus sorts in
  JCL steps instead. Null level is conforming. Revisit only if something asks.
- **`COMP` past nine digits.** A doubleword binary field on a machine with no
  64-bit arithmetic. Everything else is computed in packed decimal to 18
  digits; this would add a multi-precision path used by nothing but the
  representation tests. Stays refused, with the message it has.
- **Report Writer completion** is *not* out of scope, but it is a separate
  decision (below), because it is the one module where what remains is larger
  than what exists.

## The work, in order

Ordered so that each tier is a claim the project can make when it closes, and
so that work sharing the same machinery lands together. Sizes are honest
guesses: **S** is an afternoon, **M** is a day, **L** is several.

### Tier 0 -- Nucleus Level 1, actually (3 items) -- DONE 2026-08-29

The map says the Nucleus is complete at Level 1. It is not. Three Level 1
elements are refused, found by reading the standard's own `1 NUC 1,2` list
against the compiler rather than trusting the map:

| element | size | note |
|---|---|---|
| `GO TO ... DEPENDING ON` | S | a branch table; `GO TO` itself exists |
| `CURRENCY SIGN IS literal` | S | one more symbol in `picture.rl`, substituted for `$` |
| `DECIMAL-POINT IS COMMA` | S | swaps `.` and `,` in pictures and numeric literals; scanner and picture both |

The minimum standard is not reached until these are in. Do them first.

### Tier 1 -- Nucleus Level 2 (about 20 items) -- DONE 2026-08-29

Grouped by the machinery they touch, so each group is one piece of work.

**Scanner and reference format** -- DONE 2026-08-29:
- separators: comma and semicolon -- already worked
- continuation of words and numeric literals
- figurative constants at Level 2: `ZEROS`/`ZEROES`, `HIGH-VALUES`, `LOW-VALUES`,
  `QUOTES`, `ALL literal` as a MOVE source and in conditions
- user-defined words need not begin with a letter -- already worked

Found on the way and **not yet done**: `DISPLAY` of a subscripted item is
refused. That is a Level 1 `DISPLAY identifier`, since an identifier may carry
a subscript; it belongs with the `ACCEPT`/`DISPLAY` item below. S.

**Conditions** -- DONE 2026-08-29:
- abbreviated combined relation conditions: `IF A = 1 OR 2 OR 3`,
  `IF A > 1 AND < 100`, `IF A = B OR C`
- comparison of nonnumeric operands of unequal size: `CLC` on the common
  length, then the longer one's tail against a run of spaces, on whichever
  side the longer one is; a literal longer than its item likewise
- level-88 `VALUE ... THRU ...` and `VALUE` series, numeric and nonnumeric,
  as a chain of hidden condition names expanded to an OR of ranges
- `NOT` on the sign condition -- already worked

**Arithmetic** -- DONE 2026-08-29:
- `**` exponentiation: a literal exponent is unrolled for any base, an
  identifier exponent runs a loop for an integer base, and literal ** literal
  is folded at parse time so `2 ** 3 ** 2` works; a negative or fractional
  exponent is refused, having no exact decimal value
- `ON SIZE ERROR` on every arithmetic statement: the magnitude is compared
  against 10^digits after rounding, the item is left alone on overflow, a
  flag lets a series run its imperative once, and a zero divisor is a size
  error under the phrase
- result series on `COMPUTE`, `ADD ... TO`, `SUBTRACT ... FROM`,
  `MULTIPLY ... BY`, `DIVIDE ... INTO`, and every `GIVING`
- `DIVIDE ... INTO` without `GIVING`; `DIVIDE ... REMAINDER`, with the
  quotient truncated as stored whatever `ROUNDED` said

Found and fixed on the way: every packed result too wide for its item had
been abending 0CA rather than truncating, because naming interruption codes
8 and 10 in the program-check `SPIE` turns their program-mask bits on. The
`SPIE` no longer names the maskable codes, and the mask is cleared at entry.

**`CORRESPONDING`** -- DONE 2026-08-29: `MOVE`, `ADD`, `SUBTRACT`, with
`ROUNDED` and `ON SIZE ERROR` on the arithmetic forms. Pure front end: items
correspond by name and qualification up to the two groups (II-51); `FILLER`,
`REDEFINES`, `OCCURS` and index items are left out with everything beneath
them; a pair of groups with the same name is not moved as a group but has its
subordinates matched, since at least one of a `MOVE` pair must be elementary.

**`STRING` and `UNSTRING`** -- DONE 2026-08-29: two runtime routines,
`COBSTR` and `COBUNS`, driven by a per-statement parameter block in the data
area. `DELIMITED BY SIZE` and by value, several items sharing one `DELIMITED
BY`, `WITH POINTER`, `ON OVERFLOW`; `DELIMITED BY ... OR ...`, `ALL`,
`DELIMITER IN`, `COUNT IN`, `TALLYING IN`, no `DELIMITED BY` at all. The
routines work in bytes and know nothing about pictures: `POINTER`, `TALLYING`
and `COUNT` go through fullword cells the caller converts. Not done:
numeric receivers on `UNSTRING` (refused with a message), and `ALL literal`
as an operand.

The program-check message now carries the interrupt offset from `COBBEG` in
hex, because a fault in the runtime maps to the last source line and says
nothing; the offset is what the assembler listing is indexed by.

**`INSPECT` with multi-character operands** -- DONE 2026-08-29, together
with `BEFORE`/`AFTER INITIAL`, which the element list does not level
separately and which had been refused as level 2, and with the series form
(`REPLACING ALL 'A' BY 'B' 'C' BY 'D'`). The scan steps past a matched string
and one byte past a miss; a bounding string is found first and the range
narrowed to before or after it.

**`PERFORM VARYING ... AFTER`** -- DONE 2026-08-29, to two `AFTER` levels.
One thing to know: II-83 resets an inner identifier from the *current* value
of its FROM and augments the outer one afterwards, so `AFTER J FROM I` starts
`J` at the old `I`. COBOL-85 reversed that, GnuCOBOL follows 85, and the
test's oracle was corrected by hand from the rule's text.

**Level-66 `RENAMES`** -- DONE 2026-08-29: an alias over a range of the
record, elementary with d1's description when it renames one elementary
item, a group otherwise; the `REDEFINES` emitter already names an alias.

**`GO TO` without a procedure-name** -- DONE 2026-08-29: the branch cell
starts out zero, so a bare `GO TO` executed before any `ALTER` takes a
program check naming its own line; anywhere but a paragraph an `ALTER` names
it is refused.

**`ACCEPT ... FROM`** -- DONE 2026-08-29: `DATE` (YYMMDD), `DAY` (YYDDD),
`TIME` (HHMMSSth) through `COBADT`, which borrows `COBDATE`'s month walk and
writes zoned digits for an ordinary MOVE into the item; a mnemonic-name for
`SYSIN`; and `CONSOLE` as a `WTOR` with a wait on its ECB (built, not in the
regression suite -- a batch test cannot answer an operator prompt).
**`DISPLAY ... UPON`** -- DONE: a mnemonic for `SYSOUT`, and `CONSOLE` as a
`WTO`, which lands in the job log with a `+`.

**`ACCEPT`/`DISPLAY` with no restriction on the number of transfers** --
DONE 2026-08-29. The level 1 restriction is one transfer *of data*, not one
operand: a `DISPLAY` longer than the 120-character line now continues on the
next line, cut wherever the line ends, and an `ACCEPT` wider than a card is
filled from as many cards as it takes. `DISPLAY` of a subscripted item came
with it, and `VALUE ALL literal` because the test used one.

### Tier 2 -- Table Handling Level 2 (3 items) -- DONE 2026-08-29

| element | size | note |
|---|---|---|
| serial `SEARCH`, with `VARYING`, `AT END`, `WHEN` series | DONE 2026-08-29 | `VARYING` an integer item counts the steps, as III-9 says; GnuCOBOL sets it to the index instead, and the oracle was corrected by hand |
| `OCCURS ... ASCENDING/DESCENDING KEY` series | DONE 2026-08-29 | keys ranked in the order written; `SEARCH ALL` compares them lexicographically, inverting the bound step for a `DESCENDING` one, and its `WHEN` may be a conjunction over the keys in order |
| `OCCURS integer-1 TO integer-2 DEPENDING ON data-name` | DONE 2026-08-29, in `WORKING-STORAGE` and `LINKAGE` | every group containing the table has a run-time length -- the fixed part plus count times element -- and a `MOVE` of one goes through `COBMVL`, a runtime move of any length that space fills. III-3 rule 4 uses the current count on either side of a `MOVE` (COBOL-85 and GnuCOBOL use the maximum for a receiver; the oracle was corrected from the rule). Refused, each with a message: the table in an `FD` record (variable-length records are a separate feature), and a comparison of such a group. `COBMVL` also lifts the old 256-byte limit on alphanumeric moves |

### Tier 3 -- Sequential I-O Level 2 (about 10 items) -- DONE 2026-08-29

All through `COBADV`, which already took its line count at run time, and one
self-oracle test that reads its own print file back with the control byte as
data.

| element | how |
|---|---|
| `WRITE ... ADVANCING identifier LINES` | the identifier is loaded and passed as the count |
| `WRITE ... ADVANCING mnemonic-name` | `C01`..`C12` and `CSP` become ASA codes `1`..`9`,`A`..`C` and `+`; a channel skip behaves as `PAGE` does toward what a `BEFORE` left owing |
| `LINAGE` with `FOOTING`, `LINES AT TOP`, `LINES AT BOTTOM`; `LINAGE-COUNTER`; `WRITE ... AT END-OF-PAGE` | the runtime keeps the counter (a hidden `COMP` halfword, `LINAGE-COUNTER` by name); a count past the body, or any skip, ejects and lays the top margin; `END-OF-PAGE` is the counter reaching `FOOTING`, or a new page when there is no `FOOTING`. Integers only in the clause; `LINAGE-COUNTER OF file` is not implemented, so the name reaches the first `LINAGE` file |
| `OPEN EXTEND` on a sequential file | the OPEN macro's `EXTEND`, on the output DCB |
| `OPEN INPUT ... REVERSED` | `RDBACK` -- accepted and generated, untested (tape only) |
| `SELECT OPTIONAL` | `DEVTYPE` on the ddname before `OPEN`; absent means `READ` takes `AT END` at once and `CLOSE` does nothing |
| `CLOSE ... WITH NO REWIND`, `REEL`/`UNIT` | `CLOSE LEAVE` and `FEOV` -- generated, untested (tape only); `LOCK` and `FOR REMOVAL` close as usual |
| `RESERVE integer AREAS` | `BUFNO` on the DCB |
| `USE ... ON EXTEND` | the declarative is now dispatched for `EXTEND` as for the other modes |
| `BLOCK CONTAINS m TO n`, `VALUE OF`, `SAME RECORD AREA`, `MULTIPLE FILE TAPE` | accepted, as before |

Found on the way: the QSAM `PUT` macro clobbers R0, R1, R14 and R15, which
the first version of the `LINAGE` code learned twice -- a margin count kept in
R14 across the eject, and the `END-OF-PAGE` answer kept in R15 across the
line itself.

### Tier 4 -- Relative and Indexed I-O, Level 1 closure -- DONE 2026-08-29

`USE AFTER STANDARD ERROR PROCEDURE ON file-name`, and by mode, for relative
and indexed files, VSAM and ISAM. The rule (V-30, VI-32): a phrase on the
statement takes the error it is for; a `USE` procedure takes what no phrase
does. So every I/O statement now records whether it carried `AT END` or
`INVALID KEY`, and the VSAM status routine's failure path -- after the FILE
STATUS is set -- performs the applicable declarative when the statement had
none. The two ISAM error labels do the same. `OPEN` and `CLOSE`, which have
no phrase to carry, always route a failure to the `USE`.

Relative I-O and Indexed I-O are both complete at level 1 with this; both
already stood above it on `ACCESS DYNAMIC`, `READ NEXT` and `START`.

The "this SELECT clause is not implemented" bucket in the CCVS histogram was
not re-decomposed here; that is optimisation-phase housekeeping, since what is
left of it is COBOL-85 spellings and `ALTERNATE RECORD KEY`.

### Tier 5 -- Inter-Program Communication, both levels -- DONE 2026-08-29

| element | how |
|---|---|
| `EXIT PROGRAM` | the subprogram return; in a main program, no effect. A called program is known by its `PROCEDURE DIVISION USING`, so one without parameters gets the no-op and `GOBACK`, which always returns, is its way out |
| `CALL identifier` | the name goes into an 8-byte field and `COBDCAL` loads the program by name -- once, into a table of what is loaded -- and calls it with R1 -> the parameter list. The source comment saying ANS COBOL has no `CALL identifier` was true of IBM's compiler; `2 IPC 0,2` has it |
| `CANCEL` | `COBCANC` `DELETE`s a program in the table and forgets it, so the next `CALL` loads it afresh in its initial state, as XII-7 says; a program not loaded is left alone |

Proved by `bin/cobc-dyncall-roundtrip`: the callee is a load module of its own
in a temporary library the caller reaches through `STEPLIB`, and it counts its
calls in `WORKING-STORAGE` so a `CANCEL` shows as the count starting over.

### Tier 6 -- Library: `COPY`, host side -- DONE 2026-08-29

Level 1 is `COPY text-name [OF/IN library-name]`; level 2 adds `REPLACING
operand BY operand` with pseudo-text. Both are in, and both are host side by
nature: the scanner keeps a stack of open sources, a `COPY` statement pushes
the copybook and the parser's next token is the copybook's first, and the end
of the copybook pops back. The copybook is found on the `-I` directories,
then beside the program, as the name given and with the usual suffixes
(`.cpy`, `.cob`, ...); `OF library` is a subdirectory. `REPLACING` is applied
to each copybook line as it is read: pseudo-text replaces wherever its
characters occur, blanks collapsed on both sides, so the `:TAG:` idiom works;
a word or literal operand replaces whole words only. Nothing reaches the
guest but the copied text.

### Report Writer -- a separate decision

`1 RPW 0,1` is a single level, so the module is either complete or not
claimed. What exists is the page-manager subset: `RD` with `PAGE LIMIT`,
`HEADING`, `FIRST DETAIL`, `LAST DETAIL`; `PAGE HEADING` and `DETAIL` groups;
`LINE`, `COLUMN`, `SOURCE`, `VALUE`; `GENERATE`/`INITIATE`/`TERMINATE`. What
is missing is the part that computes: `CONTROL` clauses, the five other `TYPE`s
(`REPORT HEADING`, `CONTROL HEADING`, `CONTROL FOOTING`, `PAGE FOOTING`,
`REPORT FOOTING`), `SUM` with sum counters, `UPON` and `RESET`, control breaks
with subtotalling and rolling forward, `NEXT GROUP`, `GROUP INDICATE`,
`SUPPRESS`, `USE BEFORE REPORTING`, `LINE-COUNTER`/`PAGE-COUNTER` as
referenceable registers. **L, and then some** -- the control-break machinery
is a small language of its own.

Not required for the definition of done above. Worth doing if reports are the
point of the compiler, which for this project they are; worth skipping if the
goal is to close the language and move on. Left open when the feature work
closed on 2026-08-29; taken up on 2026-08-30 -- see "Report Writer: closing
the module" at the end of this file.

### `ALTERNATE RECORD KEY` -- also separate

`2 INX 0,2`. On MVS it is VSAM alternate indexes and paths -- an IDCAMS
`DEFINE AIX`/`DEFINE PATH`/`BLDINDEX` job and a second ACB open on the path --
so it is infrastructure first and a compiler change second. Indexed I-O Level
2 is not in the definition of done, so this is optional. **L.**

## Counting it

Tiers 0 through 6 come to roughly **40 elements**, of which about 30 are S, 8
are M and one -- `OCCURS DEPENDING ON` -- is L. Most of the S items cluster
into shared machinery (scanner, conditions, arithmetic) and land as a handful
of slices rather than 30. Call it **fifteen to twenty working sessions** at
the pace of the last three days, before optimization starts.

## Rules that stay in force

- **One slice at a time, on instruction.** The tiers are an order, not a
  licence to run ahead.
- **Verify against the standard, then against the compiler.** Every closure
  claim in this plan is to be made by walking the element list and compiling
  one program per element. The map was wrong three times by trusting memory;
  the plan is not allowed to be wrong the same way.
- **COBOL-85 stays refused.** A CCVS failure that is a correct refusal of a
  COBOL-85 spelling is a pass. Nothing here widens the language past 1974.
- **The oracle is GnuCOBOL, and GnuCOBOL is not an authority.** Disagreements
  are narrowed, not settled by deference.

## Optimization

Begun 2026-08-29, once the language work above was done. The instrument is
`bench/run.sh`: nine constructs a million times each, under both compilers,
CPU from the step accounting. The production batch is the safety net, not the
score -- it spends its time in I/O, SORT and `COMPUTE`, where the two
compilers are level, and its 13% gap hid primitives that were two to seven
times behind.

| construct (x1,000,000) | cobc370 at start | IKFCBL00 | after |
|---|---|---|---|
| packed `ADD` | 0.18 | 0.09 | **0.09** (1) |
| packed compare | 0.17 | 0.07 | **0.09** (1) |
| `CALL` a subprogram | 4.76 | 0.16 | **0.27** (2) |
| `COMP` `ADD 1` | 0.20 | 0.03 | **0.02** (4) |
| `MOVE` same picture, DISPLAY | 0.05 | 0.01 | **0.01** (3) |
| `MOVE` to a wider picture | 0.06 | 0.02 | **0.02** (3) |
| `IF` same picture, unsigned DISPLAY | 0.02 | 0.10 | already ahead |
| `COMPUTE` with `*` and `+` | 0.49 | 0.53 | **0.39** (5) |

1. Same-scale packed `ADD`/`SUBTRACT`/compare in place: `AP`, `SP`, `CP` on
   the fields themselves, literals from the tail of their constants.
2. `SPIE` armed once per load module. The first attempt skipped it with
   `BE *+14`, which lands inside the macro's expansion and skipped nothing;
   the benchmark said 4.59 and the label fix said 0.27. Cost: after a callee
   has run, a program check in the caller is reported against the callee's
   line table; the offset stays true.
3. Unsigned zoned to unsigned zoned at one scale is bytes: one `MVC` for the
   same picture, zeros then digits into a wider receiver, the low-order
   digits into a narrower one. A numeric literal into such an item is one
   `MVC` from a zoned constant. Signed items and `SIGN` clauses keep the
   `PACK`/`UNPK` path, where the sign nibble has to be made.

4. `COMP` `ADD`/`SUBTRACT` at one scale in binary: `L`, `A` (or `AH`), `ST`
   on the field, a literal from a halfword or fullword constant. The range
   guard planned for this turned out to guard nothing: the decimal path never
   truncated a `COMP` result to its picture either -- `CVB` stores whatever
   the register holds -- so the binary add is bit-identical for every value
   that fits 31 bits, and past that the standard calls the result undefined
   (`CVB` took a fixed-point-divide exception there; the add wraps). A source
   at another scale, or in `DISPLAY` or `COMP-3`, keeps the decimal path.
   First cut skipped the source-scale check and added a `V9` item to a `V99`
   one as hundredths; `tests/compadd` caught it before the sweep did.

5. Expression work areas sized to the value. A shape pass bounds the digits
   at every node with the rules the generator uses (a sum one more than its
   wider operand, a product the digits of both, a quotient the shifted
   dividend's), and each operation runs on just that tail of its 16-byte
   area. A caller that will shift the result left asks for the longer tail
   up front, so growth never copies. A `COMP-3` item or a literal on the
   right of an operator is addressed in place -- `AP`, `SP`, `MP` and `DP`
   all take a second operand in storage -- and `COMPUTE` stores from the
   tail instead of copying through `PWK1`. The benchmark's seven-digit
   `P1 * P2 + P1` went from twelve 16-byte instructions (an `MP` of 16 by
   8 among them) to seven on 5- and 8-byte tails. Found on the way: an
   item over 16 digits, or with a `SIGN` clause, in an expression was
   loaded by a helper without the wide and sign paths, and assembled a
   `PACK` with an 18-byte operand; `tests/exprsize` covers both now.

The primitives on the list are all at or ahead of IKFCBL00. What is left
is the shape of whole programs -- base-register traffic, the SPIE and
DCB work at entry -- and nothing there has been measured yet.

## Report Writer: closing the module

Taken up 2026-08-30. The target is `1 RPW 0,1` entire -- the module has one
level, so it is claimed whole or not at all. The element list (FIPS 21-1,
"List of Elements by Module") was read against the compiler; chapter VIII was
read in full, the five presentation-rules tables included. What follows is
what the standard requires and what exists, in the order the machinery
suggests.

### What exists, against the element list

Present: `REPORT IS` on the FD; `RD` with `PAGE LIMIT`, `HEADING`, `FIRST
DETAIL`, `LAST DETAIL`; `TYPE PAGE HEADING` and `TYPE DETAIL`; `LINE` absolute
and `PLUS`; `COLUMN`; `SOURCE` (unsubscripted); `VALUE`; `INITIATE`,
`GENERATE data-name`, `TERMINATE` (one report each).

The page manager underneath is not the standard's. It keeps a line counter,
an eject flag and a "first group on a fresh page" cell, tests every group
against `LAST DETAIL` whatever its type, has no `PAGE-COUNTER`, and knows
nothing of `FOOTING`, `NEXT GROUP`, `NEXT PAGE` or the saved next group
integer. It reproduces the corpus's six report programs, which use none of
that. It cannot carry control breaks, because the presentation rules for a
`CONTROL FOOTING` depend on state it does not keep.

Missing, from the element list: `LINE-COUNTER` and `PAGE-COUNTER` as special
registers; `CONTROL` with `FINAL`; `PAGE ... FOOTING`; `CODE`; the data-name
clause on report entries (sum-counter names); `GROUP INDICATE`; `LINE ...
NEXT PAGE`; `NEXT GROUP` in its three forms; `REPORTS ARE` (several per FD);
`SUM ... UPON ... RESET`; `TYPE` `RH`, `CH`, `CF`, `PF`, `RF`; `GENERATE
report-name`; `INITIATE`/`TERMINATE` series; `SUPPRESS`; `USE BEFORE
REPORTING`.

### The shape of the work

Everything hangs off two things the standard defines and the current code
lacks: a per-report state block (`LINE-COUNTER`, `PAGE-COUNTER`, the saved
next group integer, whether a body group has been presented on this page,
whether the first `GENERATE` has happened, the prior control values, the
level of the current break, the suppress flag) and per-group presentation
code generated from the tables. The tables are indexed by things known at
compile time -- the group's `TYPE`, the shape of its `LINE` sequence (`A`,
`R`, `AR`, `NP R`), the form of its `NEXT GROUP` -- so each group's renderer
can carry exactly the rules its row names, and the runtime state stays a
few halfwords. Control-break sequencing and sum-counter arithmetic are
generated per report from the `CONTROL` hierarchy.

### The slices, in order

1. **The page engine to the standard's tables.** The state block;
   `LINE-COUNTER` and `PAGE-COUNTER` as registers, usable in `SOURCE` and in
   the Procedure Division (`PAGE-COUNTER` writable); the `PAGE` clause with
   `FOOTING` and the implicit values of 2.16.4(2); Table 2 for `PAGE
   HEADING`; Table 3 fit tests 3a/3b, first-line rules 4a/4b and final
   setting 6d for `DETAIL`; page-advance processing as one routine per
   report. Retires the forced-first-detail cell. `SOURCE` with a subscript.
   The existing report tests must print the same. **M.**
2. **The rest of the page: `PAGE FOOTING`, `REPORT HEADING`, `REPORT
   FOOTING`, `NEXT GROUP`, `LINE NEXT PAGE`.** Tables 1, 4 and 5; the three
   `NEXT GROUP` forms with the saved next group integer and final-setting
   rules 6a-6c/6f; `RH` on the first `GENERATE` (on a page by itself under
   `NEXT GROUP NEXT PAGE`), `RF` from `TERMINATE`. **M.**
3. **`CONTROL`, `CH`/`CF` groups, control breaks.** `CONTROL IS ... FINAL`;
   prior values saved on the first `GENERATE`; break detection by the
   relation-condition rules per category (2.10.4(3)); the `GENERATE`
   sequence of 3.1.4(5)/(6) -- `CF` minor to major up to the break level,
   `CH` major to minor, then the detail -- and `TERMINATE` as a break in the
   most major control followed by `RF`; prior values supplied to `CF`
   `SOURCE` clauses (2.21.4(13)); `NEXT GROUP` ignored on a `CF` below the
   break level (2.15.4(3)). **L** -- this is the small language of its own.
4. **`SUM`.** Sum counters as named numeric items, zeroed by `INITIATE`;
   subtotalling on `GENERATE` with `UPON` selectivity; crossfooting and
   rolling forward at `CF` processing in the order of 2.21.4(10) and
   2.20.4(8); `RESET ON`; the counter as `SOURCE` and as an ordinary item in
   the Procedure Division; `GENERATE report-name` and summary reports
   (2.21.4(11)). **M.**
5. **The rest of the list.** `USE BEFORE REPORTING` as a declarative with
   `SUPPRESS`; `GROUP INDICATE` (first after `INITIATE`, page advance or
   break); `JUSTIFIED` and `BLANK WHEN ZERO` on printable items; `CODE`;
   `REPORTS ARE` with several reports on one file; `INITIATE`/`TERMINATE`
   series; `VALUE OF` accepted. **S-M.**

Then the conformance map says `1 RPW 0,1`, and the README's "the module is
not claimed" comes out.

### Oracles

GnuCOBOL 3 carries a full Report Writer and is the differential oracle for
every slice. IKFCBL00 has one too, of the 1968 standard's shape, and where the
two disagree the 1974 text decides -- the same rule as everywhere else in this
project. The corpus's six report programs stay the regression floor: nothing
here may change a line they print.
