# cobc370 -- COBOL cross-compiler for MVS 3.8j.
# Named to avoid colliding with GnuCOBOL's cobc, which is the oracle.
CFLAGS = -O2 -Wall -Wextra

# picture_scan.c is generated but committed, so the compiler builds without
# ragel installed. Regenerate with `make ragel` after editing picture.rl.
cobc370: cobc370.c picture.c picture_scan.c picture.h
	$(CC) $(CFLAGS) -I. -o $@ cobc370.c picture.c picture_scan.c

ragel: picture.rl
	ragel -G2 -o picture_scan.c picture.rl

test: cobc370
	./cobc370 tests/stoprun.cbl -o tests/stoprun.asm
	./cobc370 tests/hello.cbl   -o tests/hello.asm
	./cobc370 tests/arith.cbl   -o tests/arith.asm
	./cobc370 tests/compute.cbl -o tests/compute.asm
	./cobc370 tests/grpperf.cbl -o tests/grpperf.asm
	./cobc370 tests/iftest.cbl  -o tests/iftest.asm
	./cobc370 tests/filetest.cbl -o tests/filetest.asm
	./cobc370 tests/occtest.cbl -o tests/occtest.asm
	./cobc370 tests/edtest.cbl  -o tests/edtest.asm
	./cobc370 tests/perftest.cbl -o tests/perftest.asm
	./cobc370 tests/rpttest.cbl -o tests/rpttest.asm
	@echo "--- column 72 is the continuation flag, not spare room ---"
	@# A statement must stop at column 71. A line reaching column 72 is only
	@# legal when that column holds the deliberate X of a continued statement;
	@# anything else makes the assembler read the next line as a continuation
	@# and report IFO026. Checking >72 misses this by exactly one column.
	@! awk 'length($$0)>72 || (length($$0)==72 && substr($$0,72,1)!="X")' \
	    tests/*.asm | grep . || (echo "COLUMN 72 VIOLATION"; exit 1)
	@echo "--- STOP RUN alone must not drag in the runtime ---"
	@! grep -q 'COBRT' tests/stoprun.asm || (echo "runtime emitted needlessly"; exit 1)
	@echo "--- bad input must be rejected, not silently accepted ---"
	@# Assert a nonzero exit rather than matching wording, and use fixtures
	@# that cannot become valid as coverage grows: an undeclared name and a
	@# duplicate name are errors forever, and ALTER has zero uses in the
	@# corpus and is not going to be implemented. Twice now a fixture went
	@# stale because the next slice implemented the very thing it tested.
	@for t in bad-undeclared bad-verb bad-duplicate; do \
	   if ./cobc370 tests/$$t.cbl -o /dev/null >/dev/null 2>&1; then \
	     echo "  $$t ACCEPTED -- it should have been rejected"; exit 1; \
	   else echo "  $$t rejected"; fi; \
	 done

# The oracle. -std=mvs is not optional: GnuCOBOL's default dialect renders a
# decimal point for PIC 9(7)V99, which COBOL-74 does not -- V is implied, not
# stored. Comparing against the default dialect reads as a mismatch when the
# values are in fact identical.
COB_CONFIG_DIR ?= /usr/local/share/gnucobol/config
# tests/data/infile.txt must be an exact multiple of the record length with no
# newlines: ORGANIZATION SEQUENTIAL reads fixed-length records, so a trailing
# newline per line adds a byte each and manufactures a fifth, partial record.
oracle:
	@for t in arith compute grpperf iftest bigws occtest occmin edtest perftest rpttest; do \
	  COB_CONFIG_DIR=$(COB_CONFIG_DIR) cobc -x -std=mvs -o /tmp/oracle_$$t tests/$$t.cbl 2>/dev/null; \
	  /tmp/oracle_$$t > tests/$$t.expected; \
	  echo "--- $$t"; cat tests/$$t.expected; \
	done

clean:
	rm -f cobc370 tests/*.asm

.PHONY: test oracle ragel clean
