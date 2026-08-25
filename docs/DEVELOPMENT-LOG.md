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

- Group items, `OCCURS`, `REDEFINES` are not implemented — the survey says 8 and
  7 uses respectively, so this is not urgent, but groups will be.
- `GIVING` on ADD/SUBTRACT, and the `MULTIPLY`/`DIVIDE` statements, are not
  implemented (2 and 4 uses in the corpus against COMPUTE's 35).
- WORKING-STORAGE is capped at one base-register displacement (~3800 bytes) and
  says so. Base locator cells are the structural fix, and are what IKFCBL00's
  `BL=1` cells do.
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
