# cobc370

A COBOL-74 compiler for MVS 3.8j. It runs on the host, reads COBOL, and emits
S/370 assembler for the guest to assemble and link.

The reason it exists is VSAM. **IBM's ANS COBOL, the compiler MVS 3.8j actually
ships, cannot open a VSAM file at all** -- a program on that system reaches VSAM
only through a hand-written assembler shim. `cobc370` supports all three
organizations natively, and every VSAM test in this repository is checked
against such a shim to prove it behaves the same.

It is not a general-purpose COBOL compiler and does not try to be. It targets
COBOL-74 and earlier, because that is the language MVS 3.8j programs are written
in.

## State

Verified by compiling a real production workload: **18 COBOL programs making up
a 37-step monthly general-ledger batch run, reproduced byte-for-byte against the
output of IBM's own compiler.** Generated modules are roughly half the size of
ANS COBOL's for the same source.

40 regression tests, all green.

| area | supported |
|---|---|
| data | DISPLAY, COMP, COMP-3, edited pictures, REDEFINES, OCCURS, INDEXED BY, 88 levels, qualification |
| verbs | MOVE, arithmetic with GIVING/ROUNDED, IF, PERFORM (TIMES/UNTIL/VARYING/THRU), GO TO, SEARCH and SEARCH ALL, CALL, DISPLAY, ACCEPT |
| QSAM | sequential read and write, blocked and unblocked |
| ISAM | QISAM load and sequential read, BISAM random read |
| **VSAM KSDS** | read, load, update in place, read/write by key, START |
| **VSAM ESDS** | read, load, update in place, extend |
| **VSAM RRDS** | read, load, read/write by record number |
| reports | Report Writer: RD, page and control breaks, SUM |

Known gaps, all deliberate: no `ACCESS IS DYNAMIC`, no ASA carriage control, no
dynamic `CALL identifier`, and no equivalent of ANS COBOL's `ILBOERR0` -- on
malformed packed data ANS diagnoses and this abends. See
`docs/DEVELOPMENT-LOG.md`.

## Building

    cd src && make cobc370

No dependencies beyond a C compiler. Ragel is needed only to regenerate
`picture_scan.c` from `picture.rl`; the generated file is committed.

## Compiling a program

    src/cobc370 program.cbl -o program.asm

The output assembles with `ASMFCLG` on the guest. `SYS1.COBLIB` is never
referenced -- the runtime the generated code needs is emitted into the module.

## Running the tests

The tests compile on the host and then *run on a real MVS 3.8j guest*, because
the whole point is what the machine does with the code. This repository knows
nothing about how to start one. It delegates:

    export COBC370_RUNNER=/path/to/your-runner
    bin/cobc-regress

`COBC370_RUNNER` names a program taking two arguments -- a JCL deck and a file
to write the printed output to. It defaults to `tk5-run`, if one is on PATH.
Generated job cards carry `USER=HERC01,PASSWORD=@HERC01PW@`; substituting that
token is the runner's business, which is why no credential and no path to one
appears anywhere in this repository.

Individual tests: `bin/cobc-regress arith redef`.

## How correctness is established

Not by inspection. Every test is diffed against an oracle produced by an
independent implementation:

- **Non-VSAM tests** are checked against IBM's ANS COBOL compiling the same
  source, or against GnuCOBOL where the construct is portable.
- **VSAM tests** are checked against Jay Moseley's VSAMIO, an assembler engine
  that drives VSAM directly. ANS COBOL has no VSAM, so there is no other
  reference. Twelve programs, and for every one that changes a cluster the
  contents are read back and compared as well -- messages matching is not
  enough when the point is what got written.

`docs/DIFFERENTIAL-TESTING.md` explains why this matters: six real bugs came out
of running against production data and JCL that the unit tests could not have
found, including a COMP alignment error that silently selected zero of 24,525
transactions and still returned RC=0000.

## Layout

    src/     the compiler: one C file, plus a Ragel scanner for PICTURE
    tests/   47 COBOL programs and their oracles
    bin/     the regression harness
    docs/    development log, differential-testing notes, VSAM design notes

## Credits

Jay Moseley's VSAMIO package is the reference implementation the VSAM support is
verified against, and reading it answered questions no manual did. The test
fixtures under `tests/data/` are action cards from his test suite; the package
itself is not redistributed here.

The TK5 distribution by Rob Prins, and TK4- by Juergen Winkelmann and TK3 by
Volker Bandke before it, are what make running MVS 3.8j practical at all.
