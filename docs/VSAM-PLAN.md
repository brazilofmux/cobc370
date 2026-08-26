# VSAM: plan and reference

## Why this is different from every slice so far

ANS COBOL has **no VSAM at all** — that is why `SVD001.VSAMIO` exists. So unlike
ISAM, LINKAGE, or Report Writer, there is no ANS COBOL implementation to diff
against. Native VSAM in cobc370 is not a reimplementation; it is a capability
the original compiler never had.

## The reference, and it is a good one

Jay Moseley's VSAMIO package, already on the system:

- `SVD001.VSAMIO.SOURCE(VSAMIOS)` — 670 lines of assembler, a working VSAM
  engine for MVS 3.8j. `SVD001.VSAMIO.OBJECT(VSAMIO)` is it pre-assembled.
- `VSAMIO` / `VSAMIOFB` — copybooks defining the parameter block.
- Seventeen COBOL programs driving it, and 28 JCL decks in `.CNTL`.

Those programs **are** the reference: they exercise VSAM through the assembler
routine, so whatever they print is what a natively-compiled equivalent must
print. `bin/vsam-test <member>` runs one, adapting only the job card.

Established so far: `VSTEST01` builds the input, `VSTESTK1` defines
`SVD001.VSTESTKS.CLUSTER`, `VSTESTK2` loads 100 records, `VSTESTK3` reads them
back. All clean.

## The ladder

The KSDS decks map exactly onto the order to build this in:

| Deck | Program | Exercises |
|---|---|---|
| K1 | IDCAMS | DEFINE CLUSTER |
| K2 | KSDSLOAD | sequential load — `OPEN OUTPUT`, `WRITE` |
| K3 | KSDSREAD | **sequential read — `OPEN INPUT`, `READ`** |
| K4 | KSDSUPDT | `OPEN I-O`, `REWRITE` |
| K5 | KSDSRAND | `ACCESS IS RANDOM`, `READ` by key |
| K6 | KSDSSSEQ | `START`, then sequential |

Then the same again for ESDS (`ORGANIZATION SEQUENTIAL`) and RRDS
(`ORGANIZATION RELATIVE`), which have their own decks.

## Mechanics, learned from VSAMIOS

    ACB   DDNAME=x,MACRF=(KEY,SEQ,IN),EXLST=exits
    RPL   ACB=acb,AREA=rec,AREALEN=n,OPTCD=(KEY,SEQ,NUP,MVE)
    EXLST EODAD=...,LERAD=...,SYNAD=...
    OPEN  (acb)        CLOSE (acb)
    GET   RPL=rpl      PUT RPL=rpl      ERASE RPL=rpl     POINT RPL=rpl
    SHOWCB RPL=rpl,FIELDS=(FDBK|RECLEN),AREA=,LENGTH=4

VSAMIOS builds its control blocks dynamically with `MODCB` because it serves any
file at run time. **A compiler does not need that**: organization, access and
mode are known at compile time, so the ACB and RPL can be assembled statically
with the right `MACRF`/`OPTCD`. That is a large simplification.

## Slice 1: KSDS sequential read

Target is `KSDSREAD` rewritten against native VSAM, producing byte-identical
output to the VSAMIO version.

Work list:

1. `FILE STATUS IS x` on SELECT — the first thing the compiler rejects.
2. `ORGANIZATION IS INDEXED` must distinguish VSAM from ISAM. The convention is
   the ASSIGN name: `DA-I-name` is ISAM, a bare or `AS-` name is VSAM.
3. Emit ACB, RPL and EXLST instead of a DCB.
4. `OPEN INPUT` / `CLOSE` against the ACB; `READ ... AT END` as `GET RPL=`.
5. Maintain FILE STATUS: `'00'` on success, `'10'` at end of file, and the
   VSAM feedback codes mapped for the error cases.

## Slice 1: DONE

`tests/ksdsnat.cbl` — KSDSREAD written against native VSAM — produces
**byte-identical output to the VSAMIO version**: 100 records, 102 lines, no
difference.

