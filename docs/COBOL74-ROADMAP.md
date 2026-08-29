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
  decision (below), because it is the one module where the remaining half is
  larger than the half that exists.

## The work, in order

Ordered so that each tier is a claim the project can make when it closes, and
so that work sharing the same machinery lands together. Sizes are honest
guesses: **S** is an afternoon, **M** is a day, **L** is several.

### Tier 0 -- Nucleus Level 1, actually (3 items)

The map says the Nucleus is complete at Level 1. It is not. Three Level 1
elements are refused, found by reading the standard's own `1 NUC 1,2` list
against the compiler rather than trusting the map:

| element | size | note |
|---|---|---|
| `GO TO ... DEPENDING ON` | S | a branch table; `GO TO` itself exists |
| `CURRENCY SIGN IS literal` | S | one more symbol in `picture.rl`, substituted for `$` |
| `DECIMAL-POINT IS COMMA` | S | swaps `.` and `,` in pictures and numeric literals; scanner and picture both |

The minimum standard is not reached until these are in. Do them first.

### Tier 1 -- Nucleus Level 2 (about 20 items)

Grouped by the machinery they touch, so each group is one piece of work.

**Scanner and reference format** -- S each, do together:
- separators: comma and semicolon allowed as separators
- continuation of words and numeric literals (only nonnumeric literals continue today)
- figurative constants at Level 2: `ZEROS`/`ZEROES`, `HIGH-VALUES`, `LOW-VALUES`,
  `QUOTES`, `ALL literal` -- accepted everywhere a literal is, including
  conditions (`IF X = HIGH-VALUES` is refused today)
- user-defined words need not begin with a letter

**Conditions** -- M for the group:
- abbreviated combined relation conditions: `IF A = 1 OR 2 OR 3`,
  `IF A > 1 AND < 100` (the parser sees `<` as an identifier today)
- comparison of nonnumeric operands of unequal size (space-padded `CLC`
  through a work area; the shorter operand is the one to pad)
- level-88 `VALUE ... THRU ...` and `VALUE` series
- `NOT` on the sign condition -- already works; keep the test

**Arithmetic** -- M for the group:
- `**` exponentiation (integer exponent by repeated `MP`; a non-integer
  exponent has no exact packed-decimal answer -- refuse it with a message)
- `ON SIZE ERROR` on every arithmetic statement (`ROUNDED` is already accepted)
- identifier series on `COMPUTE` (`COMPUTE A B = ...`), multiple results generally
- `DIVIDE ... INTO` series without `GIVING`; `DIVIDE ... REMAINDER`
- `MULTIPLY`/`ADD`/`SUBTRACT` `GIVING` series -- `ADD` and `MULTIPLY` already work

**`CORRESPONDING`** -- S: `ADD`, `SUBTRACT`, `MOVE`. Pure front end: match
subordinate names between two groups and expand to the elementary statements.

**`STRING` and `UNSTRING`** -- M: two runtime routines in `COBRT`.
`DELIMITED BY` series, `POINTER`, `TALLYING`, `ON OVERFLOW`. The runtime
already has the shape for this (`COBADV` takes a parameter list of addresses).

**`INSPECT` with multi-character operands** -- S: the single-character
version exists; widen the compare from `CLI` to `CLC` and step by the operand
length.

**`PERFORM VARYING ... AFTER`** -- S: nested loop, one or two `AFTER` phrases.
The `VARYING` loop exists; this wraps it.

**Level-66 `RENAMES`** -- S: a symbol whose storage is a byte range of its
parent. `RENAMES ... THRU ...` too. No code generation of its own.

**`GO TO` without a procedure-name** -- S: the `ALTER` target. `ALTER` exists,
so this is only the parser accepting the bare form and the initial branch
going to a fault.

**`ACCEPT ... FROM`** -- S: mnemonic-name, `DATE`, `DAY`, `TIME`. `COBDATE`
already produces the date; `DAY` and `TIME` are the same `TIME` macro read
differently. **`DISPLAY ... UPON`** mnemonic-name -- S: `SYSOUT` and
`CONSOLE` (WTO) are the two that matter.

**`ACCEPT`/`DISPLAY` with no restriction on the number of transfers** -- S:
the Level 1 forms take one operand.

### Tier 2 -- Table Handling Level 2 (3 items)

| element | size | note |
|---|---|---|
| serial `SEARCH`, with `VARYING`, `AT END`, `WHEN` series | S | `SEARCH ALL` exists; this is the simpler loop |
| `OCCURS ... ASCENDING/DESCENDING KEY` series | S | one key already works |
| `OCCURS integer-1 TO integer-2 DEPENDING ON data-name` | **L** | variable-length groups: the containing group's size becomes a run-time value, which reaches `MOVE`, `WRITE`, comparison and subscript bounds. The one item in this plan that touches the symbol table's assumptions. Do it last in its tier, with the design written down first |

