# Replacing ANS COBOL

**Goal.** A compiler that runs on the Mac, reads COBOL, and emits S/370
assembler; MVS assembles and links that into the program's load module. The
compiler never runs on MVS. `COBUCL` gets replaced by an `ASMFCL`-shaped proc,
and the editing workflow in `~/majesty` is unchanged — `.ASM` ships up instead
of `.COBOL`.

**Why.** IKFCBL00 has no VSAM, no de-editing of numeric fields, and no dynamic
sub-modules. The scar tissue is in the tree: `SVD001.VSAMIO.*`, the `TOZONEDD`
assembler routine, and `SVD001.DYNALOAD` — every dynamic call in the corpus goes
through `CALL 'DYNALOAD'`. IBM never assigned the compiler a part number; the
earliest part-numbered COBOL is a VS COBOL targeting 32-bit MVS. Moshix's A/B
against the later VS compilers found them slightly faster and no better, so
upgrading is not the answer.

## What is actually installed

`SYS2.PROCLIB(COBUCL)` runs **`PGM=IKFCBL00`** with
`PARM='LOAD,SUPMAP,SIZE=2048K,BUF=1024K'`, linking against `SYS1.COBLIB`.
`SUPMAP` suppresses the maps, so the generated code has never been visible in
normal use. Procs captured in `baseline/procs/`.

## Characterisation without a disassembler

The compiler tells you what it does if you ask. Recompiling with
`PARM='SOURCE,DMAP,PMAP,VERB'` yields the data map and the procedure map — a
full assembler listing of generated code. `jcl/cobol/probe1.jcl` is that probe;
`baseline/probe1-listing.txt` is the output. **Read its output, not its code.**

### Storage model (DMAP)

    INTRNL NAME   LVL SOURCE NAME   BASE   DISPL   DEFINITION   USAGE
    DNM=1-048     01  WS-PACKED     BL=1     000   DS 5P        COMP-3
    DNM=1-067     01  WS-DISP       BL=1     008   DS 9C        DISP-NM
    DNM=1-084     01  WS-EDIT       BL=1     018   DS 11C       NM-EDIT
    DNM=1-118     01  WS-BIN        BL=1     028   DS 2C        COMP
    DNM=1-134     01  WS-RESULT     BL=1     030   DS 6P        COMP-3

Data is addressed as **BL cell + displacement** — a base locator per 4096-byte
chunk of WORKING-STORAGE, loaded into a register (BL=1 is R6 here). That is how
ANS COBOL survives 24-bit base-displacement addressing, and a replacement needs
the same idea or an equivalent.

Register model: **R13 = TGT** (Task Global Table; all temporaries are `D(13)`),
**R12 = PGT** (literal pool and V-cons), **R6 = BL=1** for WORKING-STORAGE.

### Runtime interface

`DISPLAY` and `STOP RUN` are calls through V-cons in the PGT, with the argument
list assembled **inline immediately after the BALR**:

    L     15,004(0,12)          V(ILBODSP0)
    BALR  1,15
    DC    X'0001' ... X'FFFF'    <- inline parm list, addressed via R1

The runtime is the `ILBO*` module set in `SYS1.COBLIB`. **A replacement that
emits its own assembler and its own runtime does not have to match any of
this** — and shouldn't, because this runtime is exactly what lacks VSAM and
de-editing. The only interfaces that must be matched are standard S/370 linkage
(R1 parmlist, R13 savearea chain, R14/R15) and the OS access methods. The
existing `TOZONEDD` / `DYNALOAD` / `VSAMIO` routines already use those, so they
keep working unchanged.

### Generated code, and how weak it is

`MOVE WS-PACKED TO WS-EDIT` — PICTURE editing *is* done in hardware, via `ED`
against a mask built in the literal pool:

    ZAP   1C0(8,13),000(5,6)     pack source into a temp
    XC    1C0(3,13),1C0(13)
    NI    1C3(13),X'0F'          strip the sign nibble
    MVC   1C8(13,13),010(12)     load the edit mask from LIT+0
    ED    1C8(13,13),1C3(13)     the edit itself
    BALR  3,0
    BC    4,008(0,3)             sign test
    MVI   1D4(13),X'40'          blank the sign position
    MVC   018(11,6),1CA(13)      copy to WS-EDIT

Nine instructions and two temporaries for one MOVE, with the result staged in a
temp and copied. `COMPUTE WS-RESULT = WS-PACKED * WS-BIN` is worse:

    MVC   1D8(2,13),028(6)       copy the COMP halfword to a temp   <- pure waste
    LH    3,1D8(0,13)            ...then load it from the temp
    CVD   3,1C0(0,13)
    MP    1C0(8,13),000(5,6)
    ZAP   030(6,6),1C2(6,13)

`LH 3,028(0,6)` would have done. This is a 1970s single-pass compiler with no
peephole, and there is a lot of room.

## Why BURG fits this target unusually well

The S/370 SS-format decimal instructions carry `(base, displacement, length)` in
the operand itself, which maps directly onto tree patterns with length
attributes — `AP`, `SP`, `ZAP`, `MP`, `DP`, `SRP`, `MVC`, `MVN`, `MVZ`, `PACK`,
`UNPK`. And `ED`/`EDMK` collapse an entire PICTURE edit into one instruction plus
a mask constant, so the rule *"MOVE numeric → edited picture"* ⇒ *(build mask,
`ED`)* is exactly what BURG is for. Cost-driven selection should beat the above
without trying hard.

The existing SSA+BURG pair in `~/slow-32/tools/dbt` (`stage5_ssa.c`,
`stage5_burg.c`) is the obvious starting skeleton.

Two things to think about early:

- **SSA over COBOL is not SSA over locals.** Every name is a fixed address in
  WORKING-STORAGE with a PICTURE. `REDEFINES`, group moves and `OCCURS` make
  aliasing the interesting problem, not renaming.
- **`DP` and `MP` have awkward operand-length rules** (`MP` needs the
  multiplicand pre-padded with high-order zeros), and ANSI `COMPUTE`
  intermediate-result sizing and rounding is where a naive implementation
  silently diverges.

## The oracle

The same 30 programs exist in GnuCOBOL under `~/majesty/src/cobol`, over the same
data, with a C++ third implementation and an established byte-identical report
bar. Acceptance testing is: compile the report suite, run it on MVS, diff the
printed output. Few compiler projects get an oracle this good.

One caveat: GnuCOBOL uses **libgmp** for `cob_decimal`, so its `COMPUTE`
intermediates are effectively unbounded, while S/370 packed decimal caps at 31
digits. Expressions with large intermediates can diverge. Worth a targeted
differential test early rather than discovering it inside a report.

(That libgmp dependency is also why porting GnuCOBOL itself to MVS is a dead
end, and why native packed decimal is the right call: the hardware does what GMP
is emulating.)

## Slice 0 is done (2026-08-25)

`cc/cobc.c` — a small C program that reads fixed-format COBOL and emits S/370
assembler. It accepts exactly one program shape: the divisions, and a PROCEDURE
DIVISION whose only statement is `STOP RUN`. Anything else is a diagnostic
naming the limit, not a silent success.

    cd cobol/cc && make && ./cobc tests/stoprun.cbl -o tests/stoprun.asm