What it took:

- `FILE STATUS IS` on SELECT, resolved after the data division like RECORD KEY.
- VSAM told from ISAM by the ASSIGN name, as planned.
- `ACB DDNAME=x,MACRF=(KEY,SEQ,IN)` and
  `RPL ACB=,AREA=,AREALEN=,OPTCD=(KEY,SEQ,NUP,MVE)`.
- `OPEN (acb)` — no mode operand, VSAM takes it from MACRF — and `CLOSE (acb)`.
- `READ` as `GET RPL=`, with R15 tested for the AT END branch.
- FILE STATUS set from R15, and from the RPL feedback code via `SHOWCB` where
  end of data has to be told from a real failure.

**No exit list.** VSAMIOS uses `EXLST EODAD=,LERAD=,SYNAD=` because a callable
routine can branch straight out to its own return path. Generated inline code
cannot: an exit is entered on VSAM's terms and has to get back. Without an
EODAD, VSAM simply returns 8 in R15 with feedback 4, which is easier to test and
impossible to get wrong.

### The bug worth remembering

The first attempt abended S0C4 in what looked like OPEN. It was not OPEN — a
hand-written assembler probe opened the same cluster cleanly, and a generated
program with no FILE STATUS ran fine. The fault was in the status code itself:

    BZ    G0001
    L     8,BL0000          base loaded only on the failure path
    MVC   D0003(2),=C'30'
    B     G0002
    G0001 DS 0H
    MVC   D0003(2),=C'00'   base register never loaded on this path

`need_sym_base` tracks which base registers are live and emits nothing when it
thinks one already is. A label is a branch target, so that belief cannot survive
it — which the base-locator comment in the compiler has said all along.
`reset_bases()` at every label inside the status generator fixes it.

## Slice 2: KSDS load, DONE

`tests/ksdsnatl.cbl` -- KSDSLOAD written against native VSAM -- matches the
VSAMIO version byte for byte on both paths, and the records it writes read back
identical to the ones VSAMIO wrote.

What it took:

- `MACRF=(KEY,SEQ,OUT,RST)` when the program opens the file OUTPUT.
- `WRITE` as `PUT RPL=`, with INVALID KEY taken when R15 is not zero.
- FILE STATUS from the feedback code: 12 is `21`, 8 is `22`, 28 is `24`.
- `RECLEN` on the RPL. This is what the first attempt was missing, and every
  `PUT` failed with status `30` until it was there. `AREALEN` is how big the
  area is; `RECLEN` is how long the record in it actually is. VSAM fills RECLEN
  in itself on a GET, which is why slice 1 never needed it, but on a PUT it is
  the program's to supply.

### RST, and why the cluster is defined REUSE

COBOL's `OPEN OUTPUT` means *this program creates the file's contents*. VSAM
spells that `MACRF=RST`: the cluster is emptied at OPEN. It requires the cluster
to be defined REUSE.

The alternative -- DELETE and DEFINE the cluster before every load, which is
what Jay's own `VSTESTK1` does -- is catalog surgery, and see the incident
below for what that cost. With REUSE the whole VSAM ladder can be re-run any
number of times and the catalog is never written to again. `bin/cobc-regress`
now runs `ksdsnatl` then `ksdsnat` on every pass, and it is idempotent.

### Duplicate keys are sequence errors in load mode

Worth knowing before writing any test that expects otherwise. Fed an input
holding both a duplicate key and a key lower than its predecessor, VSAM
reported *both* as feedback 12, and Jay's KSDSLOAD classified both as
`VSIO-SEQUENCE-ERROR`. In load mode "not greater than the previous key" covers
duplicates, so there is nothing left for feedback 8 to mean. Status `21` for
both is therefore correct rather than a limitation. The mapping of 8 to `22`
stays in the table for the random-insert slice, where the two do separate.

### A compiler bug this slice flushed out