### Tier 3 -- Sequential I-O Level 2 (about 10 items)

The `COBADV` routine already takes its line count at run time, which makes the
first two nearly free.

| element | size | note |
|---|---|---|
| `WRITE ... ADVANCING identifier LINES` | S | load the identifier instead of a constant |
| `WRITE ... ADVANCING mnemonic-name` (`C01`..`C12`, `CSP`) | S | ASA channel codes `1`..`C`, `+`; the runtime emits the byte |
| `LINAGE` with `FOOTING`, `TOP`, `BOTTOM`; `LINAGE-COUNTER`; `WRITE ... AT END-OF-PAGE` | M | logical page kept by `COBADV`; this is where the runtime routine earns its keep |
| `OPEN EXTEND` on a sequential file | S | the OPEN macro's `EXTEND` option, or `OUTPUT` on a `DISP=MOD` allocation -- verify which MVS 3.8j honours |
| `OPEN INPUT ... REVERSED` | S | tape only (`RDBACK`); accept, and refuse at run time on DASD |
| `SELECT OPTIONAL` | M | a missing DD: `RDJFCB` or `DEVTYPE` before `OPEN`, then first `READ` takes `AT END` |
| `CLOSE ... NO REWIND / REMOVAL / LOCK`, `REEL`/`UNIT` | S | accepted already; verify the macro options are passed rather than ignored |
| `BLOCK CONTAINS integer-1 TO integer-2`, `VALUE OF`, `SAME RECORD AREA`, `MULTIPLE FILE TAPE`, `RESERVE integer AREAS`, `USE ... ON EXTEND` | -- | all accepted today; `RESERVE` could become `NCP`/`BUFNO` on the DCB |

### Tier 4 -- Relative and Indexed I-O, Level 1 closure (1 item, shared)

`USE AFTER STANDARD ERROR PROCEDURE ON file-name` for relative and indexed
files. Declaratives exist for sequential files; the dispatch has to reach the
VSAM and ISAM error paths, which today go straight to `FILE STATUS`. **M.**
Closes both modules at Level 1 in one piece of work.

Also in the same area, from the CCVS histogram: the "this SELECT clause is not
implemented" bucket (35 programs) was decomposed once and found to be mostly
COBOL-85 spellings, correctly refused. Re-decompose it after Tier 3; whatever
is left that is COBOL-74 belongs here.

### Tier 5 -- Inter-Program Communication, both levels (3 items)

| element | size | note |
|---|---|---|
| `EXIT PROGRAM` | S | a return through the save area; `GOBACK` already does it under an IBM name |
| `CALL identifier` | S | `LOAD` the module named in the field, `BALR` to it. The source comment saying ANS COBOL has no `CALL identifier` is true of IBM's compiler and false of the standard; `2 IPC 0,2` lists it. A small runtime routine that does what the DYNALOAD idiom does by hand |
| `CANCEL` | S | `DELETE` the loaded module |

### Tier 6 -- Library: `COPY` (1 item, host side)

Level 1 is textual inclusion of `text-name` from a copy library; Level 2 adds
`REPLACING ==pseudo-text== BY ==pseudo-text==`. **M** for both together.

Host side by nature: cobc370 runs on the host, so the copy library is a
directory (or a list of them, `-I` style), and the scanner opens the member
and reads from it until it ends, then resumes. Nothing on the guest is
involved. `REPLACING` is a token-level substitution on the way in. This is the
one item in the plan with a stated pull behind it.

### Report Writer -- a separate decision

`1 RPW 0,1` is a single level, so the module is either complete or not
claimed. What exists is a page manager: `RD`, `PAGE LIMIT`, `PAGE HEADING`,
`DETAIL`, `LINE`, `COLUMN`, `SOURCE`, `VALUE`, `GENERATE`/`INITIATE`/
`TERMINATE`. What is missing is the half that computes: the five other `TYPE`s
(`REPORT HEADING`, `CONTROL HEADING`, `CONTROL FOOTING`, `PAGE FOOTING`,
`REPORT FOOTING`), `SUM` with sum counters, `UPON` and `RESET`, control breaks
with subtotalling and rolling forward, `NEXT GROUP`, `GROUP INDICATE`,
`SUPPRESS`, `USE BEFORE REPORTING`, `LINE-COUNTER`/`PAGE-COUNTER` as
referenceable registers. **L, and then some** -- the control-break machinery
is a small language of its own.

Not required for the definition of done above. Worth doing if reports are the
point of the compiler, which for this project they are; worth skipping if the
goal is to close the language and move on. Decide after Tier 6.

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
