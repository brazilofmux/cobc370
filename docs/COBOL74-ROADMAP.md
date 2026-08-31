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

*(Written 2026-08-29, when this was the state of things; the decision was
taken the next day and the module is complete -- see "Report Writer:
closing the module" at the end of this file.)*

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

*Taken up 2026-08-30, after the Report Writer; see "ALTERNATE RECORD KEY:
what this VSAM allows" at the end of this file.*

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
   The existing report tests must print the same. **M.** -- DONE 2026-08-30.
   `tests/rptpage` is the check, and its oracle is the standard's rather than
   GnuCOBOL's in two places: GnuCOBOL's fit test looks at a body group's first
   `LINE` only and so split a two-line detail across pages, where rule 3b
   sums every `LINE` integer and 2.16.3(9) says a group is never split; and
   GnuCOBOL ignores a `MOVE` to `PAGE-COUNTER`, which 1.2.2 allows. `COBWRL`
   learned to put an eject on a blank line when the target is not line 1,
   so a `HEADING` above 1 lands where it says instead of the eject's blanks
   printing on the page before. The counters are fullword `COMP` items under
   a hidden group named for the report, so `PAGE-COUNTER OF report-name`
   resolves like any other qualified name.
2. **The rest of the page: `PAGE FOOTING`, `REPORT HEADING`, `REPORT
   FOOTING`, `NEXT GROUP`, `LINE NEXT PAGE`.** Tables 1, 4 and 5; the three
   `NEXT GROUP` forms with the saved next group integer and final-setting
   rules 6a-6c/6f; `RH` on the first `GENERATE` (on a page by itself under
   `NEXT GROUP NEXT PAGE`), `RF` from `TERMINATE`. **M.** -- DONE 2026-08-30.
   The entry parser became clause-driven on the way (VIII 2.5.3(2): any
   order), so `LINE` and `NEXT GROUP` on the `01` and `LINE` with `COLUMN` in
   one entry both work. `tests/rptfoot` and `tests/rptnext` are the checks,
   with `LINE-COUNTER` displayed after each phase so the oracle pins the
   final-setting rules and not just the text. Both oracles are the standard's:
   GnuCOBOL 3's Report Writer ignores `NEXT GROUP PLUS` on a detail (no
   double spacing), puts the page heading one line early after a `REPORT
   HEADING` with `NEXT GROUP PLUS 1`, does not start a page for `LINE n NEXT
   PAGE` when a body group is already on one (rule 3c), skips a page after a
   `REPORT HEADING` that had one to itself, and prints a `PAGE HEADING` on
   the `REPORT FOOTING`'s own page (2.21.4(3)a says not). IKFCBL00, the
   second oracle, agrees with the text on every `LINE-COUNTER` value and on
   the spacing and the saved next-group integer in `rptfoot`; it places the
   heading of later pages one line lower (1968 rules) and cannot compile
   `LINE n NEXT PAGE`. Found on the way: `COBWRL` had used `LINE-COUNTER` as
   the paper position, and the two diverge the moment `NEXT GROUP` moves the
   register without moving paper -- each report now carries a physical-line
   cell that only `COBWRL` maintains.