`IF WS-STATUS = '24'` would not compile: `parse_primary` tested
`is_numeric_literal` before it tested `tok.literal`, so a quoted literal made
of digits was typed as numeric. The quotes decide, not the characters between
them. This was waiting for any program that tests FILE STATUS -- which is to
say, most programs that use VSAM at all -- and no test in the suite had ever
written one.

## Slice 4: update in place, DONE

`tests/ksdsnatu.cbl` -- KSDSUPDT written against native VSAM -- matches the
VSAMIO version byte for byte, and the cluster it leaves behind is byte for byte
the one VSAMIO leaves behind: 99 records, four rewritten in place, one erased.

What it took:

- `OPEN I-O` as `MACRF=(KEY,SEQ,OUT)`. OUT, because IN is retrieval only --
  but *not* RST, since emptying a file the program means to update would be a
  spectacular way to misread the verb.
- `OPTCD=UPD` in place of NUP. This is the whole of the slice. Under UPD a GET
  does not merely return the record, it *holds* it, and the PUT or ERASE that
  follows acts on what is held rather than on a key. COBOL's rule that REWRITE
  and DELETE must follow a READ is not a rule the compiler has to enforce --
  it is what the access method already does.
- `REWRITE` as `PUT RPL=`, `DELETE` as `ERASE RPL=`. Neither macro takes a key.
- Feedback 8 is an attempt to change the prime key, which COBOL folds into `21`;
  16 is no such record, which is `23`.

Nothing about GET needed changing. The same `GET RPL=` that slice 1 emits
retrieves and holds a record once the RPL says UPD, which is a good sign that
the shape of the generated code is right.

## Slice 5: random by key, DONE

`tests/ksdsnatr.cbl` -- KSDSRAND written against native VSAM -- matches the
VSAMIO version line for line across every path it has, and leaves the same
cluster behind: 102 records, three inserted by key, one rewritten, one erased.

What it took:

- `ACCESS IS RANDOM` is `DIR` where sequential is `SEQ`, in both the ACB's
  MACRF and the RPL's OPTCD. That is the whole difference, and it is also why
  a random READ carries INVALID KEY rather than AT END: there is no end to
  reach when you asked for one particular record.
- `ARG` and `KEYLEN` on the RPL, plus `KEQ` inside OPTCD. ARG is the address of
  the search key, which lives inside the record area exactly where RECORD KEY
  says it does -- the same place VSAMIOS points it. KEQ is an OPTCD sub-option,
  not an RPL keyword of its own; the first attempt wrote it as one.
- **Two RPLs.** An I-O file that also inserts needs both: retrieval, rewrite
  and erase want UPD so the record is held, while an insert is by definition
  not an update and wants NUP. VSAMIOS flips the option with MODCB around every
  insert and flips it back, guarding against the flip failing. A compiler knows
  which verb it is emitting, so it can assemble both control blocks and pick
  between them -- no runtime call, and no window in which a failed MODCB leaves
  the RPL set wrong.

### Feedback 8 finally means something

Slice 2 recorded that a duplicate key during a *load* comes back as feedback 12,
indistinguishable from any other out-of-sequence key, and kept 8 in the table
"for the random-insert slice, where the two do separate". They do. Inserting a
key that already exists through a DIR request returns 8, the program printed
`*** DUPLICATE RECORD ON FILE`, and it matched the reference exactly. Feedback
16, no record with that key, is FILE STATUS `23`.

### A note on the oracles

The reference output has a blank line that lands in a different place from
ours, because a `DISPLAY ' '` fell on a page boundary and became part of the
eject. Content is identical; only the padding moved. `cobc-regress` already
drops blank lines before comparing -- carriage control is not content -- so the
oracle is the 25 non-blank lines.

## Slice 6: START, DONE

`tests/ksdsnats.cbl` -- KSDSSSEQ written against native VSAM -- matches the
VSAMIO version line for line: four positions, one of them a key that does not
exist, one a key that falls between two records, and five records read from
each that succeeded.

