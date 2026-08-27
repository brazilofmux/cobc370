# Assembling on the host: an evaluation of as370

`cobc370` emits S/370 assembler, and today that assembler has to go to the
guest. Every one of the 45 regression tests submits a job that assembles, links
and runs, which is why a full pass takes ten minutes, and why someone who
clones this repository without a mainframe can only get as far as text.

[cc370](https://github.com/mvslovers/cc370) is a host-side cross-toolchain for
MVS 3.8j -- a GCC 3.4.6 fork targeting `i370-ibm-mvspdp`, plus **`as370`**
(assembler, "byte-identical to IBM's IFOX00"), **`ld370`** (linker, replacing
IEWL), `ar370`, `xmit370` and `file370`. GPL-2.0. Mike Großmann pointed it out
on H390-MVS after this compiler was announced there.

Two of those tools would move assembly and link-edit to the host: instant
diagnostics instead of a JES round trip, and a clone that reaches an object
deck with no MVS at all. This is what happened when that was tested.

## The question was macros

Generated code leans on nineteen IBM macros:

    DCB OPEN CLOSE GET PUT READ WRITE WAIT
    ACB RPL MODCB SHOWCB POINT ERASE
    SPIE WTO TIME ABEND GETMAIN

The VSAM ones are not gentle -- `ACB`, `RPL`, `MODCB` and `SHOWCB` are among the
denser conditional assembly IBM shipped. An assembler that cannot expand them is
no use here regardless of what else it does.

**It expands them.** `SYS1.MACLIB` came off the TK5 volume with `dasdpdsu` (747
members, MVS down), `as370` was pointed at it with `-I`, and all 48 generated
modules went through:

| | |
|---|---|
| assembled clean | **44** |
| flagged | 4, all for the same missing instruction |

Every VSAM module is in the clean 44.

## Three gaps, smallest first

**1. `ENTRY` with a list.** `ENTRY COBDISP,COBTERM,COBWRL,COBDATE` is rejected
with *"Symbol longer than 8 characters"*. IFOX00 accepts the list. One `ENTRY`
per line works, so this is a workaround rather than a blocker.

**2. `MP` is not in the opcode table.** *"Undefined operation code"*. It is the
only instruction in the whole corpus `as370` does not know -- `DP` and `MVO` are
both present, as are `ZAP AP CP SRP ED EDMK PACK UNPK CVB CVD`. This is what
flagged the other four modules.

**3. LD entries name the wrong control section.** In a module with more than one
CSECT, an `ENTRY`'s ESD item carries ESDID 1 rather than the CSECT the label is
actually in. Minimal reproduction:

    FIRST    CSECT              -> ESDID 1
    SECOND   CSECT              -> ESDID 2
             ENTRY MYENTRY
    MYENTRY  DS    0H
             BR    14
             END

`as370` records `MYENTRY`'s owning CSECT as 1; IFOX00 records 2.

## On "byte-identical to IFOX00"

Close, and closer than expected. The same source was assembled both ways -- by
`as370` on the host and by IFOX00 on the guest, its `SYSGO` kept and extracted
with `dasdseq` -- and the decks compared:

    2,519 of 2,560 bytes match          (a 32-card module)

Every one of the 41 differences traces to exactly two things:

- **gap 3 above**, which perturbs the ESD cards and, through them, the RLD
  cards that reference those ESDIDs;
- the **END card's translator-identification field**, where `as370` stamps
  `ASM3...` and IFOX00 stamps its own version. That one arguably *should*
  differ -- claiming to be IFOX00 would be worse than not.

So the claim is one bug away from being literally true.

## Where this leaves us

`as370` is close to usable and would be a real improvement to the build. It is
blocked today on `MP`, which four tests need, and on gap 3, whose effect on
linking is **untested** -- `ld370` has not been run and no linked module has
been executed. Until that is done it is not known whether the wrong ESDID breaks
resolution or merely looks wrong.

Licence note: cc370 is GPL-2.0 and this project is MIT. Invoking `as370` as an
external tool is ordinary tool use and does not affect that, the same as
compiling with GCC. Vendoring any of its source would be a different matter and
is not proposed.
