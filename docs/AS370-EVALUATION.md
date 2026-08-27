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
modules went through: 44 produced no diagnostic, 4 were flagged for the same
missing instruction. Every VSAM module is among the 44.

> **Corrected 2026-08-27.** "44 assembled clean" was the wrong claim, and the
> distinction matters: they assembled *without diagnostics*, which is not the
> same as correctly. See "The number that was wrong" below. The honest figure
> is **2**.

## The number that was wrong

Reporting the missing `MP` upstream produced a finding worth more than the bug
did. Mike Grossmann looked before fixing and found that the DC/DS handler has no
default branch, so the types `P Z E L S Q` **assemble to zero bytes with no
diagnostic** and never advance the location counter. Every symbol after one of
them in the same CSECT shifts.

That reaches directly into the figure above. Counting what this compiler emits
across the 48 modules:

    H 2552    A 1608    C  881
    P  659  <-- zero bytes
    X  556    F  513    D  140    V   94
    Z   16  <-- zero bytes

**46 of the 48 contain `P` or `Z`.** Only `hello` and `stoprun` do not. So of
the 44 that produced no diagnostic, 42 were quietly wrong -- which is a worse
outcome than the four that failed loudly.

And it narrows the end-to-end result below: the module that was link-edited and
executed is `hello`, one of those two. The chain does work; the sample did not
touch this bug. That was luck rather than coverage, and it is worth saying so.

The general lesson is the one this project keeps relearning. *Assembled without
complaint* is not *assembled correctly*, in the same way that *ran to
completion* is not *produced the right answer*, and *linked* is not *resolved to
the right addresses*. Each time, the only thing that settled it was comparing
against an independent implementation.

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

## Does it link, and does it run? Yes.

Gap 3 sounds alarming and turns out not to matter. The object deck `as370`
produced on the host was carried to the guest and link-edited by IBM's own
IEWL:

    IEBGENER  RC= 0000        the deck, off tape into a dataset
    IEWL      RC= 0000        no diagnostics at all
    HELLO FROM COBC. NO SYS1.COBLIB HERE.

**The module ran.** IEWL resolves external references by *name*, so the wrong
owning CSECT on an LD entry cost nothing *for this module*.

> **Corrected 2026-08-27.** This originally concluded "cosmetic in effect, not
> semantic", and that was over-read from one sample. Mike Grossmann showed the
> same root cause -- a symbol's owning section not being consulted -- reaches
> the RLD as well. An ordinary label carries no ESDID of its own, so a
> relocation targeting one falls back to the *current* section:
>
>     FIRST    CSECT
>     PTR      DC    A(LBL2)
>     SECOND   CSECT
>     LBL2     DS    0H
>
> emits `R=1 P=1` where IFOX00 records `R=2`. That is a wrong relocation ESDID,
> not a naming detail. It stayed invisible in both corpora because every
> cross-section adcon in them targets a CSECT name, which does have an ESDID.
>
> The evidence was already here: the 41 differing bytes were reported as "this
> field, the RLD entries that reference the affected ESDIDs, and the END card
> IDR". The RLD entries were in that sentence and were read as a consequence of
> the ESD rather than as a second instance of the same fault.

`ld370` links correctly too, at least as far as can be seen from the host: it
produced a 2341-byte load module from the same deck and resolved COBDISP and
COBTERM to the right addresses (0002C0, 000338), by name, exactly as IEWL did.

## The whole chain, on the host

`ld370`'s own load module runs. The complete build happens on the Mac and MVS
is needed only to execute:

| step | where | whose |
|---|---|---|
| `cobc370` COBOL to S/370 assembler | host | this project |
| `as370` assembler to object deck | host | cc370 |
| `ld370` object deck to load module + XMIT | host | cc370 |
| `RECV370` XMIT into a load library | guest | RECVXMIT |
| execute | guest | |

    LDGO       S1                  IEBGENER  RC= 0000
    LDGO       S2                  RECV370   RC= 0000
    LDGO       S3                  HELLO     RC= 0000

    HELLO FROM COBC. NO SYS1.COBLIB HERE.
    SECOND LINE, WITH A QUOTE: DON'T PANIC.

**No IFOX00 and no IEWL were involved.**

Two things stood in the way, and neither was `ld370`.

**It is not a TSO `RECEIVE`.** MVS 3.8j has no such command; what TK5 ships is
Larry Belmontes' RECVXMIT package, whose receiver is a *batch program*,
`PGM=RECV370`, taking `XMITIN` and `SYSUT2`. Every `RECEIVE` operand tried
against IKJPARS was rejected because the command being invoked was something
else entirely. The readme in `Packages/RECVXMIT_V0R9M02.zip` carries the proc.

**Then S047.** `RECV370` is APF-authorized, and coding an explicit
`//STEPLIB DD DSN=SYS2.LINKLIB` de-authorizes the step -- an unauthorized
concatenation poisons the whole task. Dropping the STEPLIB and letting the
linklist find it is the fix.

### The transport lesson, which is general

Binary reaches MVS 3.8j by **tape**, not FTP. The runbook's "text mode always,
never binary" for FTPD is not a preference. What worked:

    maketape INPUT: file VOLSER: X DATASET: D.NAME \
             OUTPUT: t.aws LRECL: 80 BLOCK: 1 BINARY

with a **standard label** -- `NLTAPE` discards the VOLSER and leaves MVS issuing
a mount it cannot satisfy -- and a DD naming the label's dataset, or OPEN fails
813-04 against the generated temp name. Cancelling a job that is mount-pending
unloads the drive in Hercules, so the tape has to be re-mounted before the next
attempt.

## Where this leaves us

The toolchain works for this compiler's output today, bar one missing
instruction. `MP` blocks four tests; gaps 1 and 3 have workarounds or no
practical effect, and gap 3 in particular costs nothing at link time.

Moving assembly and link-edit to the host would cut the regression loop
substantially -- a JES round trip per test becomes milliseconds -- and let a
clone reach a runnable load module with no mainframe at all. MVS would be
needed only to run the result, which is the one thing it must be there for.

Licence note: cc370 is GPL-2.0 and this project is MIT. Invoking `as370` as an
external tool is ordinary tool use and does not affect that, the same as
compiling with GCC. Vendoring any of its source would be a different matter and
is not proposed.
