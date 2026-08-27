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

## Does it link, and does it run? Yes.

Gap 3 sounds alarming and turns out not to matter. The object deck `as370`
produced on the host was carried to the guest and link-edited by IBM's own
IEWL:

    IEBGENER  RC= 0000        the deck, off tape into a dataset
    IEWL      RC= 0000        no diagnostics at all
    HELLO FROM COBC. NO SYS1.COBLIB HERE.

**The module ran.** IEWL resolves external references by *name*, so an LD entry
naming the wrong owning CSECT costs nothing at link time. The bug is real and
should be fixed -- it is what stops the object deck being byte-identical -- but
it is cosmetic in effect, not semantic.

`ld370` links correctly too, at least as far as can be seen from the host: it
produced a 2341-byte load module from the same deck and resolved COBDISP and
COBTERM to the right addresses (0002C0, 000338), by name, exactly as IEWL did.

## What is still untested: getting ld370's module onto 3.8j

Running `ld370`'s *own* load module was not achieved, and the obstacle is
transport rather than the linker.

- `-xmit` produces a TSO TRANSMIT/NETDATA file. The `RECEIVE` command on this
  system rejects `INDDNAME(...)` and `NOVOLUME` through IKJPARS, has no HELP
  member, and rejects `INDSNAME('...')` as invalid command syntax. Its actual
  operands are undetermined.
- `-iebcopy` produces an unloaded-PDS image, which IEBCOPY LOAD wants as
  RECFM=VS. The host tape tooling here writes fixed-length records only, and
  the file does not carry RDWs of its own.

So `ld370` is unproven on this system, and nothing observed suggests it is at
fault.

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

`as370` is usable for this compiler's output today, bar one missing instruction.
It is blocked on `MP`, which four tests need; gaps 1 and 3 have workarounds or
no practical effect. Moving assembly to the host would cut the regression loop
substantially and let a clone reach a linkable object deck with no mainframe at
all -- MVS would be needed only to run.

Licence note: cc370 is GPL-2.0 and this project is MIT. Invoking `as370` as an
external tool is ordinary tool use and does not affect that, the same as
compiling with GCC. Vendoring any of its source would be a different matter and
is not proposed.
