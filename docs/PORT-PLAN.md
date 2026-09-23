# Porting the compiler to MVS 3.8j itself

Asked for on r/cobol, 2026-09-21: put the compiler on the guest, so a
TK4- or TK5 system compiles COBOL with no host in the loop. This is
the survey of what that takes -- measured where it could be measured,
not guessed. It is a plan, not a commitment.

## The route

GCCMVS (Paul Edwards' GCC 3.2.3 port) with PDPCLIB, cross-compiled on
the host to S/370 assembler and assembled on the guest with IFOX00 --
the same delivery path this compiler's own output already uses, so
nothing new has to be trusted. TK4- ships GCCMVS in its user tools;
TK5 installs it. Compiling the compiler *on* the guest is possible in
principle and pointless in practice; the cross build is the route.

The source is nearly portable by accident. `cobc370.c` includes only
`stdio/stdlib/string/ctype`, allocates nothing (every table is
static), uses no floating point, and touches `long long` on exactly
two lines -- the constant folding of `**` (`cobc370.c:3187`), which
GCCMVS supports in software anyway. `atoll` is not C89; PDPCLIB may
not have it. A two-line shim either way.

## The one real obstacle: memory

An MVS 3.8j private region is roughly 8MB after the nucleus, LPA and
CSA take their share of the 16MB space. The compiler's static tables
total 11.26MB. Measured on the host build (`nm`, arm64 -- the 370
figures will be somewhat smaller, pointers being 4 bytes not 8, but
not five megabytes smaller):

| table | size | why |
|---|---|---|
| `stmts` | 7.9MB | 4096 entries x ~1.9KB. The `dop[8]` DISPLAY-operand array embeds `char lit[MAXTOK]` -- 132 bytes -- per operand: ~1.3KB of every `Stmt`, occupied or not |
| `pend_dc` | 1.2MB | 8192 x 148-byte formatted DC lines |
| `syms` | 0.8MB | fine |
| `nodes` | 0.75MB | fine |

The fix was machinery the compiler already had: an append-only text
pool in the spirit of `intern_str`, plus a side table for the
DISPLAY/CALL/DEPENDING operands most statements do not have. Done --
see Order of work, item 1. A `Stmt` is 464 bytes, the static tables
5.69MB on the host and less on the guest, and a TK4-/TK5 region
holds it with room for PDPCLIB's own stack and heap. This was the
only genuine compiler surgery in the port, and the host build keeps
the same diet.

## EBCDIC

Mostly self-correcting: an EBCDIC C compiler re-evaluates every `'A'`
and `'0'` at compile time, and the Ragel-generated `picture_scan.c`
compares character literals, not numeric byte tables, so the scanner
ports by recompilation.

Two audit items:

- `host_ebcdic()` (`cobc370.c:190`) is an ASCII-indexed translation
  table -- `t[u - 0x20]` over 0x20..0x7E. On the guest the input is
  already EBCDIC and the function must become the identity, behind
  an `#ifdef`.
- Any loop that assumes `'a'..'z'` is contiguous. EBCDIC letters
  have gaps (i-j, r-s). A grep pass over relational character
  comparisons.

## The boundary layer

`fopen(path)` and the `-I` directory walk become DD statements:

- source from `SYSIN`;
- copybooks from a PDS, PDPCLIB's `dd:SYSLIB(MEMBER)` syntax
  replacing the `-I` search -- the search order collapses to a
  SYSLIB concatenation, which is how IKFCBL00 does it anyway;
- assembler out to `SYSPUNCH` or a PDS member, fed straight to
  ASMFCLG;
- options through `PARM=` (100 characters holds `-s` and the rest
  easily).

Records arrive as F80 through PDPCLIB text mode, which the 80-column
reader already expects.

## Delivery

An XMIT of the load-module PDS plus a cataloged procedure -- COBCCLG,
chaining the compiler into ASMFCLG -- so the guest invokes it the way
it invokes IBM's compiler. RECV370 and done.

## Validation

Free, by the standard this repository already holds itself to:
compile all the test programs on the guest, translate the emitted
assembler to ASCII, and diff byte-for-byte against the host
compiler's output from the same source. If the two match, every
existing oracle-checked test result transfers to the port with no
new infrastructure. Any divergence is an EBCDIC or word-size bug by
definition, located to the line by the diff.

## Order of work

1. The memory diet -- **done, 2026-09-21**. Statement text (MOVE and
   DISPLAY literals, GO TO DEPENDING names, the pending DC lines,
   `para`/`thru`/`immdigits`) moved to one append-only 1MB pool; the
   DISPLAY/CALL/DEPENDING operand array became a side table the
   statement points into. A `Stmt` is 464 bytes, from 1,928; the
   static tables total 5.69MB, from 11.26MB, and the guest's 4-byte
   pointers shave roughly another megabyte off that. Verified the
   strong way: all 131 test programs compile to byte-identical
   assembler before and after, refusals to identical messages.
2. The EBCDIC audit -- **done, 2026-09-21**. Clean of letter-range
   comparisons; classification is ctype throughout; the Ragel tables
   compare character literals, so the scanner ports by
   recompilation. `host_ebcdic()` is the identity under
   `-DHOST_EBCDIC`. One find: the COUNT IN cell label (`SCnnnn` plus
   a letter by receiver position) ran out of alphabet at the 27th
   receiver, and would have run out at the 10th on EBCDIC where
   contiguity ends at I -- now refused at parse either way.
3. The DD-name boundary layer -- **done, 2026-09-21**, behind
   `-DMVS370` (which implies `HOST_EBCDIC`). No operands: SYSIN in,
   SYSPUNCH out, PARM carries only the options. `COPY text-name`
   reads `dd:SYSLIB(member)`; `COPY ... OF library-name` reads the
   library-name as a DD, which is what it meant on this system all
   along; member and DD names past 8 characters are refused with a
   message. `atoll` went with it -- the operands are validated
   digits, so a six-line loop reads them. The host build is
   byte-identical still, and the `-DMVS370` variant compiles clean
   under -Wall -Wextra; whether PDPCLIB's printf takes the one
   `%lld` is checked at step 4.
4. The toolchain -- **done, 2026-09-21, and not with GCCMVS.** The
   GCCMVS 3.2.3 route was walked first: patch applied, cross-compiler
   built (32-bit, in a container -- the shipped config hardcodes
   SIZEOF_LONG 4 and an LP64 host corrupts its own heap), and it
   still segfaulted on a three-line program. Abandoned for
   [mvslovers/cc370](https://github.com/mvslovers/cc370): GCC 3.4.6
   for i370, `as370` (an IFOX00 clone, byte-identical output),
   `ld370` (emits the load module *and* the XMIT), against
   [libc370](https://github.com/mvslovers/libc370). The whole build
   runs on the host; the guest receives a finished load module. Two
   -DMVS370 additions: a `strncasecmp` (POSIX's, not libc370's) and
   `src/mvs370-millicode.c` (`@@MULDI3`/`@@NEGDI2`, the 64-bit
   helpers `**` folding needs, built from 32-bit halves so they
   cannot call themselves).

   The build:

       cc370 -DMVS370 -O1 -std=gnu99 -I src \
             src/cobc370.c src/picture.c src/picture_scan.c \
             src/mvs370-millicode.c -o COBC370 -flinker-output=xmit

   (-O1 because cc370's backend documents higher levels unsafe. Do
   NOT write the output into `src/` as COBC370 -- the macOS
   filesystem is case-insensitive and it lands on the host binary.)

   For a build whose bytes reproduce, pin the two clocks as well:
   `ASMDATE`/`ASMTIME` for as370 and `LDDATE`/`LDTIME` for ld370, each
   of which otherwise stamps the current date and time into the deck
   and the module. `docs/INSTALL-MVS.txt` carries the full line, and
   the installation instructions that go with a published module.

   Delivery to TK5, all verified: the XMIT onto an AWS tape (39
   cards a block, tapemark, tapemark), `devinit 0480`, IEBGENER to
   an FB80 dataset, NJE38 RECEIVE (one line, `NOPROMPT`;
   `INDATASET(...) DATASET(...) DIR(10)` -- TK5's RECV370 abends
   U0200-09 on this XMIT, its RECEIVE works). Then
   `EXEC PGM=COBC370,REGION=8192K` with SYSIN, SYSPUNCH, SYSPRINT,
   SYSTERM -- and SYSIN must be a real dataset, because this
   compiler opens it by name and libc370's startup already holds it,
   which on instream data is a 013-C0 (mvslovers/libc370#184).

5. The byte-diff -- **done, 2026-09-21: 126 of 126 identical.**
   Every compiling test, compiled ON MVS 3.8j (`bin/cobc-port-sweep`:
   sources aboard as a PDS by xmit370 and tape, one COBC370 step per
   test, IEBPTPCH back, diff against fresh host references). What is
   normalized and why it is honest to: the provenance comment (the
   guest names dd:SYSIN), IEBPTPCH page furniture, and three bytes of
   the Hercules printer's EBCDIC-to-ASCII display table that disagree
   with CP037 on the way out -- `[`, `]` and `|` read back as 0xAA,
   0xB3, 0xD7. The dataset bytes are CP037; only the readback skews.

   One real port bug was found and fixed on the way, and it falsifies
   what the step-2 audit claimed about the scanner: Ragel bakes
   character RANGES as numeric ASCII byte values in the DFA tables
   (`48 <= (*p) && (*p) <= 57`), which no recompilation fixes. Every
   PICTURE was refused at character 1 on the guest. `picture.rl` now
   translates EBCDIC input to ASCII at pic_scan's entry (the DFA's
   alphabet), emits symbols back in the execution character set, and
   parses repeat counts with explicit ASCII arithmetic -- the host
   build's output is unchanged, byte for byte, across all 131 tests.
6. The proc -- **done, 2026-09-21**: `jcl/COBCCLG.jcl`, verified on
   TK5 end to end. COBOL member in, COBC370 -> IFOX00 -> IEWL -> GO,
   program output out, no host anywhere in the loop (job 706:
   RC=0000 four times, then HELLO's two DISPLAY lines). One trap
   recorded in the proc: IFOX00 wants `PARM='OBJ,NODECK'` or it
   assembles cleanly and produces nothing, RC=16 IFO257. The
   installable remains `COBC370.xmit`, which ld370 already packages;
   what is left of this step is only an install writeup.

## cc370 findings to send upstream

Hit while putting a 6MB-extent module through tools tuned for small
ones. Everything below except the libc370 items is fixed in
`docs/cc370-fixes.patch` (against mvslovers/cc370 main of
2026-09-21); the local toolchain install carries it. Upstream,
2026-09-22, at the maintainer's request: the size/bounds fixes are
mvslovers/cc370#444, the BSS change is #445 with its discussion in
issue #443, and the libc370 findings are mvslovers/libc370#183 and
#184 (the latter with the verified minimal reproducer):

- `as370`: `put()` writes past `text[TEXTMAX]` unchecked -- silent
  segfault. Guard added; `TEXTMAX` 1MB -> 16MB (the address space).
- `as370`: `MAXLIT` 8192 -> 65536 (clean message, but this module
  has more literals than that).
- `ld370`: `mod[1 << 20]` overflows on a >1MB module -- fortify
  abort, no message. 1MB -> 16MB.
- `libc370` (not in the patch): no `strncasecmp`; and a second
  concurrent open of an instream DD abends 013-C0 -- the startup
  already holds SYSIN as stdin, so any `fopen("dd:SYSIN")` is
  inherently a second open, which a real dataset tolerates and the
  spool dataset does not. (First reported here, imprecisely, as
  "fopen of instream data abends"; the minimal reproducer narrowed
  it.)
- **The BSS story, half landed and half withdrawn.**
  TARGET_PDPMAC's `ASM_OUTPUT_SKIP` emitted `DC nX'00'`, so 4.7MB of
  zeroed tables shipped as text. Two changes were made here; upstream
  review (mvslovers/cc370#443) accepted one and holed the other.

  *The skip becomes `DS XLn`*, as the target's other flavor already
  does. Semantically free -- as370 extends the section length, ld370
  materialises the reservation -- and it carries the whole
  deck-size win: 7,267,520 bytes to 560,480, with the member
  unmoved at 5,170,233. Filed as #446.

  *`ld370` elides all-zero text records*: withdrawn to a draft. Three
  things are wrong with it. It turns `ld370/tests/run.sh` red, by
  eliding a fixture's 32,000 deliberately written zeros until a guard
  against packing an oversized block has nothing left to refuse. Its
  predicate is the byte value while its comment claims definedness,
  and those differ exactly when something writes a zero on purpose;
  ld370 discards definedness at `ld370.c:258` before the emit loop
  can see it. And the justification recorded here and in commit
  293997f -- "the same behavior IEWL-linked assembler `DS` has always
  had" -- **is false**, measured over 5,230 `SYS1.AOS*` members and
  5,528 IFOX00 decks: IFOX00 does leave a `DS` unemitted, but IEWL
  fills it in (1,629 of 1,631), fills it with non-zero binder residue
  rather than zeros, and does not elide all-zero records even when
  given a pure one. The rework carries definedness as `o->defn` into
  `moddef[]`, so the emit loop stops asking what the bytes are, and
  hides it behind `--sparse-text` defaulting off
  (mvslovers/cc370#447). The win survives the stricter predicate
  intact -- cc370's skips emit no TXT cards at all -- and with the
  flag off the output is byte-identical to upstream.

  **The storage question is measured, and it holds.** A module
  region no TXT record covers reads as zero even when the same
  storage demonstrably held X'FF' moments earlier, under
  LOAD / DELETE / LOAD landing at an identical address: see
  `docs/fetch-probe/`, and the maintainer's independent assembler
  probe in mvslovers/cc370#443, which agrees. It is one system and
  probably page-level zeroing rather than an architectural promise,
  which is why the flag defaults off -- but the 469,197-byte COBC370
  that passes 126 of 126 stands, and the objection is answered
  rather than assumed.

## Open questions

- Guest-side compile time for a large program. Measure, don't
  assume.
