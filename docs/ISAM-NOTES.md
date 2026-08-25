# ISAM: what works, and where BISAM stopped

Sequential ISAM works. Random ISAM does not, and this records exactly how far
it got so the next attempt starts from evidence rather than from scratch.

The corpus is **read-only** on ISAM — nothing adds, modifies or deletes a
record — so `READ` is the whole requirement. The datasets were loaded in sorted
order once, out of band.

## Works: QISAM sequential

`SELECT f ASSIGN TO DA-I-name ACCESS IS SEQUENTIAL RECORD KEY IS k`, with
`DSORG=IS,MACRF=(GM)` and ordinary `GET`. `AT END` uses the same `DCBEODAD`
patch as QSAM — offset 33 turns out to be right for QISAM too.

Verified against the live `SVD001.GLACCT`:

    KEY 0000010202 Stephen's Coinbox
    KEY 0000010301 Dwolla
    KEY 0000010303 MtGox

Cross-checked independently: `IEBISAM PARM=PRINTL` dumps the same dataset in
hex, and hand-decoding the first three records from EBCDIC gives the same keys
and names. Two routes to the same answer.

## Does not work: BISAM random

`ACCESS IS RANDOM ... NOMINAL KEY` is parsed and analysed but **rejected at code
generation** rather than emitting a program that abends. What the probes in
`jcl/isam/` establish, in order:

| Probe | Result |
|---|---|
| `probe-open-only` | `OPEN (dcb,INPUT)` on `DSORG=IS,MACRF=(R)` **succeeds** — DCBOFLGS open bit set |
| `probe-read-routine-addr` | after OPEN, `DCB+88` (the read/write routine address the READ macro branches through) is **non-zero** |
| `probe-read-no-check` | the `READ` macro itself faults **S0C4**, before any `CHECK` |
| `probe-read-check` | same, S0C1 with CHECK present |
| `probe-dynamic-buffering` | `MACRF=(RUS)` with `READ ...,'S','S',key` also fails |

So the file opens, the access method loads its entry point, and the branch into
it faults. That rules out the obvious causes — wrong DSORG, wrong MACRF letter,
an unopened DCB — and points at the DECB contents or a DCB field the macro
assumes and the label did not supply.

Untried, in rough order of promise:

- `RKP` and `KEYLEN`. The key sits at **offset 1**, after the one-byte delete
  flag every ISAM record carries, so `RKP=1` and `KEYLEN=6` for GLACCT. OPEN
  should take these from the DSCB, but supplying them on the DCB is cheap to
  test.
- `BLKSIZE`/`LRECL`/`RECFM` explicitly on the DCB rather than from the label.
- `MSGDCB`/`SYNAD` register conventions: the SYNAD used here assumes R12 is
  preserved on entry, which is worth confirming before trusting the flag.
- Whether TK5's MVS 3.8j has complete BISAM support at all. QISAM demonstrably
  works; BISAM is a separate module set.

**The right long-term answer is probably not to fix this.** The corpus already
contains `SVD001.VSAMIO.*` from earlier experiments, VSAM is available on this
system, and ISAM is the access method everyone left behind for good reason.
Random retrieval via VSAM KSDS would replace this cleanly.

## A sign bug found on the way

`PIC 9(n) COMP-3` — *unsigned* packed — must carry an `F` sign nibble. `ZAP`
leaves `C`. That never mattered while packed fields were only ever compared
arithmetically, and matters the moment one is compared byte-wise, which is
exactly what an ISAM key is. `gen_store` now forces it:

    ZAP   D0008(6),PWK1(8)
    OI    D0008+5,X'0F'       unsigned: force an F sign

Same shape as the `OI ...,X'F0'` that un-overpunches an unsigned zoned field.
