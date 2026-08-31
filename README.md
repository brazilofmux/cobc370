# cobc370

A COBOL-74 compiler for MVS 3.8j. It runs on the host, reads COBOL, and emits
S/370 assembler for the guest to assemble and link. The generated module is
self-contained: the runtime it needs is emitted into it, and `SYS1.COBLIB` is
never referenced.

The reason it exists is VSAM. **IBM's ANS COBOL, the compiler MVS 3.8j actually
ships, cannot open a VSAM file at all** -- a program on that system reaches VSAM
only through a hand-written assembler shim. `cobc370` supports all three
organizations natively, and every VSAM test in this repository is checked
against such a shim to prove it behaves the same.

It targets COBOL-74 -- ANSI X3.23-1974, the standard of the machine's own era --
plus the IBM spellings the programs of that era used, and it stops there.
COBOL-85 constructs are refused, on purpose. It is not a general-purpose COBOL
compiler and does not try to be.

## State

Feature work closed on 2026-08-29; the Report Writer was completed on the
30th. Against the twelve modules of the 1974 standard, checked element by
element against the standard's own lists:

| module | level |
|---|---|
| Nucleus, Table Handling, Sequential I-O | **Level 2, complete** |
| Relative I-O, Inter-Program Communication, Library | **Level 2, complete** |
| Indexed I-O | **Level 2, complete** (alternate keys read under `OPEN INPUT`; see below) |
| Segmentation | Level 1 |
| Report Writer | **Level 1, complete** (its only level) |
| Sort-Merge, Debug, Communication | not implemented -- each has a null level, which conforms |

`docs/COBOL74-CONFORMANCE.md` is the element-by-element map;
`docs/COBOL74-ROADMAP.md` is the plan that closed it and what was left out, and
why.

Verified by compiling a real production workload: a corpus of 30 programs,
all of which compile, and whose **monthly general-ledger batch -- 37 steps, 18
of them COBOL -- prints every one of its 1,631 figures identical to the output
of IBM's own compiler.** Load modules are smaller -- about half the size in
aggregate on the measurement in `docs/MEASUREMENTS.md`, a third on the later
full build -- and, after the optimization pass, the CPU time of the COBOL steps
is at IBM's (0.6s either way, at the noise floor of the step accounting).

114 regression tests, all green, every one diffed against an oracle -- and
for the Report Writer the oracle is the 1974 text itself, hand-derived from
its presentation-rule tables, with IBM's own compiler run on the same
source wherever its 1968-vintage Report Writer reaches.

| area | supported |
|---|---|
| data | `DISPLAY`, `COMP` (to 9 digits), `COMP-3`, `INDEX`; edited pictures; `REDEFINES`, `RENAMES`, `OCCURS` to three levels with `INDEXED BY`, `KEY`, and `DEPENDING ON`; `SIGN`, `SYNCHRONIZED`, `JUSTIFIED`, `BLANK WHEN ZERO`; levels 01-49, 66, 77, 88; `OF`/`IN` qualification; `CURRENCY SIGN`, `DECIMAL-POINT IS COMMA`; up to 18 digits |
| verbs | `MOVE` (with `CORRESPONDING`), `ADD SUBTRACT MULTIPLY DIVIDE COMPUTE` with `GIVING`, `ROUNDED`, `REMAINDER`, `ON SIZE ERROR`, `**`; `IF` with class, sign, condition-name and abbreviated conditions; `PERFORM` (`TIMES`, `UNTIL`, `VARYING ... AFTER ... AFTER`, `THRU`); `GO TO` (`DEPENDING ON`, `ALTER`); `SEARCH` and `SEARCH ALL`; `SET`; `STRING`, `UNSTRING`, `INSPECT`; `DISPLAY`/`ACCEPT` with `UPON`/`FROM` and `DATE`/`DAY`/`TIME`; `CALL literal` and `CALL identifier` with `USING`, `CANCEL`, `EXIT PROGRAM`, `GOBACK`; `COPY ... REPLACING` (host side, `-I`) |
| QSAM | sequential read, write, rewrite; fixed and variable-length records (`RECORDING MODE`, `RECORD CONTAINS m TO n`), blocked and unblocked, `BLOCK CONTAINS 0`; `OPTIONAL`, `EXTEND`, `WRITE ... ADVANCING` with ASA carriage control, `LINAGE` with `END-OF-PAGE`, `USE` declaratives |
| ISAM | QISAM load and sequential read, BISAM random read |
| **VSAM KSDS** | read, load, update in place, read/write/delete by key, `START`, `ACCESS IS DYNAMIC` (with `OPEN I-O` too); `ALTERNATE RECORD KEY ... WITH DUPLICATES` on VSAM alternate indexes and paths -- `READ`/`START ... KEY IS` an alternate, the key of reference for `READ NEXT`, statuses `02`/`22`/`23` |
| **VSAM ESDS** | read, load, update in place, extend |
| **VSAM RRDS** | read, load, read/write/delete by record number, `START` |
| reports | Report Writer entire: `RD` with `CONTROL`, `PAGE` and `CODE`; all seven group `TYPE`s; `LINE` (absolute, `PLUS`, `NEXT PAGE`), `NEXT GROUP`, `COLUMN`, `SOURCE`, `VALUE`, `SUM ... UPON ... RESET`, `GROUP INDICATE`, `JUSTIFIED`, `BLANK WHEN ZERO`; `LINE-COUNTER`/`PAGE-COUNTER`; `INITIATE`, `GENERATE` (detail or summary), `TERMINATE`, `USE BEFORE REPORTING`, `SUPPRESS`; presented by the standard's tables, not an approximation of them |

