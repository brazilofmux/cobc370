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

The fix is machinery the compiler already has: `intern_str`. Intern
the `dop` literals instead of embedding them, pool the `pend_dc`
text, and a `Stmt` drops from ~1.9KB to a few hundred bytes. Total
static footprint lands near 3-4MB and fits a TK4-/TK5 region with
room for PDPCLIB's own stack and heap. This is the only genuine
compiler surgery in the port, and the host build benefits from the
same diet.

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

1. The memory diet (intern `dop`, pool `pend_dc`) -- on the host,
   under the existing regression suite, before any 370 code exists.
2. The EBCDIC audit -- `host_ebcdic` behind `#ifdef`, the
   contiguity grep.
3. The DD-name boundary layer.
4. GCCMVS build; first guest compile of `hello.cbl`.
5. The full-suite byte-diff.
6. XMIT packaging and the COBCCLG proc.

## Open questions

- Which GCCMVS release assembles cleanly under IFOX00 on TK5 as
  shipped; whether PDPCLIB's `dd:` member syntax behaves on a
  concatenation.
- Guest-side compile time for a large program. The emulated CPU is
  fast; GCCMVS-generated code is not IBM-tuned. Measure, don't
  assume.
