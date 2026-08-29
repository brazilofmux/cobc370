# What the compiler actually has to accept

Measured across the 30 programs of a private production corpus, which
are the specification: they compile under IKFCBL00 today and produce reports
that match the GnuCOBOL and C++ implementations byte for byte.

Target dialect is **COBOL-74 or a little earlier** — explicitly *not* the modern
standard — plus the two things ANS COBOL lacks and the user wants: **VSAM** and
**dynamic CALL**.

Raw counts in `survey-raw.txt`.

## The headline: the subset is small

21 verbs, in frequency order:

    MOVE 457   PERFORM 204   IF 130   EXIT 128   ADD 57   CALL 43   READ 40
    GO 36      COMPUTE 35    DISPLAY 35   GENERATE 33   OPEN 31   SUBTRACT 29
    CLOSE 24   STOP 19       WRITE 19     INITIATE 6    TERMINATE 6
    DIVIDE 4   SEARCH 4      MULTIPLY 2

**Not used at all**, and therefore not needed for v1: `SORT` (sorting is done
by external JCL steps, not in-program), `STRING`, `UNSTRING`, `INSPECT`,
`EXAMINE`, `TRANSFORM`, `ALTER`, `RELEASE`, `RETURN`, `REWRITE`, `START`,
`SEEK`, `SET`, `CANCEL`, `ACCEPT`, `DELETE`, `USE`, `ENTER`.

`PERFORM` is the classic 1970s paragraph style: **113 `PERFORM ... THRU`**, 90
plain, and no `VARYING` or `TIMES`.

**Correction (found during the Report Writer slice):** this originally reported
"exactly one `UNTIL`". That was a classifier bug, not a fact — the counter
tested `THRU` first and returned, so every `PERFORM ... THRU ... UNTIL` was
filed under THRU. **`UNTIL` appears 62 times.** `PERFORM range THRU exit UNTIL
condition` is the corpus's normal loop, and implementing it is now a
prerequisite for compiling any real program, not an optional extra.

Likewise `GOBACK` appears **21** times against 19 `STOP RUN`, so it is the more
common way these programs end. The 128 `EXIT`
statements are the paragraph terminators that style requires. So the control
flow the code generator must handle is paragraph ranges and `GO TO`, not
structured loops.

## Report Writer — used, but only the easy half

Six programs (GL022, GL023, GL030, GL036 and two others) use the real feature,
not just "programs that print". What they use:

    RD  name
        PAGE LIMIT IS n LINES
        HEADING n
        FIRST DETAIL n
        LAST DETAIL n.

    01  group TYPE PAGE HEADING.        (6 groups)
    01  group TYPE DETAIL.              (26 groups)
        02  LINE n.  /  LINE PLUS n.    (45 occurrences)
            05  COLUMN n PIC ... SOURCE identifier.
            05  COLUMN n PIC ... VALUE 'literal'.

    INITIATE / GENERATE / TERMINATE

**Every hard Report Writer feature is unused** — zero occurrences of
`CONTROL IS` / `CONTROLS ARE`, `TYPE CONTROL HEADING`, `TYPE CONTROL FOOTING`,
`TYPE REPORT HEADING`/`FOOTING`, `TYPE PAGE FOOTING`, `SUM` counters,
`GROUP INDICATE`, `NEXT GROUP`, `RESET ON`, `DECLARATIVES`, or
`USE BEFORE REPORTING`. Control breaks and totals are done by hand in the
Procedure Division.

That is roughly the cheap 30% of Report Writer: a page manager that tracks the
line counter against `PAGE LIMIT`/`FIRST DETAIL`/`LAST DETAIL`, emits the page
heading group on overflow, and renders groups by positioning `COLUMN` fields on
`LINE`/`LINE PLUS` lines with `SOURCE`/`VALUE` content. No control hierarchy, no
sum accumulation, no declaratives. Very tractable.

Note the clause spellings are the short forms — `LINE PLUS 1`, `COLUMN 13`,
`SOURCE WS-COMPANY-NAME` — not `LINE NUMBER IS`, `COLUMN NUMBER IS`,
`SOURCE IS`. The scanner must accept both, but only the short forms appear here.

## Data division

841 subordinate level entries. Clauses actually used: **`REDEFINES` 8,
`OCCURS` 7, `SYNCHRONIZED` 2**. No `OCCURS DEPENDING ON`, no `RENAMES`, no
`JUSTIFIED`, no `BLANK WHEN ZERO`, no `SIGN IS`.

That is a relief for the SSA/aliasing question: only 8 `REDEFINES` and 7
`OCCURS` in the whole corpus, none of them variable-length. Aliasing is present
but bounded, and does not need a general solution to get started.

USAGE: **`COMP` 177**, `COMP-3` 12, explicit `DISPLAY` 1 (the rest implicit).
Binary dominates — mostly `PIC S9(8) COMP` working fields.

PICTURE categories:

    X (alphanumeric)                     435
    9 (unsigned integer)                 355
    S9 (signed integer)                  113
    9..V9 (unsigned decimal)              61
    S9..V9 (signed decimal)               30
    edited (Z , . - + CR DB * $ B 0 /)    23

Only 23 edited pictures, all in report output — which is where `ED` and the
PICTURE editor matter, and confirms it is a bounded piece of runtime rather than
a pervasive one.

## Files

    ACCESS IS SEQUENTIAL   49      ASSIGN TO UT-S-xxxxx   54    (QSAM)
    ACCESS IS RANDOM        3      ASSIGN TO DA-I-xxxxx    5    (ISAM)
    RECORD KEY              5
    NOMINAL KEY             5

Sequential QSAM plus random ISAM by `NOMINAL KEY` — the ANS COBOL ISAM idiom.
21 of 30 programs have a FILE SECTION; 15 have a LINKAGE SECTION (the called
subprograms).

## CALL

**All 43 CALL statements target `DYNALOAD`.** There is not one static call to
another COBOL program in the corpus — every inter-module call goes through the
assembler shim, because ANS COBOL has no dynamic sub-modules.

This is the clearest single argument for the replacement. A compiler with native
dynamic CALL lets all 43 sites become ordinary `CALL identifier USING ...` and
`DYNALOAD` disappears.

## What v1 must implement

Front end: fixed-format source, 21 verbs, paragraph `PERFORM THRU` and `GO TO`,
the data clauses above, and the Report Writer subset. No `SORT`, no string
handling, no `ALTER`, no declaratives.

Runtime: QSAM sequential, ISAM random by nominal key, the PICTURE editor for 23
edited fields, packed and binary decimal arithmetic, `DISPLAY`, and OS-linkage
`CALL` — plus the two additions that motivate the project, VSAM and native
dynamic CALL.

That is a genuinely small compiler. The corpus is the acceptance test, and the
GnuCOBOL build of the same programs is the oracle.
