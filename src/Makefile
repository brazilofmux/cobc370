CFLAGS = -O2 -Wall -Wextra

cobc: cobc.c
	$(CC) $(CFLAGS) -o $@ $<

test: cobc
	./cobc tests/stoprun.cbl -o tests/stoprun.asm
	./cobc tests/hello.cbl   -o tests/hello.asm
	@echo "--- no line may exceed 72 columns ---"
	@! awk 'length($$0)>72' tests/*.asm | grep . || (echo "COLUMN VIOLATION"; exit 1)
	@echo "--- STOP RUN alone must not drag in the runtime ---"
	@! grep -q 'COBRT' tests/stoprun.asm || (echo "runtime emitted needlessly"; exit 1)
	@echo "--- unimplemented statements must be diagnosed ---"
	@./cobc tests/bad.cbl -o /dev/null 2>&1 | grep -q 'not implemented' \
	   && echo "ok" || (echo "diagnostic missing"; exit 1)

clean:
	rm -f cobc tests/stoprun.asm tests/hello.asm

.PHONY: test clean
