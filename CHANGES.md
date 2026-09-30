# Changes

One entry per package of the MVS load module, newest first. A package is
named by its date and what it brought; the SHA256 is of `COBC370.xmit`,
and `docs/INSTALL-MVS.txt` in the package describes that build. Each entry
names the cobc370 commit it was built from, so `git log` between two
entries has the detail. Tests: how many compile on the guest byte-identical
to the host compiler, which is what the port sweep checks.

## 2026-09-30 NOTRUNC

`cd4fbc9`, SHA256 `ff4db3a53f9d8dc7…`, 614,560 bytes, 181 tests.

- `COMP` items follow IBM's `NOTRUNC`, IKFCBL00's default on this system
  (#45): an arithmetic result or a literal stored into a `COMP` item keeps
  its binary value past the PICTURE, wrapping at 16 or 32 bits; `ON SIZE
  ERROR` still tests the PICTURE; `MOVE` of an item still truncates to the
  receiving PICTURE. Measured on IKFCBL00 case by case (`notrunc`). Before,
  cobc370 truncated to the PICTURE everywhere but the binary `ADD`.

## 2026-09-29

`23f935e`, SHA256 `6bf7e5568d27e6f9…`, 614,320 bytes, 180 tests.

- `MOVE` of a numeric literal or `ZERO` to a halfword or fullword `COMP`
  item is one `MVC` from a binary constant, as IBM's compilers do (Harry E,
  H390-MVS). IBM's default `NOTRUNC` keeps what fits the binary item where
  cobc370 then truncated to the PICTURE: recorded as #45.
- `MOVE` of an item of 4096 bytes or more assembled to IFO208: its length
  was loaded with `LA`, whose reach is 4095 (Harry E). Fixed, with a test.
- An output error, such as SYSPUNCH out of space, now ends the compile with
  RC 2 instead of 0 and a truncated deck (#44, from Mike Grossmann).

## 2026-09-26 IBM-extensions

`e9a66f9`, SHA256 `f58f4084c10c1549…`, 613,920 bytes, 178 tests.

- IKFCBL00's own statements: `EXHIBIT` (`NAMED`, `CHANGED`), `TRANSFORM`,
  `ON ... AND EVERY ... UNTIL`, `READY`/`RESET TRACE` and `NOTE`, each
  measured against IBM's compiler first.

## 2026-09-26 Segmentation

`e8410f3`, SHA256 `179fcc85a39de99a…`, 598,720 bytes, 175 tests.

- Segmentation at Level 2: `SEGMENT-LIMIT`, and an independent segment
  (50 to 99) back in its initial state whenever control reaches it from
  another segment, as IBM's overlays behave.
- An `INSPECT` test separating one pass from a pass per phrase (#42).

## 2026-09-26 Sort-Merge

`831b145`, SHA256 `1fdcb21fe0402665…`, 584,960 bytes, 173 tests.

- `MERGE`, done by the compiler itself, since the system sort merges only
  when JCL starts it (#25).
- `STOP RUN` closes the runtime in every program; a PARM test.
- Built with cc370 `335e7d0`, which fixed the 64-bit shift found here.

## 2026-09-26 floating-point

`a211d1f`, SHA256 `8169e5a8f05769ac…`, 583,680 bytes, 168 tests.

- `COMP-1` and `COMP-2`, IBM's hexadecimal floating point (#40), for John
  Pratt's COBCAL74.
- Every error in a compile is reported, not only the first (#41).
- `INSPECT` makes one ordered pass per phrase (#32); an implicit `STOP RUN`
  at the end, 64 `CALL` arguments (#39); `VALUE` with `OCCURS` and `OCCURS`
  at level 01/77 refused, as IBM refuses them (#34).

## 2026-09-26 audit

`259eb4b`, SHA256 `0a1b7586e7a7dcbf…`, 538,720 bytes, 160 tests.

- The audit's fixes across arithmetic, editing, `MOVE`, conditions,
  `PERFORM`, data layout, code size, VSAM, QSAM and ISAM.
- Literal `**` folded in decimal digits, and the runtime's 64-bit millicode
  kept clear of a cc370 shift bug (reported upstream as cc370#468).

## 2026-09-25 night

`d471118`, SHA256 `d6e7308f117b152d…`, 523,360 bytes, 141 tests.

- `CALL` leaves the called program's return code in `RETURN-CODE`, and
  each program's WORKING-STORAGE is its own (found by Vince Coen's code).

## 2026-09-25 late

`79b896d`, SHA256 `14cf5a983794a0b4…`, 523,280 bytes, 141 tests.

- Report files are 133 bytes, `RECFM=FA`, as IKFCBL00's are.

## 2026-09-25 evening

`7bb01cc`, SHA256 `fed3b3a5cc165f21…`, 523,280 bytes, 140 tests.

- `SORT`, `RELEASE` and `RETURN` through the system sort.
- A code base per paragraph, so a program's code has no size limit.
- `WRITE ... ADVANCING` as IBM writes it.
- Vince Coen's COBXREF joins the tests.
- The module links with `--sparse-text`: 5.5 MB down to 523 KB.

## 2026-09-25

`2dac69c`, SHA256 `8e83a02ef0883953…`, 5,232,960 bytes, 133 tests.

- `EXAMINE` and `TALLY`, `DISPLAY` of `COMP` items, and `GO TO DEPENDING`
  past eight names (from Ed Liss's programs).

## 2026-09-24

`8fee693`, SHA256 `6568aae58d9ed99d…`, 5,228,240 bytes, 130 tests.

- `01 NAME COPY MEMBER`, `ID DIVISION`, `EJECT`, `SKIP1`-`SKIP3` and
  `RETURN-CODE`: IBM spellings Ed Liss's programs used.

## 2026-09-22

`c4f3cfc`, SHA256 `e54a842a5b5b3903…`, 5,226,080 bytes, 126 tests.

- The first package: COBC370 as a load module that runs on MVS 3.8j.
