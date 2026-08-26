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

A likely explanation is that ANS COBOL link-edits substantial runtime from
`SYS1.COBLIB` into every module, where cobc370 emits only its own COBRT and
touches SYS1.COBLIB not at all. **That is unverified** — confirming it means
reading a module map with AMBLIST, which has not been done.

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