What is deliberately not there, each refused with a message that says so:

- Reading by an `ALTERNATE RECORD KEY` in a program that opens the file
  `I-O`. This VSAM will not have a base cluster and its paths open together
  while the base is open for output, so updates go by the prime key (VSAM
  maintains the alternate indexes) and alternate-key reads need `OPEN
  INPUT` -- two opens, or two programs. The roadmap records the probes.
- `COMP` past nine digits -- doubleword binary on a machine with no 64-bit
  arithmetic. Everything else computes in packed decimal to 18 digits.
- Sort-Merge, Debug, Communication; and every COBOL-85 spelling.

## When a program checks

A program that hits bad packed data abends S0C7 and says nothing about where.
That is true of IBM's ANS COBOL too -- measured, not assumed. This one says:

    COBC370: PROGRAM CHECK 0C7 LINE 00012 OFFSET 0003F8

A label per statement and a table pairing them with source lines let a SPIE exit
turn the interrupt address into a line number. It costs about four bytes per
statement plus a 250-byte exit, armed once per load module, and the message goes
to the job log beside the `IEF472I` that reports the abend. The completion code
becomes U3007 -- 3000 plus the interruption code -- and `docs/MEASUREMENTS.md`
explains why it is not S0C7 and what was tried.

`cobc370 -s program.cbl` removes all of it.

## Building

    cd src && make cobc370

No dependencies beyond a C compiler. Ragel is needed only to regenerate
`picture_scan.c` from `picture.rl`; the generated file is committed.

## Compiling a program

    src/cobc370 program.cbl -o program.asm
    src/cobc370 program.cbl -o program.asm -I copybooks

The output assembles with `ASMFCLG` on the guest. `COPY` members are found on
the `-I` directories or beside the program; nothing but the copied text reaches
the guest.

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

Individual tests: `bin/cobc-regress arith redef`. The sweep also runs the
static-call, dynamic-call and ISAM round trips in `bin/`.

## How correctness is established

Not by inspection. Every test is diffed against an oracle produced by an
independent implementation:

- **Non-VSAM tests** are checked against IBM's ANS COBOL compiling the same
  source, or against GnuCOBOL where the construct is portable.
- **VSAM tests** are checked against Jay Moseley's VSAMIO, an assembler engine
  that drives VSAM directly. ANS COBOL has no VSAM, so there is no other
  reference. For every test that changes a cluster the contents are read back
  and compared as well -- messages matching is not enough when the point is
  what got written.

  One test has no VSAMIO counterpart, and the README says so rather than
  implying otherwise: VSAMIO cannot express `ACCESS IS DYNAMIC` in the sense
  COBOL means it. That one is checked against cluster contents already verified
  against VSAMIO. `docs/VSAM-PLAN.md`, slice 10.

The oracle is not the authority; the 1974 text is. Four times the two
disagreed, the standard was read, and the compiler followed it against
GnuCOBOL: the reset order of `PERFORM VARYING ... AFTER`, `SEARCH VARYING` an
integer item, the receiving side of `OCCURS DEPENDING ON`, and `DISPLAY` of an
implied decimal point.

`docs/DIFFERENTIAL-TESTING.md` explains why this matters: six real bugs came out
of running against production data and JCL that the unit tests could not have
found, including a COMP alignment error that silently selected zero of 24,525
transactions and still returned RC=0000.

## Performance

`bench/run.sh` runs nine constructs a million times each under both compilers
and reads CPU seconds from the step accounting. After the optimization pass
every primitive is at or ahead of IBM's: packed add and compare level,
`COMP` add 0.02s against 0.03s, `COMPUTE` 0.39s against 0.53s, a `CALL` 0.09s
against 0.17s, an unsigned zoned compare five times faster. The table, and
what each change was, are under Optimization in `docs/COBOL74-ROADMAP.md`.

## Layout

    src/     the compiler: one C file, plus a Ragel scanner for PICTURE
    tests/   129 COBOL programs and their oracles
    bin/     the regression harness, the three round-trip checks, and
             cobc-ccvs to run the NIST CCVS-85 corpus through the front end
    bench/   the micro-benchmarks
    docs/    COBOL74-CONFORMANCE.md -- where this compiler sits against the
             twelve modules of ANSI X3.23-1974; COBOL74-ROADMAP.md -- the plan
             that closed the language, and the optimization record; the
             development log, differential-testing notes, measurements, VSAM
             and ISAM design notes

## License

MIT. See `LICENSE`.

The test fixtures under `tests/data/` are action cards from Jay Moseley's VSAMIO
test suite, included so the tests stand alone; that package is his work and is
not redistributed here.

## Credits

Jay Moseley's VSAMIO package is the reference implementation the VSAM support is
verified against, and reading it answered questions no manual did. The test
fixtures under `tests/data/` are action cards from his test suite; the package
itself is not redistributed here.

The TK5 distribution by Rob Prins, and TK4- by Juergen Winkelmann and TK3 by
Volker Bandke before it, are what make running MVS 3.8j practical at all.
