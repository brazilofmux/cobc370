# Micro-benchmarks

Nine COBOL programs, each running one construct a million times (100 x 1000
iterations of a paragraph that does it ten times), plus an empty loop to
subtract. `run.sh` assembles each with cobc370 and compiles it with IKFCBL00
on the guest, runs both, and reads the CPU seconds from the step accounting.
That is the instrument for optimization work: the production batch is the
safety net, but it spends its time in I/O, SORT and COMPUTE and moves a few
hundredths of a second whatever is done to a primitive.

Results are recorded in `docs/COBOL74-ROADMAP.md`, under Optimization.
