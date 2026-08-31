# cobc370 -- COBOL cross-compiler for MVS 3.8j.
# Named to avoid colliding with GnuCOBOL's cobc, which is the oracle.
CFLAGS = -O2 -Wall -Wextra

# picture_scan.c is generated but committed, so the compiler builds without
# ragel installed. Regenerate with `make ragel` after editing picture.rl.
cobc370: cobc370.c picture.c picture_scan.c picture.h
	$(CC) $(CFLAGS) -I. -o $@ cobc370.c picture.c picture_scan.c

ragel: picture.rl
	ragel -G2 -o picture_scan.c picture.rl

T=../tests
test: cobc370
	./cobc370 $(T)/stoprun.cbl -o $(T)/stoprun.asm
	./cobc370 $(T)/hello.cbl   -o $(T)/hello.asm
	./cobc370 $(T)/arith.cbl   -o $(T)/arith.asm
	./cobc370 $(T)/compute.cbl -o $(T)/compute.asm
	./cobc370 $(T)/grpperf.cbl -o $(T)/grpperf.asm
	./cobc370 $(T)/iftest.cbl  -o $(T)/iftest.asm
	./cobc370 $(T)/filetest.cbl -o $(T)/filetest.asm
	./cobc370 $(T)/occtest.cbl -o $(T)/occtest.asm
	./cobc370 $(T)/edtest.cbl  -o $(T)/edtest.asm
	./cobc370 $(T)/perftest.cbl -o $(T)/perftest.asm
	./cobc370 $(T)/rpttest.cbl -o $(T)/rpttest.asm
	./cobc370 $(T)/link1st.cbl -o $(T)/link1st.asm
	./cobc370 $(T)/nextsent.cbl -o $(T)/nextsent.asm
	./cobc370 $(T)/dup88.cbl -o $(T)/dup88.asm
	./cobc370 $(T)/pgroup.cbl -o $(T)/pgroup.asm
	./cobc370 $(T)/gusage.cbl -o $(T)/gusage.asm
	./cobc370 $(T)/ucomp.cbl -o $(T)/ucomp.asm
	./cobc370 $(T)/bwzdisp.cbl -o $(T)/bwzdisp.asm
	./cobc370 $(T)/dline.cbl -o $(T)/dline.asm
	./cobc370 $(T)/use77.cbl -o $(T)/use77.asm
	./cobc370 $(T)/exitonly.cbl -o $(T)/exitonly.asm
	./cobc370 $(T)/callsub.cbl -o $(T)/callsub.asm
	./cobc370 $(T)/sub2dif.cbl -o $(T)/sub2dif.asm
	./cobc370 $(T)/movelit.cbl -o $(T)/movelit.asm
	./cobc370 $(T)/cdcopy.cbl -o $(T)/cdcopy.asm -I $(T)
	@echo "--- column 72 is the continuation flag, not spare room ---"
	@# A statement must stop at column 71. A line reaching column 72 is only
	@# legal when that column holds the deliberate X of a continued statement;
	@# anything else makes the assembler read the next line as a continuation
	@# and report IFO026. Checking >72 misses this by exactly one column.
	@! awk 'length($$0)>72 || (length($$0)==72 && substr($$0,72,1)!="X")' \
	    $(T)/*.asm | grep . || (echo "COLUMN 72 VIOLATION"; exit 1)
	@echo "--- STOP RUN alone must not drag in the runtime ---"
	@! grep -q 'COBRT' $(T)/stoprun.asm || (echo "runtime emitted needlessly"; exit 1)
	@echo "--- P occupies no storage: a group PIC 9(3)PP is three bytes ---"
	@grep -q "CL3'123'" $(T)/pgroup.asm || (echo "PGROUP layout wrong"; exit 1)
	@echo "--- group USAGE COMP-3 is inherited ---"
	@grep -q 'COMP-3' $(T)/gusage.asm || (echo "GUSAGE not packed"; exit 1)
	@echo "--- unaligned COMP goes through DWK, not LH on the field ---"
	@grep -q 'not on its boundary' $(T)/ucomp.asm || (echo "UCOMP still uses LH on an odd offset"; exit 1)
	@echo "--- BLANK WHEN ZERO skips the UNPK when the value is zero ---"
	@grep -q 'then spaces' $(T)/bwzdisp.asm || (echo "BWZDISP missing space fill"; exit 1)
	@echo "--- a D in column 7 is a comment ---"
	@! grep -q "DEBUG" $(T)/dline.asm || (echo "DLINE compiled a debugging line"; exit 1)
	@echo "--- a 40-character MOVE literal is stored whole ---"
	@grep -q "CL40'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB'" $(T)/movelit.asm \
	    || (echo "MOVELIT constant truncated"; exit 1)
	@echo "--- bad input must be rejected, not silently accepted ---"
	@# Assert a nonzero exit rather than matching wording, and use fixtures
	@# that cannot become valid as coverage grows: an undeclared name and a
	@# duplicate name are errors forever, and ALTER has zero uses in the
	@# corpus and is not going to be implemented. Twice now a fixture went
	@# stale because the next slice implemented the very thing it tested.
	@for t in bad-undeclared bad-verb bad-duplicate bad-ambiguous bad-vspace; do \
	   if ./cobc370 $(T)/$$t.cbl -o /dev/null >/dev/null 2>&1; then \
	     echo "  $$t ACCEPTED -- it should have been rejected"; exit 1; \
	   else echo "  $$t rejected"; fi; \
	 done

# GnuCOBOL is not authoritative everywhere. DISPLAY of a signed DISPLAY item is
# one place it diverges: -std=mvs renders PIC S9(5) VALUE 12345 as "12345+",
# while ANS COBOL on the guest prints "1234E" -- the sign overpunched into the
# low-order digit, X'C5'. ANS only produces a separate trailing sign as an
# edited field, and that conversion is one-way. Where the two disagree the
# guest's own IKFCBL00 is the oracle; see tests/misc2.expected.
#
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
	rm -f cobc370 ../tests/*.asm

.PHONY: test oracle ragel clean