What it emits is standard OS/360 entry linkage and nothing else — no
`ILBOSTP1`, no `SYS1.COBLIB`:

    STOPRUN  CSECT
             STM   14,12,12(13)        save caller's registers
             BALR  12,0                establish addressability
             USING *,12
             ST    13,SAVEAREA+4       backward chain to caller
             LA    11,SAVEAREA
             ST    11,8(13)            forward chain from caller
             LR    13,11               our save area is now current
             L     13,4(13)            restore caller's save area
             LM    14,12,12(13)        restore caller's registers
             SR    15,15               return code 0
             BR    14                  return to caller
    SAVEAREA DS    18F
             END

`jcl/cobol/slice0.jcl` runs it through `ASMFCLG` (assembler is **IFOX00**;
steps ASM / LKED / GO). Result:

    STOPRUN  ASM   IFOX00    RC= 0000
    STOPRUN  LKED  IEWL      RC= 0000
    STOPRUN  GO    PGM=*.DD  RC= 0000

**And a control test, because RC=0 proves little on its own.** The same source
with `SR 15,15` replaced by `LA 15,7` returned:

    RCPROOF  GO    PGM=*.DD  RC= 0007

So the generated code genuinely executes and determines the return code. The
savearea chaining and register restore are correct too — a broken return would
have abended rather than returning cleanly.

The path from COBOL on the Mac to a running load module on MVS 3.8j is proven,
with `SYS1.COBLIB` never referenced. That was the point of the slice.

## Slice 1 is done: DISPLAY, with our own runtime

The decision was to write our own rather than call `ILBODSP0`, so the compiler
never acquires a `SYS1.COBLIB` dependency it would later have to remove.

`cobc` now emits a second CSECT, **`COBRT`**, with two entry points. It reaches
SYSOUT through the QSAM macros directly — `OPEN`/`PUT`/`CLOSE`, the same
access-method path IKFCBL00 uses for its own file I/O. SYS1.MACLIB supplies the
macros, which is the OS interface, not IBM's COBOL runtime.

    COBDISP   write one line to SYSOUT; opens it on demand
    COBTERM   close it, if COBDISP ever opened it

The runtime calling convention is deliberately the OS one, so COBOL `CALL` can
use it unchanged later: `R1` → parameter list with the high-order bit set on the
last entry, `R13` → save area, `R14`/`R15` return and entry.

    LA    1,PARM0001
    L     15,VDISP
    BALR  14,15
    ...
    PARM0001 DC    A(LIT0001)
             DC    X'80',AL3(LEN0001)   last parameter
    LIT0001  DC    C'HELLO FROM COBC. NO SYS1.COBLIB HERE.'
    LEN0001  DC    H'37'

Output line is `CL121` — ASA carriage control byte plus 120 columns, blank
filled, with a variable-length `MVC` done by `EX` and truncation at the line
width.

Result (`jcl/cobol/slice1.jcl`):

    HELLO  ASM   IFOX00    RC= 0000
    HELLO  LKED  IEWL      RC= 0000
    HELLO  GO    PGM=*.DD  RC= 0000

    HELLO FROM COBC. NO SYS1.COBLIB HERE.
    SECOND LINE, WITH A QUOTE: DON'T PANIC.

**The link is the proof.** `ASMFCLG` runs LKED with `PARM=NCAL` — no automatic
library call — so an unresolved external reference fails the link rather than
being quietly satisfied from a library. RC=0000 with NCAL, and a module map
containing exactly `HELLO` and `COBRT`, means nothing from `SYS1.COBLIB` was
needed or used.

Two details worth keeping: `STOP RUN` alone still emits no runtime at all (the
slice-0 output is unchanged), and quote doubling is assembler-level only — the
COBOL literal `DON''T` has length 39 and prints as `DON'T`.

## Slice 2 is done: WORKING-STORAGE, MOVE, ADD, SUBTRACT, and the oracle

