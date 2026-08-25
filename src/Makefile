# cobc370 -- COBOL cross-compiler for MVS 3.8j.
# Named to avoid colliding with GnuCOBOL's cobc, which is the oracle.
CFLAGS = -O2 -Wall -Wextra

cobc370: cobc370.c
	$(CC) $(CFLAGS) -o $@ $<

test: cobc370
	./cobc370 tests/stoprun.cbl -o tests/stoprun.asm
	./cobc370 tests/hello.cbl   -o tests/hello.asm
	./cobc370 tests/arith.cbl   -o tests/arith.asm
	@echo "--- no line may exceed 72 columns ---"
	@! awk 'length($$0)>72' tests/*.asm | grep . || (echo "COLUMN VIOLATION"; exit 1)
	@echo "--- STOP RUN alone must not drag in the runtime ---"
	@! grep -q 'COBRT' tests/stoprun.asm || (echo "runtime emitted needlessly"; exit 1)
	@echo "--- unimplemented constructs must be diagnosed ---"
	@./cobc370 tests/bad.cbl -o /dev/null 2>&1 | grep -q 'not implemented' \
	   && echo "  MOVE          diagnosed" || (echo "MOVE not diagnosed"; exit 1)
	@./cobc370 tests/baddisp.cbl -o /dev/null 2>&1 | grep -q 'not implemented' \
	   && echo "  DISPLAY ident diagnosed" || (echo "DISPLAY ident not diagnosed"; exit 1)

# The oracle. -std=mvs is not optional: GnuCOBOL's default dialect renders a
# decimal point for PIC 9(7)V99, which COBOL-74 does not -- V is implied, not
# stored. Comparing against the default dialect reads as a mismatch when the
# values are in fact identical.
COB_CONFIG_DIR ?= /usr/local/share/gnucobol/config
oracle:
	COB_CONFIG_DIR=$(COB_CONFIG_DIR) cobc -x -std=mvs -o /tmp/arith_oracle tests/arith.cbl
	/tmp/arith_oracle > tests/arith.expected
	@cat tests/arith.expected

clean:
	rm -f cobc370 tests/*.asm

.PHONY: test oracle clean
