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

   Delivery to TK5, all verified: the XMIT onto an AWS tape (39
   cards a block, tapemark, tapemark), `devinit 0480`, IEBGENER to
   an FB80 dataset, NJE38 RECEIVE (one line, `NOPROMPT`;
   `INDATASET(...) DATASET(...) DIR(10)` -- TK5's RECV370 abends
   U0200-09 on this XMIT, its RECEIVE works). Then
   `EXEC PGM=COBC370,REGION=8192K` with SYSIN, SYSPUNCH, SYSPRINT,
   SYSTERM -- and SYSIN must be a real dataset: libc370's open path
   013-C0s on JES2 instream data.

5. The byte-diff -- **holds.** `hello.cbl` compiled on MVS 3.8j
   emits 1,036 lines of assembler identical to the host compiler's
   but one: the provenance comment names `dd:SYSIN` instead of the
   file. The full 131-test sweep on the guest remains to be scripted.
6. XMIT packaging and the compile-assemble-link proc, now with
   ld370 doing the packaging.

## cc370 findings to send upstream

Hit while putting a 6MB-extent module through tools tuned for small
ones; each fixed locally in the working copy:

- `as370`: `put()` writes past `text[TEXTMAX]` unchecked -- silent
  segfault. Guard added; `TEXTMAX` 1MB -> 16MB (the address space).
- `as370`: `MAXLIT` 8192 -> 65536 (clean message, but this module
  has more literals than that).
- `ld370`: `mod[1 << 20]` overflows on a >1MB module -- fortify
  abort, no message. 1MB -> 16MB.
- `libc370`: no `strncasecmp`; `fopen` of JES2 instream data abends
  013-C0.
- The backend has no BSS story: 4.7MB of zeroed tables emit as
  `DC X'00'` text, so the load module is 5.2MB where ~450KB is
  content. The right fix is DS emission (or linker-side gap
  handling), in cc370 itself.

## Open questions

- Guest-side compile time for a large program. Measure, don't
  assume.
- Whether the DS/BSS fix should trust program fetch to zero text
  gaps (fresh region pages are zero; reused ones are not).