`START ... KEY IS EQUAL TO` is `OPTCD=KEQ`, `KEY IS NOT LESS THAN` is `KGE`,
and both are followed by `POINT RPL=`. The key named must be the RECORD KEY,
which is where the RPL's search argument already points, so there is nothing to
move and nothing to choose.

### The one place MODCB earns its keep

Slice 5 replaced VSAMIOS's runtime MODCB with two assembled RPLs, on the
grounds that a compiler knows which verb it is emitting. START is the exception,
and it is worth being precise about why: VSAM keeps position **per RPL**, so
the RPL that POINT positions has to be the same one the following GET reads
from. A second control block cannot stand in for a runtime change when the
thing being changed is the object whose state matters. So KEQ and KGE are set
with MODCB -- once per START, not once per record.

A MODCB that fails takes the INVALID KEY path, where the feedback decoded will
be whatever the RPL last held rather than a reason for this failure. That is
worth knowing and not worth machinery: MODCB against a control block the
assembler built cannot fail for anything a COBOL program can cause.

## The KSDS is finished

Every access pattern a COBOL program can ask of a key-sequenced dataset now
works and is checked against VSAMIO on every regression run:

| | verb | VSAM |
|---|---|---|
| read forwards | `READ` | `GET` SEQ |
| create | `OPEN OUTPUT`, `WRITE` | `PUT` SEQ, MACRF RST |
| update in place | `OPEN I-O`, `REWRITE`, `DELETE` | `PUT`/`ERASE`, OPTCD UPD |
| by key | `ACCESS IS RANDOM`, `READ`/`WRITE` | GET/PUT DIR, ARG |
| browse from a key | `START` | `POINT`, KEQ/KGE |

ESDS and RRDS are next, and both are narrower than any of the above: ESDS is
ADR addressing with no key at all, RRDS is a record number in place of one.

## DEFINE CLUSTER against UCSVD001 is not safe, and that is now the finding

Three of four `DEFINE CLUSTER` commands issued against the SVD001 user catalog
have taken it offline. Each reported complete success -- condition code 0,
allocation status 0 -- and the next job to touch the catalog failed with

    IEC331I 024-002,...,IORA,IGG0CLAG
    IEC333I P004,A0,181,CI=000003

Every time. The morning's guess that DELETE was the culprit is wrong: the second
and third failures were plain DEFINEs with no DELETE in front of them, one for a
NONINDEXED cluster and one job holding a NONINDEXED and a NUMBERED define
together. The one that survived was a single INDEXED define. That is not enough
to call it a pattern, and it is certainly not enough to build on.

**So the ESDS and RRDS slices are blocked on their fixtures, not on the
compiler.** The generated code for ESDS is written and compiles -- `ADR`
addressing in MACRF and OPTCD, `OPEN EXTEND` as OUT-without-RST, no ARG or
KEYLEN, ERASE refused because entry sequence is fixed once written -- but there
is nowhere safe to put a cluster to test it against.

### What to do instead

Stop using UCSVD001 for test fixtures. Put the VSAM test clusters in their own
user catalog on a scratch volume, so that a catalog casualty costs a
`DEFINE USERCATALOG` and not a DASD restore:

    DEFINE USERCATALOG (NAME(UCVSTEST) VOLUMES(WORK01) CYLINDERS(5 1))
    DEFINE ALIAS (NAME(VSTEST) RELATE(UCVSTEST))

then name the fixtures `VSTEST.KSDS.CLUSTER` and so on. The one thing to weigh
first: `DEFINE USERCATALOG` writes to the **master** catalog, which is on
`tk5cat.391` and is not covered by `dasd.checkpoint-20260826`. Clone that volume
before trying it.

Until then: the KSDS cluster that exists works, is REUSE, and is captured in the
checkpoint. Do not define anything else against UCSVD001.

## The rig wedged in the middle of this, and it was our own doing

Halfway through slice 4 every submission started hanging. The visible symptom
was Hercules refusing the card reader:

    HHC01038E 0:000C COMM: client localhost ... rejected: client ... still connected

