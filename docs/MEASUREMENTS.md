# Measurements: cobc370 vs ANS COBOL

All figures from the real monthly BATCH job on real data, 2026-08-26.

## Wall clock

Unchanged: **0.04 minutes** either way, as the job accounting reports it. That
field has 0.01-minute resolution, so it cannot resolve anything under 0.6
seconds — "the same" here means "within 0.6s", not "identical".

## CPU, per step

The step accounting blocks carry centisecond resolution, which is fine enough
to see the difference.

| | ANS COBOL | cobc370 | delta |
|---|---|---|---|
| COBOL steps | 0.64s | 0.88s | **+0.24s (+37%)** |
| SORT and utilities | 1.16s | 1.13s | unchanged, as they must be |
| Total | 1.80s | 1.97s | +0.17s |

cobc370 spends about a third more CPU inside the COBOL steps. That is invisible
in the wall clock because **COBOL is a minority of the work** — roughly 0.6s of
COBOL inside a job whose time is dominated by sorts and I/O.

Caveats: 14 of the 18 COBOL steps were comparable (STEP3, STEP4, STEP12 and
STEP14 have accounting blocks in one capture and not the other). Individual
deltas of 0.02s are two ticks and are noisy; what makes the result believable is
that the direction is consistent across every step.

## Load module size

Decoded from `PDS2STOR` in each library's directory entries.

| | cobc370 | ANS COBOL | ratio |
|---|---|---|---|
| **18 modules, total** | **79,992** | **167,336** | **0.48x** |

Every module is smaller. Two are dramatically so — GL030 at 0.18x and GL036 at
0.20x, both around 34K under ANS COBOL against roughly 6K here — while the
closest, GL040, is still 0.86x.

### Why — settled with AMBLIST, and my first guess was wrong

I predicted the difference was ANS COBOL link-editing bulky `SYS1.COBLIB`
runtime into every module. `AMBLIST LISTLOAD ... OUTPUT=MAP` on both libraries
says otherwise. ANS's library routines are small; the difference is **generated
code**.

Comparing like with like — ANS folds WORKING-STORAGE into its single CSECT, so
ours is `program + COBWS`:

| | ANS code+data | cobc370 code+data | ratio | ANS runtime | cobc370 runtime |
|---|---|---|---|---|---|
| GL025 | 2,426 | 1,249 | 0.51x | 53 | 0 |
| GL040 | 10,744 | 9,920 | 0.92x | 1,845 | 848 |
| GL042 | 12,666 | 11,042 | 0.87x | 3,305 | 848 |
| GL030 | 30,342 | 5,088 | **0.17x** | 3,129 | 848 |
| GL036 | 30,764 | 5,658 | **0.18x** | 3,129 | 848 |

ANS's runtime is 53 to 3,305 bytes — `ILBODSP0` (DISPLAY), `ILBOSTP0` (STOP RUN),
`ILBOSPA0`, `ILBOSCH0` (SEARCH), `ILBOERR0`. Against our 848-byte COBRT that is
a couple of KB either way, nowhere near the 87K total gap.

Most programs are modestly smaller here (0.87–0.92x) or half. **GL030 and GL036
are the outliers**: IKFCBL00 generates about 30K where cobc370 generates about
5K for the same source. Why those two specifically is not established.

### The gap that was claimed here, and was not real

**Retracted.** This section used to say that `ILBOERR0` — 556 bytes linked into
ANS's GL030 and GL036 — meant ANS COBOL diagnoses bad numeric data where
cobc370 would "most likely take an S0C7 with no explanation". That was inferred
from a module map and never tested. It is wrong.

Tested since, with the same program compiled both ways: a `PIC S9(5) COMP-3`
field redefined over `'ABC'` and then added to. **Both abend S0C7, at the same
point, having printed the same preceding DISPLAY.** ANS COBOL says nothing
about the field, the statement or the line, and `ILBOERR0` is not even linked
into the module that does it. Whatever pulls that routine in, a data exception
is not it.

The lesson is the one the differential-testing note already makes: a module map
tells you what was linked, not what runs. Reading behaviour out of a link map is
the same kind of mistake as reading it out of source.

### What was actually missing, and now is not

Neither compiler told you *where*. `cobc370` now does:

    COBC370: PROGRAM CHECK 0C7 AT SOURCE LINE 00012

A label per statement and a table pairing those labels with source lines let a
SPIE exit turn the interrupt address into a line number. The cost is about four
bytes per statement plus a 250-byte exit; `-s` removes both and restores a bare
S0C7.

Two things are worth knowing about it. The completion code becomes **U3007** —
3000 plus the program interruption code — rather than S0C7, because the tidier
ending was tried and does not work: backing the resume address up by the
instruction length and cancelling SPIE, so the instruction checks again for
real, was attempted both before and after the cancel and in both cases the
program simply carried on and returned a meaningless code. Under 3.8j the PIE's
PSW does not appear to steer the resume. The message names the real code, which
is the part a programmer needs.

And the message goes to the **job log**, not SYSOUT, because WTO needs no DCB
and is safe from an interrupt exit — which also puts it next to the `IEF472I`
that reports the abend.

Smaller modules *and* slightly more CPU is not a contradiction: the generated
code does more work per operation, particularly round-tripping through packed
decimal, while carrying far less library baggage.

## Printed output

`batch-run --pdf` routes output to class A, which reaches PRINTER1 on device 00E
— the sockdev virtual1403 bridges into a green-bar PDF. Both builds were
rendered and **compared visually: the two PDFs look the same, with the same
positioning on the page.**

Two differences that are not what they appear:

- **98 PDF pages against 97.** The extra page is in the **job log**, not any
  report: the swap deck carries an extra STEPLIB card on each of 18 steps, so
  the JCL listing runs a page longer. Every report page range compared equal.
- **One extra print record** in the PROFIT LOSS section, 122 against 121. That
  is the ASA encoding difference — cobc370 writes an explicit blank record where
  ANS COBOL sets a `0` skip code in the carriage control. Same ink on the page,
  one more record on the wire.