3. **`CONTROL`, `CH`/`CF` groups, control breaks.** `CONTROL IS ... FINAL`;
   prior values saved on the first `GENERATE`; break detection by the
   relation-condition rules per category (2.10.4(3)); the `GENERATE`
   sequence of 3.1.4(5)/(6) -- `CF` minor to major up to the break level,
   `CH` major to minor, then the detail -- and `TERMINATE` as a break in the
   most major control followed by `RF`; prior values supplied to `CF`
   `SOURCE` clauses (2.21.4(13)); `NEXT GROUP` ignored on a `CF` below the
   break level (2.15.4(3)). **L** -- this is the small language of its own.
   -- DONE 2026-08-30. Each control data item gets a hidden clone of its
   own description; the clones are the values the next `GENERATE` senses
   against and the prior values a `CONTROL FOOTING` or `REPORT FOOTING`
   `SOURCE` reads. Sensing is major to minor by the item's category (`CP`
   for numeric, `CLC` otherwise) and stops at the first change, which is the
   highest level that changed; the report's break cell holds the level, the
   footings and headings each test it, and `TERMINATE` sets it to the most
   major level. `tests/rptctl` covers `CONTROL FINAL DEPT ACCT` with a
   heading and footing at each level, footings past `LAST DETAIL`, and a
   footing's `NEXT GROUP` applied at its own level and ignored below the
   break. GnuCOBOL 3 produced doubled headings and could not serve as an
   oracle at all; the oracle was derived from the text by hand, and IKFCBL00
   then produced the identical sequence with identical `LINE-COUNTER`
   values. One blank line in the captured output matched neither derivation
   nor either compiler's arithmetic -- stamps on every line proved `COBWRL`
   had written none -- and turned out to be the capture converting the
   printer's `CR LF` into two line breaks; fixed in `tk5-run`, in the
   operator repository.
4. **`SUM`.** Sum counters as named numeric items, zeroed by `INITIATE`;
   subtotalling on `GENERATE` with `UPON` selectivity; crossfooting and
   rolling forward at `CF` processing in the order of 2.21.4(10) and
   2.20.4(8); `RESET ON`; the counter as `SOURCE` and as an ordinary item in
   the Procedure Division; `GENERATE report-name` and summary reports
   (2.21.4(11)). **M.** -- DONE 2026-08-30. A counter is a signed packed
   item sized by its entry's `PICTURE`, under the footing's name when the
   entry has one (so `TOTAL OF DEPT-FOOT` qualifies) or the report's.
   Operands and `UPON` names may refer to entries later in the section, so
   they are held as text and resolved when the section ends; a `SOURCE` may
   do the same, since a detail line may show a running total. Subtotalling
   is emitted into each `GENERATE` after the break sequence and before the
   detail; crossfooting and rolling forward go before each footing's
   renderer in the footing sequence, resets after it, and a level with no
   footing group still gets its resets (2.21.4(10) note). `tests/rptsum`
   and `tests/rptsumm` are the checks. GnuCOBOL was not consulted --
   slice 3 showed its Report Writer unusable here. IKFCBL00 agreed on every
   subtotal, running total, `UPON` selection, `RESET ON`, roll into the
   next level and `DISPLAY` of a counter mid-report, and differed from the
   1974 text in three places, all 1968-shaped: a counter summing a counter
   of its own group (crossfooting) stayed zero; a counter summing one two
   levels down (rolling forward past a level) stayed zero; and `GENERATE
   report-name` with no `DETAIL` group subtotalled nothing, where
   2.21.4(11) says to proceed as though one `DETAIL` existed. The oracles
   follow the text in those three places.
5. **The rest of the list.** `USE BEFORE REPORTING` as a declarative with
   `SUPPRESS`; `GROUP INDICATE` (first after `INITIATE`, page advance or
   break); `JUSTIFIED` and `BLANK WHEN ZERO` on printable items; `CODE`;
   `REPORTS ARE` with several reports on one file; `INITIATE`/`TERMINATE`
   series; `VALUE OF` accepted. **S-M.** -- DONE 2026-08-30. The USE
   procedure is called from the group's renderer before anything else, by
   the same range mechanism the file declaratives use; `SUPPRESS` sets a
   cell the renderer tests on return and clears on exit, so a suppressed
   group moves no paper and leaves `LINE-COUNTER` alone while its
   footing's resets still run. `GROUP INDICATE` is a cell per `DETAIL`
   group, set by `INITIATE`, the eject and a break, cleared when the group
   presents. `CODE` widens the file's records to 135 and every line, blank
   ones included, carries it -- `COBWRL` now takes the report's own blank
   line from the parameter block. `tests/rptuse` and `tests/rptcode`.
   IKFCBL00 cannot compile `SUPPRESS` (1974) and takes `CODE` as a
   mnemonic-name, so both oracles are derived from the text. **Found on
   the way: `BLANK WHEN ZERO` was not implemented at all** -- not on
   report items and not in the Nucleus, where it is a Level 1 element
   (II-14). The map had said the Nucleus was complete; the element list
   had been walked, and this one was missed. It is in now, in `gen_store`
   for every numeric and numeric-edited `DISPLAY` receiver, and the two
   zoned `MVC` shortcuts step aside for it.