which looks like a Hercules problem and is not. `$DQ` told the truth:

    $HASP000 1009 PPU LOCAL    ANY
    $HASP000  71 PERCENT SPOOL UTILIZATION
    $HASP050 JES2 RESOURCE SHORTAGE.   CODE = JQES

`tk5-run` captured each job's output off PRINTER2 and then walked away, so JES2
kept a job queue element for every job it had ever run. A thousand runs later
the queue was full, JES2 stopped creating jobs, the reader never consumed the
deck it had been fed, the socket that fed it was never released, and every
later submission bounced off the stale connection.

`$PJ1-9999` cleared it -- 1009 jobs to 15, 71 percent spool to 0 -- and
`devinit 000C 3505 sockdev ascii trunc eof` released the socket. `tk5-run` now
purges each job as it finishes, which is one console command and means it
cannot recur. Worth knowing that the shortage message appears **once**, hours
before anything visibly breaks, and never again.

## The incident: UCSVD001, 2026-08-26

Running Jay's `VSTESTK1` (DELETE + DEFINE of the test cluster) took the
`SVD001` VSAM user catalog offline. The job itself reported complete success --
`IDC0550I ... DELETED`, allocation status 0, maximum condition code 0 -- and
every job after it failed:

    IEC331I 024-002,...,IORA,IGG0CLAG
    IEC333I P004,A0,181,CI=000003
    IEC161I 036(024,002,IGG0CLAG)-001,...,UCSVD001
    IEF361I ... UNABLE TO ALLOCATE / OPEN PRIVATE CATALOG OR ALLOCATE CVOL

An error reading control interval 3 of the catalog. A clean shutdown and re-IPL
did not clear it, so it was on disk rather than in storage. Everything named
`SVD001.*` is cataloged there, so the monthly pipeline could not allocate its
inputs by name.

**Nothing was lost.** What made that clear, in order:

1. `IEHLIST LISTVTOC` on SVD002/003/004 -- reads the VTOC, needs no catalog --
   showed all 27 datasets present.
2. `IEBPTPCH` on `SVD001.DEFTLY.COBOL(GL022)` with an explicit
   `UNIT=3380,VOL=SER=SVD003` read it, RC=0000. Physically intact and
   reachable.
3. `LISTCAT ENTRIES(UCSVD001)` against the *master* catalog answered normally,
   with the right VOLSER and VOLFLAG. Only the user catalog's own contents were
   unreachable.

Recovery, about ten minutes:

1. `tk5-down`.
2. `cp -c dasd/svd001.3380e dasd/svd001.3380e.broken-20260826` -- keep the
   broken image, so the whole thing is reversible.
3. `cp -c dasd.post-intercomm-clone/svd001.3380e dasd/` (2026-08-21).
4. `tk5-up`, then confirm with a LISTCAT and an IEFBR14 that allocates a few
   `SVD001.*` datasets by name.
5. `LISTCAT` every dataset the VTOCs listed to find what the older catalog did
   not know about. Two: `SVD001.COBC370.LOADLIB` and `SVD001.VSAMTEST.DATA`.
6. `DEFINE NONVSAM (NAME(...) DEVICETYPES(3380) VOLUMES(SVD003))` for the
   first; the second is instream in `VSTESTK01` and was simply re-run.

The DEFINE that followed, without a DELETE in front of it, was clean -- no
`IEC331I` at all, where the DELETE had emitted two. That is suggestive but not
proof, and there is no intention of proving it.

**The standing rule that comes out of this: do not DELETE/DEFINE against
UCSVD001.** Test clusters are defined once, REUSE, and emptied by `OPEN
OUTPUT`. If a cluster ever genuinely has to be redefined, take a DASD clone
first.

## Scale

This is bigger than the ISAM slice. SELECT grows several clauses, the verb set
grows `REWRITE`, `DELETE` and `START`, and FILE STATUS is a cross-cutting
concern ISAM never needed. Several sessions.
