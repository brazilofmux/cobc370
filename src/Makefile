CFLAGS = -O2 -Wall -Wextra

cobc: cobc.c
	$(CC) $(CFLAGS) -o $@ $<

test: cobc
	./cobc tests/stoprun.cbl -o tests/stoprun.asm
	@echo "--- generated ---"
	@cat tests/stoprun.asm
	@echo "--- diagnostics ---"
	@! ./cobc tests/bad.cbl -o /dev/null 2>&1 | grep -q 'DISPLAY' || \
	   echo "unsupported statements are reported, good"

clean:
	rm -f cobc tests/stoprun.asm

.PHONY: test clean