**With that, `1 RPW 0,1` is claimed** (2026-08-30): every element in the
module's list is implemented and checked. The conformance map says so and
the README's "the module is not claimed" is gone.

### Oracles

GnuCOBOL 3 carries a full Report Writer and is the differential oracle for
every slice. IKFCBL00 has one too, of the 1968 standard's shape, and where the
two disagree the 1974 text decides -- the same rule as everywhere else in this
project. The corpus's six report programs stay the regression floor: nothing
here may change a line they print.

## ALTERNATE RECORD KEY: what this VSAM allows

Done 2026-08-30. The clause, `WITH DUPLICATES`, `READ ... KEY IS` an
alternate, `START ... KEY IS` an alternate, the key of reference for `READ
NEXT` (VI-24, VI-30), status `02` on a read whose next record shares the key
of reference's value and on a write or rewrite that made a duplicate (VI-3),
`22` on a duplicate where none is allowed, `23` on a missing value. Each
alternate key is a VSAM alternate index reached through a path: the compiler
emits an ACB and RPL per path, with IBM's ddname convention (the first five
characters of the base ddname and `01`, `02`, ...), and a cell naming the key
of reference's RPL, through which sequential requests go. The infrastructure
is the user's IDCAMS: `DEFINE ALTERNATEINDEX` (`NONUNIQUEKEY` for `WITH
DUPLICATES`, `UPGRADE`), `DEFINE PATH`, and `BLDINDEX` after a load, because
VSAM does not maintain an alternate index while the base is being created.
The test harness carries that JCL for `tests/aixnatl`, `aixnat`, `aixnatu`
and `aixnatr`; the oracles are GnuCOBOL's indexed files where the two agree
and the 1974 text where they do not (GnuCOBOL returns `00` on a keyed read
that duplicates follow; VI-3 says `02`, and VSAM signals it).

What the machine allowed was found by probing, and each limit is a fact
about MVS 3.8's VSAM, not a choice:

- A base cluster defined `REUSE` cannot carry an alternate index
  (`IDC3022I INVALID RELATED OBJECT`). So an alternate-key file is not
  reset by `OPEN OUTPUT`; the load relies on the cluster being empty, and
  the harness deletes and redefines it before each load.
- Nothing else in the sphere can be open in the same region while the base
  is open for output: a path opened for output gets ACB error 168; a path
  opened for input after the base opens for output takes an operation
  exception inside VSAM on its first `GET`; the base opened after the paths
  gets 168; the alternate index opened as a cluster gets 168. Local Shared
  Resources, which would have given every ACB one buffer pool, refuse a base
  with an upgrade set (error 212) while accepting a plain KSDS.
- Therefore: **`OPEN INPUT` opens the base and every path, and reads by any
  key; `OPEN I-O` opens the base alone, updates by the prime key, and VSAM
  maintains the alternate indexes through the upgrade set -- with status
  `02` and `22` reported as the standard says -- and a program that opens a
  file `I-O` may not read it by an alternate key** (refused at compile time
  with the reason). `OPEN OUTPUT` loads the base; `BLDINDEX` follows in JCL.
  A program that needs both updates and alternate-key reads does them in
  two opens.
- `ENDREQ` against an RPL that has never carried a request goes through a
  placeholder VSAM has not built and ends in an operation exception; each
  RPL now carries a flag in the fullword before it, and a failed request is
  `ENDREQ`ed (it may hold its interval in exclusive control, which the next
  request on another string would meet as feedback 20).
- The insert RPL of a `DYNAMIC` file must be `DIR`: a sequential `PUT`
  keeps its string positioned and its interval held, and the retrieval
  string's next keyed `GET` met the hold (feedback 20). That was the last
  fault, and lifting it also lifted the old refusal of `ACCESS IS DYNAMIC`
  with `OPEN I-O` -- `UPD` keeps a direct request's position just as `NSP`
  does, so one RPL serves both kinds of `READ` on an I-O file.
- VSAM keeps an alternate index's pointers in the order the records were
  written, which is the standard's order for duplicates (a record rewritten
  under a new value comes last), and GnuCOBOL's.

`COBC370_VSDEBUG=1` at compile time makes an unmapped VSAM failure leave the
raw feedback or ACB error code in the `FILE STATUS`, as two hex digits, in
place of `30`; it is how every code above was read.

With this the Indexed I-O module is at Level 2 -- the one caveat being the
I-O split above -- and the map's last exception is gone.

## Dataset formats: variable-length records

Done 2026-08-30, the first of the dataset-format items discussed after the
alternate keys. The facts first, because the online threads about "COBOL
and blocking" are confusing only until the three layers are separated:

1. **The DCB merge is MVS's, and precise.** Each DCB field is filled from the
   program's DCB, then the DD statement's `DCB=`, then the label -- and a
   field already non-zero is never overwritten. For output the merged
   result becomes the label.
2. **What the compiler puts in the DCB is the compiler's table.** IBM's:
   `RECORD CONTAINS n` gives `LRECL`; a `TO` range, records of different
   lengths, or `RECORDING MODE V` give `RECFM=V` with `LRECL` four more (the
   RDW); `BLOCK CONTAINS n RECORDS` gives `RECFM=FB`/`VB` and the block
   size (for V, n records plus the BDW); `BLOCK CONTAINS` omitted asserts
   unblocked -- the classic trap on input, because the program's `F/LRECL`
   then wins the merge against a blocked label; `BLOCK CONTAINS 0` leaves
   the block size to the DD or the label.
3. **QSAM does the rest**: move mode copies `LRECL` bytes; a V block is
   walked by its RDWs; a block longer than `BLKSIZE` is S001-4.

cobc370's table, now: input DCBs state nothing, so the label decides
(unchanged, and the reason none of the input traps apply). Output states
`RECFM`, `LRECL` and `BLKSIZE` from the FD as IBM does, and now knows
variable-length records: `RECORDING MODE IS F/V` (IBM's clause; `U` and `S`
refused), `RECORD CONTAINS m TO n`, and IBM's inference from record
descriptions of different lengths. A V file gets a four-byte RDW cell in
the doubleword before its record area, which is where QSAM's move mode
wants it: `READ` gets the record behind its RDW, `WRITE` sets the RDW to the
length of the record description named (IV-33) and puts from it, and the
update-mode `READ`/`REWRITE` copy the record at the length in the buffer's
RDW with `MVCL`, so a rewritten record keeps its size (IV-29). `BLOCK
CONTAINS 0` came along: `BLKSIZE` is left out of the DCB.

Checked three ways. `tests/vrec` writes five records of two lengths blocked
three to a block, reads them back, rewrites one of each length in place,
and reads again; GnuCOBOL agrees line for line. Then the same dataset,
written and rewritten by cobc370, was read by IKFCBL00 and dumped by
IDCAMS: all five records, the right lengths (`X'28'` data bytes for the
long ones), the rewritten text. One implementation-defined difference
showed itself there and is worth knowing: what a program sees in the
record area *beyond* a short record. IBM's compiler reads V files in locate
mode with the record description over the buffer, so beyond a short record
it shows the next record's RDW and text; cobc370 moves the record into its
area and leaves the rest as it was. The standard defines neither -- IV-24
says only that the record is made available -- and a program that looks
there is wrong under both.

Found on the way, by the production batch and nothing else: the hidden items
the Report Writer creates (the counter group, the sum counters, the printable
items) had `fd_file` left at zero by `memset`, and zero is file number 0.
Nothing had read that field for them until the record-format inference
did, and then the first FD of every report program had an eight-byte
"record" and was taken for variable-length: every step ended CC 0000 and
every report was a heading on an empty page. The sweep's 101 tests passed
throughout. The batch is the safety net for exactly this.

The probe matrix from that discussion -- F/FB/V/VB by `BLOCK CONTAINS`
omitted, stated and `0`, by input and output, eighteen cells, each run
under IKFCBL00 and under cobc370 -- is in the conformance map under
Sequential I-O. It came out as the three layers predict: IBM's compiler
abends in exactly the two cells where an FD without `BLOCK CONTAINS` meets
a blocked label (S001-4 on FB, S002-04 on VB); cobc370 reads all twelve
input cells, because its input DCBs leave the label to decide; and on
output the two compilers write identical labels in five cells of six, the
sixth being `RECFM=F` against `RECFM=FB` with `BLKSIZE=LRECL` for an
unblocked fixed file, which is the same dataset under another name. The
first run of the matrix as one job stopped at the first abend and hid the
rest -- one job per cell is the way to run a matrix that is expected to
abend.

## The external review round

On 2026-08-30 the compiler was reviewed by a second machine -- xAI's Grok
Build, reading the source cold -- which filed issues 1-20 on the tracker and
committed fixes for all of them, with test programs, but had no way to run
the guest. This session reviewed that commit line by line, wired the tests
into the harness with oracles, and put everything through TK5.

What the review found was real: an interior COMP item after a PIC X was
loaded with LH off its halfword boundary (a S0C6 on real iron -- the sweep
never noticed because every COMP item it declared happened to land aligned);
PICTURE P occupied storage inside a group; a MOVE literal past 33 characters
overflowed a buffer; USAGE on a group was not inherited; a D in column 7
compiled; CURRENT-DATE named only in COPY text was never created; and the
rest. The fix quality was good, and two fixes came with design sense: the
dynamic-CALL table now reuses freed slots instead of treating the first
empty one as end-of-table, and COPY REPLACING no longer re-scans its own
substitutions.

Three fixes arrived with no test at all and have them now: `searchz`
(SEARCH from index 0), `copyrep2` (AA BY BB, BB BY CC on one line of COPY
text), `mp16` (a 16-digit multiplicand on the right of `*`, which MP's
8-byte second operand used to truncate to 15 digits silently -- now the
operands swap sides). One issue was not a bug: the text defines AT END only
for an index past the last occurrence, so an index below 1 is undefined --
taking AT END is this compiler's choice, and GnuCOBOL, which reads storage
in front of the table and answers HIT, is no counter-argument.

Verified the usual three ways: the host checks, the sweep (111 tests before
the three new ones, 114 after, all green), and the production batch --
figure multiset identical to the previous build, the only diffs job
numbers, device allocations and step accounting.

The status-key loose ends were then finished in a follow-up commit. The
QISAM load DCB says why a PUT failed in DCBEXCD2 (DCB+81, X'80' sequence,
X'40' duplicate), so the WRITE error path now stores 21 or 22 by cause and
30 otherwise, clearing the exception bytes before each PUT so one error
cannot masquerade as the next; a failed BISAM READ tests the DECB's
exception byte (DECB+24, X'80' record not found) and stores 23 or 30; the
QSAM update REWRITE stores 00 after its PUTX. Proved on the guest by two
programs added to the ISAM roundtrip on a second scratch dataset:
`tests/isamstat` loads clean/duplicate/out-of-sequence/clean and reads back
00, 22, 21, 00 -- showing loading continues past both errors, which is
QISAM's documented behaviour when SYNAD returns -- and `tests/isamrnf`
reads a present key (00), a missing one (23), and a present one again.
There is no oracle for these values but the VI-3 table itself: IBM's ANS
COBOL predates FILE STATUS entirely. Also removed: `emit_literal`, orphaned
by the review commit's MOVE-literal rework.
