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

## Suggested order

Narrowest end-to-end slice first, each verifiable:

1. `STOP RUN` only → emit ASM → assemble → link → RC=0. Proves the toolchain.
2. `DISPLAY` of a literal. Proves the output path and the runtime decision.
3. Packed-decimal arithmetic, diffed against GnuCOBOL. Proves the numeric core.
4. Sequential `FD`/`01` read and write. Proves QSAM.
5. The first real GL program.

## Still to characterise

- A probe with `FD`/file I/O, to capture the QSAM/DCB interface
- A probe with `CALL` (static and via `DYNALOAD`), to capture the linkage
- `SYS1.COBLIB` member list — the full `ILBO*` runtime surface