`cobc370` (renamed from `cobc`, which collides with GnuCOBOL's) now has a data
model and an arithmetic core.

Accepted: 01/77 elementary items with numeric PICTUREs (`S`, `9`, `V`, with
`(n)` repetition), `USAGE DISPLAY`/`COMP`/`COMP-3`, `VALUE` numeric literals;
`MOVE`, `ADD ... TO`, `SUBTRACT ... FROM`, and `DISPLAY` of an unsigned
DISPLAY item.

**Everything computes in packed decimal**, which is what COBOL semantics want
and what the hardware does natively — no bignum, which is the whole reason
porting GnuCOBOL's libgmp-based runtime was never the path. Binary `COMP`
operands convert in and out with `CVD`/`CVB`, zoned with `PACK`/`UNPK`, and
scale alignment is `SRP`, whose shift count is the low six bits of the second
operand address read as signed — so a right shift of 1 is encoded as 63.

One semantic point worth having got right early: `ADD`/`SUBTRACT` compute at the
**wider** of the two scales and truncate once on store. Rescaling the source
down first is wrong — at scale 1, `0.05 + 0.05` must give `0.1`, not `0.0`.

### Verified against the oracle

`tests/arith.cbl` run through both implementations, output byte-identical:

    000019134   packed + packed, same scale
    000020184   packed + packed, DIFFERENT scales -- exercises SRP
    000001250   binary COMP arithmetic through CVD/CVB
    000002346   zoned subtraction going negative, MOVEd to unsigned
    0000        repeated truncating adds at scale 1

**The oracle must run `-std=mvs`.** GnuCOBOL's default dialect renders a decimal
point for `PIC 9(7)V99` (`0000191.34`), which COBOL-74 does not — `V` is
implied, not stored. Against the default dialect every line reads as a mismatch
while the values are in fact identical. `make oracle` regenerates the reference
with the right dialect.

### Known limits, deliberately

- `REDEFINES` is not implemented (7 uses in the corpus).
- `GIVING` on ADD/SUBTRACT, and the `MULTIPLY`/`DIVIDE` statements, are not
  implemented (2 and 4 uses in the corpus against COMPUTE's 35).
- ~~WORKING-STORAGE is capped at one base-register displacement.~~ **Fixed in
  slice 7.**
- Decimal overflow on store is not masked; test values stay in range.
- `DISPLAY` of signed or COMP items is rejected with a diagnostic telling you to
  MOVE to a display item first, which keeps the oracle exact rather than
  guessing at sign rendering.

## Slice 3 is done: COMPUTE, and the oracle still agrees

Recursive-descent expression parser onto a small AST, evaluated on a stack of
six 16-byte packed work areas. Precedence, parentheses, unary minus, and
`ROUNDED`.

    COMPUTE R1 = A + B * C          142.00
    COMPUTE R2 = ( A + B ) * C      742.00
    COMPUTE R3 = A / B               16.66   truncated
    COMPUTE R4 ROUNDED = A / B       16.67   rounded
    COMPUTE R5 = - B + A             94.00
    COMPUTE R6 = A / C               14.28
    COMPUTE R7 ROUNDED = ( A + B ) / C  15.14

All seven byte-identical to GnuCOBOL `-std=mvs`.

Multiply is `MP`, which takes at most eight bytes on the right, so the operand
is staged through an 8-byte area. Divide is `DP`: the dividend is pre-shifted
left so the quotient lands at the wanted scale, and the remainder in the
trailing bytes is dropped. `ROUNDED` is `SRP` with rounding digit 5 on the
final right shift; truncation is the same shift with digit 0.

### Intermediate-result rules

This is the part the standard leaves open, and where fixed-point can disagree
with GnuCOBOL's unbounded intermediates:

- add and subtract carry the **wider** of the two scales
- multiply carries the **sum** of the scales, exactly
- **divide carries the destination's scale plus four guard digits, capped at
  twelve** — a choice, not a derivation, and the first thing to suspect if a
  real program ever disagrees

The seven cases above agree, including two divisions that do not terminate.
That is evidence, not proof: division is where to look first when something
diverges.

### A bug the tests did not catch, and now do

The assembler reported `IFO026 CHARACTERS APPEAR BETWEEN THE BEGIN AND CONTINUE
COLUMNS`. Long comments were pushing statements to exactly 72 columns, and
**column 72 is the continuation indicator**, so the assembler read the next line
as a continuation. The generator now stops every statement at column 71 and
truncates the comment rather than the code.

The column check had been asserting `length > 72`, which misses this by exactly
one column. It now asserts that reaching column 72 is legal only when that
column holds the deliberate `X` of a continued statement.

## Slice 4 is done: group items and PERFORM THRU

The first slice where control flow actually moves, and where the data model
stopped being a flat list.

**Group items.** Levels 01-49 and 77, nested, with sizes computed as items
close. A group emits `DS 0CLn` so it labels its subordinates without advancing
the location counter. `PIC X` arrives too — the most common picture in the
corpus at 435 uses — with `VALUE 'literal'` and `VALUE SPACES`.

**Alphanumeric and group MOVE** is `MVC`, left justified, space filled,
truncated on the right. A group moves as bytes whatever its subordinates are,
which is why `MOVE CUSTOMER-REC TO SAVE-REC` becomes one 16-byte `MVC`.

**PERFORM ... THRU** uses the classic per-range exit cell rather than a stack.
Each range's last paragraph ends by branching through a cell that normally
holds the fall-through address:

    * PERFORM ADD-PARA THRU ADD-EXIT
             LA    15,R0001            return here
             ST    15,X0002            into the range's exit cell
             B     P0001
    R0001    DS    0H
             LA    15,F0002            restore fall-through
             ST    15,X0002
    ...
    * end of a PERFORM range: return through its cell
             L     15,X0002
             BR    15
    F0002    DS    0H                  fall-through when not performed

Restoring the cell after the return is what keeps a later fall-through from
being diverted into a stale return point. `EXIT` is a no-op that exists to
terminate a range, and `STOP RUN` is now emitted where it appears rather than at
the end, since paragraphs after it are reachable only by PERFORM.

Verified against GnuCOBOL `-std=mvs`, byte-identical: a range performed three
times accumulating in both a COMP counter and a COMP-3 balance, a group move, a
short-to-long alphanumeric move, and a single-paragraph PERFORM after STOP RUN.

### A bug worth recording

The first run emitted `CUSTOMER-DSC   0CL16`. **Assembler labels are eight
characters; COBOL names run to thirty**, so a long name overran the opcode
field and silently corrupted the statement. Data names are now mangled to
`D0000`-style labels with the COBOL name kept in the comment, and `asm_line`
refuses a name field longer than eight characters so this cannot recur quietly.
It is the same class of problem as PROGRAM-ID, which was already checked — I
had just not carried the check to data names.

## Slice 5 is done: IF, ELSE, and conditions

Scoped from the corpus rather than the standard. What the 30 programs actually
use: `=`, `<`, `>` with their `NOT` forms and the word spellings (`EQUAL TO`,
`GREATER THAN`, `NOT GREATER THAN`), `OR` far more than `AND` (66 against 4),
`ELSE` 36 times, **no `END-IF`** — COBOL-74 confirmed — and level-88 condition
names. No `IS NUMERIC`, no `IS ALPHABETIC`, no `NEXT SENTENCE`.

- Relations on numeric operands compare in packed decimal: both sides through
  `gen_expr`, scales aligned, then `CP`.
- Alphanumeric comparison is `CLC`, with the shorter operand space padded to
  the longer, which is what COBOL specifies. A literal is interned already
  padded to the field width.
- `AND` and `OR` **short-circuit**, emitted with the standard two-polarity
  scheme: `gen_cond(c, label, jump_if_true)` recurses with the sense inverted
  for `NOT`, and `AND`/`OR` introduce a skip label in whichever polarity needs
  one.
- **Level 88 condition names** are stored against their parent and expand to a
  comparison at the point of use. They occupy no storage.
- Figurative constants `ZERO` and `SPACES` are accepted in conditions and
  moves.

**COBOL-74 period scoping.** There is no `END-IF`: a period ends the whole
sentence and unwinds every open `IF`. The statement list parser carries that up
through an `at_period` flag, which also gives correct dangling-`ELSE` binding —
`ELSE` attaches to the nearest unmatched `IF`. The test covers this explicitly:
a nested `IF` where the first `ELSE` binds inward and the second outward.

Byte-identical to GnuCOBOL `-std=mvs` across relational, `AND`, short-circuit
`OR`, alphanumeric compare, a level-88 name tested before and after its flag
changes, `NOT = ZERO`, and the nested dangling-`ELSE`.

### Two bugs, one caught by reading and one by the assembler

The first version emitted **no conditional code at all** — the statement types
existed and the parser built them, but `generate` had no cases for them, so both
arms of every `IF` ran unconditionally. Reading the generated assembler before
running it is what caught that; the program would have run and produced
plausible-looking wrong answers.

The second: level-88 items were being emitted into WORKING-STORAGE as
`DC ZL0'0'`, and the assembler rejected it with `IFO199 INVALID LENGTH
MODIFIER`. A condition name has no storage.

## Slice 6 is done: sequential files

The first slice that is mostly runtime rather than codegen, and the one where
the characterisation from probe 2 pays off directly.

`SELECT f ASSIGN TO UT-S-DDNAME` (the ddname is the part after the last hyphen
of the ANS COBOL system-name), `FD` with its record description, and
`OPEN INPUT/OUTPUT`, `READ ... AT END`, `WRITE`, `CLOSE`. `GO TO` came with it —
36 uses in the corpus, and a read loop needs it.

Each file gets a DCB in the program CSECT, QSAM **move mode** (`MACRF=(GM)` or
`(PM)`), so the `01` record under the FD is a real area that `GET`/`PUT` move
into and out of. `OPEN` and `CLOSE` are the macros, which is `SVC 19` and
`SVC 20` — the same path IKFCBL00 takes. Still nothing from `SYS1.COBLIB`.

**`AT END` is the interesting part.** COBOL's `AT END` is per-READ, but
`DCBEODAD` is per-file, so the address has to be patched before each `GET`:

    * READ IN-FILE
             LA    1,L0001             this READ's AT END
             STCM  1,7,FD000+33        into DCBEODAD
             GET   FD000,D0000         QSAM move mode
             B     L0002
    L0001    DS    0H                  AT END

Offset 33 is `X'21'`, the low three bytes of the `DCBEODAD` fullword. That
number came straight from reading what IKFCBL00 emitted back in slice 0's
probe — `MVC 021(3,2),011(12)`. Characterising the old compiler by reading its
listings, rather than disassembling it, paid for itself here.

The classic ANS COBOL read loop works end to end: a `PERFORM range THRU exit`
with `GO TO exit` inside it, the range's exit cell returning control to the
caller.

Verified against GnuCOBOL: four records copied in order and a record count of
`00004` from both.

### Two traps, neither of them the compiler

`cc ... 2>&1 | head -3` sent the C compiler `SIGPIPE` before it finished
linking, so a stale `cobc370` ran and reported a construct as unimplemented that
had just been implemented. Do not pipe the build through `head`.

The GnuCOBOL side first reported **five** records from a four-line input.
`ORGANIZATION SEQUENTIAL` reads fixed-length records and does not care about
newlines, so four 80-character lines each ending in a newline is 324 bytes, not
320 — a fifth, partial record. The oracle's input must be an exact multiple of
the record length with no line terminators at all. On MVS the same data comes
from `DD *` card images, which are already exactly 80 bytes.

## Slice 7 is done: base locator cells

WORKING-STORAGE and the file records now live in their own CSECT, `COBWS`, cut
into 4096-byte chunks. Each chunk has a **base locator cell** in the program
CSECT holding its address; a data base register is loaded from the cell and a
`USING` makes that chunk's symbols resolvable:

    * MOVE MSG1 -> MSG2
             L     8,BL0001            base locator
             USING WSC0001,8
             L     9,BL0000            base locator
             USING WSC0000,9
             MVC   D0003(5),D0000      alphanumeric move

Three registers (R8, R9, R10) rotate, and a chunk already loaded is not
reloaded. Chunk origins are `EQU`s, so no padding is needed and a field may
straddle a boundary — only its first byte has to be within 4095 of the base.
The limit went from 3800 bytes to 64K.

This is the same mechanism the DMAP showed IKFCBL00 using with its `BL=1` cells,
and it is what decouples the size of the data from code addressability. The code
side got a second base register at the same time, doubling the reach to 8K.

**The `USING` state has to match what the registers actually hold at run time**,
which is the part that needs care: it is dropped at every label, every
paragraph, at an `AT END` target, and after every `PERFORM` — anywhere control
can arrive from somewhere that left different chunks loaded.

### The bug, which is a good one

All eight tests abended, including six that had passed for slices. The cause:

    000004 05C0    COBBEG   BALR  12,0
                            USING COBBEG,12

`BALR` loads the address of the **next** instruction, 0x006, but `COBBEG`
labelled the `BALR` itself at 0x004 — so `USING` declared a base two bytes below
what the register actually held, and every R12-relative displacement was off by
two. The earlier `USING *,12` was right because `*` is the location *after* the
instruction. The base label now sits after the `BALR` as an `EQU *`.

What made it interesting is that `stoprun` still passed. Its only R12 references
are the save-area store and the matching load, both off by the same two bytes,
so they cancelled and it returned cleanly. A test suite of one would have said
the compiler was fine.

Found by reading the assembler's object code in the listing — the displacement
`C126` against a register whose value was two higher than the `USING` claimed.
Not by staring at the generator.

### And a scripted regression, at last

`bin/cobc-regress` assembles, links and runs every test on the guest and checks
each program's output against its recorded oracle. Every slice so far was
verified this way by hand; base locators touched every emit site at once, which
is what made a scripted full-suite run worth the twenty minutes.

## Slice 8 is done: OCCURS and subscripting

Scoped from the corpus again, and it scoped well: every table there is
**one-dimensional and fixed size**, with no `OCCURS DEPENDING ON`. `INDEXED BY`,
`SEARCH` and `ASCENDING KEY` do appear (4, 5 and 4 uses) but are a separate
feature and are diagnosed rather than half-implemented.

`OCCURS n TIMES` on both elementary items and groups, subscripted by a literal
or an integer data item. A subscripted reference cannot use an index register,
because SS format has none, so the element address is computed:

    LH    6,D0006             subscript
    BCTR  6,0                 subscript-1
    MH    6,H0001             times element size
    LA    6,D0002(6)          element address
    ZAP   0(5,6),PWK1(8)

The tokenizer needed a mode for this: parentheses are separators in the
PROCEDURE DIVISION but part of the word in a PICTURE, since `S9(7)V99` has to
survive as one token.

### Three bugs, and what each one teaches

**`MVC D0003(5),D0000(5)`.** MVC and CLC are SS-**a** format with one length;
`ZAP`, `PACK`, `AP` and friends are SS-**b** with two. Writing a length on MVC's
second operand means *base register 5*, not *length 5* — so it assembles
perfectly and then branches into hyperspace. `field_ref` now takes an explicit
operand mode.

**`H0001 IS AN UNDEFINED SYMBOL`.** Element sizes were interned but never
emitted. The regression script had been reporting this as an abend, because it
only checked the GO step; it now checks the assembler's return code first, so an
assembly error is reported as the diagnostic it is rather than as a bewildering
S0C1 later.

**A table reserved one element.** A group emits `DS 0CLn` — which occupies
**zero** bytes, being only a label — so a five-element table laid down ten bytes
and the next item sat on top of elements two onward. Storage is now emitted one
element at a time with a gap filler reserving the remainder. Underneath that was
a second fault: an `01`-level *group* never ran through the path that advances
`wslen`, so the next `01` restarted at the group's own offset. Both were layout
bugs; the addressing had been right all along.

## Slice 9: PICTURE moves to Ragel

The trigger was not that the hand-written scanner got ugly. It was that
PICTURE stopped being lexing and became compiling: it has to yield digit
count, scale, sign, field width **and the byte pattern for the S/370 `ED`
instruction**. A small regular language with real semantic output is what a
state machine is for.

Split the way `~/tinymux/mux/lib/date_scan.rl` does it — Ragel `-G2` scanner,
hand-written analysis on top:

- `picture.rl` tokenises into `(symbol, repeat)` pairs. Nothing else.
- `picture.c` assigns meaning, in C, where the intricate rule can have prose
  beside it.

`picture_scan.c` is generated but **committed**, so `cobc370` builds without
ragel installed. `make ragel` regenerates it.

The main token stream stays hand-written. It has one context flag
(`lex_parens`) and the corpus contains **zero continuation lines**, which is
the single thing that would make a hand-scanned COBOL reader miserable. No
pressure yet.

### The rule that needed the prose

A floating insertion string is not broken by the insertion characters inside
it. `----,---,--9` is **one** floating string of nine `-`, giving eight digit
positions and one sign position — not three separate runs. Counting per-run
loses a digit at every comma, which is exactly what the first version did.

### Validated against GnuCOBOL before any codegen

    PICTURE            GnuCOBOL width   ours
    ----,---,--9.99          15          15
    ZZZ,ZZ9.99               10          10
    +99999                    6           6
    ***,**9.99               10          10
    $$$,$$9.99               10          10
    ---,---,--9              11          11

and all **54 distinct PICTUREs in the corpus** parse.

Edited fields carry their `ED` pattern, and slice 10 uses it. `PIC X` and numeric PICTUREs go
through the same path now, so the old hand-rolled `parse_picture` is gone.

Regression: 10 passed, 0 failed.

## Slice 10: ED and EDMK

Edited output works. All five forms byte-identical to GnuCOBOL:

    MOVE -1234567.89   TO PIC ----,---,--9.99   ->   "  -1,234,567.89"
    MOVE 1234.56       TO PIC ZZZ,ZZ9.99        ->   "  1,234.56"
    MOVE 42            TO PIC +99999            ->   "+00042"
    MOVE 1234.56       TO PIC ***,**9.99        ->   "**1,234.56"
    MOVE -98765        TO PIC ---,---,--9       ->   "    -98,765"

**Floating signs use `EDMK`**, which reports where the first significant digit
landed so the sign can go one byte to its left:

    LA    1,EDWK+2             default sign position
    EDMK  EDWK(16),EDSRC
    BCTR  1,0                  one left of the first significant digit
    BNM   G0001                not negative?
    MVI   0(1),C'-'

R1 is preloaded because `EDMK` leaves it alone when significance was already on
at the start.

### Two things about the significance starter

**It goes before the first `9`, not on it.** `X'21'` stores its digit using the
significance state as it was *before* setting it, so a leading zero at that
position comes out as fill. Putting the starter on the `9` made `+99999` print
`+ 0042`. It belongs on the digit selector immediately *preceding* the first
`9`, so significance is already on when that `9` is examined.

**And when the first `9` is the first digit position**, there is no preceding
selector and every digit must print. That is what the spare leading selector is
for. Which finally explains IKFCBL00's mask being *two* bytes longer than the
field rather than one: fill byte, plus a spare selector that swallows a leading
zero and turns significance on. Computing the parity spare exactly — 0 or 1 —
was the right optimisation and the wrong one, because it removed the byte that
case needs. The generator now takes `max(parity spare, 1 when a leading starter
is required)`.

### Sizing the source

A packed field of *n* bytes holds 2*n*−1 digits, so an even digit count leaves
one spare leading nibble — a zero that would eat a selector and shift the whole
result right. The source is ZAPped into an exactly-sized temp and the pattern
gets that many extra leading selectors, with the field taking the tail of the
result.

Regression: 11 passed, 0 failed.

## Slice 11: Report Writer

The last remaining feature rather than gap-filling, and the reason six of the
thirty programs would not compile.

`FD ... REPORT IS`, `REPORT SECTION` with `RD` (PAGE LIMIT, HEADING, FIRST
DETAIL, LAST DETAIL), `TYPE PAGE HEADING` and `TYPE DETAIL` groups, `LINE n` /
`LINE PLUS n`, `COLUMN n` with `SOURCE` or `VALUE`, driven by `INITIATE` /
`GENERATE` / `TERMINATE`. `GOBACK` came along with it, since that is how these
programs end.

**Each `COLUMN` entry becomes an ordinary hidden data item with its own
PICTURE.** `SOURCE` placement is then just a `MOVE` into it followed by an
`MVC` into the print buffer at the column — which means report fields get the
whole editing path, `ED` and `EDMK` included, for nothing.

Groups compile to internal renderers reached by `BAL`; page geometry is
compile-time, so the only runtime state is the current line. `COBWRL` in the
runtime advances the paper — writing blank lines until the target — and writes.
Carriage control is ASA, with the report file's DCB switched to `RECFM=FBA`.

Verified against GnuCOBOL: two pages, page break after the seventh detail at
`LAST DETAIL 10`, heading repeated, and `LINE PLUS 2` leaving line 2 blank.

### The bug: R0 is not a register

    LH    0,RL000
    LA    0,2(0)        intended: current line + 2

`LA 0,2(0)` loads **2**, not `R0+2`. Register 0 as a base or index contributes
nothing — that is what "0 means no register" costs you. So `LINE PLUS n`
computed an absolute *n*, every line landed on top of the last, and the page
counter never advanced far enough to break. Two visible symptoms, one cause.
Everything moved to R2.

Also: the runtime CSECT was only emitted when a program used `DISPLAY`, so a
report program with no `DISPLAY` anywhere left `COBWRL` unresolved. `NCAL`
turned that into a link **warning** and an S0C1 at run time rather than an
error, which is the argument for reading the module map and not just the
return code.

Regression: 12 passed, 0 failed.

## Slice 12: the rest of PERFORM

`UNTIL`, `VARYING ... FROM ... BY`, and `n TIMES` — which clears the whole
PERFORM family. Measured across the corpus: **191 PERFORM statements, 62 with
UNTIL, 9 with VARYING (all `FROM 1 BY 1`), 2 with TIMES**, and 127 plain or
THRU, which already worked.

The exit-cell machinery from slice 4 did not change; the loop wraps it:

    L0007    DS    0H
             <condition; branch to L0008 when TRUE>
             LA    15,R0003            return here
             ST    15,X0002            into the range's exit cell
             B     P0003
    R0003    DS    0H
             <restore fall-through, step the VARYING identifier>
             B     L0007
    L0008    DS    0H

`UNTIL` tests **before** each iteration, so the body may run zero times. That is
the COBOL rule rather than an implementation choice, and the test covers it
directly.

The conditions the corpus uses in `UNTIL` were already supported: level-88
condition names, `OR`, and subscripted operands like `ENT-YEAR (WS-IDX) > 3`.
Verified against GnuCOBOL across VARYING, a subscripted compound UNTIL, a
level-88 UNTIL, `n TIMES`, and a zero-trip loop.

### A latent bug that only became reachable now

`is_numeric_literal(".")` returned **true** — it accepted a sign and a decimal
point without requiring a single digit. It had been wrong since slice 2 and
never mattered, because nothing asked "is this token a number?" in a position
where the sentence terminator could turn up. `PERFORM x n TIMES` does: the
repeat count is optional, so the parser has to peek. Two previously passing
tests failed with `expected TIMES, found 'CLOSE'`.

The fix is one line, but the interesting part is the shape: a predicate that is
merely *too permissive* stays invisible until some caller asks it about input
the old callers never produced.

Regression: 13 passed, 0 failed.

## Slice 13: ISAM, both halves

QISAM sequential and BISAM random both work against the live `SVD001.GLACCT`.
Random retrieval took four independent fixes, any one of which is fatal on its
own: `CHECK` is not a BISAM macro (it loads DCB+52, which in an ISAM DCB is
`DCBOPTCD`, and branches into it); `WAITF`, which *is* the right macro, is
missing from TK5's `SYS1.MACLIB`, so synchronisation is a plain `WAIT` on the
DECB's own ECB; the read area must hold a whole 4161-byte block rather than a
57-byte record; and ISAM wants 16 bytes of working room at the front of it.
`cobol/ISAM-NOTES.md` has the full recipe and the verified DCB offsets.

Two things came along with it:

**Multi-operand DISPLAY.** `DISPLAY 'GOT ' OUT-KEY ' ' GLAC-NAME`.

**An unsigned packed sign bug.** `PIC 9(n) COMP-3` must carry an `F` sign;
`ZAP` leaves `C`. Invisible while packed fields were only compared
arithmetically, and wrong the moment one is compared byte-wise. `GLAC-KEY` is
`PIC 9(10) COMP-3`, so without this fix no random read can ever match.

Regression: 15 passed, 0 failed.

### The regression earned its keep

Three tests broke at once, and the cause was in none of them: an index-based
edit that sliced from `case ST_DISPLAY_LIT:` to `case ST_MOVE:` silently
deleted the `case ST_COMPUTE:` sitting between them. Every program using
COMPUTE quietly stopped computing and displayed its initialised values instead.
Nothing complained, because a missing `switch` case is not an error.

## Slice 14: ISAM load mode

`OPEN OUTPUT` on an indexed file, `PUT`, and `WRITE ... INVALID KEY`. The
specification was `GL039` from the corpus, which BATCH runs on every reporting
pass to build and then discard `SVD001.DESCIDX`. It compiles unchanged.

Creating an ISAM dataset means there is no label to read attributes from, so the
DCB now carries `LRECL`, `BLKSIZE`, `KEYLEN`, `RKP` and `OPTCD=L` — which is why
`BLOCK CONTAINS` is finally parsed instead of skipped, and why `RKP` is computed
from where the `RECORD KEY` sits inside the 01 record. Verified by
`bin/cobc-isam-roundtrip`, which creates a dataset with cobc370's output and
reads it back with cobc370's output; the DSCB it produces matches the shape ANS
COBOL produced for GLACCT.

Two bugs came out of it, both found by real code rather than by the tests:

**A DCB could swallow the DCB after it.** `asm_cont` wrote the continuation
operand without a length check, so an operand reaching column 72 became a
continuation flag and ate the next card. It now splits at commas across as many
cards as it takes.

**`DISPLAY` was greedy.** Multi-operand `DISPLAY` consumed everything up to the
next period, so two consecutive DISPLAYs inside an `INVALID KEY` clause — which
is exactly what GL039 has — swallowed the second verb as an operand.

Not implemented: `MOVE SPACE` and the other figurative constants as a MOVE
source.

Regression: 16 passed, 0 failed.

## Slice 15: figurative constants in MOVE

`MOVE SPACES` / `MOVE SPACE` and `MOVE ZERO` / `ZEROS` / `ZEROES`. The corpus
uses ZERO 19 times and SPACES 4 times as a MOVE source; `LOW-VALUES` and
`HIGH-VALUES` never appear there, only in VALUE clauses, so they are refused
with a message rather than half-implemented.

Conditions already worked — `parse_expr` has handled `SPACE` and `ZERO` since
the IF slice, which is why `IF WS-NAME = SPACES` has always compiled. MOVE has
its own source parser and did not. A numeric target takes the existing literal
path, so `MOVE ZERO TO a COMP-3 item` is just `MOVE 0` and picks up the sign
handling for free; an alphanumeric target gets an MVC against a constant built
to exactly the receiving item's length.

### Two sign bugs the new test caught

`PIC 9(5) VALUE 12345` displayed as **`1234E`**. The assembler's `Z` constant
puts a **C** sign in the last byte's zone, but an *unsigned* item must carry
`F` — so the final digit came out as a letter. Unsigned DISPLAY items are now
emitted as `CL5'12345'`.

The same bug sat in packed VALUEs: `P` also signs `C`, so `PIC 9(5) COMP-3
VALUE 999` was initialised `00999C`. That one is invisible to DISPLAY, which
unpacks and forces the zone — and it is exactly the hazard the ISAM slice
documented, because an unsigned packed field compared byte-wise is what an ISAM
key is. Unsigned packed VALUEs are now emitted as `XL3'00999F'`.

Regression: 17 passed, 0 failed.

### Corpus status

8 of the 30 programs in `SVD001.DEFTLY.COBOL` now compile. The single largest
remaining blocker is **`VALUE LOW-VALUES`**, which stops 12 of them — every one
on the same `05 WS-MODULE-ADDR PIC X(4) VALUE LOW-VALUES` line, the DYNALOAD
module-address cell.

## Slice 16: VALUE LOW-VALUES

`VALUE LOW-VALUES`, `HIGH-VALUES` and `QUOTES` on `PIC X` items, emitted as a
repeated byte with a duplication factor — `4X'00'`, `4X'FF'`, `4X'7D'` — so
length and OCCURS need no special case. They are refused on numeric items.

This was the single largest blocker in the corpus, stopping 12 of the 30
programs, every one on the DYNALOAD control block's
`05 WS-MODULE-ADDR PIC X(4) VALUE LOW-VALUES`.

`tests/figval.cbl` checks the *bytes*, not just that it compiles: `X'00'` must
sort below space and `X'FF'` above it, which a field that quietly stayed blank
would fail. It also displays the fields on either side, since a wrong length
here would corrupt the neighbours rather than the field itself.

Regression: 18 passed, 0 failed.

### Corpus status

Still 8 of 30 compiling — the 12 unblocked programs each moved on to their
*next* missing feature, which is progress that the headline number hides. What
now stands in the way, in order:

| Blocker | Programs |
|---|---|
| `LINKAGE SECTION` / `PROCEDURE DIVISION USING` | 7 |
| qualification with `OF` / `IN` | 5 |
| `REDEFINES` | 4 |
| `OPEN` of something not declared as a file | 3 |
| `WRITE FROM`, `READ INTO` | 1 each |

The `LINKAGE SECTION` seven are the DYNALOAD sub-modules — `DIV`, `DIVMOD`,
`FTL`, `ISLEAP`, `LTF`, `GL024`, `GL040` — so callable modules with parameters
are both the largest remaining group and the feature ANS COBOL never had, which
is why DYNALOAD exists at all.

## Slice 17: REDEFINES

Both shapes the corpus uses: an 01-level group laid over another 01 group — the
classic table-over-initialised-FILLERs idiom — and an elementary item
redefining another elementary item inside a group.

A redefinition defines no storage, so it is emitted as an `EQU` to the address
it shares rather than a `DC`:

    D0003    DC    CL14'IIncome       '  FILL0003 PIC X(14)
    D0004    EQU   COBWS+0             CLASS-TABLE REDEFINES
    D0005    EQU   COBWS+0             CLASS-ENTRY REDEFINES
    D0006    EQU   COBWS+0             CLASS-LETTER REDEFINES
    D0007    EQU   COBWS+1             CLASS-NAME REDEFINES

`COBWS` is the CSECT origin, so the assembler works out the displacement from
whichever chunk base is loaded and base-locator addressing needs no special
case. A redefinition longer than what it covers is rejected.

### The bug worth remembering

The cursor has to be *resumed* after a redefinition, since the item that
follows sits after the ORIGINAL. Applying that to every item while a
redefinition was open was wrong: the **subordinates** of a group redefinition
must keep walking forward through the aliased area. Getting it wrong put
`CLASS-NAME` at offset 42 instead of 1 — every field after the first landing on
the end of the redefined item — and left a phantom 123-byte gap behind it. Only
the item actually carrying the REDEFINES clause resumes.

Regression: 19 passed, 0 failed.

### Corpus status

Still 8 of 30, with the five REDEFINES programs each advancing to their next
gap. Remaining blockers:

| Blocker | Programs |
|---|---|
| `LINKAGE SECTION` / `PROCEDURE DIVISION USING` | 7 |
| qualification with `OF` / `IN` | 6 |
| `OPEN` of something not declared as a file | 3 |
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `WRITE FROM`, `READ INTO`, and two one-offs | 1 each |

## Slice 18: the comma is a separator

`OPEN INPUT COMPANIES-FILE, ACCOUNTS-FILE.` failed with *"OPEN names something
that is not a file"* — not because of the file, but because the tokenizer glued
the comma onto the word and then looked up `COMPANIES-FILE,`. The same fault
produced `undeclared identifier 'NUMBER1A,'` elsewhere, which is how a lexer bug
gets reported three different ways and looks like three different features.

COBOL allows a comma or semicolon anywhere a space may appear. What makes that
safe to implement is COBOL's own disambiguation rule: **it is a separator only
when a space follows it**. That is exactly what keeps the commas inside
`PIC ZZZ,ZZ9.99` and `PIC ---,---,--9` part of the picture, where a naive
"strip all commas" would have quietly wrecked every edited field.

`tests/commatok.cbl` pins both halves — an edited PICTURE containing commas, and
`DISPLAY A, B` — so neither can regress without the other noticing.

Regression: 20 passed, 0 failed.

### Corpus status

**8 → 10 of 30.** GL022 and GL023 compile. The `OPEN` blocker is gone entirely,
and it was never about the `UT-S-` / `DA-I-` assign names.

| Blocker | Programs |
|---|---|
| `LINKAGE SECTION` / `PROCEDURE DIVISION USING` | 7 |
| qualification with `OF` / `IN` | 6 |
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `CALL`, `WRITE FROM`, `READ INTO`, and three one-offs | 1 each |

## Slice 19: qualification with OF / IN

The same data name may appear in different groups; a reference then has to name
enough enclosing groups to be unique. The corpus does this constantly and never
qualifies anything — every DYNALOAD control block carries its own
`WS-MODULE-NAME` and `WS-MODULE-ADDR`, and only the 01 group is ever passed to
`CALL`. So the important half was making duplicates **legal at declaration**,
with ambiguity an error only where a name is actually *used*:

- two items with the same name in the same group is still an error, because no
  qualification could ever separate them
- an unqualified reference to a name that several items share reports how many
  share it and asks for `OF`/`IN`
- `LEAF OF OUTER-A` may skip intervening levels, as COBOL allows

Sym gained a `gparent` link for the enclosing group, which is what makes the
ancestor walk possible.

### A refactor hazard worth naming

Switching the eight reference sites from `need_sym(tok.text)` to a
`consume_sym()` that advances the token itself was mechanical everywhere except
DISPLAY, whose operand loop had its own trailing `next()` shared by both
branches. The result was a **double advance** that stepped over whatever ended
the statement: in one program past `GO` onto `TO`, in another past the `.` onto
the next paragraph name. Both surfaced as `undeclared identifier` for a token
that was never meant to be an identifier — and both were programs that had
compiled *before* the slice, which is the only reason it was caught.

Regression: 21 passed, 0 failed. Negative fixtures now include `bad-ambiguous`.

### Corpus status

10 of 30, with the duplicate-name blocker gone entirely.

| Blocker | Programs |
|---|---|
| `LINKAGE SECTION` / `PROCEDURE DIVISION USING` | 12 |
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `CALL`, `WRITE FROM`, `READ INTO`, `DISPLAY` of a group, and two one-offs | 1 each |

`LINKAGE SECTION` is now the whole story: 12 of the 20 remaining programs stop
there, and `CALL` is its other half.

## Slice 20: LINKAGE SECTION, PROCEDURE DIVISION USING, and CALL

Callable subprograms, which is the feature ANS COBOL's absence of made DYNALOAD
necessary in the first place.

**Static or dynamic?** Neither deduced nor assumed: ANS COBOL has *only*
`CALL 'literal'`, which the linkage editor resolves. There is no `CALL
identifier` form to distinguish — that arrived with VS COBOL, where the literal
form is static, the identifier form dynamic, and the `DYNAM` option forces all
calls dynamic. Every `CALL` in the corpus is `CALL 'DYNALOAD' USING …`, and
DYNALOAD's own header spells out the consequence: *"it is necessary to link the
object module for the DYNALOAD routine to each calling program."* So the
compiler always emits a static `V`-con and never needs to know DYNALOAD is
special; the dynamism is entirely DYNALOAD's, at run time.

**Callee.** Each LINKAGE 01 becomes a DSECT plus a `PBL` cell holding whatever
address arrived in the parameter list. The entry sequence lifts them out of R1,
which survives the prologue untouched:

    L     0,0(0,1)
    ST    0,PBL0000           LS-IN
    L     0,4(0,1)
    ST    0,PBL0001           LS-OUT

After that a LINKAGE item is addressed exactly like WORKING-STORAGE — load a
cell, `USING`, `DROP` on reset. The base machinery needed only one change:
areas are now numbered, with `0..` a WS chunk and `-(n+1)` a LINKAGE area.

**Caller.** The parameter list is built at run time rather than assembled, so an
argument may itself be a LINKAGE item, with the OS/360 high bit marking the last
entry. `GOBACK` in a subprogram returns without closing the runtime's SYSOUT,
which the caller may still be using.

Verified by `bin/cobc-call-roundtrip`: two programs compiled separately by
cobc370, link-edited together, called twice with different data, parameters
passed both ways.

    RESULT 00001042 TAG [DONE]
    RESULT 00001007 TAG [DONE]

Regression: 22 passed, 0 failed.

### Corpus status

Still 10 of 30 — but the LINKAGE blocker is gone entirely and all 12 programs
that stopped there moved on. What is left is a long tail rather than one wall:

| Blocker | Programs |
|---|---|
| `GIVING` | 4 |
| `expected TO` (an ADD/MOVE form not yet parsed) | 4 |
| `IF NOT <condition-name>` | 3 |
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `WRITE FROM`, `READ INTO`, and five one-offs | 1 each |

`GIVING` is the largest and appears in 13 programs overall, so it will keep
turning up behind whatever else is fixed.

## Slice 21: GIVING

`ADD a b GIVING c`, `ADD a TO b GIVING c`, `SUBTRACT a FROM b GIVING c`,
`MULTIPLY a BY b GIVING c`, `DIVIDE a INTO b GIVING c` and `DIVIDE a BY b
GIVING c`. MULTIPLY and DIVIDE had no parser at all before this.

GIVING turns an arithmetic verb into an assignment, so each form is built as an
expression tree and handed to the **COMPUTE** path, which already does the
scaling, the packed arithmetic, the rescale and the store. The whole slice is
parsing; there is no new code generation.

The awkward part is that `ADD` has two shapes. `ADD a TO b GIVING c` parses like
the ordinary in-place form right up to the destination, at which point the item
just parsed turns out to be the second *operand* and the real destination
follows GIVING. `ADD a b GIVING c` has no `TO` at all and every operand is a
source. Both are handled without disturbing the non-GIVING path, which still
emits the simpler `ST_ADD`/`ST_SUB` code.

Not implemented, because the corpus has neither: `REMAINDER`, and `ROUNDED`
anywhere COMPUTE does not already accept it.

Regression: 23 passed, 0 failed.

### Corpus status

**10 → 15 of 30.** Half the corpus compiles. This one feature cleared two
entries from the blocker table at once — the four programs reported as
`GIVING is not implemented yet` and the four reported as `expected TO`, which
were the same feature seen from the two different `ADD` shapes.

| Blocker | Programs |
|---|---|
| `IF NOT <condition-name>` | 3 |
| `WRITE FROM` | 2 |
| `MOVE SPACES` to an item over 127 bytes | 2 |
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `READ INTO` and five one-offs | 1 each |

The `MOVE SPACES` one is **our own limit, not the language's**: the fill is
built as a constant in a `MAXTOK` buffer. Filling with `MVI` plus a propagating
`MVC` would drop the limit entirely and remove the constant as well.

## Slice 22: sign conditions, and a fill that is not capped

Two small things, both of which had been *mis-diagnosed* from their error
messages alone.

**Sign conditions.** `IF FD-X IS NOT NEGATIVE` and `IF FDM-MOD IS NOT ZERO`
reported *"NOT must be followed by a relational operator"*, which reads like a
condition-name problem. They are `IS [NOT] POSITIVE / NEGATIVE / ZERO` — a
comparison against an implicit zero with no right-hand operand to parse. `relop`
now returns a third result meaning "the caller supplies a zero". Class
conditions (`IS NUMERIC`, `IS ALPHABETIC`) are refused explicitly rather than
falling into the same misleading message.

**A figurative MOVE no longer builds a constant.** It sets the first byte and
lets `MVC` propagate it across the rest, one byte at a time, which is what the
overlapping operands of an SS instruction do:

    LA    1,D0004             MOVE ZEROS
    MVI   0(1),C'0'
    MVC   1(199,1),0(1)       propagate across the item

The old version built a constant the width of the receiving item, which capped
`MOVE SPACES` at the token buffer — an artificial limit at 127 bytes that had
nothing to do with the language. Going through R1 keeps it working the same
whether the item is subscripted or reached off a base locator.

Regression: 24 passed, 0 failed.

### Corpus status

**15 → 20 of 30.** Two thirds compile.

| Blocker | Programs |
|---|---|
| `WRITE FROM` | 2 |
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `READ INTO`, `DISPLAY` of a group, and four one-offs | 1 each |

## Slice 23: READ INTO and WRITE FROM

Both are a MOVE fused onto the I/O verb, and both reuse `gen_move_alpha`, which
already truncates and space-pads the way a COBOL group move does.

The only thing that needs care is *where* the move goes. `READ f INTO x` must
move on the success path only — never on AT END — so it is emitted after the
`GET` and before the branch that skips the AT END statements:

    GET   FD000,D0000     QSAM move mode
    MVC   D0002(80),D0000 alphanumeric move     <- INTO, success path only
    B     L0002
    L0001 DS 0H                                 <- AT END

`tests/intofrom.cbl` checks exactly that: after the loop ends it displays the
buffer again, and `LAST [DDD444]` proves the INTO move did not fire on the AT
END that terminated the loop. The random-ISAM READ gets the same treatment,
placed after the record is lifted out of the block.

Regression: 25 passed, 0 failed.

### Corpus status

**20 → 23 of 30.**

| Blocker | Programs |
|---|---|
| `INDEXED BY` / `SEARCH ALL` | 2 |
| `DISPLAY` of a group, `DISPLAY` of a signed item, and three one-offs | 1 each |

`INDEXED BY` / `SEARCH ALL` is now the only substantial feature left.

## Slice 24: INDEXED BY and SEARCH ALL

The corpus scopes this tightly: only `SEARCH ALL` (never the serial `SEARCH`),
always exactly one `WHEN`, and **no `SET` anywhere**. So an index is only ever
written by SEARCH itself and read as a subscript.

That makes the representation free: COBOL says an index holds a displacement,
but this one holds the **occurrence number**, which is what the existing
subscript machinery already expects and is indistinguishable from outside as
long as nothing else may touch it.

`INDEXED BY` names a new data item from inside an OCCURS clause on a group whose
subordinates are still to come — creating it there would put it *inside the
table's own storage*. The declarations are recorded and the items appended to
WORKING-STORAGE once the data division is complete.

`SEARCH ALL` generates a binary search over the `ASCENDING KEY`. Low and high
live in storage rather than registers, because `gen_cond` may use any work
register and so nothing may stay live across it. The `WHEN` condition is reused
twice — once as `=` to detect a hit, once rewritten to `<` to decide which half
to keep — which is why the equality form is required and enforced.

`parse_stmt_list` now stops at `WHEN`, which ends a SEARCH's `AT END` clause.
Nothing else begins with that word.

Verified on the guest with a five-entry table: every key found, plus two misses
— one past the end and one (`B`) sorting *between* existing keys, so the middle
of the search is exercised and not just its bounds.

Regression: 26 passed, 0 failed.

### Corpus status

23 of 30. SEARCH ALL is done and GL042/GL043 moved past it. What remains is
seven one-off gaps, the largest being `MOVE x TO a b c` — one source, several
receiving fields — which blocks three programs.

## Suggested order

Narrowest end-to-end slice first, each verifiable:

1. ~~`STOP RUN` only → emit ASM → assemble → link → RC=0.~~ **Done.**
2. ~~`DISPLAY` of a literal.~~ **Done — see below.**
3. ~~Packed-decimal arithmetic, diffed against GnuCOBOL.~~ **Done — see below.**
4. Sequential `FD`/`01` read and write. Proves QSAM.
5. The first real GL program.

## File I/O — and the most useful finding so far

`jcl/cobol/probe2.jcl`, listing in `baseline/probe2-listing.txt`.

**The compiler already bypasses its own runtime for file I/O.** `OPEN` builds an
open parameter list in the TGT, sets the option bytes, and issues **`SVC 19`**
directly:

    L     1,020(0,12)          DCB=1
    MVC   032(2,1),028(12)     option bytes from the literal pool
    ST    1,200(0,13)          SAV3
    MVI   200(13),X'00'
    ...
    LA    1,200(0,13)
    SVC   19                   <- OPEN, no ILBO involved

`READ` and `WRITE` go straight to QSAM through the routine address at
**`DCB+x'30'`**, in **locate mode** — the record address comes back in R1 and
becomes the record's base register:

    L     1,020(0,12)          DCB=1
    LR    2,1
    MVC   021(3,2),011(12)     patch DCBEODAD to the AT END branch
    L     15,030(0,1)          QSAM GET
    BALR  14,15
    ST    1,1BC(0,13)          BL=1  <- record address
    L     7,1BC(0,13)

Note `AT END` is implemented by **patching `DCBEODAD` before each GET**. `CLOSE`
adjusts DEB fields, then builds a close list the same way `OPEN` did.

This matters more than it looks. The `ILBO*` runtime is only reached for things
like `DISPLAY` (`ILBODSP0`) and `STOP RUN` (`ILBOSTP1`) — file I/O is the
compiler emitting OS access-method calls inline. A replacement can do exactly
the same and never link `SYS1.COBLIB` at all. **And VSAM is the same shape of
work** — build an ACB/RPL, issue the macro-equivalent calls — so the feature ANS
COBOL lacks is not a different kind of problem, just more of what it already
does for QSAM.

## CALL linkage

Both forms are plain OS linkage, which is why the existing assembler routines
work and will keep working:

    LA    1,010(0,6)           address of arg 1
    ST    1,20C(0,13)          PRM=1 in the TGT
    LA    1,008(0,6)           address of arg 2
    ST    1,210(0,13)          PRM=2
    OI    210(13),X'80'        high-order bit marks the last argument
    LA    1,20C(0,13)          R1 -> parameter list
    L     15,008(0,12)         V(DYNALOAD)   resolved by the linkage editor
    BALR  14,15
    STH   15,05C(0,13)         RETURN-CODE

R1 → address list, high bit on the final entry, R15 entry / R14 return, return
code out of R15. `CALL 'SUBPROG'` and `CALL 'DYNALOAD'` differ only in argument
count. A replacement must emit exactly this and nothing more.

## The runtime surface is small

`SYS1.COBLIB` holds **73 `ILBO*` modules** — `ILBODSP0` (DISPLAY), `ILBOSTP0/1`
(STOP RUN), `ILBOERR0-6`, the `ILBOBI*` / `ILBOIF*` conversion families, and so
on. Full list in `../inventory/catalog-raw.txt`. That is the entire surface you
would have to replace if you chose to replace it — and for file I/O you don't,
because the compiler never used it there.
